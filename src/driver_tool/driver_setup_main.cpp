#include "ios_vcam/dshow_guids.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>
#endif

namespace {

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ToLowerWide(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elev{};
    DWORD ret_len = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret_len) && elev.TokenIsElevated != 0;
    CloseHandle(token);
    return ok;
}

std::filesystem::path ExeDirectory() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return std::filesystem::path(buf).parent_path();
}

std::vector<std::wstring> ParseMultiSz(const BYTE* raw, DWORD byte_len) {
    std::vector<std::wstring> list;
    if (!raw || byte_len < sizeof(wchar_t)) {
        return list;
    }
    const auto* wptr = reinterpret_cast<const wchar_t*>(raw);
    size_t max_chars = byte_len / sizeof(wchar_t);
    size_t start = 0;
    while (start < max_chars && wptr[start] != L'\0') {
        size_t end = start;
        while (end < max_chars && wptr[end] != L'\0') {
            ++end;
        }
        list.emplace_back(wptr + start, end - start);
        start = end + 1;
    }
    return list;
}

std::vector<BYTE> SerializeMultiSz(const std::vector<std::wstring>& items) {
    std::vector<wchar_t> wbuf;
    for (const auto& s : items) {
        if (!s.empty()) {
            wbuf.insert(wbuf.end(), s.begin(), s.end());
            wbuf.push_back(L'\0');
        }
    }
    wbuf.push_back(L'\0');
    if (wbuf.size() == 1) {
        wbuf.push_back(L'\0');
    }
    const auto* bptr = reinterpret_cast<const BYTE*>(wbuf.data());
    return std::vector<BYTE>(bptr, bptr + wbuf.size() * sizeof(wchar_t));
}

bool RestartUsbDeviceNode(HDEVINFO dev_info, SP_DEVINFO_DATA& dev_data) {
    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = DICS_PROPCHANGE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;

    if (!SetupDiSetClassInstallParamsW(
            dev_info, &dev_data, &params.ClassInstallHeader, sizeof(params))) {
        return false;
    }
    return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, dev_info, &dev_data) != FALSE;
}

bool InstallLibUsb0KernelService(const std::filesystem::path& driver_dir) {
    std::filesystem::path src_sys = driver_dir / L"libusb0.sys";
    std::filesystem::path src_dll = driver_dir / L"libusb0.dll";

    wchar_t sys_root[MAX_PATH] = {0};
    GetSystemDirectoryW(sys_root, MAX_PATH);
    std::filesystem::path sys32_dir(sys_root);
    std::filesystem::path dst_sys = sys32_dir / L"drivers" / L"libusb0.sys";
    std::filesystem::path dst_dll = sys32_dir / L"libusb0.dll";

    std::error_code ec;
    if (std::filesystem::exists(src_sys, ec)) {
        std::filesystem::copy_file(src_sys, dst_sys, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[Driver] Warning: Could not copy libusb0.sys to System32\\drivers: "
                      << ec.message() << "\n";
        } else {
            std::cout << "[Driver] Staged signed libusb0.sys -> " << WideToUtf8(dst_sys.wstring()) << "\n";
        }
    }
    if (std::filesystem::exists(src_dll, ec)) {
        std::filesystem::copy_file(src_dll, dst_dll, std::filesystem::copy_options::overwrite_existing, ec);
    }

    if (!std::filesystem::exists(dst_sys, ec)) {
        std::cerr << "[Driver] Notice: " << WideToUtf8(dst_sys.wstring())
                  << " not found yet. Place signed amd64\\libusb0.sys into drivers\\ before attaching UpperFilter.\n";
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        std::cerr << "[Driver] OpenSCManagerW failed (error " << GetLastError() << ").\n";
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, L"libusb0", SERVICE_ALL_ACCESS);
    if (!svc) {
        svc = CreateServiceW(
            scm,
            L"libusb0",
            L"libusb-win32 - Kernel Driver",
            SERVICE_ALL_ACCESS,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            L"System32\\drivers\\libusb0.sys",
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (!svc) {
            std::cerr << "[Driver] CreateServiceW(libusb0) failed (error " << GetLastError() << ").\n";
            CloseServiceHandle(scm);
            return false;
        }
        std::cout << "[Driver] Created Windows kernel driver service 'libusb0'.\n";
    } else {
        std::cout << "[Driver] Verified Windows kernel driver service 'libusb0' is registered.\n";
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

int ConfigureAppleUpperFilters(bool attach_filter, bool status_only) {
    HDEVINFO dev_info = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
        std::cerr << "[SetupAPI] SetupDiGetClassDevsW failed.\n";
        return 0;
    }

    int matched_devices = 0;
    SP_DEVINFO_DATA dev_data{};
    dev_data.cbSize = sizeof(dev_data);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(dev_info, idx, &dev_data); ++idx) {
        BYTE hwid_buf[2048] = {0};
        DWORD reg_type = 0;
        DWORD req_size = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_HARDWAREID, &reg_type, hwid_buf, sizeof(hwid_buf), &req_size)) {
            continue;
        }

        auto hwids = ParseMultiSz(hwid_buf, req_size);
        bool is_apple_parent = false;
        std::wstring primary_hwid;
        for (const auto& id : hwids) {
            std::wstring low = ToLowerWide(id);
            // Match root Apple iOS device (VID_05AC & PID_1290..12AF) and exclude child &MI_xx interfaces
            if (low.find(L"vid_05ac&pid_12") != std::wstring::npos &&
                low.find(L"&mi_") == std::wstring::npos) {
                is_apple_parent = true;
                primary_hwid = id;
                break;
            }
        }
        if (!is_apple_parent) {
            continue;
        }
        ++matched_devices;

        wchar_t svc_buf[256] = {0};
        SetupDiGetDeviceRegistryPropertyW(
            dev_info,
            &dev_data,
            SPDRP_SERVICE,
            nullptr,
            reinterpret_cast<BYTE*>(svc_buf),
            sizeof(svc_buf) - sizeof(wchar_t),
            nullptr);
        std::wstring service_name(svc_buf);
        std::wstring low_svc = ToLowerWide(service_name);

        std::cout << "[Device] Found Apple iOS Parent USB Device: " << WideToUtf8(primary_hwid)
                  << " (Active Service: " << WideToUtf8(service_name) << ")\n";

        // Detect accidental Zadig full driver replacement that breaks iTunes/USBMux pairing!
        if (low_svc == L"winusb" || low_svc == L"libusbk" || low_svc == L"libusb0") {
            std::cerr
                << "  [WARNING] Detected non-Apple function driver '" << WideToUtf8(service_name)
                << "' on the parent iOS device (likely installed by Zadig)!\n"
                << "            Replacing 'usbccgp'/'usbaapl64' breaks iTunes trust & USBMux pairing.\n"
                << "            Recommended fix: In Device Manager -> Update Driver -> Browse my computer ->\n"
                << "            Let me pick -> Restore 'USB Composite Device' or 'Apple Mobile Device USB Driver',\n"
                << "            then re-run install_all_admin.ps1.\n";
        }

        BYTE filt_buf[1024] = {0};
        DWORD filt_size = 0;
        std::vector<std::wstring> filters;
        if (SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_UPPERFILTERS, &reg_type, filt_buf, sizeof(filt_buf), &filt_size)) {
            filters = ParseMultiSz(filt_buf, filt_size);
        }

        if (status_only) {
            std::cout << "  [Status] Current UpperFilters:";
            if (filters.empty()) {
                std::cout << " (none)";
            } else {
                for (const auto& f : filters) std::cout << " " << WideToUtf8(f);
            }
            std::cout << "\n";
            continue;
        }

        bool has_libusb0 = false;
        for (const auto& f : filters) {
            if (ToLowerWide(f) == L"libusb0") {
                has_libusb0 = true;
                break;
            }
        }

        if (attach_filter) {
            if (!has_libusb0) {
                filters.push_back(L"libusb0");
                auto raw_msz = SerializeMultiSz(filters);
                if (SetupDiSetDeviceRegistryPropertyW(
                        dev_info,
                        &dev_data,
                        SPDRP_UPPERFILTERS,
                         raw_msz.data(),
                        static_cast<DWORD>(raw_msz.size()))) {
                    std::cout << "  [OK] Attached 'libusb0' UpperFilter while preserving '"
                              << WideToUtf8(service_name) << "'!\n";
                    RestartUsbDeviceNode(dev_info, dev_data);
                } else {
                    std::cerr << "  [Error] SetupDiSetDeviceRegistryPropertyW failed (code "
                              << GetLastError() << ").\n";
                }
            } else {
                std::cout << "  [OK] 'libusb0' UpperFilter is already active on this device.\n";
            }
        } else {
            if (has_libusb0) {
                filters.erase(
                    std::remove_if(
                        filters.begin(),
                        filters.end(),
                        [](const std::wstring& f) { return ToLowerWide(f) == L"libusb0"; }),
                    filters.end());
                BOOL ok = FALSE;
                if (filters.empty()) {
                    ok = SetupDiSetDeviceRegistryPropertyW(
                        dev_info, &dev_data, SPDRP_UPPERFILTERS, nullptr, 0);
                } else {
                    auto raw_msz = SerializeMultiSz(filters);
                    ok = SetupDiSetDeviceRegistryPropertyW(
                        dev_info,
                        &dev_data,
                        SPDRP_UPPERFILTERS,
                        raw_msz.data(),
                        static_cast<DWORD>(raw_msz.size()));
                }
                if (ok) {
                    std::cout << "  [OK] Removed 'libusb0' UpperFilter cleanly.\n";
                    RestartUsbDeviceNode(dev_info, dev_data);
                }
            } else {
                std::cout << "  [OK] No 'libusb0' UpperFilter present on this device.\n";
            }
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return matched_devices;
}

bool ManageDirectShowDll(const std::filesystem::path& dll_path, bool register_server) {
    if (!std::filesystem::exists(dll_path)) {
        std::cerr << "[DirectShow] DLL not found at: " << WideToUtf8(dll_path.wstring()) << "\n";
        return false;
    }
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    if (!mod) {
        std::cerr << "[DirectShow] LoadLibraryW failed for ios_vcam_dshow.dll (error "
                  << GetLastError() << ").\n";
        return false;
    }
    using DllRegFn = HRESULT(STDAPICALLTYPE*)();
    const char* proc_name = register_server ? "DllRegisterServer" : "DllUnregisterServer";
    auto fn = reinterpret_cast<DllRegFn>(GetProcAddress(mod, proc_name));
    if (!fn) {
        std::cerr << "[DirectShow] Missing export " << proc_name << " in ios_vcam_dshow.dll.\n";
        FreeLibrary(mod);
        return false;
    }
    HRESULT hr = fn();
    FreeLibrary(mod);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] " << proc_name << " failed with HRESULT 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }
    std::cout << "[DirectShow] " << (register_server ? "Registered" : "Unregistered")
              << " Video & Audio virtual capture cards (" << WideToUtf8(dll_path.filename().wstring()) << ").\n";
    return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);

    bool do_install = true;
    bool do_uninstall = false;
    bool status_only = false;
    std::filesystem::path base_dir = ExeDirectory();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--uninstall" || arg == "/u") {
            do_uninstall = true;
            do_install = false;
        } else if (arg == "--install" || arg == "/i") {
            do_install = true;
            do_uninstall = false;
        } else if (arg == "--status") {
            status_only = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "iOS USB Capture Card Safe Driver & DirectShow Setup (ios_vcam_driver_setup.exe)\n"
                << "Usage:\n"
                << "  ios_vcam_driver_setup.exe --install     Register DirectShow DLL & attach libusb0 UpperFilter\n"
                << "  ios_vcam_driver_setup.exe --uninstall   Remove libusb0 UpperFilter & unregister DirectShow DLL\n"
                << "  ios_vcam_driver_setup.exe --status      Inspect Apple USB driver & filter status\n";
            return 0;
        }
    }

    if (status_only) {
        int n = ConfigureAppleUpperFilters(false, true);
        if (n == 0) {
            std::cout << "[Status] No Apple iOS USB device currently plugged in.\n";
        }
        return 0;
    }

    if (!IsProcessElevated()) {
        std::cerr << "[Error] Administrator privileges are required. Please run via install_all_admin.ps1 "
                     "or an elevated PowerShell/Command Prompt.\n";
        return 1;
    }

    std::filesystem::path dshow_dll = base_dir / L"ios_vcam_dshow.dll";
    std::filesystem::path drivers_dir = base_dir / L"drivers";

    if (do_install) {
        std::cout << "=== Installing iOS USB Universal Virtual Capture Card ===\n";
        bool dshow_ok = ManageDirectShowDll(dshow_dll, true);
        bool svc_ready = InstallLibUsb0KernelService(drivers_dir);
        if (svc_ready) {
            int count = ConfigureAppleUpperFilters(true, false);
            if (count == 0) {
                std::cout << "[Notice] No iPhone/iPad currently plugged in via USB. "
                             "Plug in your unlocked iPhone and re-run to attach the UpperFilter to its USB node.\n";
            }
        }
        return dshow_ok ? 0 : 1;
    }

    if (do_uninstall) {
        std::cout << "=== Uninstalling iOS USB Universal Virtual Capture Card ===\n";
        ConfigureAppleUpperFilters(false, false);
        ManageDirectShowDll(dshow_dll, false);
        return 0;
    }
    return 0;
#else
    (void)argc;
    (void)argv;
    std::cerr << "ios_vcam_driver_setup is a Windows SetupAPI tool.\n";
    return 1;
#endif
}
