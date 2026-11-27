#include "ios_vcam/usb_ios_transport.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ios_vcam {

#ifdef _WIN32
namespace {

#define LIBUSB_CALL WINAPI

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

#pragma pack(push, 1)
struct libusb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
};
#pragma pack(pop)

struct libusb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
    const unsigned char* extra;
    int extra_length;
};

struct libusb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
    const libusb_endpoint_descriptor* endpoint;
    const unsigned char* extra;
    int extra_length;
};

struct libusb_interface {
    const libusb_interface_descriptor* altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t MaxPower;
    const libusb_interface* interface;
    const unsigned char* extra;
    int extra_length;
};

struct LibUsbApi {
    HMODULE dll = nullptr;
    int(LIBUSB_CALL* init)(libusb_context**) = nullptr;
    void(LIBUSB_CALL* exit)(libusb_context*) = nullptr;
    SSIZE_T(LIBUSB_CALL* get_device_list)(libusb_context*, libusb_device***) = nullptr;
    void(LIBUSB_CALL* free_device_list)(libusb_device**, int) = nullptr;
    int(LIBUSB_CALL* get_device_descriptor)(libusb_device*, libusb_device_descriptor*) = nullptr;
    int(LIBUSB_CALL* get_device_speed)(libusb_device*) = nullptr;
    int(LIBUSB_CALL* get_config_descriptor)(libusb_device*, uint8_t, libusb_config_descriptor**) = nullptr;
    void(LIBUSB_CALL* free_config_descriptor)(libusb_config_descriptor*) = nullptr;
    int(LIBUSB_CALL* open)(libusb_device*, libusb_device_handle**) = nullptr;
    void(LIBUSB_CALL* close)(libusb_device_handle*) = nullptr;
    int(LIBUSB_CALL* get_configuration)(libusb_device_handle*, int*) = nullptr;
    int(LIBUSB_CALL* set_configuration)(libusb_device_handle*, int) = nullptr;
    int(LIBUSB_CALL* claim_interface)(libusb_device_handle*, int) = nullptr;
    int(LIBUSB_CALL* release_interface)(libusb_device_handle*, int) = nullptr;
    int(LIBUSB_CALL* clear_halt)(libusb_device_handle*, unsigned char) = nullptr;
    int(LIBUSB_CALL* control_transfer)(
        libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, unsigned int) = nullptr;
    int(LIBUSB_CALL* bulk_transfer)(
        libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int) = nullptr;
    int(LIBUSB_CALL* get_string_descriptor_ascii)(
        libusb_device_handle*, uint8_t, unsigned char*, int) = nullptr;
    const char*(LIBUSB_CALL* strerror)(int) = nullptr;
};

constexpr uint16_t kAppleVendorId = 0x05AC;
constexpr uint8_t kUsbClassVendorSpec = 0xFF;
constexpr uint8_t kSubClassQuickTimeAv = 0x2A;
constexpr uint8_t kSubClassUsbMux = 0xFE;
constexpr uint8_t kEndpointTransferMask = 0x03;
constexpr uint8_t kEndpointTransferBulk = 0x02;
constexpr uint8_t kEndpointDirIn = 0x80;

bool IsIosProductId(uint16_t pid) {
    return (pid >= 0x1290 && pid <= 0x12AF);
}

UsbLinkSpeed ConvertRawLibUsbSpeed(int raw_speed, uint16_t bcd_usb) {
    if (raw_speed >= 1 && raw_speed <= 6) {
        return static_cast<UsbLinkSpeed>(raw_speed);
    }
    // Fallback heuristic from negotiated USB descriptor version if hub speed query returned 0
    if (bcd_usb >= 0x0310) return UsbLinkSpeed::SuperPlus_10Gbps;
    if (bcd_usb >= 0x0300) return UsbLinkSpeed::Super_5Gbps;
    if (bcd_usb >= 0x0200) return UsbLinkSpeed::High_480Mbps;
    if (bcd_usb >= 0x0110) return UsbLinkSpeed::Full_12Mbps;
    return UsbLinkSpeed::Unknown;
}

struct QtInterfaceMatch {
    bool found_qt = false;
    int qt_config_value = -1;
    int qt_interface_number = -1;
    uint8_t bulk_in_ep = 0;
    uint8_t bulk_out_ep = 0;
    int usbmux_config_value = 1;
};

QtInterfaceMatch InspectDeviceConfigs(LibUsbApi& api, libusb_device* dev, const libusb_device_descriptor& desc) {
    QtInterfaceMatch m;
    for (uint8_t c = 0; c < desc.bNumConfigurations; ++c) {
        libusb_config_descriptor* cfg = nullptr;
        if (api.get_config_descriptor(dev, c, &cfg) != 0 || !cfg) {
            continue;
        }
        bool has_qt_in_cfg = false;
        bool has_mux_only = false;
        for (uint8_t i = 0; i < cfg->bNumInterfaces; ++i) {
            const auto& iface = cfg->interface[i];
            for (int a = 0; a < iface.num_altsetting; ++a) {
                const auto& alt = iface.altsetting[a];
                if (alt.bInterfaceClass == kUsbClassVendorSpec && alt.bInterfaceSubClass == kSubClassUsbMux) {
                    has_mux_only = true;
                }
                if (alt.bInterfaceClass == kUsbClassVendorSpec && alt.bInterfaceSubClass == kSubClassQuickTimeAv) {
                    uint8_t ep_in = 0, ep_out = 0;
                    for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                        const auto& ep = alt.endpoint[e];
                        if ((ep.bmAttributes & kEndpointTransferMask) == kEndpointTransferBulk) {
                            if ((ep.bEndpointAddress & kEndpointDirIn) != 0) {
                                ep_in = ep.bEndpointAddress;
                            } else {
                                ep_out = ep.bEndpointAddress;
                            }
                        }
                    }
                    if (ep_in != 0 && ep_out != 0) {
                        m.found_qt = true;
                        m.qt_config_value = cfg->bConfigurationValue;
                        m.qt_interface_number = alt.bInterfaceNumber;
                        m.bulk_in_ep = ep_in;
                        m.bulk_out_ep = ep_out;
                        has_qt_in_cfg = true;
                    }
                }
            }
        }
        if (has_mux_only && !has_qt_in_cfg) {
            m.usbmux_config_value = cfg->bConfigurationValue;
        }
        api.free_config_descriptor(cfg);
    }
    return m;
}

}  // namespace
#endif

struct UsbIosTransport::Impl {
#ifdef _WIN32
    LibUsbApi api;
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    int claimed_interface = -1;
    int usbmux_config_value = 1;
    uint8_t bulk_in_ep = 0;
    uint8_t bulk_out_ep = 0;
    uint32_t read_timeout_ms = 100;
    uint32_t write_timeout_ms = 1000;
#endif
};

UsbIosTransport::UsbIosTransport() : impl_(new Impl()) {}

UsbIosTransport::~UsbIosTransport() {
    CloseAndRestoreUsbMux();
#ifdef _WIN32
    if (impl_->ctx && impl_->api.exit) {
        impl_->api.exit(impl_->ctx);
        impl_->ctx = nullptr;
    }
    if (impl_->api.dll) {
        FreeLibrary(impl_->api.dll);
        impl_->api.dll = nullptr;
    }
#endif
    delete impl_;
}

#ifdef _WIN32
namespace {

HMODULE LoadBundledLibUsbDll() {
    wchar_t exe_buf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH * 2 - 1);
    if (n == 0) return nullptr;
    std::wstring dll_path(exe_buf, n);
    size_t slash = dll_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return nullptr;
    dll_path = dll_path.substr(0, slash + 1) + L"libusb-1.0.dll";
    return LoadLibraryExW(
        dll_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

}  // namespace
#endif

UsbLinkSpeed UsbIosTransport::ProbeConnectedIosLinkSpeed(uint16_t* out_bcd_usb) {
    if (out_bcd_usb) *out_bcd_usb = 0;
#ifdef _WIN32
    HMODULE dll = LoadBundledLibUsbDll();
    if (!dll) {
        return UsbLinkSpeed::Unknown;
    }

    auto fn_init = reinterpret_cast<int(LIBUSB_CALL*)(libusb_context**)>(GetProcAddress(dll, "libusb_init"));
    auto fn_exit = reinterpret_cast<void(LIBUSB_CALL*)(libusb_context*)>(GetProcAddress(dll, "libusb_exit"));
    auto fn_list = reinterpret_cast<SSIZE_T(LIBUSB_CALL*)(libusb_context*, libusb_device***)>(
        GetProcAddress(dll, "libusb_get_device_list"));
    auto fn_free = reinterpret_cast<void(LIBUSB_CALL*)(libusb_device**, int)>(
        GetProcAddress(dll, "libusb_free_device_list"));
    auto fn_desc = reinterpret_cast<int(LIBUSB_CALL*)(libusb_device*, libusb_device_descriptor*)>(
        GetProcAddress(dll, "libusb_get_device_descriptor"));
    auto fn_speed = reinterpret_cast<int(LIBUSB_CALL*)(libusb_device*)>(
        GetProcAddress(dll, "libusb_get_device_speed"));

    UsbLinkSpeed detected = UsbLinkSpeed::Unknown;
    if (fn_init && fn_exit && fn_list && fn_free && fn_desc) {
        libusb_context* ctx = nullptr;
        if (fn_init(&ctx) == 0 && ctx) {
            libusb_device** list = nullptr;
            SSIZE_T count = fn_list(ctx, &list);
            if (count > 0 && list) {
                for (SSIZE_T i = 0; i < count; ++i) {
                    libusb_device_descriptor desc{};
                    if (fn_desc(list[i], &desc) == 0 &&
                        desc.idVendor == kAppleVendorId && IsIosProductId(desc.idProduct)) {
                        int raw_spd = fn_speed ? fn_speed(list[i]) : 0;
                        detected = ConvertRawLibUsbSpeed(raw_spd, desc.bcdUSB);
                        if (out_bcd_usb) *out_bcd_usb = desc.bcdUSB;
                        break;
                    }
                }
                fn_free(list, 1);
            }
            fn_exit(ctx);
        }
    }
    FreeLibrary(dll);
    return detected;
#else
    return UsbLinkSpeed::Unknown;
#endif
}

bool UsbIosTransport::Initialize(std::string* out_error) {
#ifdef _WIN32
    if (impl_->ctx != nullptr) {
        return true;
    }
    HMODULE dll = LoadBundledLibUsbDll();
    if (!dll) {
        if (out_error) {
            *out_error = "Unable to load libusb-1.0.dll from application directory.";
        }
        return false;
    }
    impl_->api.dll = dll;

#define LOAD_SYM(field, name)                                                        \
    impl_->api.field = reinterpret_cast<decltype(impl_->api.field)>(                 \
        GetProcAddress(dll, name));                                                  \
    if (!impl_->api.field) {                                                         \
        if (out_error) *out_error = std::string("Missing symbol in libusb: ") + name; \
        return false;                                                                \
    }

    LOAD_SYM(init, "libusb_init");
    LOAD_SYM(exit, "libusb_exit");
    LOAD_SYM(get_device_list, "libusb_get_device_list");
    LOAD_SYM(free_device_list, "libusb_free_device_list");
    LOAD_SYM(get_device_descriptor, "libusb_get_device_descriptor");
    LOAD_SYM(get_config_descriptor, "libusb_get_config_descriptor");
    LOAD_SYM(free_config_descriptor, "libusb_free_config_descriptor");
    LOAD_SYM(open, "libusb_open");
    LOAD_SYM(close, "libusb_close");
    LOAD_SYM(get_configuration, "libusb_get_configuration");
    LOAD_SYM(set_configuration, "libusb_set_configuration");
    LOAD_SYM(claim_interface, "libusb_claim_interface");
    LOAD_SYM(release_interface, "libusb_release_interface");
    LOAD_SYM(clear_halt, "libusb_clear_halt");
    LOAD_SYM(control_transfer, "libusb_control_transfer");
    LOAD_SYM(bulk_transfer, "libusb_bulk_transfer");
    LOAD_SYM(get_string_descriptor_ascii, "libusb_get_string_descriptor_ascii");
    impl_->api.get_device_speed = reinterpret_cast<decltype(impl_->api.get_device_speed)>(
        GetProcAddress(dll, "libusb_get_device_speed"));
    impl_->api.strerror = reinterpret_cast<decltype(impl_->api.strerror)>(
        GetProcAddress(dll, "libusb_strerror"));
#undef LOAD_SYM

    int rc = impl_->api.init(&impl_->ctx);
    if (rc != 0) {
        if (out_error) {
            *out_error = "libusb_init failed with error code " + std::to_string(rc);
        }
        return false;
    }
    return true;
#else
    if (out_error) {
        *out_error = "UsbIosTransport Windows backend is only active on Windows.";
    }
    return false;
#endif
}

bool UsbIosTransport::OpenAndClaimQuickTimeInterface(const UsbTransportConfig& cfg, std::string* out_error) {
#ifdef _WIN32
    CloseAndRestoreUsbMux();
    if (!Initialize(out_error)) {
        return false;
    }

    impl_->read_timeout_ms = cfg.read_timeout_ms;
    impl_->write_timeout_ms = cfg.write_timeout_ms;

    bool sent_hidden_config_request = false;

    for (int attempt = 0; attempt < 25; ++attempt) {
        libusb_device** list = nullptr;
        SSIZE_T count = impl_->api.get_device_list(impl_->ctx, &list);
        if (count < 0 || !list) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        bool found_any_ios = false;
        for (SSIZE_T idx = 0; idx < count; ++idx) {
            libusb_device* dev = list[idx];
            libusb_device_descriptor desc{};
            if (impl_->api.get_device_descriptor(dev, &desc) != 0) {
                continue;
            }
            if (desc.idVendor != kAppleVendorId || !IsIosProductId(desc.idProduct)) {
                continue;
            }
            found_any_ios = true;

            int raw_spd = impl_->api.get_device_speed ? impl_->api.get_device_speed(dev) : 0;
            UsbLinkSpeed dev_speed = ConvertRawLibUsbSpeed(raw_spd, desc.bcdUSB);

            libusb_device_handle* h = nullptr;
            if (impl_->api.open(dev, &h) != 0 || !h) {
                if (out_error && out_error->empty()) {
                    *out_error =
                        "Found iPhone/iPad (VID 05AC), but libusb_open failed. Ensure install_all_admin.ps1 "
                        "mounted the libusb0 UpperFilter driver and the phone is unlocked & trusted.";
                }
                continue;
            }

            unsigned char serial_buf[128] = {0};
            std::string serial_str;
            if (desc.iSerialNumber != 0 &&
                impl_->api.get_string_descriptor_ascii(h, desc.iSerialNumber, serial_buf, sizeof(serial_buf) - 1) > 0) {
                serial_str = reinterpret_cast<const char*>(serial_buf);
            }
            if (!cfg.preferred_udid.empty() && serial_str.find(cfg.preferred_udid) == std::string::npos) {
                impl_->api.close(h);
                continue;
            }

            unsigned char prod_buf[128] = {0};
            std::string prod_str = "Apple iOS Device";
            if (desc.iProduct != 0 &&
                impl_->api.get_string_descriptor_ascii(h, desc.iProduct, prod_buf, sizeof(prod_buf) - 1) > 0) {
                prod_str = reinterpret_cast<const char*>(prod_buf);
            }

            auto match = InspectDeviceConfigs(impl_->api, dev, desc);
            if (!match.found_qt) {
                if (!sent_hidden_config_request) {
                    unsigned char dummy[4] = {0};
                    impl_->api.control_transfer(h, 0x40, 0x52, 0x0000, 0x0002, dummy, 0, 1000);
                    sent_hidden_config_request = true;
                }
                impl_->api.close(h);
                continue;
            }

            int current_cfg = -1;
            if (impl_->api.get_configuration(h, &current_cfg) != 0 || current_cfg != match.qt_config_value) {
                int set_rc = impl_->api.set_configuration(h, match.qt_config_value);
                if (set_rc != 0) {
                    if (out_error) {
                        *out_error = "libusb_set_configuration(" + std::to_string(match.qt_config_value) +
                                     ") failed (code " + std::to_string(set_rc) +
                                     "). Check that libusb0 UpperFilter is installed on the composite parent.";
                    }
                    impl_->api.close(h);
                    continue;
                }
            }

            int claim_rc = impl_->api.claim_interface(h, match.qt_interface_number);
            if (claim_rc != 0) {
                if (out_error) {
                    *out_error = "libusb_claim_interface(" + std::to_string(match.qt_interface_number) +
                                 ") failed (code " + std::to_string(claim_rc) + ").";
                }
                impl_->api.close(h);
                continue;
            }

            impl_->api.clear_halt(h, match.bulk_in_ep);
            impl_->api.clear_halt(h, match.bulk_out_ep);

            impl_->handle = h;
            impl_->claimed_interface = match.qt_interface_number;
            impl_->usbmux_config_value = match.usbmux_config_value;
            impl_->bulk_in_ep = match.bulk_in_ep;
            impl_->bulk_out_ep = match.bulk_out_ep;
            connected_speed_ = dev_speed;

            char info[320];
            std::snprintf(
                info,
                sizeof(info),
                "%s | %s | PID=0x%04X, Serial=%s",
                prod_str.c_str(),
                UsbLinkSpeedName(dev_speed),
                desc.idProduct,
                serial_str.empty() ? "N/A" : serial_str.c_str());
            device_desc_ = info;

            impl_->api.free_device_list(list, 1);
            return true;
        }

        impl_->api.free_device_list(list, 1);

        if (!found_any_ios && !sent_hidden_config_request) {
            if (out_error) {
                *out_error = "No iOS USB device (VID 0x05AC) detected. Connect iPhone/iPad via USB and unlock.";
            }
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (out_error && out_error->empty()) {
        *out_error = "Timed out waiting for iOS QuickTime SubClass 0x2A interface after re-enumeration.";
    }
    return false;
#else
    (void)cfg;
    if (out_error) *out_error = "Unsupported OS";
    return false;
#endif
}

bool UsbIosTransport::SendBulk(std::span<const uint8_t> data) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!impl_->handle || data.empty()) {
        return false;
    }
    size_t offset = 0;
    while (offset < data.size()) {
        int chunk = static_cast<int>(data.size() - offset);
        int transferred = 0;
        int rc = impl_->api.bulk_transfer(
            impl_->handle,
            impl_->bulk_out_ep,
            const_cast<unsigned char*>(data.data() + offset),
            chunk,
            &transferred,
            impl_->write_timeout_ms);
        if (rc != 0 || transferred <= 0) {
            return false;
        }
        offset += static_cast<size_t>(transferred);
    }
    return true;
#else
    (void)data;
    return false;
#endif
}

bool UsbIosTransport::RunBulkReadLoop(
    const std::atomic<bool>& stop_flag,
    const OnUsbDataFn& on_data,
    std::string* out_error) {
#ifdef _WIN32
    if (!impl_->handle) {
        return false;
    }
    constexpr int kReadBufSize = 256 * 1024;
    std::vector<unsigned char> buf(kReadBufSize);

    while (!stop_flag.load(std::memory_order_relaxed)) {
        int transferred = 0;
        int rc = impl_->api.bulk_transfer(
            impl_->handle,
            impl_->bulk_in_ep,
            buf.data(),
            kReadBufSize,
            &transferred,
            impl_->read_timeout_ms);

        if (transferred > 0 && on_data) {
            if (!on_data(std::span<const uint8_t>(buf.data(), static_cast<size_t>(transferred)))) {
                return false;
            }
        }
        if (rc == 0 || rc == -7) {
            continue;
        }
        if (out_error) {
            const char* err_name = impl_->api.strerror ? impl_->api.strerror(rc) : "USB transfer error";
            *out_error = std::string("Bulk IN transfer ended: ") + err_name + " (" + std::to_string(rc) + ")";
        }
        return false;
    }
    return true;
#else
    (void)stop_flag;
    (void)on_data;
    (void)out_error;
    return false;
#endif
}

void UsbIosTransport::CloseAndRestoreUsbMux() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!impl_->handle) {
        return;
    }
    if (impl_->claimed_interface >= 0) {
        impl_->api.release_interface(impl_->handle, impl_->claimed_interface);
        impl_->claimed_interface = -1;
    }
    unsigned char dummy[4] = {0};
    impl_->api.control_transfer(impl_->handle, 0x40, 0x52, 0x0000, 0x0000, dummy, 0, 500);
    impl_->api.set_configuration(impl_->handle, impl_->usbmux_config_value);

    impl_->api.close(impl_->handle);
    impl_->handle = nullptr;
    device_desc_.clear();
    connected_speed_ = UsbLinkSpeed::Unknown;
#endif
}

}  // namespace ios_vcam
