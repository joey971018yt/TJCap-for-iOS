#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>

namespace ios_vcam {

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

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::mutex write_mutex_;
    std::string device_desc_;
};

}  // namespace ios_vcam
