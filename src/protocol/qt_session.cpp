#include "ios_vcam/qt_session.h"

namespace ios_vcam {

namespace {
constexpr size_t kMaxPacketSize = 16 * 1024 * 1024;  // 16 MB safety bound for 1080p60 IDR frames
}  // namespace

QtStreamSession::QtStreamSession(
    QtSessionConfig config,
    SendUsbPacketFn send_fn,
    OnVideoPacketFn on_video,
    OnAudioPacketFn on_audio)
    : config_(config),
      send_fn_(std::move(send_fn)),
      on_video_(std::move(on_video)),
      on_audio_(std::move(on_audio)),
      clock_origin_(std::chrono::steady_clock::now()) {
    rx_buffer_.reserve(256 * 1024);
}

void QtStreamSession::Reset() {
    rx_buffer_.clear();
    clock_origin_ = std::chrono::steady_clock::now();
    next_local_clock_id_ = 0x00007FA66CE20CB0ULL;
    device_audio_clock_ref_ = 0;
    local_audio_clock_ref_ = 0;
    device_video_clock_ref_ = 0;
    local_video_clock_ref_ = 0;
    ping_completed_ = false;
    cwpa_completed_ = false;
    cvrp_completed_ = false;
    force_next_sps_pps_ = true;
    current_avc_config_ = {};
    current_width_ = 1920;
    current_height_ = 1080;
    audio_format_ = {};
    video_packets_count_ = 0;
    audio_packets_count_ = 0;
}

bool QtStreamSession::FeedUsbBytes(std::span<const uint8_t> chunk) {
    if (chunk.empty()) {
        return true;
    }
    rx_buffer_.insert(rx_buffer_.end(), chunk.begin(), chunk.end());

    size_t consumed = 0;
    while (rx_buffer_.size() - consumed >= 8) {
        std::span<const uint8_t> view(rx_buffer_.data() + consumed, rx_buffer_.size() - consumed);
        uint32_t pkt_len = CoreMediaCodec::ReadU32LE(view, 0);
        uint32_t pkt_magic = CoreMediaCodec::ReadU32LE(view, 4);

        // Validate packet header synchronization
        bool known_magic = (pkt_magic == magic::kPing || pkt_magic == magic::kSync ||
                            pkt_magic == magic::kAsyn || pkt_magic == magic::kRply);
        if (!known_magic || pkt_len < 16 || pkt_len > kMaxPacketSize) {
            // Resynchronize stream by scanning byte-by-byte for a valid top-level packet header
            ++consumed;
            continue;
        }

        if (view.size() < pkt_len) {
            break;  // Wait for remaining USB bulk fragments
        }

        if (!HandleCompletePacket(view.subspan(0, pkt_len))) {
            return false;
        }
        consumed += pkt_len;
    }

    if (consumed > 0) {
        rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return true;
}

bool QtStreamSession::HandleCompletePacket(std::span<const uint8_t> pkt) {
    uint32_t top_magic = CoreMediaCodec::ReadU32LE(pkt, 4);
    if (top_magic == magic::kPing) {
        ping_completed_ = true;
        return SendPacket(CoreMediaCodec::BuildPingPacket());
    }
    if (top_magic == magic::kSync) {
        return HandleSyncPacket(pkt);
    }
    if (top_magic == magic::kAsyn) {
        return HandleAsynPacket(pkt);
    }
    return true;
}

bool QtStreamSession::HandleSyncPacket(std::span<const uint8_t> pkt) {
    // SYNC header: [0..3] len, [4..7] 'sync', [8..15] clock_ref, [16..19] subtype, [20..27] correlation_id
    if (pkt.size() < 28) {
        return true;
    }
    uint32_t subtype = CoreMediaCodec::ReadU32LE(pkt, 16);
    uint64_t correlation_id = CoreMediaCodec::ReadU64LE(pkt, 20);

    switch (subtype) {
        case magic::kCwpa: {
            if (pkt.size() < 36) {
                return true;
            }
            device_audio_clock_ref_ = CoreMediaCodec::ReadU64LE(pkt, 28);
            local_audio_clock_ref_ = next_local_clock_id_++;
            cwpa_completed_ = true;

            if (!SendPacket(CoreMediaCodec::BuildRplyClockRef(correlation_id, local_audio_clock_ref_))) {
                return false;
            }

            // Immediately advertise HPD1 (Video Display Capabilities) and HPA1 (Audio Capabilities)
            auto hpd1_dict = CoreMediaCodec::BuildHpd1DeviceInfoDict(
                config_.valeria_clean_status_bar, config_.max_width, config_.max_height);
            if (!SendPacket(CoreMediaCodec::BuildAsynDictPacket(magic::kHpd1, magic::kEmptyCFType, hpd1_dict))) {
                return false;
            }

            auto hpa1_dict = CoreMediaCodec::BuildHpa1DeviceInfoDict();
            return SendPacket(CoreMediaCodec::BuildAsynDictPacket(magic::kHpa1, device_audio_clock_ref_, hpa1_dict));
        }

        case magic::kAfmt: {
            if (pkt.size() >= 68) {
                auto asbd = CoreMediaCodec::ParseAudioStreamBasicDescription(pkt.subspan(28));
                if (asbd.has_value()) {
                    audio_format_ = *asbd;
                }
            }
            return SendPacket(CoreMediaCodec::BuildRplyAfmtOk(correlation_id));
        }

        case magic::kCvrp: {
            if (pkt.size() < 36) {
                return true;
            }
            device_video_clock_ref_ = CoreMediaCodec::ReadU64LE(pkt, 28);
            local_video_clock_ref_ = next_local_clock_id_++;

            // Parse optional CVRP dictionary containing initial FormatDescription (SPS/PPS)
            if (pkt.size() > 36) {
                auto dict = CoreMediaCodec::ParseStringDict(pkt.subspan(36));
                if (dict.has_value()) {
                    for (const auto& entry : dict->entries) {
                        if (std::holds_alternative<FormatDescription>(entry.value)) {
                            const auto& fd = std::get<FormatDescription>(entry.value);
                            if (!fd.avc_config.sps_list.empty()) {
                                current_avc_config_ = fd.avc_config;
                                force_next_sps_pps_ = true;
                            }
                            if (fd.width > 0 && fd.height > 0) {
                                current_width_ = fd.width;
                                current_height_ = fd.height;
                            }
                        }
                    }
                }
            }

            cvrp_completed_ = true;
            if (!SendPacket(CoreMediaCodec::BuildRplyClockRef(correlation_id, local_video_clock_ref_))) {
                return false;
            }
            return SendPacket(CoreMediaCodec::BuildAsynNeedPacket(device_video_clock_ref_));
        }

        case magic::kClok: {
            uint64_t new_clock = next_local_clock_id_++;
            return SendPacket(CoreMediaCodec::BuildRplyClockRef(correlation_id, new_clock));
        }

        case magic::kTime: {
            return SendPacket(CoreMediaCodec::BuildRplyCMTime(correlation_id, CurrentMonotonicCMTime()));
        }

        case magic::kSkew: {
            double nominal_rate = audio_format_.sample_rate > 0.0 ? audio_format_.sample_rate : 48000.0;
            return SendPacket(CoreMediaCodec::BuildRplySkew(correlation_id, nominal_rate));
        }

        case magic::kOg:
        case magic::kStop: {
            return SendPacket(CoreMediaCodec::BuildRplyZero64(correlation_id));
        }

        default: {
            // Gracefully acknowledge any future SYNC subtype with 8 zero bytes so iOS never stalls
            return SendPacket(CoreMediaCodec::BuildRplyZero64(correlation_id));
        }
    }
}

bool QtStreamSession::HandleAsynPacket(std::span<const uint8_t> pkt) {
    // ASYN header: [0..3] len, [4..7] 'asyn', [8..15] clock_ref, [16..19] subtype
    if (pkt.size() < 20) {
        return true;
    }
    uint32_t subtype = CoreMediaCodec::ReadU32LE(pkt, 16);

    if (subtype == magic::kFeed) {
        if (pkt.size() <= 20) {
            return true;
        }
        auto sbuf = CoreMediaCodec::ParseSampleBuffer(pkt.subspan(20), magic::kVide);
        if (sbuf.has_value()) {
            if (sbuf->has_format_description) {
                const auto& fd = sbuf->format_description;
                if (!fd.avc_config.sps_list.empty()) {
                    current_avc_config_ = fd.avc_config;
                    force_next_sps_pps_ = true;
                }
                if (fd.width > 0 && fd.height > 0) {
                    current_width_ = fd.width;
                    current_height_ = fd.height;
                }
            }

            if (!sbuf->sample_data.empty() && on_video_) {
                bool is_idr = false;
                uint8_t nal_len_size = current_avc_config_.nal_length_size > 0
                                           ? current_avc_config_.nal_length_size
                                           : 4;
                // First probe if this sample is an IDR keyframe so we know whether to prepend SPS/PPS
                auto annexb = CoreMediaCodec::ConvertAvccToAnnexB(
                    sbuf->sample_data, nal_len_size, nullptr, &is_idr);
                if ((is_idr || force_next_sps_pps_) && !current_avc_config_.sps_list.empty()) {
                    annexb = CoreMediaCodec::ConvertAvccToAnnexB(
                        sbuf->sample_data, nal_len_size, &current_avc_config_, &is_idr);
                    force_next_sps_pps_ = false;
                }

                if (!annexb.empty()) {
                    EncodedVideoPacket vpkt;
                    vpkt.annexb_data = std::move(annexb);
                    vpkt.width = current_width_;
                    vpkt.height = current_height_;
                    vpkt.pts_ns = static_cast<uint64_t>(sbuf->pts.Seconds() * 1e9);
                    vpkt.is_keyframe = is_idr;
                    ++video_packets_count_;
                    on_video_(vpkt);
                }
            }
        }

        // Request next frame immediately to maintain zero-stall 60fps flow
        if (device_video_clock_ref_ != 0) {
            return SendPacket(CoreMediaCodec::BuildAsynNeedPacket(device_video_clock_ref_));
        }
        return true;
    }

    if (subtype == magic::kEat) {
        if (pkt.size() <= 20) {
            return true;
        }
        auto sbuf = CoreMediaCodec::ParseSampleBuffer(pkt.subspan(20), magic::kSoun);
        if (sbuf.has_value() && !sbuf->sample_data.empty() && on_audio_) {
            DecodedPcmPacket apkt;
            apkt.pcm_s16le = std::move(sbuf->sample_data);
            apkt.sample_rate = audio_format_.sample_rate > 0.0
                                   ? static_cast<uint32_t>(audio_format_.sample_rate)
                                   : 48000U;
            apkt.channels = audio_format_.channels_per_frame > 0 ? audio_format_.channels_per_frame : 2U;
            apkt.pts_ns = static_cast<uint64_t>(sbuf->pts.Seconds() * 1e9);
            ++audio_packets_count_;
            on_audio_(apkt);
        }
        return true;
    }

    // SPRP, SRAT, TBAS, TJMP, RELS require no response
    return true;
}

bool QtStreamSession::SendPacket(const std::vector<uint8_t>& pkt) {
    if (!send_fn_) {
        return true;
    }
    return send_fn_(pkt);
}

std::vector<std::vector<uint8_t>> QtStreamSession::BuildStopPackets() const {
    std::vector<std::vector<uint8_t>> pkts;
    if (device_audio_clock_ref_ != 0) {
        pkts.push_back(CoreMediaCodec::BuildAsynHpa0Packet(device_audio_clock_ref_));
    }
    pkts.push_back(CoreMediaCodec::BuildAsynHpd0Packet());
    return pkts;
}

CMTime QtStreamSession::CurrentMonotonicCMTime() const {
    auto elapsed = std::chrono::steady_clock::now() - clock_origin_;
    uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    CMTime t;
    t.value = ns;
    t.timescale = 1000000000U;
    t.flags = 1U;
    t.epoch = 0;
    return t;
}

}  // namespace ios_vcam
