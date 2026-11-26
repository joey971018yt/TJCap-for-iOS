#pragma once

#include <cstddef>
#include <cstdint>

namespace ios_vcam {

// Scales an NV12 frame (src_y, src_uv) into a fixed NV12 canvas (dst_nv12, dst_w x dst_h)
// while preserving aspect ratio and filling letterbox/pillarbox bars with BT.709 black (Y=16, UV=128).
void ScaleAndLetterboxNV12(
    const uint8_t* src_y,
    uint32_t src_y_stride,
    const uint8_t* src_uv,
    uint32_t src_uv_stride,
    uint32_t src_w,
    uint32_t src_h,
    uint8_t* dst_nv12,
    uint32_t dst_w,
    uint32_t dst_h);

// Downscales or copies a contiguous 1920x1080 NV12 frame to dst_w x dst_h NV12
void ResizeNV12Fast(
    const uint8_t* src_nv12,
    uint32_t src_w,
    uint32_t src_h,
    uint8_t* dst_nv12,
    uint32_t dst_w,
    uint32_t dst_h);

// Converts contiguous NV12 (w x h) to packed YUY2 (w * h * 2 bytes)
void ConvertNV12ToYUY2(
    const uint8_t* src_nv12,
    uint32_t width,
    uint32_t height,
    uint8_t* dst_yuy2);

// Converts contiguous NV12 (w x h) to Windows DirectShow bottom-up BGR24 (w * h * 3 bytes)
void ConvertNV12ToRGB24BottomUp(
    const uint8_t* src_nv12,
    uint32_t width,
    uint32_t height,
    uint8_t* dst_bgr24);

// Generates animated BT.709 HD color bars in contiguous NV12 format (for standby & --mock self-test)
void GenerateAnimatedColorBarsNV12(
    uint8_t* dst_nv12,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index);

}  // namespace ios_vcam
