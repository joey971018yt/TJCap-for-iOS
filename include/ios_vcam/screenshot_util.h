#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace ios_vcam {

// Resolves "%USERPROFILE%\Pictures\JCast Screenshots" (creates directory if needed), falling back to app_dir\Screenshots
std::filesystem::path GetDefaultScreenshotDirectory(const std::filesystem::path& fallback_app_dir);

// Builds a collision-free millisecond-timestamped PNG filename, e.g. "JCast_20261126_232015_128_2868x1320.png"
std::filesystem::path BuildTimestampedScreenshotPath(
    const std::filesystem::path& dir,
    uint32_t width,
    uint32_t height);

// Encodes top-down 32-bit BGRA pixels as a lossless PNG file via Windows Imaging Component (WIC)
bool SaveTopDownBGRA32AsPng(
    const uint8_t* bgra_topdown,
    uint32_t width,
    uint32_t height,
    const std::filesystem::path& file_path,
    std::string* out_error = nullptr);

// Copies top-down 32-bit BGRA pixels onto the Windows Clipboard (CF_DIB) for instant Ctrl+V pasting
bool CopyTopDownBGRA32ToClipboard(
    void* owner_hwnd,
    const uint8_t* bgra_topdown,
    uint32_t width,
    uint32_t height);

}  // namespace ios_vcam
