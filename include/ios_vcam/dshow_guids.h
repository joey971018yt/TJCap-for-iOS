#pragma once

#ifdef _WIN32
#include <guiddef.h>
#endif

namespace ios_vcam {

// {8E74A2B1-5C19-4D83-9A41-2F6C8D90E101}
// iOS USB Capture Card (Video) CLSID
#ifdef _WIN32
static const GUID CLSID_IosUsbVideoCapture = {
    0x8e74a2b1, 0x5c19, 0x4d83, {0x9a, 0x41, 0x2f, 0x6c, 0x8d, 0x90, 0xe1, 0x01}};

// {8E74A2B2-5C19-4D83-9A41-2F6C8D90E102}
// iOS USB Capture Card (Audio) CLSID
static const GUID CLSID_IosUsbAudioCapture = {
    0x8e74a2b2, 0x5c19, 0x4d83, {0x9a, 0x41, 0x2f, 0x6c, 0x8d, 0x90, 0xe1, 0x02}};
#endif

inline constexpr const wchar_t* kVideoDeviceFriendlyName = L"iOS USB Capture Card (Video)";
inline constexpr const wchar_t* kAudioDeviceFriendlyName = L"iOS USB Capture Card (Audio)";

}  // namespace ios_vcam
