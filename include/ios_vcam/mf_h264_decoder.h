#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace ios_vcam {

struct DecodedNV12View {
    const uint8_t* y_plane = nullptr;
    uint32_t y_stride = 0;
    const uint8_t* uv_plane = nullptr;
    uint32_t uv_stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t pts_ns = 0;
};

class MfH264Decoder {
public:
    using OnDecodedFrameFn = std::function<void(const DecodedNV12View&)>;

    MfH264Decoder();
    ~MfH264Decoder();

    MfH264Decoder(const MfH264Decoder&) = delete;
    MfH264Decoder& operator=(const MfH264Decoder&) = delete;

    bool Initialize(bool prefer_d3d11_hw = true, std::string* out_error = nullptr);
    void Shutdown();

    bool DecodeAnnexB(
        std::span<const uint8_t> annexb_nalus,
        uint64_t pts_ns,
        uint32_t hint_width,
        uint32_t hint_height,
        const OnDecodedFrameFn& on_frame);

    bool IsHardwareAccelerated() const { return hw_accelerated_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool hw_accelerated_ = false;
};

}  // namespace ios_vcam
