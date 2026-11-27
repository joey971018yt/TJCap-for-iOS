#include "ios_vcam/image_convert.h"

#include <algorithm>
#include <cstring>

namespace ios_vcam {

LetterboxRect ComputeAspectFitRect(uint32_t src_w, uint32_t src_h, uint32_t box_w, uint32_t box_h) {
    LetterboxRect r{};
    if (src_w == 0 || src_h == 0 || box_w == 0 || box_h == 0) {
        return r;
    }
    uint64_t lhs = static_cast<uint64_t>(src_w) * box_h;
    uint64_t rhs = static_cast<uint64_t>(box_w) * src_h;
    uint32_t fit_w = box_w;
    uint32_t fit_h = box_h;
    if (lhs > rhs) {
        fit_w = box_w;
        fit_h = static_cast<uint32_t>((static_cast<uint64_t>(box_w) * src_h + src_w / 2U) / src_w);
    } else if (lhs < rhs) {
        fit_h = box_h;
        fit_w = static_cast<uint32_t>((static_cast<uint64_t>(box_h) * src_w + src_h / 2U) / src_h);
    }
    r.width = std::clamp(fit_w, 1U, box_w);
    r.height = std::clamp(fit_h, 1U, box_h);
    r.x = (box_w - r.width) / 2U;
    r.y = (box_h - r.height) / 2U;
    return r;
}

void ScaleAndLetterboxNV12(
    const uint8_t* src_y,
    uint32_t src_y_stride,
    const uint8_t* src_uv,
    uint32_t src_uv_stride,
    uint32_t src_w,
    uint32_t src_h,
    uint8_t* dst_nv12,
    uint32_t dst_w,
    uint32_t dst_h) {
    if (!src_y || !src_uv || !dst_nv12 || src_w == 0 || src_h == 0 || dst_w < 2 || dst_h < 2) {
        return;
    }

    dst_w &= ~1U;
    dst_h &= ~1U;
    uint8_t* dst_y = dst_nv12;
    uint8_t* dst_uv = dst_nv12 + static_cast<size_t>(dst_w) * dst_h;

    // Fast path: exact 1:1 dimensions
    if (src_w == dst_w && src_h == dst_h) {
        if (src_y_stride == dst_w) {
            std::memcpy(dst_y, src_y, static_cast<size_t>(dst_w) * dst_h);
        } else {
            for (uint32_t y = 0; y < dst_h; ++y) {
                std::memcpy(dst_y + static_cast<size_t>(y) * dst_w,
                            src_y + static_cast<size_t>(y) * src_y_stride,
                            dst_w);
            }
        }
        uint32_t uv_h = dst_h / 2;
        if (src_uv_stride == dst_w) {
            std::memcpy(dst_uv, src_uv, static_cast<size_t>(dst_w) * uv_h);
        } else {
            for (uint32_t y = 0; y < uv_h; ++y) {
                std::memcpy(dst_uv + static_cast<size_t>(y) * dst_w,
                            src_uv + static_cast<size_t>(y) * src_uv_stride,
                            dst_w);
            }
        }
        return;
    }

    // Fill canvas with BT.709 studio black (Y = 16, U = 128, V = 128)
    std::memset(dst_y, 16, static_cast<size_t>(dst_w) * dst_h);
    std::memset(dst_uv, 128, static_cast<size_t>(dst_w) * (dst_h / 2));

    // Compute aspect-preserving target rectangle (even-aligned for 4:2:0 chroma)
    uint64_t lhs = static_cast<uint64_t>(src_w) * dst_h;
    uint64_t rhs = static_cast<uint64_t>(dst_w) * src_h;
    uint32_t fit_w = dst_w;
    uint32_t fit_h = dst_h;
    if (lhs > rhs) {
        fit_w = dst_w;
        fit_h = static_cast<uint32_t>((static_cast<uint64_t>(dst_w) * src_h) / src_w);
    } else if (lhs < rhs) {
        fit_h = dst_h;
        fit_w = static_cast<uint32_t>((static_cast<uint64_t>(dst_h) * src_w) / src_h);
    }
    fit_w = std::max(2U, fit_w & ~1U);
    fit_h = std::max(2U, fit_h & ~1U);

    uint32_t off_x = ((dst_w - fit_w) / 2U) & ~1U;
    uint32_t off_y = ((dst_h - fit_h) / 2U) & ~1U;

    // Bilinear Luma scaling into (off_x, off_y, fit_w, fit_h)
    for (uint32_t dy = 0; dy < fit_h; ++dy) {
        uint64_t sy_fp = (static_cast<uint64_t>(dy) * (src_h > 1 ? (src_h - 1) : 0) << 8) /
                         (fit_h > 1 ? (fit_h - 1) : 1);
        uint32_t sy0 = std::min(static_cast<uint32_t>(sy_fp >> 8), src_h - 1);
        uint32_t sy1 = std::min(sy0 + 1, src_h - 1);
        uint32_t wy = static_cast<uint32_t>(sy_fp & 0xFFU);

        const uint8_t* row0 = src_y + static_cast<size_t>(sy0) * src_y_stride;
        const uint8_t* row1 = src_y + static_cast<size_t>(sy1) * src_y_stride;
        uint8_t* drow = dst_y + static_cast<size_t>(off_y + dy) * dst_w + off_x;

        uint64_t x_step_fp = (static_cast<uint64_t>(src_w > 1 ? (src_w - 1) : 0) << 8) /
                             (fit_w > 1 ? (fit_w - 1) : 1);
        uint64_t sx_fp = 0;
        for (uint32_t dx = 0; dx < fit_w; ++dx, sx_fp += x_step_fp) {
            uint32_t sx0 = std::min(static_cast<uint32_t>(sx_fp >> 8), src_w - 1);
            uint32_t sx1 = std::min(sx0 + 1, src_w - 1);
            uint32_t wx = static_cast<uint32_t>(sx_fp & 0xFFU);

            uint32_t top = row0[sx0] * (256U - wx) + row0[sx1] * wx;
            uint32_t bot = row1[sx0] * (256U - wx) + row1[sx1] * wx;
            drow[dx] = static_cast<uint8_t>((top * (256U - wy) + bot * wy + 32768U) >> 16);
        }
    }

    // Interleaved UV plane scaling
    uint32_t src_uv_w = std::max(1U, src_w / 2U);
    uint32_t src_uv_h = std::max(1U, src_h / 2U);
    uint32_t fit_uv_w = fit_w / 2U;
    uint32_t fit_uv_h = fit_h / 2U;

    for (uint32_t dy = 0; dy < fit_uv_h; ++dy) {
        uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_uv_h) / fit_uv_h);
        sy = std::min(sy, src_uv_h - 1);
        const uint8_t* srow = src_uv + static_cast<size_t>(sy) * src_uv_stride;
        uint8_t* drow = dst_uv + static_cast<size_t>((off_y / 2U) + dy) * dst_w + off_x;

        for (uint32_t dx = 0; dx < fit_uv_w; ++dx) {
            uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_uv_w) / fit_uv_w);
            sx = std::min(sx, src_uv_w - 1);
            drow[dx * 2] = srow[sx * 2];
            drow[dx * 2 + 1] = srow[sx * 2 + 1];
        }
    }
}

void ResizeNV12Fast(
    const uint8_t* src_nv12,
    uint32_t src_w,
    uint32_t src_h,
    uint8_t* dst_nv12,
    uint32_t dst_w,
    uint32_t dst_h) {
    if (src_w == dst_w && src_h == dst_h) {
        std::memcpy(dst_nv12, src_nv12, static_cast<size_t>(src_w) * src_h * 3 / 2);
        return;
    }
    const uint8_t* src_y = src_nv12;
    const uint8_t* src_uv = src_nv12 + static_cast<size_t>(src_w) * src_h;
    ScaleAndLetterboxNV12(src_y, src_w, src_uv, src_w, src_w, src_h, dst_nv12, dst_w, dst_h);
}

void ConvertNV12ToYUY2(
    const uint8_t* src_nv12,
    uint32_t width,
    uint32_t height,
    uint8_t* dst_yuy2) {
    const uint8_t* src_y = src_nv12;
    const uint8_t* src_uv = src_nv12 + static_cast<size_t>(width) * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* y_row = src_y + static_cast<size_t>(y) * width;
        const uint8_t* uv_row = src_uv + static_cast<size_t>(y / 2U) * width;
        uint8_t* out = dst_yuy2 + static_cast<size_t>(y) * width * 2U;

        for (uint32_t x = 0; x + 1 < width; x += 2) {
            out[0] = y_row[x];
            out[1] = uv_row[x];
            out[2] = y_row[x + 1];
            out[3] = uv_row[x + 1];
            out += 4;
        }
    }
}

void ConvertNV12ToRGB24BottomUp(
    const uint8_t* src_nv12,
    uint32_t width,
    uint32_t height,
    uint8_t* dst_bgr24) {
    const uint8_t* src_y = src_nv12;
    const uint8_t* src_uv = src_nv12 + static_cast<size_t>(width) * height;
    size_t dst_stride = static_cast<size_t>(width) * 3U;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* y_row = src_y + static_cast<size_t>(y) * width;
        const uint8_t* uv_row = src_uv + static_cast<size_t>(y / 2U) * width;
        // Bottom-up DIB row order for DirectShow MEDIASUBTYPE_RGB24
        uint8_t* out = dst_bgr24 + static_cast<size_t>(height - 1U - y) * dst_stride;

        for (uint32_t x = 0; x + 1 < width; x += 2) {
            int u = static_cast<int>(uv_row[x]) - 128;
            int v = static_cast<int>(uv_row[x + 1]) - 128;

            // BT.709 integer approximation
            int r_add = (459 * v) >> 8;
            int g_sub = (55 * u + 136 * v) >> 8;
            int b_add = (541 * u) >> 8;

            for (int k = 0; k < 2; ++k) {
                int c = std::max(0, static_cast<int>(y_row[x + k]) - 16);
                int y_scaled = (298 * c + 128) >> 8;
                int r = std::clamp(y_scaled + r_add, 0, 255);
                int g = std::clamp(y_scaled - g_sub, 0, 255);
                int b = std::clamp(y_scaled + b_add, 0, 255);
                out[0] = static_cast<uint8_t>(b);
                out[1] = static_cast<uint8_t>(g);
                out[2] = static_cast<uint8_t>(r);
                out += 3;
            }
        }
    }
}

void ConvertNV12ToBGRA32TopDown(
    const uint8_t* src_nv12,
    uint32_t width,
    uint32_t height,
    uint8_t* dst_bgra) {
    if (!src_nv12 || !dst_bgra || width < 2 || height < 2) {
        return;
    }
    const uint8_t* src_y = src_nv12;
    const uint8_t* src_uv = src_nv12 + static_cast<size_t>(width) * height;
    size_t dst_stride = static_cast<size_t>(width) * 4U;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* y_row = src_y + static_cast<size_t>(y) * width;
        const uint8_t* uv_row = src_uv + static_cast<size_t>(y / 2U) * width;
        uint8_t* out = dst_bgra + static_cast<size_t>(y) * dst_stride;

        for (uint32_t x = 0; x + 1 < width; x += 2) {
            int u = static_cast<int>(uv_row[x]) - 128;
            int v = static_cast<int>(uv_row[x + 1]) - 128;

            int r_add = (459 * v) >> 8;
            int g_sub = (55 * u + 136 * v) >> 8;
            int b_add = (541 * u) >> 8;

            for (int k = 0; k < 2; ++k) {
                int c = std::max(0, static_cast<int>(y_row[x + k]) - 16);
                int y_scaled = (298 * c + 128) >> 8;
                out[0] = static_cast<uint8_t>(std::clamp(y_scaled + b_add, 0, 255));
                out[1] = static_cast<uint8_t>(std::clamp(y_scaled - g_sub, 0, 255));
                out[2] = static_cast<uint8_t>(std::clamp(y_scaled + r_add, 0, 255));
                out[3] = 255U;
                out += 4;
            }
        }
        if ((width & 1U) != 0) {
            uint32_t last_x = width - 1U;
            uint32_t uv_x = last_x & ~1U;
            int u = static_cast<int>(uv_row[uv_x]) - 128;
            int v = static_cast<int>(uv_row[uv_x + 1]) - 128;
            int c = std::max(0, static_cast<int>(y_row[last_x]) - 16);
            int y_scaled = (298 * c + 128) >> 8;
            out[0] = static_cast<uint8_t>(std::clamp(y_scaled + ((541 * u) >> 8), 0, 255));
            out[1] = static_cast<uint8_t>(std::clamp(y_scaled - ((55 * u + 136 * v) >> 8), 0, 255));
            out[2] = static_cast<uint8_t>(std::clamp(y_scaled + ((459 * v) >> 8), 0, 255));
            out[3] = 255U;
        }
    }
}

void SampleNV12ToBGRA32Bilinear(
    const uint8_t* src_nv12,
    uint32_t src_w,
    uint32_t src_h,
    uint8_t* dst_bgra,
    uint32_t dst_w,
    uint32_t dst_h) {
    if (!src_nv12 || !dst_bgra || src_w < 2 || src_h < 2 || dst_w == 0 || dst_h == 0) {
        return;
    }
    if (src_w == dst_w && src_h == dst_h) {
        ConvertNV12ToBGRA32TopDown(src_nv12, src_w, src_h, dst_bgra);
        return;
    }

    const uint8_t* src_y = src_nv12;
    const uint8_t* src_uv = src_nv12 + static_cast<size_t>(src_w) * src_h;
    const uint32_t uv_max_x = (src_w - 2U) & ~1U;
    const uint32_t uv_max_y = (src_h / 2U) > 0 ? ((src_h / 2U) - 1U) : 0U;

    const uint64_t x_step_fp =
        (dst_w > 1) ? ((static_cast<uint64_t>(src_w - 1U) << 8) / (dst_w - 1U)) : 0ULL;
    const uint64_t y_step_fp =
        (dst_h > 1) ? ((static_cast<uint64_t>(src_h - 1U) << 8) / (dst_h - 1U)) : 0ULL;

    uint64_t sy_fp = 0;
    for (uint32_t dy = 0; dy < dst_h; ++dy, sy_fp += y_step_fp) {
        uint32_t sy0 = std::min(static_cast<uint32_t>(sy_fp >> 8), src_h - 1U);
        uint32_t sy1 = std::min(sy0 + 1U, src_h - 1U);
        uint32_t wy = static_cast<uint32_t>(sy_fp & 0xFFU);

        const uint8_t* y_row0 = src_y + static_cast<size_t>(sy0) * src_w;
        const uint8_t* y_row1 = src_y + static_cast<size_t>(sy1) * src_w;
        const uint8_t* uv_row = src_uv + static_cast<size_t>(std::min(sy0 >> 1, uv_max_y)) * src_w;
        uint8_t* out = dst_bgra + static_cast<size_t>(dy) * dst_w * 4U;

        uint64_t sx_fp = 0;
        for (uint32_t dx = 0; dx < dst_w; ++dx, sx_fp += x_step_fp) {
            uint32_t sx0 = std::min(static_cast<uint32_t>(sx_fp >> 8), src_w - 1U);
            uint32_t sx1 = std::min(sx0 + 1U, src_w - 1U);
            uint32_t wx = static_cast<uint32_t>(sx_fp & 0xFFU);

            uint32_t top = y_row0[sx0] * (256U - wx) + y_row0[sx1] * wx;
            uint32_t bot = y_row1[sx0] * (256U - wx) + y_row1[sx1] * wx;
            int y_val = static_cast<int>((top * (256U - wy) + bot * wy + 32768U) >> 16);

            uint32_t uv_x = std::min(sx0 & ~1U, uv_max_x);
            int u = static_cast<int>(uv_row[uv_x]) - 128;
            int v = static_cast<int>(uv_row[uv_x + 1U]) - 128;

            int c = std::max(0, y_val - 16);
            int y_scaled = (298 * c + 128) >> 8;
            out[0] = static_cast<uint8_t>(std::clamp(y_scaled + ((541 * u) >> 8), 0, 255));
            out[1] = static_cast<uint8_t>(std::clamp(y_scaled - ((55 * u + 136 * v) >> 8), 0, 255));
            out[2] = static_cast<uint8_t>(std::clamp(y_scaled + ((459 * v) >> 8), 0, 255));
            out[3] = 255U;
            out += 4;
        }
    }
}

void GenerateAnimatedColorBarsNV12(
    uint8_t* dst_nv12,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index) {
    // 8 standard BT.709 75% HD color bars (White, Yellow, Cyan, Green, Magenta, Red, Blue, Black)
    static constexpr uint8_t kBarsYUV[8][3] = {
        {180, 128, 128},  // White
        {168, 44, 136},   // Yellow
        {145, 147, 44},   // Cyan
        {133, 63, 52},    // Green
        {63, 193, 204},   // Magenta
        {51, 109, 212},   // Red
        {28, 212, 120},   // Blue
        {16, 128, 128},   // Black
    };

    uint8_t* dst_y = dst_nv12;
    uint8_t* dst_uv = dst_nv12 + static_cast<size_t>(width) * height;

    // Prebuild one luma scanline & one UV scanline then replicate for speed
    for (uint32_t x = 0; x < width; ++x) {
        uint32_t bar = std::min(7U, (x * 8U) / width);
        dst_y[x] = kBarsYUV[bar][0];
        if ((x & 1U) == 0 && x + 1 < width) {
            dst_uv[x] = kBarsYUV[bar][1];
            dst_uv[x + 1] = kBarsYUV[bar][2];
        }
    }

    for (uint32_t y = 1; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * width, dst_y, width);
    }
    uint32_t uv_h = height / 2U;
    for (uint32_t y = 1; y < uv_h; ++y) {
        std::memcpy(dst_uv + static_cast<size_t>(y) * width, dst_uv, width);
    }

    // Draw smooth horizontal animated progress pulse bar in bottom 12% region
    uint32_t band_top = (height * 82U / 100U) & ~1U;
    uint32_t band_bot = (height * 90U / 100U) & ~1U;
    uint32_t marker_w = std::max(32U, (width / 12U) & ~1U);
    uint32_t marker_x = static_cast<uint32_t>((frame_index * 12ULL) % (width - marker_w)) & ~1U;

    for (uint32_t y = band_top; y < band_bot; ++y) {
        std::memset(dst_y + static_cast<size_t>(y) * width, 28, width);
        std::memset(dst_y + static_cast<size_t>(y) * width + marker_x, 235, marker_w);
    }
    for (uint32_t y = band_top / 2U; y < band_bot / 2U; ++y) {
        std::memset(dst_uv + static_cast<size_t>(y) * width, 128, width);
        uint8_t* m_uv = dst_uv + static_cast<size_t>(y) * width + marker_x;
        for (uint32_t x = 0; x + 1 < marker_w; x += 2) {
            m_uv[x] = 90;       // Warm cyan/gold pulse
            m_uv[x + 1] = 160;
        }
    }
}

}  // namespace ios_vcam
