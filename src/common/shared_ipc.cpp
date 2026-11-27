#include "ios_vcam/shared_ipc.h"
#include "ios_vcam/image_convert.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ios_vcam {

uint64_t MonotonicMillisNow() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

StreamHealthGrade EvaluateStreamHealthGrade(const StreamHealthSnapshot& s) {
    if (!s.active) {
        return StreamHealthGrade::Idle;
    }
    if (s.waiting_first_frame) {
        return StreamHealthGrade::WaitingStream;
    }
    if (s.pipeline_latency_ms > 14.0 || s.decode_latency_ms > 12.0) {
        return StreamHealthGrade::Warning;
    }
    if (!s.mock_source && (s.last_frame_age_ms > 450ULL || (s.fps < 18.0 && s.bitrate_mbps < 3.5))) {
        return StreamHealthGrade::StaticVfr;
    }
    if (s.fps < 22.0 && s.bitrate_mbps >= 3.5) {
        return StreamHealthGrade::Warning;
    }
    return StreamHealthGrade::Excellent;
}

SharedCaptureProducer::~SharedCaptureProducer() {
    Close();
}

bool SharedCaptureProducer::Open(const std::string& shm_name) {
    Close();
    shm_name_ = shm_name;

#ifdef _WIN32
    std::wstring wname = L"Local\\";
    for (char c : shm_name) {
        wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    DWORD size_low = static_cast<DWORD>(sizeof(SharedCaptureLayout) & 0xFFFFFFFFULL);
    DWORD size_high = static_cast<DWORD>((static_cast<uint64_t>(sizeof(SharedCaptureLayout)) >> 32) & 0xFFFFFFFFULL);

    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, size_high, size_low, wname.c_str());
    if (!hMap) {
        return false;
    }
    bool already_existed = (GetLastError() == ERROR_ALREADY_EXISTS);
    void* view = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedCaptureLayout));
    if (!view) {
        CloseHandle(hMap);
        return false;
    }
    mapping_handle_ = hMap;
    layout_ = static_cast<SharedCaptureLayout*>(view);
    if (!already_existed || layout_->magic != kShmMagic || layout_->version != kShmVersion) {
        std::memset(view, 0, sizeof(SharedCaptureLayout));
        layout_ = new (view) SharedCaptureLayout();
        layout_->magic = kShmMagic;
        layout_->version = kShmVersion;
    }
#else
    std::string posix_name = "/" + shm_name;
    int fd = shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(sizeof(SharedCaptureLayout))) != 0) {
        close(fd);
        return false;
    }
    void* view = mmap(nullptr, sizeof(SharedCaptureLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        close(fd);
        return false;
    }
    shm_fd_ = fd;
    owns_unlink_ = true;
    layout_ = static_cast<SharedCaptureLayout*>(view);
    if (layout_->magic != kShmMagic || layout_->version != kShmVersion) {
        std::memset(view, 0, sizeof(SharedCaptureLayout));
        layout_ = new (view) SharedCaptureLayout();
        layout_->magic = kShmMagic;
        layout_->version = kShmVersion;
    }
#endif

    layout_->total_video_bytes.store(0, std::memory_order_release);
    layout_->total_audio_packets.store(0, std::memory_order_release);
    layout_->live_bitrate_kbps.store(0, std::memory_order_release);
    layout_->live_fps_x100.store(0, std::memory_order_release);
    TouchHeartbeat(true);
    return true;
}

void SharedCaptureProducer::Close() {
    if (layout_) {
        layout_->producer_streaming.store(0, std::memory_order_release);
        layout_->producer_heartbeat_ms.store(0, std::memory_order_release);
#ifdef _WIN32
        UnmapViewOfFile(layout_);
#else
        munmap(layout_, sizeof(SharedCaptureLayout));
#endif
        layout_ = nullptr;
    }
#ifdef _WIN32
    if (mapping_handle_) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
#else
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
    if (owns_unlink_ && !shm_name_.empty()) {
        std::string posix_name = "/" + shm_name_;
        shm_unlink(posix_name.c_str());
        owns_unlink_ = false;
    }
#endif
}

void SharedCaptureProducer::SetAdvertisedDimensions(uint32_t width, uint32_t height) {
    if (!layout_) return;
    uint32_t aw = 0, ah = 0;
    AlignAdaptiveDimensions(width, height, aw, ah);
    layout_->active_width.store(aw, std::memory_order_release);
    layout_->active_height.store(ah, std::memory_order_release);
}

void SharedCaptureProducer::AddEncodedVideoBytes(uint64_t byte_count) {
    if (!layout_ || byte_count == 0) return;
    layout_->total_video_bytes.fetch_add(byte_count, std::memory_order_acq_rel);
}

void SharedCaptureProducer::SetAudioPacketCount(uint64_t packet_count) {
    if (!layout_) return;
    layout_->total_audio_packets.store(packet_count, std::memory_order_release);
}

uint8_t* SharedCaptureProducer::AcquireBackbufferSlot(uint32_t& out_slot_index) {
    if (!layout_) {
        out_slot_index = 0;
        return nullptr;
    }
    uint32_t latest = layout_->latest_video_slot.load(std::memory_order_relaxed);
    if (write_slot_cursor_ == latest) {
        write_slot_cursor_ = (write_slot_cursor_ + 1U) % static_cast<uint32_t>(kVideoSlotCount);
    }
    out_slot_index = write_slot_cursor_;
    return layout_->video_slots[out_slot_index];
}

void SharedCaptureProducer::PublishBackbufferSlot(
    uint32_t slot_index,
    uint32_t frame_width,
    uint32_t frame_height,
    uint64_t pts_ns,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t bitrate_kbps,
    const FrameTelemetryMeta& meta) {
    if (!layout_ || slot_index >= kVideoSlotCount) {
        return;
    }
    uint32_t fw = 0, fh = 0;
    AlignAdaptiveDimensions(frame_width, frame_height, fw, fh);

    uint64_t now_ms = MonotonicMillisNow();
    uint64_t seq = layout_->total_video_frames.load(std::memory_order_relaxed) + 1ULL;
    VideoSlotHeader& hdr = layout_->slot_headers[slot_index];
    hdr.frame_seq = seq;
    hdr.pts_ns = pts_ns;
    hdr.frame_width = fw;
    hdr.frame_height = fh;
    hdr.source_width = src_width > 0 ? src_width : fw;
    hdr.source_height = src_height > 0 ? src_height : fh;
    hdr.bitrate_kbps = bitrate_kbps;
    hdr.fps_x100 = meta.fps_x100;
    hdr.decode_latency_us = meta.decode_latency_us;
    hdr.pipeline_latency_us = meta.pipeline_latency_us;
    hdr.stream_flags = meta.stream_flags;
    hdr.dropped_frames = meta.dropped_frames;
    hdr.publish_mono_ms = now_ms;

    layout_->active_width.store(fw, std::memory_order_release);
    layout_->active_height.store(fh, std::memory_order_release);
    if (bitrate_kbps > 0) {
        layout_->live_bitrate_kbps.store(bitrate_kbps, std::memory_order_release);
    }
    if (meta.fps_x100 > 0) {
        layout_->live_fps_x100.store(meta.fps_x100, std::memory_order_release);
    }
    layout_->latest_video_slot.store(slot_index, std::memory_order_release);
    layout_->total_video_frames.store(seq, std::memory_order_release);
    layout_->producer_heartbeat_ms.store(now_ms, std::memory_order_release);
    layout_->producer_streaming.store(1U, std::memory_order_release);

    write_slot_cursor_ = (slot_index + 1U) % static_cast<uint32_t>(kVideoSlotCount);
}

void SharedCaptureProducer::PublishNV12Frame(
    const uint8_t* nv12_data,
    uint32_t frame_width,
    uint32_t frame_height,
    uint64_t pts_ns,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t bitrate_kbps,
    const FrameTelemetryMeta& meta) {
    if (!layout_ || !nv12_data) {
        return;
    }
    uint32_t fw = 0, fh = 0;
    AlignAdaptiveDimensions(frame_width, frame_height, fw, fh);
    size_t frame_bytes = static_cast<size_t>(fw) * fh * 3ULL / 2ULL;
    if (frame_bytes > kMaxNv12FrameBytes) {
        return;
    }
    uint32_t slot = 0;
    uint8_t* dst = AcquireBackbufferSlot(slot);
    if (!dst) {
        return;
    }
    std::memcpy(dst, nv12_data, frame_bytes);
    PublishBackbufferSlot(slot, fw, fh, pts_ns, src_width, src_height, bitrate_kbps, meta);
}

void SharedCaptureProducer::PushAudioPcmS16LE(const int16_t* stereo_samples, size_t stereo_frame_count) {
    if (!layout_ || !stereo_samples || stereo_frame_count == 0) {
        return;
    }
    if (stereo_frame_count > kAudioRingFrames / 2ULL) {
        stereo_samples += (stereo_frame_count - kAudioRingFrames / 2ULL) * kAudioChannels;
        stereo_frame_count = kAudioRingFrames / 2ULL;
    }

    uint64_t wpos = layout_->audio_write_frame_pos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < stereo_frame_count; ++i) {
        size_t idx = static_cast<size_t>((wpos + i) & kAudioRingMask) * kAudioChannels;
        layout_->audio_ring_samples[idx] = stereo_samples[i * 2];
        layout_->audio_ring_samples[idx + 1] = stereo_samples[i * 2 + 1];
    }
    layout_->audio_write_frame_pos.store(wpos + stereo_frame_count, std::memory_order_release);
    TouchHeartbeat(true);
}

void SharedCaptureProducer::TouchHeartbeat(bool active_streaming) {
    if (!layout_) {
        return;
    }
    layout_->producer_heartbeat_ms.store(MonotonicMillisNow(), std::memory_order_release);
    layout_->producer_streaming.store(active_streaming ? 1U : 0U, std::memory_order_release);
}

SharedCaptureConsumer::~SharedCaptureConsumer() {
    Close();
}

bool SharedCaptureConsumer::Open(const std::string& shm_name) {
    if (layout_) {
        return true;
    }
#ifdef _WIN32
    std::wstring wname = L"Local\\";
    for (char c : shm_name) {
        wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
    if (!hMap) {
        return false;
    }
    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedCaptureLayout));
    if (!view) {
        CloseHandle(hMap);
        return false;
    }
    auto* candidate = static_cast<SharedCaptureLayout*>(view);
    if (candidate->magic != kShmMagic || candidate->version != kShmVersion) {
        UnmapViewOfFile(view);
        CloseHandle(hMap);
        return false;
    }
    mapping_handle_ = hMap;
    layout_ = candidate;
#else
    std::string posix_name = "/" + shm_name;
    int fd = shm_open(posix_name.c_str(), O_RDONLY, 0666);
    if (fd < 0) {
        return false;
    }
    void* view = mmap(nullptr, sizeof(SharedCaptureLayout), PROT_READ, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        close(fd);
        return false;
    }
    auto* candidate = static_cast<SharedCaptureLayout*>(view);
    if (candidate->magic != kShmMagic || candidate->version != kShmVersion) {
        munmap(view, sizeof(SharedCaptureLayout));
        close(fd);
        return false;
    }
    shm_fd_ = fd;
    layout_ = candidate;
#endif
    audio_cursor_initialized_ = false;
    return true;
}

void SharedCaptureConsumer::Close() {
    if (layout_) {
#ifdef _WIN32
        UnmapViewOfFile(layout_);
#else
        munmap(layout_, sizeof(SharedCaptureLayout));
#endif
        layout_ = nullptr;
    }
#ifdef _WIN32
    if (mapping_handle_) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
#else
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
#endif
    audio_cursor_initialized_ = false;
    telem_prev_mono_ms_ = 0;
    telem_prev_frames_ = 0;
    telem_prev_bytes_ = 0;
    telem_ema_fps_ = 0.0;
    telem_ema_mbps_ = 0.0;
    telem_peak_mbps_ = 0.0;
}

bool SharedCaptureConsumer::IsProducerAlive(uint64_t max_age_ms) const {
    if (!layout_) {
        return false;
    }
    if (layout_->producer_streaming.load(std::memory_order_acquire) == 0U) {
        return false;
    }
    uint64_t hb = layout_->producer_heartbeat_ms.load(std::memory_order_acquire);
    uint64_t now = MonotonicMillisNow();
    return (now >= hb) ? ((now - hb) <= max_age_ms) : true;
}

bool SharedCaptureConsumer::QueryAdvertisedDimensions(uint32_t& out_width, uint32_t& out_height) const {
    if (!layout_) {
        return false;
    }
    uint64_t hb = layout_->producer_heartbeat_ms.load(std::memory_order_acquire);
    uint64_t now = MonotonicMillisNow();
    if (now > hb && (now - hb) > 2500ULL) {
        return false;
    }
    uint32_t w = layout_->active_width.load(std::memory_order_acquire);
    uint32_t h = layout_->active_height.load(std::memory_order_acquire);
    if (w < 64 || h < 64 || static_cast<uint64_t>(w) * h > kMaxVideoPixels) {
        return false;
    }
    AlignAdaptiveDimensions(w, h, out_width, out_height);
    return true;
}

bool SharedCaptureConsumer::ReadLatestNV12Frame(
    uint8_t* out_nv12,
    size_t out_capacity,
    uint32_t& out_frame_w,
    uint32_t& out_frame_h,
    uint64_t& out_frame_seq,
    uint64_t& out_pts_ns,
    uint32_t* out_src_w,
    uint32_t* out_src_h) {
    if (!layout_ || !out_nv12 || !IsProducerAlive()) {
        return false;
    }
    uint64_t total = layout_->total_video_frames.load(std::memory_order_acquire);
    if (total == 0) {
        return false;
    }
    uint32_t slot = layout_->latest_video_slot.load(std::memory_order_acquire);
    if (slot >= kVideoSlotCount) {
        return false;
    }

    const VideoSlotHeader hdr = layout_->slot_headers[slot];
    uint32_t fw = 0, fh = 0;
    AlignAdaptiveDimensions(hdr.frame_width, hdr.frame_height, fw, fh);
    size_t needed_bytes = static_cast<size_t>(fw) * fh * 3ULL / 2ULL;
    if (needed_bytes > out_capacity || needed_bytes > kMaxNv12FrameBytes) {
        return false;
    }

    std::memcpy(out_nv12, layout_->video_slots[slot], needed_bytes);

    out_frame_w = fw;
    out_frame_h = fh;
    out_frame_seq = hdr.frame_seq;
    out_pts_ns = hdr.pts_ns;
    if (out_src_w) {
        *out_src_w = hdr.source_width;
    }
    if (out_src_h) {
        *out_src_h = hdr.source_height;
    }
    return true;
}

bool SharedCaptureConsumer::ReadLatestPreviewBGRA32(
    uint32_t box_w,
    uint32_t box_h,
    bool force_refresh,
    uint64_t& io_last_seq,
    std::vector<uint8_t>& out_bgra_topdown,
    uint32_t& out_fit_w,
    uint32_t& out_fit_h,
    uint32_t& out_src_w,
    uint32_t& out_src_h,
    bool* out_pixels_updated,
    uint32_t* out_bitrate_kbps) {
    if (out_pixels_updated) *out_pixels_updated = false;
    if (!layout_ || box_w < 2 || box_h < 2 || !IsProducerAlive()) {
        return false;
    }
    uint64_t total = layout_->total_video_frames.load(std::memory_order_acquire);
    if (total == 0) {
        return false;
    }
    uint32_t slot = layout_->latest_video_slot.load(std::memory_order_acquire);
    if (slot >= kVideoSlotCount) {
        return false;
    }

    const VideoSlotHeader hdr = layout_->slot_headers[slot];
    uint32_t fw = 0, fh = 0;
    AlignAdaptiveDimensions(hdr.frame_width, hdr.frame_height, fw, fh);
    size_t nv12_bytes = static_cast<size_t>(fw) * fh * 3ULL / 2ULL;
    if (nv12_bytes > kMaxNv12FrameBytes) {
        return false;
    }

    LetterboxRect fit = ComputeAspectFitRect(fw, fh, box_w, box_h);
    out_fit_w = fit.width;
    out_fit_h = fit.height;
    out_src_w = fw;
    out_src_h = fh;
    if (out_bitrate_kbps) {
        *out_bitrate_kbps = hdr.bitrate_kbps;
    }

    const size_t target_bgra_bytes = static_cast<size_t>(fit.width) * fit.height * 4ULL;
    if (!force_refresh && hdr.frame_seq == io_last_seq && out_bgra_topdown.size() == target_bgra_bytes) {
        return true;
    }

    out_bgra_topdown.resize(target_bgra_bytes);
    SampleNV12ToBGRA32Bilinear(
        layout_->video_slots[slot],
        fw,
        fh,
        out_bgra_topdown.data(),
        fit.width,
        fit.height);

    io_last_seq = hdr.frame_seq;
    if (out_pixels_updated) *out_pixels_updated = true;
    return true;
}

bool SharedCaptureConsumer::CaptureLatestFullResBGRA32(
    std::vector<uint8_t>& out_bgra_topdown,
    uint32_t& out_width,
    uint32_t& out_height) {
    if (!layout_ || !IsProducerAlive()) {
        return false;
    }
    uint64_t total = layout_->total_video_frames.load(std::memory_order_acquire);
    if (total == 0) {
        return false;
    }
    uint32_t slot = layout_->latest_video_slot.load(std::memory_order_acquire);
    if (slot >= kVideoSlotCount) {
        return false;
    }

    const VideoSlotHeader hdr = layout_->slot_headers[slot];
    uint32_t fw = 0, fh = 0;
    AlignAdaptiveDimensions(hdr.frame_width, hdr.frame_height, fw, fh);
    size_t nv12_bytes = static_cast<size_t>(fw) * fh * 3ULL / 2ULL;
    if (nv12_bytes > kMaxNv12FrameBytes) {
        return false;
    }

    out_bgra_topdown.resize(static_cast<size_t>(fw) * fh * 4ULL);
    ConvertNV12ToBGRA32TopDown(layout_->video_slots[slot], fw, fh, out_bgra_topdown.data());
    out_width = fw;
    out_height = fh;
    return true;
}

bool SharedCaptureConsumer::ReadStreamHealth(StreamHealthSnapshot& out_snapshot) {
    auto reset_telem = [&]() {
        telem_prev_mono_ms_ = 0;
        telem_prev_frames_ = 0;
        telem_prev_bytes_ = 0;
        telem_ema_fps_ = 0.0;
        telem_ema_mbps_ = 0.0;
        telem_peak_mbps_ = 0.0;
        out_snapshot = StreamHealthSnapshot{};
    };

    if (!layout_) {
        reset_telem();
        return false;
    }

    uint64_t hb = layout_->producer_heartbeat_ms.load(std::memory_order_acquire);
    uint64_t now_ms = MonotonicMillisNow();
    bool hb_fresh = (hb > 0) && ((now_ms >= hb) ? ((now_ms - hb) <= 2200ULL) : true);
    if (!hb_fresh) {
        reset_telem();
        return false;
    }

    uint32_t streaming = layout_->producer_streaming.load(std::memory_order_acquire);
    uint64_t cur_frames = layout_->total_video_frames.load(std::memory_order_acquire);
    if (streaming == 0U || cur_frames == 0ULL) {
        telem_prev_mono_ms_ = 0;
        out_snapshot = StreamHealthSnapshot{};
        out_snapshot.active = true;
        out_snapshot.waiting_first_frame = true;
        out_snapshot.grade = StreamHealthGrade::WaitingStream;
        return true;
    }

    uint32_t slot = layout_->latest_video_slot.load(std::memory_order_acquire);
    if (slot >= kVideoSlotCount) {
        reset_telem();
        return false;
    }

    const VideoSlotHeader hdr = layout_->slot_headers[slot];
    uint64_t cur_bytes = layout_->total_video_bytes.load(std::memory_order_acquire);
    uint64_t cur_aud = layout_->total_audio_packets.load(std::memory_order_acquire);
    uint64_t age_ms =
        (hdr.publish_mono_ms > 0 && now_ms >= hdr.publish_mono_ms) ? (now_ms - hdr.publish_mono_ms) : 0ULL;

    double host_fps = static_cast<double>(hdr.fps_x100) / 100.0;
    double host_mbps = static_cast<double>(hdr.bitrate_kbps) / 1000.0;

    if (telem_prev_mono_ms_ == 0 || cur_frames < telem_prev_frames_ || cur_bytes < telem_prev_bytes_) {
        telem_prev_mono_ms_ = now_ms;
        telem_prev_frames_ = cur_frames;
        telem_prev_bytes_ = cur_bytes;
        telem_ema_fps_ = (age_ms > 800ULL) ? 0.0 : host_fps;
        telem_ema_mbps_ = (age_ms > 800ULL) ? 0.0 : host_mbps;
    } else if (now_ms > telem_prev_mono_ms_) {
        double dt_sec = static_cast<double>(now_ms - telem_prev_mono_ms_) / 1000.0;
        if (dt_sec >= 0.20) {
            uint64_t df = cur_frames - telem_prev_frames_;
            uint64_t db = cur_bytes - telem_prev_bytes_;
            double inst_fps = static_cast<double>(df) / dt_sec;
            double inst_mbps = (static_cast<double>(db) * 8.0) / (dt_sec * 1e6);

            if (df == 0 && age_ms > 500ULL) {
                telem_ema_fps_ = (telem_ema_fps_ < 0.4) ? 0.0 : (telem_ema_fps_ * 0.35);
                telem_ema_mbps_ = (telem_ema_mbps_ < 0.08) ? 0.0 : (telem_ema_mbps_ * 0.35);
            } else {
                telem_ema_fps_ = (telem_ema_fps_ <= 0.1) ? inst_fps : (telem_ema_fps_ * 0.50 + inst_fps * 0.50);
                telem_ema_mbps_ = (telem_ema_mbps_ <= 0.05) ? inst_mbps : (telem_ema_mbps_ * 0.50 + inst_mbps * 0.50);
            }
            telem_prev_mono_ms_ = now_ms;
            telem_prev_frames_ = cur_frames;
            telem_prev_bytes_ = cur_bytes;
        } else {
            if (host_fps > 0.0 && telem_ema_fps_ <= 0.01 && age_ms <= 500ULL) {
                telem_ema_fps_ = host_fps;
            }
            if (host_mbps > 0.0 && telem_ema_mbps_ <= 0.01 && age_ms <= 500ULL) {
                telem_ema_mbps_ = host_mbps;
            }
        }
    }

    telem_peak_mbps_ = std::max({telem_peak_mbps_, telem_ema_mbps_, host_mbps});

    out_snapshot = StreamHealthSnapshot{};
    out_snapshot.active = true;
    out_snapshot.waiting_first_frame = false;
    out_snapshot.fps = telem_ema_fps_;
    out_snapshot.bitrate_mbps = telem_ema_mbps_;
    out_snapshot.peak_bitrate_mbps = telem_peak_mbps_;
    out_snapshot.decode_latency_ms = static_cast<double>(hdr.decode_latency_us) / 1000.0;
    out_snapshot.pipeline_latency_ms = static_cast<double>(hdr.pipeline_latency_us) / 1000.0;
    out_snapshot.frame_width = hdr.frame_width;
    out_snapshot.frame_height = hdr.frame_height;
    out_snapshot.source_width = hdr.source_width;
    out_snapshot.source_height = hdr.source_height;
    out_snapshot.total_frames = cur_frames;
    out_snapshot.total_video_bytes = cur_bytes;
    out_snapshot.total_audio_packets = cur_aud;
    out_snapshot.dropped_frames = hdr.dropped_frames;
    out_snapshot.last_frame_age_ms = age_ms;
    out_snapshot.hw_accelerated = (hdr.stream_flags & kStreamFlagHwAccelerated) != 0;
    out_snapshot.mock_source = (hdr.stream_flags & kStreamFlagMockSource) != 0;
    out_snapshot.adaptive_res = (hdr.stream_flags & kStreamFlagAdaptiveRes) != 0;
    out_snapshot.usb_speed_code = static_cast<uint8_t>((hdr.stream_flags >> 8) & 0xFU);
    out_snapshot.grade = EvaluateStreamHealthGrade(out_snapshot);
    return true;
}

size_t SharedCaptureConsumer::ReadAudioPcmS16LE(int16_t* out_stereo_samples, size_t requested_stereo_frames) {
    if (!out_stereo_samples || requested_stereo_frames == 0) {
        return 0;
    }
    if (!layout_ || !IsProducerAlive()) {
        std::memset(out_stereo_samples, 0, requested_stereo_frames * kAudioBytesPerFrame);
        audio_cursor_initialized_ = false;
        return 0;
    }

    uint64_t wpos = layout_->audio_write_frame_pos.load(std::memory_order_acquire);
    if (!audio_cursor_initialized_) {
        audio_read_cursor_ = (wpos > 480ULL) ? (wpos - 480ULL) : wpos;
        audio_cursor_initialized_ = true;
    }

    if (wpos > audio_read_cursor_ + 7200ULL || audio_read_cursor_ > wpos) {
        audio_read_cursor_ = (wpos > 480ULL) ? (wpos - 480ULL) : wpos;
    }

    uint64_t avail = wpos - audio_read_cursor_;
    size_t to_copy = static_cast<size_t>(std::min<uint64_t>(avail, requested_stereo_frames));

    for (size_t i = 0; i < to_copy; ++i) {
        size_t idx = static_cast<size_t>((audio_read_cursor_ + i) & kAudioRingMask) * kAudioChannels;
        out_stereo_samples[i * 2] = layout_->audio_ring_samples[idx];
        out_stereo_samples[i * 2 + 1] = layout_->audio_ring_samples[idx + 1];
    }
    audio_read_cursor_ += to_copy;

    if (to_copy < requested_stereo_frames) {
        std::memset(
            out_stereo_samples + to_copy * kAudioChannels,
            0,
            (requested_stereo_frames - to_copy) * kAudioBytesPerFrame);
    }
    return to_copy;
}

}  // namespace ios_vcam
