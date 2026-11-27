#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>

namespace ios_vcam {

// Matches libusb_speed enum values for USB link speed detection
enum class UsbLinkSpeed : int {
    Unknown = 0,
    Low_1_5Mbps = 1,       // USB 1.0 Low-Speed (1.5 Mbps)
    Full_12Mbps = 2,       // USB 1.1 Full-Speed (12 Mbps)
    High_480Mbps = 3,      // USB 2.0 High-Speed (480 Mbps)
    Super_5Gbps = 4,       // USB 3.0 / 3.2 Gen 1 SuperSpeed (5 Gbps)
    SuperPlus_10Gbps = 5,  // USB 3.1 / 3.2 Gen 2 SuperSpeed+ (10 Gbps)
    SuperPlus_20Gbps = 6,  // USB 3.2 Gen 2x2 SuperSpeed+ (20 Gbps)
};

// Returns human-readable USB link speed description
inline const char* UsbLinkSpeedName(UsbLinkSpeed speed) {
    switch (speed) {
        case UsbLinkSpeed::Low_1_5Mbps:
            return "USB 1.0 Low-Speed (1.5 Mbps)";
        case UsbLinkSpeed::Full_12Mbps:
            return "USB 1.1 Full-Speed (12 Mbps)";
        case UsbLinkSpeed::High_480Mbps:
            return "USB 2.0 High-Speed (480 Mbps)";
        case UsbLinkSpeed::Super_5Gbps:
            return "USB 3.0 SuperSpeed (5 Gbps)";
        case UsbLinkSpeed::SuperPlus_10Gbps:
            return "USB 3.2 Gen 2 SuperSpeed+ (10 Gbps)";
        case UsbLinkSpeed::SuperPlus_20Gbps:
            return "USB 3.2 Gen 2x2 SuperSpeed+ (20 Gbps)";
        default:
            return "Unknown Speed";
    }
}

// Returns the maximum selectable resolution long-edge cap allowed for a given USB link speed tier
inline constexpr uint32_t MaxSelectableDimForUsbSpeed(UsbLinkSpeed speed) {
    switch (speed) {
        case UsbLinkSpeed::Super_5Gbps:
        case UsbLinkSpeed::SuperPlus_10Gbps:
        case UsbLinkSpeed::SuperPlus_20Gbps:
            return 3840U;  // USB 3.x (5-20 Gbps): unlock full 2.8K / 4K unrestricted native tier
        case UsbLinkSpeed::High_480Mbps:
            return 2560U;  // USB 2.0 (480 Mbps): cap selectable max at 2K (2560p)
        case UsbLinkSpeed::Low_1_5Mbps:
        case UsbLinkSpeed::Full_12Mbps:
            return 1280U;  // USB 1.1 (12 Mbps): cap selectable max at 720p (1280p)
        default:
            return 2560U;  // Unplugged / unknown: conservative 2K cap until USB 3.x is detected
    }
}

struct UsbTransportConfig {
    std::string preferred_udid;
    uint32_t read_timeout_ms = 100;
    uint32_t write_timeout_ms = 1000;
};

class UsbIosTransport {
public:
    using OnUsbDataFn = std::function<bool(std::span<const uint8_t>)>;

    UsbIosTransport();
    ~UsbIosTransport();

    UsbIosTransport(const UsbIosTransport&) = delete;
    UsbIosTransport& operator=(const UsbIosTransport&) = delete;

    // Non-intrusive static probe: queries USB Hub link speed of any attached iPhone/iPad
    // via libusb_get_device_speed without opening or switching the device configuration!
    static UsbLinkSpeed ProbeConnectedIosLinkSpeed(uint16_t* out_bcd_usb = nullptr);

    // Dynamically loads libusb-1.0.dll
    bool Initialize(std::string* out_error = nullptr);

    // Discovers iPhone/iPad (VID 0x05AC), enables hidden QuickTime config (0x40, 0x52, 0x00, 0x02)
    // if SubClass 0x2A is not yet exposed, and claims the Bulk AV endpoints.
    bool OpenAndClaimQuickTimeInterface(const UsbTransportConfig& cfg, std::string* out_error = nullptr);

    // Thread-safe Bulk OUT write
    bool SendBulk(std::span<const uint8_t> data);

    // Blocking Bulk IN loop until stop_flag becomes true or device disconnects
    bool RunBulkReadLoop(const std::atomic<bool>& stop_flag, const OnUsbDataFn& on_data, std::string* out_error = nullptr);

    // Restores standard USBMux configuration and closes libusb handles
    void CloseAndRestoreUsbMux();

    std::string ConnectedDeviceDescription() const { return device_desc_; }
    UsbLinkSpeed ConnectedLinkSpeed() const { return connected_speed_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::mutex write_mutex_;
    std::string device_desc_;
    UsbLinkSpeed connected_speed_ = UsbLinkSpeed::Unknown;
};

}  // namespace ios_vcam
