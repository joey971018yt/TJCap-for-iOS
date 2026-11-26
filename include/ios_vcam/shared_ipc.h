#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ios_vcam {

inline constexpr uint32_t kShmMagic = 0x4956434DU;  // 'IVCM'
inline constexpr uint32_t kShmVersion = 1U;

inline constexpr uint32_t kCanvasWidth = 1920U;
inline constexpr uint32_t kCanvasHeight = 1080U;
inline constexpr size_t kNv12FrameBytes = static_cast<size_t>(kCanvasWidth) * kCanvasHeight * 3U / 2U;
inline constexpr size_t kVideoSlotCount = 3U;

// 48000 Hz, 16-bit signed stereo (4 bytes per stereo sample frame)
inline constexpr uint32_t kAudioSampleRate = 48000U;
inline constexpr uint32_t kAudioChannels = 2U;
inline constexpr uint32_t kAudioBytesPerFrame = 4U;
inline constexpr uint64_t kAudioRingFrames = 65536ULL;  // Power of two (~1.365s buffer)
inline constexpr uint64_t kAudioRingMask = kAudioRingFrames - 1ULL;

inline constexpr const char* kDefaultShmName = "IosUsbVcamSharedMemV1";

struct alignas(64) VideoSlotHeader {
    uint64_t frame_seq = 0;
    uint64_t pts_ns = 0;
    uint32_t source_width = 1920;
    uint32_t source_height = 1080;
};

struct alignas(64) SharedCaptureLayout {
    uint32_t magic = 0;
    uint32_t version = 0;

    std::atomic<uint64_t> producer_heartbeat_ms{0};
    std::atomic<uint32_t> producer_streaming{0};

    // Triple-buffered lock-free video state
    std::atomic<uint32_t> latest_video_slot{0};
    std::atomic<uint64_t> total_video_frames{0};
    VideoSlotHeader slot_headers[kVideoSlotCount];
    alignas(64) uint8_t video_slots[kVideoSlotCount][kNv12FrameBytes];

    // Lock-free SPMC (Single-Producer Multi-Consumer) 48kHz stereo LPCM ring buffer
    std::atomic<uint64_t> audio_write_frame_pos{0};
    alignas(64) int16_t audio_ring_samples[kAudioRingFrames * kAudioChannels];
};

uint64_t MonotonicMillisNow();

class SharedCaptureProducer {
public:
    SharedCaptureProducer() = default;
    ~SharedCaptureProducer();

    SharedCaptureProducer(const SharedCaptureProducer&) = delete;
    SharedCaptureProducer& operator=(const SharedCaptureProducer&) = delete;

    bool Open(const std::string& shm_name = kDefaultShmName);
    void Close();
    bool IsOpen() const { return layout_ != nullptr; }

    // Direct write access to next backbuffer slot to avoid an extra 3MB copy
    uint8_t* AcquireBackbufferSlot(uint32_t& out_slot_index);
    void PublishBackbufferSlot(
        uint32_t slot_index,
        uint64_t pts_ns,
        uint32_t src_width,
        uint32_t src_height);

    // Convenience helper when NV12 is already in a contiguous buffer
    void PublishNV12Frame(
        const uint8_t* nv12_1080p,
        uint64_t pts_ns,
        uint32_t src_width = kCanvasWidth,
        uint32_t src_height = kCanvasHeight);

    // Writes 48kHz 16-bit stereo LPCM frames into the SPMC ring buffer
    void PushAudioPcmS16LE(const int16_t* stereo_samples, size_t stereo_frame_count);

    void TouchHeartbeat(bool active_streaming);

private:
    SharedCaptureLayout* layout_ = nullptr;
    uint32_t write_slot_cursor_ = 1;
    std::string shm_name_;
#ifdef _WIN32
    void* mapping_handle_ = nullptr;
#else
    int shm_fd_ = -1;
    bool owns_unlink_ = false;
#endif
};

class SharedCaptureConsumer {
public:
    SharedCaptureConsumer() = default;
    ~SharedCaptureConsumer();

    SharedCaptureConsumer(const SharedCaptureConsumer&) = delete;
    SharedCaptureConsumer& operator=(const SharedCaptureConsumer&) = delete;

    bool Open(const std::string& shm_name = kDefaultShmName);
    void Close();
    bool IsOpen() const { return layout_ != nullptr; }

    // True if producer heartbeat updated within max_age_ms and streaming flag is active
    bool IsProducerAlive(uint64_t max_age_ms = 1500ULL) const;

    // Copies latest 1920x1080 NV12 frame into out_nv12 (returns false if no active producer)
    bool ReadLatestNV12Frame(
        uint8_t* out_nv12,
        uint64_t& out_frame_seq,
        uint64_t& out_pts_ns,
        uint32_t* out_src_w = nullptr,
        uint32_t* out_src_h = nullptr);

    // Independently reads up to requested_frames of 48kHz stereo LPCM without affecting other consumers.
    // Missing frames (when producer idle or underrun) are zero-filled for click-free silence.
    size_t ReadAudioPcmS16LE(int16_t* out_stereo_samples, size_t requested_stereo_frames);

private:
    SharedCaptureLayout* layout_ = nullptr;
    uint64_t audio_read_cursor_ = 0;
    bool audio_cursor_initialized_ = false;
#ifdef _WIN32
    void* mapping_handle_ = nullptr;
#else
    int shm_fd_ = -1;
#endif
};

}  // namespace ios_vcam
