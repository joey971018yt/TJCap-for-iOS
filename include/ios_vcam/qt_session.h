#pragma once

#include "ios_vcam/coremedia_codec.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ios_vcam {

struct EncodedVideoPacket {
    std::vector<uint8_t> annexb_data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t pts_ns = 0;
    bool is_keyframe = false;
};

struct DecodedPcmPacket {
    std::vector<uint8_t> pcm_s16le;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint64_t pts_ns = 0;
};

struct QtSessionConfig {
    // true = QuickTime Valeria mode (clean 9:41 AM & 100% battery status bar)
    // false = Clean native mode (--mode clean, preserves real iPhone clock & battery bar)
    bool valeria_clean_status_bar = true;
    uint32_t max_width = 1920;
    uint32_t max_height = 1200;
};

class QtStreamSession {
public:
    using SendUsbPacketFn = std::function<bool(std::span<const uint8_t>)>;
    using OnVideoPacketFn = std::function<void(const EncodedVideoPacket&)>;
    using OnAudioPacketFn = std::function<void(const DecodedPcmPacket&)>;

    explicit QtStreamSession(
        QtSessionConfig config = {},
        SendUsbPacketFn send_fn = {},
        OnVideoPacketFn on_video = {},
        OnAudioPacketFn on_audio = {});

    void Reset();

    // Feed arbitrary raw bytes received from the USB Bulk IN endpoint (supports fragmented/coalesced reads)
    bool FeedUsbBytes(std::span<const uint8_t> chunk);

    // Build graceful stop packets (HPA0 + HPD0) before releasing USB interface
    std::vector<std::vector<uint8_t>> BuildStopPackets() const;

    bool IsStreaming() const { return cvrp_completed_; }
    uint64_t VideoFramesReceived() const { return video_packets_count_; }
    uint64_t AudioPacketsReceived() const { return audio_packets_count_; }
    AudioStreamBasicDescription NegotiatedAudioFormat() const { return audio_format_; }

private:
    bool HandleCompletePacket(std::span<const uint8_t> pkt);
    bool HandleSyncPacket(std::span<const uint8_t> pkt);
    bool HandleAsynPacket(std::span<const uint8_t> pkt);
    bool SendPacket(const std::vector<uint8_t>& pkt);
    CMTime CurrentMonotonicCMTime() const;

    QtSessionConfig config_;
    SendUsbPacketFn send_fn_;
    OnVideoPacketFn on_video_;
    OnAudioPacketFn on_audio_;

    std::vector<uint8_t> rx_buffer_;
    std::chrono::steady_clock::time_point clock_origin_;

    uint64_t next_local_clock_id_ = 0x00007FA66CE20CB0ULL;
    uint64_t device_audio_clock_ref_ = 0;
    uint64_t local_audio_clock_ref_ = 0;
    uint64_t device_video_clock_ref_ = 0;
    uint64_t local_video_clock_ref_ = 0;

    bool ping_completed_ = false;
    bool cwpa_completed_ = false;
    bool cvrp_completed_ = false;
    bool force_next_sps_pps_ = true;

    AvcDecoderConfig current_avc_config_;
    uint32_t current_width_ = 1920;
    uint32_t current_height_ = 1080;
    AudioStreamBasicDescription audio_format_;

    uint64_t video_packets_count_ = 0;
    uint64_t audio_packets_count_ = 0;
};

}  // namespace ios_vcam
