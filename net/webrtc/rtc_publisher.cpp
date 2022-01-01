#include "rtc_publisher.hpp"
#include "rtc_session_pub.hpp"
#include "rtc_base_session.hpp"
#include "pack_handle_h264.hpp"
#include "pack_handle_audio.hpp"
#include "format/audio_pub.hpp"
#include "net/rtprtcp/rtcp_pspli.hpp"
#include "format/flv/flv_mux.hpp"
#include "format/flv/flv_pub.hpp"
#include "utils/timer.hpp"
#include "utils/timeex.hpp"
#include "utils/logger.hpp"
#include "utils/uuid.hpp"
#include "utils/byte_stream.hpp"
#include "utils/av/media_packet.hpp"
#include "utils/av/media_stream_manager.hpp"
#include <sstream>
#include <cstring>

extern boost::asio::io_context& get_global_io_context();

rtc_publisher::rtc_publisher(const std::string& roomId, const std::string& uid,
        room_callback_interface* room, rtc_base_session* session, const MEDIA_RTC_INFO& media_info):timer_interface(get_global_io_context(), 500)
        , roomId_(roomId)
        , uid_(uid)
        , room_(room)
        , session_(session)
        , media_info_(media_info)
        , jb_handler_(this, get_global_io_context()) {
    pid_ = make_uuid();
    media_type_ = media_info_.media_type;

    clock_rate_ = media_info_.rtp_encodings[0].clock_rate;

    has_rtx_ = false;
    for (auto enc_item : media_info_.rtp_encodings) {
        if (enc_item.codec == "rtx") {
            has_rtx_ = true;
            rtx_payloadtype_ = (uint8_t)enc_item.payload;
        } else {
            payloadtype_ = (uint8_t)enc_item.payload;
        }
    }

    if (has_rtx_ && !media_info_.ssrc_groups.empty()) {
        SSRC_GROUPS group = media_info_.ssrc_groups[0];
        if (group.ssrcs.size() >= 2) {
            rtp_ssrc_ = group.ssrcs[0];
            rtx_ssrc_ = group.ssrcs[1];
        } else {
            rtp_ssrc_ = group.ssrcs[0];
            log_warnf("the rtc publisher has only rtp ssrc:%u, but has no rtx ssrc", rtp_ssrc_);
        }
    } else if (!has_rtx_ && !media_info_.ssrc_groups.empty()) {
        SSRC_GROUPS group = media_info_.ssrc_groups.at(0);
        rtp_ssrc_ = group.ssrcs.at(0);
    } else {
        size_t index = 0;
        for (auto info : media_info_.ssrc_infos) {
            if (index == 0) {
                rtp_ssrc_ = info.ssrc;
            } else {
                rtx_ssrc_ = info.ssrc;
            }
        }
    }

    for (auto ext_item : media_info_.header_extentions) {
        if (ext_item.uri == "urn:ietf:params:rtp-hdrext:sdes:mid") {
            mid_extension_id_ = ext_item.value;
        } else if (ext_item.uri == "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time") {
            abs_time_extension_id_ = ext_item.value;
        }
    }
    if (media_type_ == "video") {
        pack_handle_ = new pack_handle_h264(this, get_global_io_context());
    } else {
        pack_handle_ = new pack_handle_audio(this);
    }
    
    start_timer();
    log_infof("rtc_publisher construct media type:%s, rtp ssrc:%u, rtx ssrc:%u, clock rate:%d, \
payload:%d, has rtx:%d, rtx payload:%d, mid:%d, mid extension id:%d, abs_time_extension_id:%d, id:%s",
        this->get_media_type().c_str(), rtp_ssrc_, rtx_ssrc_, clock_rate_, payloadtype_,
        has_rtx_, rtx_payloadtype_, get_mid(), mid_extension_id_, abs_time_extension_id_,
        pid_.c_str());
}

rtc_publisher::~rtc_publisher() {
    log_infof("rtc_publisher destruct media type:%s, rtp ssrc:%u, rtx ssrc:%u, mid:%d",
        this->get_media_type().c_str(), rtp_ssrc_, rtx_ssrc_, get_mid());
    stop_timer();
    if (rtp_handler_) {
        delete rtp_handler_;
    }
    if (pack_handle_) {
        delete pack_handle_;
    }
}

std::string rtc_publisher::get_media_type() {
    return media_type_;
}

int rtc_publisher::get_clockrate() {
    return clock_rate_;
}

uint8_t rtc_publisher::get_rtp_payloadtype() {
    return payloadtype_;
}

uint8_t rtc_publisher::get_rtx_payloadtype() {
    return rtx_payloadtype_;
}

bool rtc_publisher::has_rtx() {
    return has_rtx_;
}

void rtc_publisher::on_handle_rtppacket(rtp_packet* pkt) {
    //set mid
    pkt->set_mid_extension_id((uint8_t)mid_extension_id_);
    pkt->set_abs_time_extension_id((uint8_t)abs_time_extension_id_);

    if ((pkt->get_ssrc() == rtp_ssrc_) && (pkt->get_payload_type() == payloadtype_)) {
        if (!rtp_handler_) {
            rtp_handler_ = new rtp_recv_stream(this, media_type_, pkt->get_ssrc(), payloadtype_, false, get_clockrate());
            if (has_rtx()) {
                rtp_handler_->set_rtx_ssrc(rtx_ssrc_);
                rtp_handler_->set_rtx_payloadtype(rtx_payloadtype_);
            }
        }
        rtp_handler_->on_handle_rtp(pkt);
    } else if (has_rtx() && (pkt->get_ssrc() == rtx_ssrc_) && (pkt->get_payload_type() == rtx_payloadtype_)) {
        if (rtp_handler_) {
            rtp_handler_->on_handle_rtx_packet(pkt);
        } else {
            log_warnf("rtp(%s) handler is not ready for rtx, rtx_ssrc:%u, rtx_payload_type:%d",
                media_type_.c_str(), rtx_ssrc_, rtx_payloadtype_);
        }
    } else {
        log_errorf("unkown packet payload:%d, packet ssrc:%u, media type:%s, has rtx:%d, rtp ssrc:%u, rtx ssrc:%u",
            pkt->get_payload(), pkt->get_ssrc(), media_type_.c_str(), has_rtx_, rtp_ssrc_, rtx_ssrc_);
        return;
    }

    uint8_t pkt_mid = 0;
    bool ret_mid = false;
    uint32_t abs_time = 0;
    bool ret_abs_time = false;
    ret_mid = pkt->read_mid(pkt_mid);
    ret_abs_time = pkt->read_abs_time(abs_time);
    log_debugf("rtp media:%s mid:%d:%d, abs_time:%u:%d",
        media_type_.c_str(), pkt_mid, ret_mid, abs_time, ret_abs_time);
    
    jb_handler_.input_rtp_packet(roomId_, uid_, media_type_, stream_type_, clock_rate_, pkt);
    
    room_->on_rtppacket_publisher2room(session_, this, pkt);
}

void rtc_publisher::on_handle_rtcp_sr(rtcp_sr_packet* sr_pkt) {
    rtp_handler_->on_handle_rtcp_sr(sr_pkt);
}

void rtc_publisher::stream_send_rtcp(uint8_t* data, size_t len) {
    session_->send_rtcp_data_in_dtls(data, len);
}

void rtc_publisher::stream_send_rtp(uint8_t* data, size_t len) {

}

void rtc_publisher::request_keyframe(uint32_t media_ssrc) {
    if (rtp_ssrc_ != media_ssrc) {
        log_errorf("the request keyframe media ssrc(%u) is error, the publisher rtp ssrc:%u",
            media_ssrc, rtp_ssrc_);
        return;
    }
    rtcp_pspli* pspli_pkt = new rtcp_pspli();

    pspli_pkt->set_sender_ssrc(1);
    pspli_pkt->set_media_ssrc(media_ssrc);
    
    session_->send_rtcp_data_in_dtls(pspli_pkt->get_data(), pspli_pkt->get_data_len());

    delete pspli_pkt;
}

void rtc_publisher::on_timer() {
    if (rtp_handler_) {
        rtp_handler_->on_timer();
    }
    if (((++key_count_) % 6 == 0) && (media_type_ == "video")) {
        request_keyframe(rtp_ssrc_);
    }
}

void rtc_publisher::rtp_packet_reset(std::shared_ptr<rtp_packet_info> pkt_ptr) {
    if (!pkt_ptr) {
        return;
    }
    if (media_type_ != "video") {
        return;
    }
    uint32_t media_ssrc = pkt_ptr->pkt->get_ssrc();

    log_warnf("jitter buffer lost and request keyframe, ssrc:%u", media_ssrc);
    request_keyframe(media_ssrc);
    return;
}

void rtc_publisher::rtp_packet_output(std::shared_ptr<rtp_packet_info> pkt_ptr) {
    log_debugf("jitterbuffer output roomid:%s uid:%s mediatype:%s, stream_type:%s ssrc:%u, seq:%d, ext_seq:%d, mark:%d, length:%lu",
        pkt_ptr->roomId_.c_str(), pkt_ptr->uid_.c_str(), pkt_ptr->media_type_.c_str(), pkt_ptr->stream_type_.c_str(),
        pkt_ptr->pkt->get_ssrc(), pkt_ptr->pkt->get_seq(), pkt_ptr->extend_seq_, pkt_ptr->pkt->get_marker(),
        pkt_ptr->pkt->get_data_length());
    
    if (pack_handle_) {
        pack_handle_->input_rtp_packet(pkt_ptr);
    }
}

void rtc_publisher::pack_handle_reset(std::shared_ptr<rtp_packet_info> pkt_ptr) {
    if (!pkt_ptr) {
        return;
    }
    uint32_t media_ssrc = pkt_ptr->pkt->get_ssrc();
    log_warnf("pack handle lost and request keyframe, ssrc:%u", media_ssrc);
    request_keyframe(media_ssrc);
    return;
}

void rtc_publisher::media_packet_output(std::shared_ptr<MEDIA_PACKET> pkt_ptr) {
    log_debugf("packet get packet dts:%ld, data len:%lu, av type:%d, codec type:%d, fmt type:%d",
            pkt_ptr->dts_, pkt_ptr->buffer_ptr_->data_len(),
            pkt_ptr->av_type_, pkt_ptr->codec_type_, pkt_ptr->fmt_type_);
    
    if (pkt_ptr->av_type_ == MEDIA_VIDEO_TYPE) {
        uint8_t nalu_len_data[4];
        if (pkt_ptr->is_key_frame_) {
            uint8_t extra_data[2048];
            int extra_len = 0;

            get_video_extradata((uint8_t*)pps_data_.data(), pps_data_.data_len(), 
                                (uint8_t*)sps_data_.data(), sps_data_.data_len(), 
                                extra_data, extra_len);

            std::shared_ptr<MEDIA_PACKET> seq_pkt_ptr = std::make_shared<MEDIA_PACKET>();
            seq_pkt_ptr->buffer_ptr_->append_data((char*)extra_data, (size_t)extra_len);
            seq_pkt_ptr->copy_properties(pkt_ptr);
            seq_pkt_ptr->is_key_frame_ = false;
            seq_pkt_ptr->is_seq_hdr_   = true;
            set_rtmp_info(seq_pkt_ptr);
            seq_pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;

            room_->on_rtmp_callback(roomId_, uid_, stream_type_, seq_pkt_ptr);
        } else if (pkt_ptr->is_seq_hdr_) {
            uint8_t* p = (uint8_t*)pkt_ptr->buffer_ptr_->data();
            uint8_t nalu_type = p[4] & 0x1f;
            if (nalu_type == (uint8_t)kAvcNaluTypeSPS) {
                sps_data_.reset();
                sps_data_.append_data(pkt_ptr->buffer_ptr_->data() + 4, pkt_ptr->buffer_ptr_->data_len() - 4);
            } else if (nalu_type == (uint8_t)kAvcNaluTypePPS) {
                pps_data_.reset();
                pps_data_.append_data(pkt_ptr->buffer_ptr_->data() + 4, pkt_ptr->buffer_ptr_->data_len() - 4);
            } else {
                log_errorf("the video nalu type:0x%02x", nalu_type);
            }
            return;
        }
        write_4bytes(nalu_len_data, (uint32_t)pkt_ptr->buffer_ptr_->data_len() - 4);
        uint8_t* p = (uint8_t*)pkt_ptr->buffer_ptr_->data();
        memcpy(p, nalu_len_data, sizeof(nalu_len_data));
    } else {
        uint8_t audio_flv_header[2];

        if (first_flv_audio_) {
            const uint8_t opus_seq_data[] = {0x4f, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64,
                                            0x01, 0x02, 0x38, 0x01, 0x80, 0xbb, 0x00, 0x00,
                                            0x00, 0x00, 0x00};
            uint16_t seq_data    = 0;
            int samplerate_index = 0;
            int channel = 2;

            first_flv_audio_ = false;

            for (samplerate_index = 0; samplerate_index < 16; samplerate_index++) {
                if (48000 == mpeg4audio_sample_rates[samplerate_index])
                    break;
            }
            seq_data |= 2 << 11;//profile aac lc
            seq_data |= samplerate_index << 7;
            seq_data |= channel << 3;
            write_2bytes(audio_flv_header, seq_data);

            std::shared_ptr<MEDIA_PACKET> seq_pkt_ptr = std::make_shared<MEDIA_PACKET>();
            //seq_pkt_ptr->buffer_ptr_->append_data((char*)audio_flv_header, sizeof(audio_flv_header));
            seq_pkt_ptr->buffer_ptr_->append_data((char*)opus_seq_data, sizeof(opus_seq_data));
            seq_pkt_ptr->copy_properties(pkt_ptr);
            seq_pkt_ptr->is_key_frame_ = false;
            seq_pkt_ptr->is_seq_hdr_   = true;
            set_rtmp_info(seq_pkt_ptr);
            seq_pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;

            log_info_data((uint8_t*)seq_pkt_ptr->buffer_ptr_->data(),
                    seq_pkt_ptr->buffer_ptr_->data_len(), "opus seq data");
            room_->on_rtmp_callback(roomId_, uid_, stream_type_, seq_pkt_ptr);
        }
        set_rtmp_info(pkt_ptr);
        pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;

        room_->on_rtmp_callback(roomId_, uid_, stream_type_, pkt_ptr);
        return;
    }

    set_rtmp_info(pkt_ptr);
    room_->on_rtmp_callback(roomId_, uid_, stream_type_, pkt_ptr);
    
    return;
}

void rtc_publisher::set_rtmp_info(std::shared_ptr<MEDIA_PACKET> pkt_ptr) {
    pkt_ptr->app_ = roomId_;
    pkt_ptr->streamname_ = uid_;
    pkt_ptr->key_ = roomId_;
    pkt_ptr->key_ += "/";
    pkt_ptr->key_ += uid_;

    pkt_ptr->dts_ = pkt_ptr->dts_ * 1000 / clock_rate_;
    pkt_ptr->pts_ = pkt_ptr->pts_ * 1000 / clock_rate_;

    flv_muxer::add_flv_media_header(pkt_ptr);
}