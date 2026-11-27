#include "ios_vcam/app_lang.h"
#include "ios_vcam/app_resource.h"
#include "ios_vcam/app_updater.h"
#include "ios_vcam/diagnostics.h"
#include "ios_vcam/dshow_guids.h"
#include "ios_vcam/image_convert.h"
#include "ios_vcam/screenshot_util.h"
#include "ios_vcam/shared_ipc.h"
#include "ios_vcam/usb_ios_transport.h"
#include "ios_vcam/wasapi_renderer.h"

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <dbt.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ios_vcam;

// Control IDs
constexpr int kIdBtnSmartStart = 1001;
constexpr int kIdBtnMockTest = 1002;
constexpr int kIdBtnRepairDriver = 1003;
constexpr int kIdBtnUninstall = 1004;
constexpr int kIdBtnCopyObsGuide = 1005;
constexpr int kIdBtnCheckUpdate = 1006;
constexpr int kIdBtnPopoutMonitor = 1007;
constexpr int kIdBtnScreenshot = 1008;
constexpr int kIdBtnOpenScreenshots = 1009;
constexpr int kIdComboResMode = 1010;
constexpr int kIdComboStatusBar = 1011;
constexpr int kIdComboAudioOut = 1012;
constexpr int kIdChkHwDecode = 1013;
constexpr int kIdChkInlinePreview = 1014;
constexpr int kIdBtnDiagnostics = 1015;
constexpr int kIdComboLanguage = 1016;
constexpr int kIdEditLog = 1020;

constexpr UINT kWmAppendLog = WM_APP + 1;
constexpr UINT kWmHostExited = WM_APP + 2;
constexpr UINT kWmUpdateCheckDone = WM_APP + 3;
constexpr UINT kWmHostDiagEvent = WM_APP + 4;
constexpr UINT_PTR kTimerRefreshId = 1;
constexpr UINT_PTR kTimerStartupUpdateId = 2;
constexpr UINT_PTR kTimerPreviewId = 3;
constexpr UINT_PTR kTimerTelemetryId = 4;

enum class DshowRegState {
    NotRegistered,
    PathMismatch,
    Ready,
};

enum class IosUsbState {
    Unplugged,
    PluggedNeedFilter,
    PluggedZadigConflict,
    PluggedReady,
};

struct SystemHealth {
    DshowRegState dshow = DshowRegState::NotRegistered;
    bool libusb_service_ok = false;
    bool libusb_dll_present = false;
    bool apple_service_installed = false;
    bool apple_service_running = false;
    bool mf_h264_ok = true;
    IosUsbState ios_usb = IosUsbState::Unplugged;
    std::wstring ios_service_name;
    std::vector<std::wstring> missing_core_files;
    std::wstring os_version_desc;
    std::wstring hw_summary_desc;
};

struct DiagEventRecord {
    DiagCode code = DiagCode::None;
    DiagSeverity severity = DiagSeverity::Error;
    std::wstring timestamp_str;
    std::wstring detail_wide;
};

struct ResOptionSpec {
    uint32_t max_dim = 2560;
    std::wstring mode_tag;  // e.g. L"native", L"1440p", L"1080p", L"720p"
    std::wstring label;
    std::wstring cli_args;
};

std::wstring FormatUsbSpeedBadgeWide(UsbLinkSpeed speed, AppLanguage lang) {
    const bool en = (lang == AppLanguage::EnUS);
    switch (speed) {
        case UsbLinkSpeed::SuperPlus_20Gbps:
            return en ? L"⚡ USB 3.2 Gen2x2 (20Gbps) | Unlocked up to 3840p"
                      : L"⚡ USB 3.2 Gen2x2 (20Gbps)｜已解鎖最高 3840p";
        case UsbLinkSpeed::SuperPlus_10Gbps:
            return en ? L"⚡ USB 3.2 Gen2 (10Gbps) | Unlocked up to 3840p"
                      : L"⚡ USB 3.2 Gen2 (10Gbps)｜已解鎖最高 3840p";
        case UsbLinkSpeed::Super_5Gbps:
            return en ? L"⚡ USB 3.0 (5Gbps) | Unlocked up to 3840p"
                      : L"⚡ USB 3.0 (5Gbps)｜已解鎖最高 3840p";
        case UsbLinkSpeed::High_480Mbps:
            return en ? L"🔌 USB 2.0 (480Mbps) | Max 2K (2560p)"
                      : L"🔌 USB 2.0 (480Mbps)｜最高可選 2K (2560p)";
        case UsbLinkSpeed::Full_12Mbps:
            return en ? L"⚠️ USB 1.1 (12Mbps) | Max 720p (1280p)"
                      : L"⚠️ USB 1.1 (12Mbps)｜最高可選 720p (1280p)";
        case UsbLinkSpeed::Low_1_5Mbps:
            return en ? L"⚠️ USB 1.0 (1.5Mbps) | Max 720p (1280p)"
                      : L"⚠️ USB 1.0 (1.5Mbps)｜最高可選 720p (1280p)";
        default:
            return L"";
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8Str(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring CurrentLocalTimeShortWide() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32] = {0};
    std::swprintf(buf, 32, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool CopyWideTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t cb = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, cb);
    bool ok = false;
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            std::memcpy(ptr, text.c_str(), cb);
            GlobalUnlock(hMem);
            ok = (SetClipboardData(CF_UNICODETEXT, hMem) != nullptr);
        }
        if (!ok) {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
    return ok;
}

std::wstring ToLowerWide(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::filesystem::path AppDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return std::filesystem::path(buf).parent_path();
}

void UnblockDownloadedFiles(const std::filesystem::path& base_dir) {
    const wchar_t* rel_files[] = {
        L"JCast for iOS.exe",
        L"ios_vcam_gui.exe",
        L"ios_vcam_dshow.dll",
        L"ios_vcam_host.exe",
        L"ios_vcam_driver_setup.exe",
        L"ios_vcam_tests.exe",
        L"libusb-1.0.dll",
        L"install_all_admin.ps1",
        L"uninstall_all_admin.ps1",
        L"drivers\\libusb0.sys",
        L"drivers\\libusb0.dll",
    };
    for (const wchar_t* rel : rel_files) {
        std::wstring ads = (base_dir / rel).wstring() + L":Zone.Identifier";
        DeleteFileW(ads.c_str());
    }
}

std::vector<std::wstring> ParseMultiSz(const BYTE* raw, DWORD byte_len) {
    std::vector<std::wstring> list;
    if (!raw || byte_len < sizeof(wchar_t)) return list;
    const auto* wptr = reinterpret_cast<const wchar_t*>(raw);
    size_t max_chars = byte_len / sizeof(wchar_t);
    size_t start = 0;
    while (start < max_chars && wptr[start] != L'\0') {
        size_t end = start;
        while (end < max_chars && wptr[end] != L'\0') ++end;
        list.emplace_back(wptr + start, end - start);
        start = end + 1;
    }
    return list;
}

SystemHealth ProbeSystemHealth(const std::filesystem::path& base_dir) {
    SystemHealth h;
    std::error_code ec;
    h.libusb_dll_present = std::filesystem::exists(base_dir / L"libusb-1.0.dll", ec);

    const wchar_t* kRequiredFiles[] = {
        L"ios_vcam_host.exe",
        L"ios_vcam_driver_setup.exe",
        L"ios_vcam_dshow.dll",
        L"libusb-1.0.dll",
        L"drivers\\libusb0.sys",
    };
    for (const wchar_t* req : kRequiredFiles) {
        if (!std::filesystem::exists(base_dir / req, ec)) {
            h.missing_core_files.emplace_back(req);
        }
    }

    wchar_t sys_dir[MAX_PATH] = {0};
    if (GetSystemDirectoryW(sys_dir, MAX_PATH) > 0) {
        std::filesystem::path sp(sys_dir);
        h.mf_h264_ok = std::filesystem::exists(sp / L"mfplat.dll", ec) &&
                       std::filesystem::exists(sp / L"msmpeg2vdec.dll", ec);
    }

    {
        struct RtlOsVersionInfoExW {
            ULONG dwOSVersionInfoSize;
            ULONG dwMajorVersion;
            ULONG dwMinorVersion;
            ULONG dwBuildNumber;
            ULONG dwPlatformId;
            WCHAR szCSDVersion[128];
        };
        using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfoExW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
            RtlOsVersionInfoExW ver{};
            ver.dwOSVersionInfoSize = sizeof(ver);
            if (fn && fn(&ver) == 0) {
                const wchar_t* win_name = (ver.dwBuildNumber >= 22000) ? L"Windows 11" : L"Windows 10";
                wchar_t obuf[96] = {0};
                std::swprintf(
                    obuf,
                    96,
                    L"%ls (%lu.%lu Build %lu x64)",
                    win_name,
                    ver.dwMajorVersion,
                    ver.dwMinorVersion,
                    ver.dwBuildNumber);
                h.os_version_desc = obuf;
            }
        }
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        wchar_t hwbuf[96] = {0};
        std::swprintf(
            hwbuf,
            96,
            L"%lu 邏輯核心 ｜ 記憶體 %.1f / %.1f GB 可用",
            si.dwNumberOfProcessors,
            static_cast<double>(ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0),
            static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
        h.hw_summary_desc = hwbuf;
    }

    const wchar_t* kRegPath = L"CLSID\\{8E74A2B1-5C19-4D83-9A41-2F6C8D90E101}\\InprocServer32";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, kRegPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t reg_dll[MAX_PATH * 2] = {0};
        DWORD cb = sizeof(reg_dll) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(reg_dll), &cb) == ERROR_SUCCESS) {
            std::filesystem::path expected = std::filesystem::weakly_canonical(base_dir / L"ios_vcam_dshow.dll", ec);
            std::filesystem::path actual = std::filesystem::weakly_canonical(std::filesystem::path(reg_dll), ec);
            if (ToLowerWide(expected.wstring()) == ToLowerWide(actual.wstring()) &&
                std::filesystem::exists(actual, ec)) {
                h.dshow = DshowRegState::Ready;
            } else {
                h.dshow = DshowRegState::PathMismatch;
            }
        }
        RegCloseKey(hKey);
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, GENERIC_READ);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"libusb0", SERVICE_QUERY_CONFIG);
        if (svc) {
            h.libusb_service_ok = true;
            CloseServiceHandle(svc);
        }

        const wchar_t* kAppleServices[] = {
            L"Apple Mobile Device Service",
            L"usbaapl64",
            L"AppleKmdfFilter",
            L"AppleLowerFilter",
        };
        for (const wchar_t* asvc_name : kAppleServices) {
            SC_HANDLE asvc = OpenServiceW(scm, asvc_name, SERVICE_QUERY_STATUS);
            if (asvc) {
                h.apple_service_installed = true;
                SERVICE_STATUS ss{};
                if (QueryServiceStatus(asvc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
                    h.apple_service_running = true;
                }
                CloseServiceHandle(asvc);
            }
        }
        CloseServiceHandle(scm);
    }

    HDEVINFO dev_info = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (dev_info != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev_data{};
        dev_data.cbSize = sizeof(dev_data);
        for (DWORD idx = 0; SetupDiEnumDeviceInfo(dev_info, idx, &dev_data); ++idx) {
            BYTE hwid_buf[2048] = {0};
            DWORD reg_type = 0, req_size = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(
                    dev_info, &dev_data, SPDRP_HARDWAREID, &reg_type, hwid_buf, sizeof(hwid_buf), &req_size)) {
                continue;
            }
            auto hwids = ParseMultiSz(hwid_buf, req_size);
            bool is_apple_parent = false;
            for (const auto& id : hwids) {
                std::wstring low = ToLowerWide(id);
                if (low.find(L"vid_05ac&pid_12") != std::wstring::npos && low.find(L"&mi_") == std::wstring::npos) {
                    is_apple_parent = true;
                    break;
                }
            }
            if (!is_apple_parent) continue;

            wchar_t svc_buf[256] = {0};
            SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_SERVICE, nullptr,
                reinterpret_cast<BYTE*>(svc_buf), sizeof(svc_buf) - sizeof(wchar_t), nullptr);
            h.ios_service_name = svc_buf;
            std::wstring low_svc = ToLowerWide(h.ios_service_name);
            if (!low_svc.empty()) {
                h.apple_service_installed = true;
            }

            if (low_svc == L"winusb" || low_svc == L"libusbk" || low_svc == L"libusb0") {
                h.ios_usb = IosUsbState::PluggedZadigConflict;
                break;
            }

            BYTE filt_buf[1024] = {0};
            DWORD filt_sz = 0;
            bool has_filter = false;
            if (SetupDiGetDeviceRegistryPropertyW(
                    dev_info, &dev_data, SPDRP_UPPERFILTERS, &reg_type, filt_buf, sizeof(filt_buf), &filt_sz)) {
                for (const auto& f : ParseMultiSz(filt_buf, filt_sz)) {
                    if (ToLowerWide(f) == L"libusb0") {
                        has_filter = true;
                        break;
                    }
                }
            }
            h.ios_usb = has_filter ? IosUsbState::PluggedReady : IosUsbState::PluggedNeedFilter;
            break;
        }
        SetupDiDestroyDeviceInfoList(dev_info);
    }

    return h;
}

std::wstring NormalizeLineEndingsCrlf(std::string_view utf8_text) {
    std::string norm;
    norm.reserve(utf8_text.size() + 64);
    for (size_t i = 0; i < utf8_text.size(); ++i) {
        char c = utf8_text[i];
        if (c == '\r') {
            norm.push_back('\r');
            norm.push_back('\n');
            if (i + 1 < utf8_text.size() && utf8_text[i + 1] == '\n') {
                ++i;
            }
        } else if (c == '\n') {
            norm.push_back('\r');
            norm.push_back('\n');
        } else {
            norm.push_back(c);
        }
    }
    return Utf8ToWide(norm);
}

std::wstring FormatBytesMb(uint64_t bytes) {
    wchar_t buf[64] = {0};
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::swprintf(buf, 64, L"%.2f MB", mb);
    return buf;
}

void OpenSafeGitHubWebUrl(HWND hwnd, const std::string& candidate_utf8) {
    const std::wstring fallback =
        L"https://github.com/" IOS_VCAM_GITHUB_OWNER_W L"/" IOS_VCAM_GITHUB_REPO_W L"/releases/latest";
    std::wstring wurl = Utf8ToWide(candidate_utf8);
    std::wstring low = ToLowerWide(wurl);
    if (low.rfind(L"https://github.com/", 0) != 0) {
        wurl = fallback;
    }
    ShellExecuteW(hwnd, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

class UpdateModalWindow {
public:
    UpdateModalWindow(
        HINSTANCE hinst,
        HWND parent,
        std::filesystem::path app_dir,
        const UpdateCheckResult& info,
        UINT dpi,
        AppLanguage lang,
        HFONT font_normal,
        HFONT font_bold,
        HFONT font_title,
        HFONT font_mono,
        HICON icon_small,
        std::function<void()> on_stop_capture)
        : hinst_(hinst),
          parent_(parent),
          app_dir_(std::move(app_dir)),
          info_(info),
          dpi_(dpi),
          lang_(lang),
          font_normal_(font_normal),
          font_bold_(font_bold),
          font_title_(font_title),
          font_mono_(font_mono),
          icon_small_(icon_small),
          on_stop_capture_(std::move(on_stop_capture)) {
        bool installed_mode = IsInstalledAppMode(app_dir_);
        if (installed_mode && !info_.latest.setup_exe.browser_download_url.empty()) {
            use_setup_exe_ = true;
            target_asset_ = info_.latest.setup_exe;
        } else if (!installed_mode && !info_.latest.portable_zip.browser_download_url.empty()) {
            use_setup_exe_ = false;
            target_asset_ = info_.latest.portable_zip;
        } else if (!info_.latest.setup_exe.browser_download_url.empty()) {
            use_setup_exe_ = true;
            target_asset_ = info_.latest.setup_exe;
        } else if (!info_.latest.portable_zip.browser_download_url.empty()) {
            use_setup_exe_ = false;
            target_asset_ = info_.latest.portable_zip;
        }
    }

    ~UpdateModalWindow() {
        cancel_flag_.store(true, std::memory_order_relaxed);
        if (dl_thread_.joinable()) {
            dl_thread_.join();
        }
    }

    bool RunModal() {
        static bool class_registered = false;
        const wchar_t* kClsName = L"JCastUpdateModalWndClass";
        if (!class_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &UpdateModalWindow::StaticDlgProc;
            wc.hInstance = hinst_;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.lpszClassName = kClsName;
            RegisterClassExW(&wc);
            class_registered = true;
        }

        int dlg_w = MulDiv(590, static_cast<int>(dpi_), 96);
        int dlg_h = MulDiv(450, static_cast<int>(dpi_), 96);

        RECT prc{};
        GetWindowRect(parent_, &prc);
        int pos_x = prc.left + ((prc.right - prc.left) - dlg_w) / 2;
        int pos_y = prc.top + ((prc.bottom - prc.top) - dlg_h) / 2;

        std::wstring title =
            std::wstring(LText(lang_, L"JCast for iOS — 發現新版本 ", L"JCast for iOS — Update Available ")) +
            Utf8ToWide(info_.latest.tag_name);
        EnableWindow(parent_, FALSE);

        dlg_hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kClsName,
            title.c_str(),
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            pos_x,
            pos_y,
            dlg_w,
            dlg_h,
            parent_,
            nullptr,
            hinst_,
            this);

        if (!dlg_hwnd_) {
            EnableWindow(parent_, TRUE);
            return false;
        }

        if (icon_small_) {
            SendMessageW(dlg_hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));
        }

        ShowWindow(dlg_hwnd_, SW_SHOWNORMAL);
        UpdateWindow(dlg_hwnd_);

        MSG msg{};
        while (!modal_done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg_hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (dl_thread_.joinable()) {
            dl_thread_.join();
        }

        EnableWindow(parent_, TRUE);
        if (IsWindow(dlg_hwnd_)) {
            DestroyWindow(dlg_hwnd_);
            dlg_hwnd_ = nullptr;
        }
        SetForegroundWindow(parent_);
        return update_launched_;
    }

private:
    static constexpr int kIdDlgBtnDownload = 2001;
    static constexpr int kIdDlgBtnOpenWeb = 2002;
    static constexpr int kIdDlgBtnCancel = 2003;
    static constexpr int kIdDlgEditNotes = 2004;
    static constexpr int kIdDlgProgress = 2005;

    static constexpr UINT kWmDlgProgress = WM_APP + 10;
    static constexpr UINT kWmDlgFinished = WM_APP + 11;

    int S(int px) const {
        return MulDiv(px, static_cast<int>(dpi_), 96);
    }

    const wchar_t* T(const wchar_t* zh, const wchar_t* en) const {
        return LText(lang_, zh, en);
    }

    void BuildDialogControls() {
        RECT rc{};
        GetClientRect(dlg_hwnd_, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        int pad = S(16);
        int inner_w = cw - pad * 2;

        std::wstring hdr = std::wstring(T(L"🎉 發現新版本：", L"🎉 New Release Available: ")) +
                           Utf8ToWide(info_.latest.title);
        HWND lbl_hdr = CreateWindowExW(
            0, L"STATIC", hdr.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            pad, S(14), inner_w, S(26), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_hdr, WM_SETFONT, reinterpret_cast<WPARAM>(font_title_), TRUE);

        std::wstring pkg_desc;
        if (!target_asset_.browser_download_url.empty()) {
            pkg_desc = std::wstring(use_setup_exe_
                                        ? T(L"安裝版自動升級 (", L"Installer Auto-Upgrade (")
                                        : T(L"便攜版原地自動更新 (", L"Portable In-Place Auto-Update (")) +
                       Utf8ToWide(target_asset_.name) + L", " + FormatBytesMb(target_asset_.size_bytes) +
                       (target_asset_.sha256_hex.empty() ? L")" : T(L", 含 SHA-256 驗證)", L", SHA-256 verified)"));
        } else {
            pkg_desc = T(L"請開啟 GitHub Releases 網頁下載更新檔案", L"Please open GitHub Releases to download");
        }
        std::wstring sub = std::wstring(T(L"目前版本：v", L"Current: v")) + IOS_VCAM_VER_DISPLAY_WSTR +
                           T(L"  ➔  最新版本：", L"  ->  Latest: ") +
                           Utf8ToWide(info_.latest.tag_name) + L"  |  " + pkg_desc;
        HWND lbl_sub = CreateWindowExW(
            0, L"STATIC", sub.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            pad, S(44), inner_w, S(22), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_sub, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        HWND lbl_notes = CreateWindowExW(
            0, L"STATIC", T(L"版本更新日誌 (GitHub Release Notes)：", L"Release Notes (GitHub):"),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            pad, S(72), inner_w, S(20), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_notes, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int notes_top = S(96);
        int notes_h = ch - notes_top - S(116);
        edit_notes_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            pad, notes_top, inner_w, std::max(S(100), notes_h),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgEditNotes)), hinst_, nullptr);
        SendMessageW(edit_notes_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
        std::wstring notes_w = info_.latest.body_markdown.empty()
                                   ? T(L"（此版本未附帶詳細文字說明，請點擊「開啟 GitHub 發布頁」查看。）",
                                       L"(No release notes provided. Click 'Open GitHub Release' to view details.)")
                                   : NormalizeLineEndingsCrlf(info_.latest.body_markdown);
        SetWindowTextW(edit_notes_, notes_w.c_str());

        int prog_y = ch - S(106);
        progress_bar_ = CreateWindowExW(
            0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            pad, prog_y, inner_w, S(20),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgProgress)), hinst_, nullptr);
        SendMessageW(progress_bar_, PBM_SETRANGE32, 0, 1000);
        SendMessageW(progress_bar_, PBM_SETPOS, 0, 0);

        lbl_status_ = CreateWindowExW(
            0, L"STATIC",
            T(L"準備就緒 — 點擊下方按鈕即可自動從 GitHub 下載並完成升級。",
              L"Ready — Click below to download, verify SHA-256, and install the update."),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            pad, prog_y + S(24), inner_w, S(20), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        int btn_y = ch - S(50);
        int gap = S(10);
        int w_dl = (inner_w - gap * 2) * 46 / 100;
        int w_web = (inner_w - gap * 2) * 30 / 100;
        int w_can = inner_w - gap * 2 - w_dl - w_web;

        btn_download_ = CreateWindowExW(
            0, L"BUTTON", T(L"🚀 一鍵下載並自動更新", L"🚀 Download & Auto-Update"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            pad, btn_y, w_dl, S(36),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnDownload)), hinst_, nullptr);
        SendMessageW(btn_download_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);
        if (target_asset_.browser_download_url.empty()) {
            EnableWindow(btn_download_, FALSE);
        }

        btn_web_ = CreateWindowExW(
            0, L"BUTTON", T(L"🌐 開啟 GitHub 發布頁", L"🌐 Open GitHub Release"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            pad + w_dl + gap, btn_y, w_web, S(36),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnOpenWeb)), hinst_, nullptr);
        SendMessageW(btn_web_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        btn_cancel_ = CreateWindowExW(
            0, L"BUTTON", T(L"稍後再說", L"Later"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            pad + w_dl + gap + w_web + gap, btn_y, w_can, S(36),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnCancel)), hinst_, nullptr);
        SendMessageW(btn_cancel_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
    }

    void StartDownloadWorker() {
        if (downloading_ || target_asset_.browser_download_url.empty()) return;

        wchar_t tmp_buf[MAX_PATH * 2] = {0};
        DWORD n = GetTempPathW(MAX_PATH * 2 - 1, tmp_buf);
        std::filesystem::path tmp_dir = (n > 0) ? std::filesystem::path(tmp_buf) : app_dir_;
        tmp_dir /= L"JCast_For_iOS_Update";
        std::filesystem::path safe_leaf = std::filesystem::path(Utf8ToWide(target_asset_.name)).filename();
        if (safe_leaf.empty()) safe_leaf = use_setup_exe_ ? L"JCast-Update-Setup.exe" : L"JCast-Update-Portable.zip";
        downloaded_file_ = tmp_dir / safe_leaf;

        downloading_ = true;
        cancel_flag_.store(false, std::memory_order_relaxed);
        EnableWindow(btn_download_, FALSE);
        SetWindowTextW(btn_cancel_, T(L"取消下載", L"Cancel"));
        SendMessageW(progress_bar_, PBM_SETPOS, 0, 0);
        SetWindowTextW(lbl_status_, T(L"正在連線至 GitHub Releases 伺服器...", L"Connecting to GitHub Releases..."));

        if (dl_thread_.joinable()) {
            dl_thread_.join();
        }

        ReleaseAssetInfo asset_copy = target_asset_;
        std::filesystem::path dst_copy = downloaded_file_;
        HWND target_hwnd = dlg_hwnd_;
        AppLanguage cur_lang = lang_;

        dl_thread_ = std::thread([this, asset_copy, dst_copy, target_hwnd, cur_lang]() {
            std::string err;
            uint64_t last_ui_tick = 0;
            bool ok = DownloadReleaseAssetHttps(
                asset_copy,
                dst_copy,
                &cancel_flag_,
                [&](uint64_t done, uint64_t total) {
                    uint64_t now = GetTickCount64();
                    if (done < total && now - last_ui_tick < 60) return;
                    last_ui_tick = now;

                    int permille = (total > 0)
                                       ? static_cast<int>(std::min<uint64_t>(1000ULL, (done * 1000ULL) / total))
                                       : 0;
                    std::wstring msg;
                    if (total > 0) {
                        int pct = permille / 10;
                        msg = std::wstring(LText(cur_lang, L"正在下載更新包... ", L"Downloading update... ")) +
                              std::to_wstring(pct) + L"% (" +
                              FormatBytesMb(done) + L" / " + FormatBytesMb(total) + L")";
                    } else {
                        msg = std::wstring(LText(cur_lang, L"正在下載更新包... 已接收 ", L"Downloading update... Received ")) +
                              FormatBytesMb(done);
                    }
                    auto* heap_msg = new std::wstring(std::move(msg));
                    if (!PostMessageW(
                            target_hwnd,
                            kWmDlgProgress,
                            static_cast<WPARAM>(permille),
                            reinterpret_cast<LPARAM>(heap_msg))) {
                        delete heap_msg;
                    }
                },
                &err);

            auto* heap_err = new std::wstring(Utf8ToWide(err));
            if (!PostMessageW(
                    target_hwnd,
                    kWmDlgFinished,
                    ok ? 1 : 0,
                    reinterpret_cast<LPARAM>(heap_err))) {
                delete heap_err;
            }
        });
    }

    void RequestCloseOrCancel() {
        if (downloading_) {
            cancel_flag_.store(true, std::memory_order_relaxed);
            close_after_cancel_ = true;
            EnableWindow(btn_cancel_, FALSE);
            SetWindowTextW(lbl_status_, T(L"正在取消下載，請稍候...", L"Cancelling download, please wait..."));
        } else {
            modal_done_ = true;
        }
    }

    static LRESULT CALLBACK StaticDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        UpdateModalWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<UpdateModalWindow*>(cs->lpCreateParams);
            self->dlg_hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<UpdateModalWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->DlgProc(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT DlgProc(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE:
                BuildDialogControls();
                return 0;

            case WM_CTLCOLORSTATIC: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                HWND ctrl = reinterpret_cast<HWND>(lParam);
                if (ctrl != edit_notes_) {
                    SetBkMode(hdc, TRANSPARENT);
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
                }
                break;
            }

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                int code = HIWORD(wParam);
                if (code == BN_CLICKED) {
                    if (id == kIdDlgBtnDownload) {
                        StartDownloadWorker();
                        return 0;
                    }
                    if (id == kIdDlgBtnOpenWeb) {
                        OpenSafeGitHubWebUrl(dlg_hwnd_, info_.latest.html_url);
                        return 0;
                    }
                    if (id == kIdDlgBtnCancel || id == IDCANCEL) {
                        RequestCloseOrCancel();
                        return 0;
                    }
                }
                break;
            }

            case kWmDlgProgress: {
                auto* str = reinterpret_cast<std::wstring*>(lParam);
                SendMessageW(progress_bar_, PBM_SETPOS, wParam, 0);
                if (str) {
                    SetWindowTextW(lbl_status_, str->c_str());
                    delete str;
                }
                return 0;
            }

            case kWmDlgFinished: {
                auto* err_str = reinterpret_cast<std::wstring*>(lParam);
                std::wstring err_msg = err_str ? *err_str : L"";
                delete err_str;

                if (dl_thread_.joinable()) {
                    dl_thread_.join();
                }
                downloading_ = false;

                if (close_after_cancel_) {
                    modal_done_ = true;
                    return 0;
                }

                if (wParam == 1) {
                    SendMessageW(progress_bar_, PBM_SETPOS, 1000, 0);
                    SetWindowTextW(
                        lbl_status_,
                        T(L"✅ 下載與雜湊驗證完成！正在啟動自動升級程序...",
                          L"✅ Download & SHA-256 verified! Launching updater..."));
                    UpdateWindow(dlg_hwnd_);

                    if (on_stop_capture_) {
                        on_stop_capture_();
                    }

                    std::string launch_err;
                    bool launched = use_setup_exe_
                                        ? LaunchInstallerUpdate(downloaded_file_, dlg_hwnd_, &launch_err)
                                        : LaunchPortableZipSelfUpdate(
                                              downloaded_file_, app_dir_, GetCurrentProcessId(), &launch_err);
                    if (launched) {
                        update_launched_ = true;
                        modal_done_ = true;
                        return 0;
                    }

                    std::wstring w_lerr = Utf8ToWide(launch_err);
                    SetWindowTextW(lbl_status_, (L"⚠️ " + w_lerr).c_str());
                    EnableWindow(btn_download_, TRUE);
                    EnableWindow(btn_cancel_, TRUE);
                    SetWindowTextW(btn_cancel_, T(L"關閉", L"Close"));
                } else {
                    std::wstring fail_txt = L"❌ " + (err_msg.empty() ? T(L"下載失敗", L"Download failed") : err_msg);
                    SetWindowTextW(lbl_status_, fail_txt.c_str());
                    EnableWindow(btn_download_, TRUE);
                    EnableWindow(btn_cancel_, TRUE);
                    SetWindowTextW(btn_cancel_, T(L"關閉", L"Close"));
                    if (!cancel_flag_.load(std::memory_order_relaxed)) {
                        MessageBoxW(
                            dlg_hwnd_,
                            fail_txt.c_str(),
                            T(L"下載更新失敗", L"Update Download Failed"),
                            MB_ICONWARNING | MB_OK);
                    }
                }
                return 0;
            }

            case WM_CLOSE:
                RequestCloseOrCancel();
                return 0;
        }
        return DefWindowProcW(hwnd_or_dlg(), msg, wParam, lParam);
    }

    HWND hwnd_or_dlg() const { return dlg_hwnd_; }

    HINSTANCE hinst_ = nullptr;
    HWND parent_ = nullptr;
    HWND dlg_hwnd_ = nullptr;
    std::filesystem::path app_dir_;
    UpdateCheckResult info_;
    UINT dpi_ = 96;
    AppLanguage lang_ = AppLanguage::ZhTW;

    HFONT font_normal_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_mono_ = nullptr;
    HICON icon_small_ = nullptr;
    std::function<void()> on_stop_capture_;

    bool use_setup_exe_ = true;
    ReleaseAssetInfo target_asset_;
    std::filesystem::path downloaded_file_;

    HWND edit_notes_ = nullptr;
    HWND progress_bar_ = nullptr;
    HWND lbl_status_ = nullptr;
    HWND btn_download_ = nullptr;
    HWND btn_web_ = nullptr;
    HWND btn_cancel_ = nullptr;

    std::atomic<bool> cancel_flag_{false};
    bool downloading_ = false;
    bool close_after_cancel_ = false;
    bool modal_done_ = false;
    bool update_launched_ = false;
    std::thread dl_thread_;
};

class DiagnosticsModalWindow {
public:
    DiagnosticsModalWindow(
        HINSTANCE hinst,
        HWND parent,
        std::filesystem::path app_dir,
        UINT dpi,
        AppLanguage lang,
        HFONT font_normal,
        HFONT font_bold,
        HFONT font_title,
        HFONT font_mono,
        HICON icon_small,
        UsbLinkSpeed usb_speed,
        const StreamHealthSnapshot& telem,
        std::vector<DiagEventRecord> faults,
        std::wstring raw_log_text,
        std::function<SystemHealth()> query_health,
        std::function<bool()> on_repair_driver,
        std::function<void()> on_clear_faults)
        : hinst_(hinst),
          parent_(parent),
          app_dir_(std::move(app_dir)),
          dpi_(dpi),
          lang_(lang),
          font_normal_(font_normal),
          font_bold_(font_bold),
          font_title_(font_title),
          font_mono_(font_mono),
          icon_small_(icon_small),
          usb_speed_(usb_speed),
          telem_(telem),
          faults_(std::move(faults)),
          raw_log_text_(std::move(raw_log_text)),
          query_health_(std::move(query_health)),
          on_repair_driver_(std::move(on_repair_driver)),
          on_clear_faults_(std::move(on_clear_faults)) {}

    void RunModal() {
        const wchar_t* kClassName = L"JCastDiagnosticsModalWndClass";
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &DiagnosticsModalWindow::StaticWndProc;
            wc.hInstance = hinst_;
            wc.hIcon = icon_small_ ? icon_small_ : LoadIconW(nullptr, IDI_APPLICATION);
            wc.hIconSm = wc.hIcon;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.lpszClassName = kClassName;
            RegisterClassExW(&wc);
            registered = true;
        }

        int win_w = MulDiv(780, static_cast<int>(dpi_), 96);
        int win_h = MulDiv(580, static_cast<int>(dpi_), 96);

        RECT prc{};
        GetWindowRect(parent_, &prc);
        int pos_x = prc.left + ((prc.right - prc.left) - win_w) / 2;
        int pos_y = prc.top + ((prc.bottom - prc.top) - win_h) / 2;

        EnableWindow(parent_, FALSE);

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kClassName,
            T(L"JCast for iOS — 🩺 智慧故障偵測與一鍵診斷報錯中心",
              L"JCast for iOS — 🩺 Smart Fault Diagnostics & Bug Report Center"),
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            pos_x,
            pos_y,
            win_w,
            win_h,
            parent_,
            nullptr,
            hinst_,
            this);

        if (!hwnd_) {
            EnableWindow(parent_, TRUE);
            return;
        }

        MSG msg{};
        while (!modal_done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
        }

        EnableWindow(parent_, TRUE);
        SetForegroundWindow(parent_);
    }

private:
    static constexpr int kBtnRepair = 4001;
    static constexpr int kBtnCopy = 4002;
    static constexpr int kBtnSaveTxt = 4003;
    static constexpr int kBtnGitHubIssue = 4004;
    static constexpr int kBtnClose = 4005;

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi_), 96); }
    const wchar_t* T(const wchar_t* zh, const wchar_t* en) const { return LText(lang_, zh, en); }
    const char* Tu(const char* zh, const char* en) const { return LTextUtf8(lang_, zh, en); }

    void RebuildReportAndRefreshUi() {
        const bool en = (lang_ == AppLanguage::EnUS);
        SystemHealth h = query_health_ ? query_health_() : ProbeSystemHealth(app_dir_);
        primary_code_ = DiagCode::None;

        std::vector<DiagCode> active_codes;
        auto push_unique_code = [&](DiagCode c) {
            if (c == DiagCode::None) return;
            if (std::find(active_codes.begin(), active_codes.end(), c) == active_codes.end()) {
                active_codes.push_back(c);
            }
            if (primary_code_ == DiagCode::None) {
                primary_code_ = c;
            }
        };

        if (!h.missing_core_files.empty()) push_unique_code(DiagCode::E101_MissingCoreFiles);
        if (!h.mf_h264_ok) push_unique_code(DiagCode::E103_MissingMediaFoundation);
        if (!h.apple_service_installed) push_unique_code(DiagCode::E102_MissingAppleService);
        if (h.dshow != DshowRegState::Ready) push_unique_code(DiagCode::E104_DshowNotRegistered);
        if (h.ios_usb == IosUsbState::PluggedZadigConflict) {
            push_unique_code(DiagCode::E201_ZadigDriverConflict);
        } else if (h.ios_usb == IosUsbState::PluggedNeedFilter) {
            push_unique_code(DiagCode::E202_UpperFilterMissing);
        }

        for (auto it = faults_.rbegin(); it != faults_.rend(); ++it) {
            push_unique_code(it->code);
        }

        if (lbl_headline_) {
            if (active_codes.empty()) {
                SetWindowTextW(
                    lbl_headline_,
                    T(L"✅ 8 項系統環境自檢正常｜目前未偵測到錯誤碼（若仍遇異常可直接複製報告或前往 GitHub 回報）",
                      L"✅ All 8 environment checks passed | No faults detected (you can still copy or file a report)"));
            } else {
                const auto& pspec = LookupDiagSpec(primary_code_, lang_);
                std::wstring head =
                    std::wstring(T(L"⚠️ 共偵測到 ", L"⚠️ Detected ")) +
                    std::to_wstring(active_codes.size()) +
                    T(L" 項異常或提醒｜主要代碼：", L" issue(s) | Primary: ") +
                    Utf8ToWide(pspec.code_str) + L" (" + Utf8ToWide(pspec.title_utf8) + L")";
                SetWindowTextW(lbl_headline_, head.c_str());
            }
        }

        // Synthesize full UTF-8 diagnostic report, then redact sensitive user paths & UDIDs
        std::string rpt;
        rpt.reserve(8192);
        rpt += "======================================================================\n";
        rpt += std::string("  JCast for iOS v" IOS_VCAM_VER_DISPLAY_STR) +
               Tu(" — 系統環境自檢與故障診斷報錯單\n", " — System Self-Check & Fault Diagnostic Report\n");
        rpt += "======================================================================\n\n";

        rpt += Tu("【一、8 項系統環境與硬體全面自檢】\n", "[1. 8-Point System & Hardware Environment Check]\n");
        rpt += std::string(Tu("1. 作業系統環境  : ", "1. Operating System : ")) +
               (h.os_version_desc.empty() ? "Windows x64" : WideToUtf8Str(h.os_version_desc)) + "\n";
        rpt += std::string(Tu("2. 處理器與記憶體: ", "2. CPU & Memory     : ")) +
               WideToUtf8Str(h.hw_summary_desc) + "\n";
        if (h.missing_core_files.empty()) {
            rpt += Tu("3. 核心檔案完整性: [PASS] 5 項核心執行檔、DLL 與簽章驅動皆完整\n",
                      "3. Core Files       : [PASS] All 5 core binaries, DLLs & signed drivers present\n");
        } else {
            rpt += Tu("3. 核心檔案完整性: [FAIL] 缺少檔案: ", "3. Core Files       : [FAIL] Missing: ");
            for (size_t i = 0; i < h.missing_core_files.size(); ++i) {
                if (i > 0) rpt += ", ";
                rpt += WideToUtf8Str(h.missing_core_files[i]);
            }
            rpt += " (JCAST-E101)\n";
        }
        rpt += std::string(Tu("4. 系統 H.264 元件: ", "4. System H.264 MFT : ")) +
               (h.mf_h264_ok
                    ? Tu("[PASS] Windows Media Foundation (mfplat / msmpeg2vdec) 正常\n",
                         "[PASS] Windows Media Foundation (mfplat / msmpeg2vdec) ready\n")
                    : Tu("[FAIL] 缺少 Media Feature Pack H.264 解碼元件 (JCAST-E103)\n",
                         "[FAIL] Missing Media Feature Pack H.264 decoder (JCAST-E103)\n"));
        if (h.apple_service_running) {
            rpt += Tu("5. Apple 配對服務: [PASS] Apple 行動裝置服務與驅動運作中\n",
                      "5. Apple Pairing    : [PASS] Apple Mobile Device service & driver active\n");
        } else if (h.apple_service_installed) {
            rpt += Tu("5. Apple 配對服務: [PASS] 已偵測到 Apple USB 驅動元件\n",
                      "5. Apple Pairing    : [PASS] Apple USB driver components detected\n");
        } else {
            rpt += Tu("5. Apple 配對服務: [WARN] 未偵測到 Apple 裝置 / iTunes 服務 (JCAST-E102)\n",
                      "5. Apple Pairing    : [WARN] Apple Devices / iTunes service not detected (JCAST-E102)\n");
        }
        rpt += std::string(Tu("6. WHQL 過濾服務 : ", "6. WHQL Filter Svc  : ")) +
               (h.libusb_service_ok
                    ? Tu("[PASS] libusb0 核心服務已安裝就緒\n", "[PASS] libusb0 kernel service installed & ready\n")
                    : Tu("[WARN] libusb0 尚未安裝（點「一鍵修復驅動」自動完成）\n",
                         "[WARN] libusb0 not installed yet (click 'Repair Driver' to install)\n"));
        if (h.dshow == DshowRegState::Ready) {
            rpt += Tu("7. DirectShow 鏡頭: [PASS] JCast for iOS (Video/Audio) 註冊正常\n",
                      "7. DirectShow Cam   : [PASS] JCast for iOS (Video/Audio) registered\n");
        } else if (h.dshow == DshowRegState::PathMismatch) {
            rpt += Tu("7. DirectShow 鏡頭: [WARN] 資料夾路徑已變更，需重新註冊 (JCAST-E104)\n",
                      "7. DirectShow Cam   : [WARN] App folder moved; re-registration needed (JCAST-E104)\n");
        } else {
            rpt += Tu("7. DirectShow 鏡頭: [WARN] 尚未註冊虛擬擷取卡 (JCAST-E104)\n",
                      "7. DirectShow Cam   : [WARN] Virtual capture card not registered yet (JCAST-E104)\n");
        }

        switch (h.ios_usb) {
            case IosUsbState::PluggedReady:
                rpt += std::string(Tu("8. iOS USB 裝置  : [PASS] 已連線且已掛載 UpperFilter=libusb0 (",
                                      "8. iOS USB Device   : [PASS] Connected with UpperFilter=libusb0 (")) +
                       std::string(UsbLinkSpeedName(usb_speed_)) + ")\n";
                break;
            case IosUsbState::PluggedNeedFilter:
                rpt += Tu("8. iOS USB 裝置  : [WARN] 已插入手機但尚未掛載 libusb0 過濾層 (JCAST-E202)\n",
                          "8. iOS USB Device   : [WARN] Device plugged in but UpperFilter=libusb0 missing (JCAST-E202)\n");
                break;
            case IosUsbState::PluggedZadigConflict:
                rpt += std::string(Tu("8. iOS USB 裝置  : [FAIL] 父複合裝置遭 ",
                                      "8. iOS USB Device   : [FAIL] Composite parent overwritten by ")) +
                       WideToUtf8Str(h.ios_service_name) +
                       Tu(" 覆蓋原廠驅動 (JCAST-E201)\n", " instead of Apple driver (JCAST-E201)\n");
                break;
            case IosUsbState::Unplugged:
                rpt += Tu("8. iOS USB 裝置  : [INFO] 目前尚未插入 iPhone / iPad USB\n",
                          "8. iOS USB Device   : [INFO] No iPhone / iPad connected via USB\n");
                break;
        }

        rpt += Tu("\n【二、偵測到的錯誤碼解析與排除步驟】\n", "\n[2. Detected Error Codes & Troubleshooting Steps]\n");
        if (active_codes.empty()) {
            rpt += Tu("• 目前未偵測到異常錯誤碼。若您在 OBS 看不到畫面，請確認先點擊本程式「智慧一鍵啟動」再開啟 OBS。\n",
                      "• No error codes detected. If OBS shows a blank screen, start capture in JCast first, then toggle the OBS source.\n");
        } else {
            for (DiagCode dc : active_codes) {
                const auto& sp = LookupDiagSpec(dc, lang_);
                rpt += "----------------------------------------------------------------------\n";
                rpt += std::string("▶ [") + sp.code_str + "] " + sp.title_utf8 + "\n";
                rpt += std::string(Tu("  • 可能原因：", "  • Root Cause: ")) + sp.cause_utf8 + "\n";
                rpt += Tu("  • 排除步驟：\n", "  • Fix Steps:\n");
                std::string steps = sp.fix_steps_utf8;
                size_t pos = 0;
                while (pos < steps.size()) {
                    size_t nl = steps.find('\n', pos);
                    std::string one = (nl == std::string::npos) ? steps.substr(pos) : steps.substr(pos, nl - pos);
                    rpt += "      " + one + "\n";
                    if (nl == std::string::npos) break;
                    pos = nl + 1;
                }
            }
            rpt += "----------------------------------------------------------------------\n";
        }

        if (!faults_.empty()) {
            rpt += Tu("\n【三、近期捕捉到的執行期異常事件紀錄】\n", "\n[3. Recent Runtime Diagnostic Events]\n");
            for (const auto& ev : faults_) {
                const auto& sp = LookupDiagSpec(ev.code, lang_);
                rpt += "• [" + WideToUtf8Str(ev.timestamp_str) + "] " + sp.code_str + " — " +
                       WideToUtf8Str(ev.detail_wide) + "\n";
            }
        }

        rpt += Tu("\n【四、即時串流健康度快照】\n", "\n[4. Live Stream Health Telemetry Snapshot]\n");
        if (telem_.active && !telem_.waiting_first_frame) {
            char tbuf[256] = {0};
            std::snprintf(
                tbuf,
                sizeof(tbuf),
                en ? "• Status: Streaming (%ux%u) | FPS: %.1f | Bitrate: %.1f Mbps (Peak %.1fM) | Pipeline: %.1f ms (Decode %.1f ms) | Engine: %s | Dropped: %u\n"
                   : "• 狀態: 串流中 (%ux%u) | FPS: %.1f | 碼率: %.1f Mbps (峰值 %.1fM) | 管線延遲: %.1f ms (純解碼 %.1f ms) | 加速: %s | 掉幀: %u\n",
                telem_.frame_width,
                telem_.frame_height,
                telem_.fps,
                telem_.bitrate_mbps,
                telem_.peak_bitrate_mbps,
                telem_.pipeline_latency_ms,
                telem_.decode_latency_ms,
                telem_.hw_accelerated ? "D3D11VA GPU" : "CPU SW",
                telem_.dropped_frames);
            rpt += tbuf;
        } else if (telem_.waiting_first_frame) {
            rpt += Tu("• 狀態: 背景擷取程序已啟動，等待手機傳送首張影格中\n",
                      "• Status: Capture host running, waiting for first video frame from iOS device\n");
        } else {
            rpt += Tu("• 狀態: 待機中（未啟動串流）\n", "• Status: Idle (not streaming)\n");
        }

        rpt += Tu("\n【五、近期執行日誌摘要（已自動隱私脫敏：隱藏本機帳號與裝置序號）】\n",
                  "\n[5. Recent Log Excerpt (Privacy-Redacted: Local User & Serial/UDID Masked)]\n");
        std::string raw_log_u8 = WideToUtf8Str(raw_log_text_);
        if (raw_log_u8.size() > 5500) {
            size_t cut = raw_log_u8.find('\n', raw_log_u8.size() - 5500);
            raw_log_u8 = "...\n" +
                         ((cut != std::string::npos) ? raw_log_u8.substr(cut + 1)
                                                     : raw_log_u8.substr(raw_log_u8.size() - 5500));
        }
        rpt += raw_log_u8;
        if (!rpt.empty() && rpt.back() != '\n') rpt.push_back('\n');

        redacted_report_utf8_ = RedactSensitiveReportText(rpt);
        redacted_report_wide_ = NormalizeLineEndingsCrlf(redacted_report_utf8_);

        if (edit_report_) {
            SetWindowTextW(edit_report_, redacted_report_wide_.c_str());
        }
    }

    void BuildDialogControls() {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        int pad = S(16);
        int inner_w = cw - pad * 2;

        lbl_headline_ = CreateWindowExW(
            0,
            L"STATIC",
            T(L"正在執行 8 項系統環境與驅動深度自檢...", L"Running 8-point system and driver self-check..."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad,
            S(12),
            inner_w,
            S(26),
            hwnd_,
            nullptr,
            hinst_,
            nullptr);
        SendMessageW(lbl_headline_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        HWND lbl_sub = CreateWindowExW(
            0,
            L"STATIC",
            T(L"🔒 隱私保護：下方診斷報告已自動隱藏您的電腦使用者帳號與手機序號，可安心複製或匯出回報。",
              L"🔒 Privacy Safe: Windows usernames and iPhone/iPad serial/UDIDs are automatically redacted below."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad,
            S(38),
            inner_w,
            S(20),
            hwnd_,
            nullptr,
            hinst_,
            nullptr);
        SendMessageW(lbl_sub, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        int edit_top = S(64);
        int btn_h = S(34);
        int btn_y = ch - pad - btn_h;
        int status_y = btn_y - S(26);
        int edit_h = std::max(S(140), status_y - S(6) - edit_top);

        edit_report_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            pad,
            edit_top,
            inner_w,
            edit_h,
            hwnd_,
            nullptr,
            hinst_,
            nullptr);
        SendMessageW(edit_report_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
        SendMessageW(edit_report_, EM_SETLIMITTEXT, 256 * 1024, 0);

        lbl_feedback_ = CreateWindowExW(
            0,
            L"STATIC",
            T(L"提示：遇到問題時可先點「🔧 一鍵修復驅動」，或點「🐞 開啟 GitHub 回報問題」一鍵提交報錯單。",
              L"Tip: Try 'Repair Driver' first, or click 'Report on GitHub' to submit a pre-filled bug report."),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            pad,
            status_y,
            inner_w,
            S(22),
            hwnd_,
            nullptr,
            hinst_,
            nullptr);
        SendMessageW(lbl_feedback_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        int gap = S(8);
        int avail = inner_w - gap * 4;
        int w_rep = avail * 21 / 100;
        int w_cpy = avail * 21 / 100;
        int w_sav = avail * 20 / 100;
        int w_gh  = avail * 25 / 100;
        int w_cls = avail - w_rep - w_cpy - w_sav - w_gh;
        int x = pad;

        HWND b1 = CreateWindowExW(
            0, L"BUTTON", T(L"🔧 一鍵修復驅動", L"🔧 Repair Driver"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, btn_y, w_rep, btn_h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRepair)), hinst_, nullptr);
        SendMessageW(b1, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        x += w_rep + gap;

        HWND b2 = CreateWindowExW(
            0, L"BUTTON", T(L"📋 複製脫敏報告", L"📋 Copy Report"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, btn_y, w_cpy, btn_h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnCopy)), hinst_, nullptr);
        SendMessageW(b2, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        x += w_cpy + gap;

        HWND b3 = CreateWindowExW(
            0, L"BUTTON", T(L"💾 匯出報錯檔 (.txt)", L"💾 Export (.txt)"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, btn_y, w_sav, btn_h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnSaveTxt)), hinst_, nullptr);
        SendMessageW(b3, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        x += w_sav + gap;

        HWND b4 = CreateWindowExW(
            0, L"BUTTON", T(L"🐞 開啟 GitHub 回報問題", L"🐞 Report on GitHub"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            x, btn_y, w_gh, btn_h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnGitHubIssue)), hinst_, nullptr);
        SendMessageW(b4, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);
        x += w_gh + gap;

        HWND b5 = CreateWindowExW(
            0, L"BUTTON", T(L"關閉", L"Close"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, btn_y, w_cls, btn_h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnClose)), hinst_, nullptr);
        SendMessageW(b5, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        RebuildReportAndRefreshUi();
    }

    void ExportReportToTxt() {
        std::filesystem::path diag_dir = GetDefaultScreenshotDirectory(app_dir_) / L"Diagnostics";
        std::error_code ec;
        std::filesystem::create_directories(diag_dir, ec);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t fname[96] = {0};
        std::swprintf(
            fname,
            96,
            L"JCast_DiagReport_%04u%02u%02u_%02u%02u%02u.txt",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond);
        std::filesystem::path out_path = diag_dir / fname;

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            SetWindowTextW(
                lbl_feedback_,
                T(L"❌ 無法寫入診斷檔案，請改用「📋 複製脫敏報告」。",
                  L"❌ Could not write report file. Please use 'Copy Report' instead."));
            return;
        }
        const char kBom[3] = {'\xEF', '\xBB', '\xBF'};
        ofs.write(kBom, 3);
        ofs.write(redacted_report_utf8_.data(), static_cast<std::streamsize>(redacted_report_utf8_.size()));
        ofs.close();

        std::wstring msg = std::wstring(T(L"✅ 已儲存診斷報錯檔：", L"✅ Saved diagnostic report: ")) + out_path.wstring();
        SetWindowTextW(lbl_feedback_, msg.c_str());
        ShellExecuteW(hwnd_, L"open", diag_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        DiagnosticsModalWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<DiagnosticsModalWindow*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<DiagnosticsModalWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->WndProc(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE:
                BuildDialogControls();
                return 0;

            case WM_CTLCOLORSTATIC: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                HWND ctrl = reinterpret_cast<HWND>(lParam);
                if (ctrl != edit_report_) {
                    SetBkMode(hdc, TRANSPARENT);
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
                }
                break;
            }

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                if (HIWORD(wParam) == BN_CLICKED) {
                    if (id == kBtnRepair) {
                        if (on_repair_driver_ && on_repair_driver_()) {
                            if (on_clear_faults_) on_clear_faults_();
                            faults_.clear();
                            RebuildReportAndRefreshUi();
                            SetWindowTextW(
                                lbl_feedback_,
                                T(L"✅ 已完成驅動與虛擬擷取卡修復，並重新整理自檢結果！",
                                  L"✅ Driver & virtual capture card repaired. Self-check refreshed!"));
                        }
                        return 0;
                    }
                    if (id == kBtnCopy) {
                        if (CopyWideTextToClipboard(hwnd_, redacted_report_wide_)) {
                            SetWindowTextW(
                                lbl_feedback_,
                                T(L"✅ 已將完整脫敏診斷報告複製到剪貼簿，可直接 Ctrl+V 貼上！",
                                  L"✅ Redacted diagnostic report copied to clipboard (ready to Ctrl+V)!"));
                        }
                        return 0;
                    }
                    if (id == kBtnSaveTxt) {
                        ExportReportToTxt();
                        return 0;
                    }
                    if (id == kBtnGitHubIssue) {
                        CopyWideTextToClipboard(hwnd_, redacted_report_wide_);
                        std::string issue_url = BuildGitHubNewIssueUrl(
                            IOS_VCAM_GITHUB_OWNER,
                            IOS_VCAM_GITHUB_REPO,
                            IOS_VCAM_VER_DISPLAY_STR,
                            primary_code_,
                            faults_.empty() ? "" : WideToUtf8Str(faults_.back().detail_wide),
                            lang_);
                        OpenSafeGitHubWebUrl(hwnd_, issue_url);
                        SetWindowTextW(
                            lbl_feedback_,
                            T(L"✅ 已複製診斷報告並開啟 GitHub Issue 頁面，請於網頁內文區塊按 Ctrl+V 貼上提交！",
                              L"✅ Report copied & GitHub Issue opened! Press Ctrl+V in the issue box to paste."));
                        return 0;
                    }
                    if (id == kBtnClose || id == IDCANCEL) {
                        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                        return 0;
                    }
                }
                break;
            }

            case WM_CLOSE:
                modal_done_ = true;
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    HINSTANCE hinst_ = nullptr;
    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    std::filesystem::path app_dir_;
    UINT dpi_ = 96;
    AppLanguage lang_ = AppLanguage::ZhTW;
    HFONT font_normal_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_mono_ = nullptr;
    HICON icon_small_ = nullptr;

    UsbLinkSpeed usb_speed_ = UsbLinkSpeed::Unknown;
    StreamHealthSnapshot telem_{};
    std::vector<DiagEventRecord> faults_;
    std::wstring raw_log_text_;
    std::function<SystemHealth()> query_health_;
    std::function<bool()> on_repair_driver_;
    std::function<void()> on_clear_faults_;

    DiagCode primary_code_ = DiagCode::None;
    std::string redacted_report_utf8_;
    std::wstring redacted_report_wide_;

    HWND lbl_headline_ = nullptr;
    HWND edit_report_ = nullptr;
    HWND lbl_feedback_ = nullptr;
    bool modal_done_ = false;
};

std::wstring FormatBytesCompactWide(uint64_t bytes) {
    wchar_t buf[64] = {0};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::swprintf(buf, 64, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::swprintf(buf, 64, L"%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::swprintf(buf, 64, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

std::wstring FormatLiveMonitorHudBadge(const StreamHealthSnapshot& s, AppLanguage lang) {
    if (!s.active || s.waiting_first_frame) {
        return L"";
    }
    const bool en = (lang == AppLanguage::EnUS);
    const wchar_t* grade_lbl = en ? L"🟢 Optimal" : L"🟢 極佳";
    if (s.grade == StreamHealthGrade::StaticVfr) {
        grade_lbl = en ? L"🔵 Static VFR" : L"🔵 靜態VFR";
    } else if (s.grade == StreamHealthGrade::Warning) {
        grade_lbl = en ? L"🟡 High Load" : L"🟡 負載偏高";
    }
    const wchar_t* dec_lbl = s.mock_source
                                 ? (en ? L"Mock" : L"自測")
                                 : (s.hw_accelerated ? L"D3D11VA" : (en ? L"CPU SW" : L"CPU軟解"));
    wchar_t buf[192] = {0};
    std::swprintf(
        buf,
        192,
        en ? L"%ls | %.1f FPS | %.1f Mbps | Latency %.1f ms (%ls) | %u×%u"
           : L"%ls ｜ %.1f FPS ｜ %.1f Mbps ｜ 延遲 %.1f ms (%ls) ｜ %u×%u",
        grade_lbl,
        s.fps,
        s.bitrate_mbps,
        s.pipeline_latency_ms,
        dec_lbl,
        s.frame_width,
        s.frame_height);
    return buf;
}

void DrawSparklineGdi(
    HDC hdc,
    const RECT& rc,
    const std::vector<double>& history,
    double max_scale,
    COLORREF stroke_color) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 8 || h < 6) return;

    HBRUSH box_bg = CreateSolidBrush(RGB(11, 15, 22));
    HBRUSH box_bd = CreateSolidBrush(RGB(34, 44, 62));
    FillRect(hdc, &rc, box_bg);
    FrameRect(hdc, &rc, box_bd);
    DeleteObject(box_bg);
    DeleteObject(box_bd);

    if (history.size() < 2 || max_scale <= 0.001) return;

    HPEN pen = CreatePen(PS_SOLID, 1, stroke_color);
    HGDIOBJ old_pen = SelectObject(hdc, pen);

    const size_t n = history.size();
    const int inner_w = w - 4;
    const int inner_h = h - 4;
    for (size_t i = 0; i < n; ++i) {
        double norm = std::clamp(history[i] / max_scale, 0.0, 1.0);
        int px = rc.left + 2 + static_cast<int>((static_cast<double>(i) * inner_w) / static_cast<double>(n - 1));
        int py = rc.bottom - 2 - static_cast<int>(norm * inner_h + 0.5);
        if (i == 0) {
            MoveToEx(hdc, px, py, nullptr);
        } else {
            LineTo(hdc, px, py);
        }
    }

    SelectObject(hdc, old_pen);
    DeleteObject(pen);
}

void DrawPreviewCanvasGdi(
    HDC dst_hdc,
    const RECT& target_rc,
    bool has_frame,
    const std::vector<uint8_t>& bgra_topdown,
    uint32_t fit_w,
    uint32_t fit_h,
    bool draw_border,
    const std::wstring& badge_text,
    bool badge_is_toast,
    const std::wstring& idle_title,
    const std::wstring& idle_subtitle,
    HFONT font_bold,
    HFONT font_normal) {
    int w = target_rc.right - target_rc.left;
    int h = target_rc.bottom - target_rc.top;
    if (w <= 4 || h <= 4) return;

    HDC mem_dc = CreateCompatibleDC(dst_hdc);
    HBITMAP mem_bmp = CreateCompatibleBitmap(dst_hdc, w, h);
    HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

    RECT local_rc{0, 0, w, h};
    HBRUSH bg_brush = CreateSolidBrush(has_frame ? RGB(0, 0, 0) : RGB(18, 21, 28));
    FillRect(mem_dc, &local_rc, bg_brush);
    DeleteObject(bg_brush);

    if (has_frame && fit_w > 0 && fit_h > 0 &&
        bgra_topdown.size() >= static_cast<size_t>(fit_w) * fit_h * 4ULL) {
        int off_x = (w - static_cast<int>(fit_w)) / 2;
        int off_y = (h - static_cast<int>(fit_h)) / 2;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(fit_w);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(fit_h);  // Top-down BGRA32
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(
            mem_dc,
            off_x,
            off_y,
            static_cast<int>(fit_w),
            static_cast<int>(fit_h),
            0,
            0,
            static_cast<int>(fit_w),
            static_cast<int>(fit_h),
            bgra_topdown.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);
    } else {
        SetBkMode(mem_dc, TRANSPARENT);
        HGDIOBJ old_font = SelectObject(mem_dc, font_bold);
        SetTextColor(mem_dc, RGB(205, 214, 230));
        RECT t1{12, h / 2 - 26, w - 12, h / 2};
        DrawTextW(mem_dc, idle_title.c_str(), -1, &t1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mem_dc, font_normal);
        SetTextColor(mem_dc, RGB(128, 140, 162));
        RECT t2{12, h / 2 + 2, w - 12, h / 2 + 26};
        DrawTextW(mem_dc, idle_subtitle.c_str(), -1, &t2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem_dc, old_font);
    }

    if (!badge_text.empty()) {
        SetBkMode(mem_dc, TRANSPARENT);
        HGDIOBJ old_f = SelectObject(mem_dc, font_normal);
        RECT calc_rc{0, 0, w - 24, 32};
        DrawTextW(mem_dc, badge_text.c_str(), -1, &calc_rc, DT_CALCRECT | DT_SINGLELINE);
        int pill_w = std::min(w - 16, static_cast<int>(calc_rc.right - calc_rc.left) + 18);
        int pill_h = static_cast<int>(calc_rc.bottom - calc_rc.top) + 8;
        RECT pill_rc{8, 8, 8 + pill_w, 8 + pill_h};

        HBRUSH pill_bg = CreateSolidBrush(badge_is_toast ? RGB(18, 68, 40) : RGB(20, 24, 32));
        HBRUSH pill_bd = CreateSolidBrush(badge_is_toast ? RGB(54, 206, 118) : RGB(68, 142, 236));
        FillRect(mem_dc, &pill_rc, pill_bg);
        FrameRect(mem_dc, &pill_rc, pill_bd);
        DeleteObject(pill_bg);
        DeleteObject(pill_bd);

        SetTextColor(mem_dc, badge_is_toast ? RGB(220, 255, 232) : RGB(236, 242, 252));
        DrawTextW(mem_dc, badge_text.c_str(), -1, &pill_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem_dc, old_f);
    }

    if (draw_border) {
        HBRUSH border_br = CreateSolidBrush(RGB(62, 72, 92));
        FrameRect(mem_dc, &local_rc, border_br);
        DeleteObject(border_br);
    }

    BitBlt(dst_hdc, target_rc.left, target_rc.top, w, h, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
}

class LiveMonitorWindow {
public:
    LiveMonitorWindow(
        HINSTANCE hinst,
        HICON icon_big,
        HICON icon_small,
        HFONT font_bold,
        HFONT font_normal,
        AppLanguage lang,
        std::function<void(HWND)> on_screenshot,
        std::function<void()> on_open_folder)
        : hinst_(hinst),
          icon_big_(icon_big),
          icon_small_(icon_small),
          font_bold_(font_bold),
          font_normal_(font_normal),
          lang_(lang),
          on_screenshot_(std::move(on_screenshot)),
          on_open_folder_(std::move(on_open_folder)) {}

    ~LiveMonitorWindow() {
        CloseMonitor();
        if (dark_bg_brush_) {
            DeleteObject(dark_bg_brush_);
            dark_bg_brush_ = nullptr;
        }
    }

    HWND Hwnd() const { return hwnd_; }
    bool IsOpen() const { return hwnd_ != nullptr && IsWindow(hwnd_); }

    void SetLanguage(AppLanguage lang) {
        lang_ = lang;
        if (IsOpen()) {
            last_title_.clear();
            UpdateTitleBar();
            if (show_hud_) {
                hud_text_ = FormatLiveMonitorHudBadge(hud_snap_, lang_);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void ShowToast(const std::wstring& text, uint64_t duration_ms = 2200) {
        toast_text_ = text;
        toast_until_ms_ = GetTickCount64() + duration_ms;
        if (IsOpen()) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void ShowOrActivate(UINT dpi) {
        if (IsOpen()) {
            if (IsIconic(hwnd_)) {
                ShowWindow(hwnd_, SW_RESTORE);
            }
            SetForegroundWindow(hwnd_);
            return;
        }

        const wchar_t* kCls = L"JCastLiveMonitorWndClass";
        if (!dark_bg_brush_) {
            dark_bg_brush_ = CreateSolidBrush(RGB(10, 12, 16));
        }
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = &LiveMonitorWindow::StaticWndProc;
            wc.hInstance = hinst_;
            wc.hIcon = icon_big_ ? icon_big_ : LoadIconW(nullptr, IDI_APPLICATION);
            wc.hIconSm = icon_small_ ? icon_small_ : wc.hIcon;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = dark_bg_brush_;
            wc.lpszClassName = kCls;
            RegisterClassExW(&wc);
            registered = true;
        }

        uint32_t adv_w = 1920, adv_h = 1080;
        if (!shm_.IsOpen()) shm_.Open(kDefaultShmName);
        shm_.QueryAdvertisedDimensions(adv_w, adv_h);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int max_w = (work.right - work.left) * 82 / 100;
        int max_h = (work.bottom - work.top) * 82 / 100;

        int client_w = MulDiv(1120, static_cast<int>(dpi), 96);
        int client_h = MulDiv(630, static_cast<int>(dpi), 96);
        if (adv_h > adv_w && adv_h > 0) {
            client_h = std::min(max_h, MulDiv(860, static_cast<int>(dpi), 96));
            client_w = std::max(320, static_cast<int>(static_cast<int64_t>(client_h) * adv_w / adv_h));
        } else if (adv_w > 0 && adv_h > 0) {
            client_w = std::min(max_w, MulDiv(1160, static_cast<int>(dpi), 96));
            client_h = std::max(280, static_cast<int>(static_cast<int64_t>(client_w) * adv_h / adv_w));
        }

        RECT adj{0, 0, client_w, client_h};
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        AdjustWindowRectEx(&adj, style, FALSE, 0);
        int win_w = std::min(max_w, static_cast<int>(adj.right - adj.left));
        int win_h = std::min(max_h, static_cast<int>(adj.bottom - adj.top));
        int pos_x = work.left + ((work.right - work.left) - win_w) / 2;
        int pos_y = work.top + ((work.bottom - work.top) - win_h) / 2;

        fullscreen_ = false;
        topmost_ = false;
        last_seq_ = 0;
        force_refresh_ = true;

        hwnd_ = CreateWindowExW(
            0,
            kCls,
            T(L"JCast Live Monitor — 獨立外接螢幕視窗 [雙擊 / F11: 全螢幕 ｜ F12: 原畫質截圖 ｜ 右鍵: 選單]",
              L"JCast Live Monitor — Standalone Display [DblClick / F11: Fullscreen | F12: Screenshot | Right-Click: Menu]"),
            style,
            pos_x,
            pos_y,
            win_w,
            win_h,
            nullptr,
            nullptr,
            hinst_,
            this);
        if (!hwnd_) return;

        if (icon_big_) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big_));
        if (icon_small_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));

        SetTimer(hwnd_, kTimerId, 16, nullptr);
        TickFrame(true);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
    }

    void CloseMonitor() {
        HWND w = hwnd_;
        hwnd_ = nullptr;
        if (w && IsWindow(w)) {
            KillTimer(w, kTimerId);
            DestroyWindow(w);
        }
        shm_.Close();
        frame_bgra_.clear();
        has_live_frame_ = false;
    }

private:
    static constexpr UINT_PTR kTimerId = 1;
    static constexpr int kCmdShot = 3001;
    static constexpr int kCmdOpenDir = 3002;
    static constexpr int kCmdFullscreen = 3003;
    static constexpr int kCmdTopmost = 3004;
    static constexpr int kCmdFitAspect = 3005;
    static constexpr int kCmdClose = 3006;
    static constexpr int kCmdToggleHud = 3007;

    const wchar_t* T(const wchar_t* zh, const wchar_t* en) const { return LText(lang_, zh, en); }

    void UpdateTitleBar() {
        if (!hwnd_ || fullscreen_) return;
        std::wstring t = L"JCast Live Monitor";
        if (has_live_frame_ && src_w_ > 0 && src_h_ > 0) {
            t += L" (" + std::to_wstring(src_w_) + L"×" + std::to_wstring(src_h_) + L")";
        }
        if (topmost_) t += T(L" [📌置頂]", L" [📌On Top]");
        t += T(L" — F11: 全螢幕 ｜ Tab/F3: 健康度HUD ｜ F12: 原畫質截圖 ｜ 右鍵: 選單",
               L" — F11: Fullscreen | Tab/F3: Health HUD | F12: Screenshot | Right-Click: Menu");
        if (t != last_title_) {
            last_title_ = t;
            SetWindowTextW(hwnd_, t.c_str());
        }
    }

    void ToggleFullscreen() {
        if (!hwnd_) return;
        if (!fullscreen_) {
            wp_prev_.length = sizeof(wp_prev_);
            GetWindowPlacement(hwnd_, &wp_prev_);
            style_prev_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
                SetWindowLongPtrW(hwnd_, GWL_STYLE, style_prev_ & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(
                    hwnd_,
                    topmost_ ? HWND_TOPMOST : HWND_TOP,
                    mi.rcMonitor.left,
                    mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left,
                    mi.rcMonitor.bottom - mi.rcMonitor.top,
                    SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                fullscreen_ = true;
                force_refresh_ = true;
                ShowToast(
                    T(L"⛶ 已進入無邊框全螢幕（按 Esc 退出｜按 Tab 切換健康度 HUD）",
                      L"⛶ Borderless Fullscreen (Esc to exit | Tab to toggle Health HUD)"),
                    2000);
            }
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style_prev_);
            SetWindowPlacement(hwnd_, &wp_prev_);
            SetWindowPos(
                hwnd_,
                topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = false;
            force_refresh_ = true;
            last_title_.clear();
            UpdateTitleBar();
        }
    }

    void ToggleTopmost() {
        if (!hwnd_) return;
        topmost_ = !topmost_;
        SetWindowPos(
            hwnd_,
            topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        UpdateTitleBar();
        ShowToast(
            topmost_ ? T(L"📌 已開啟視窗永遠置頂", L"📌 Always on Top enabled")
                     : T(L"📌 已取消視窗置頂", L"📌 Always on Top disabled"),
            1600);
    }

    void ToggleHud() {
        show_hud_ = !show_hud_;
        if (show_hud_) {
            shm_.ReadStreamHealth(hud_snap_);
            hud_text_ = FormatLiveMonitorHudBadge(hud_snap_, lang_);
            if (hud_text_.empty()) {
                ShowToast(
                    T(L"📊 已開啟串流健康度 HUD（待投影訊號開始即顯示 FPS / Mbps / ms）",
                      L"📊 Health HUD enabled (FPS / Mbps / ms will appear when live)"),
                    1800);
            } else {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else {
            hud_text_.clear();
            ShowToast(
                T(L"📊 已隱藏串流健康度 HUD（按 Tab 或 F3 可重新顯示）",
                  L"📊 Health HUD hidden (press Tab or F3 to show)"),
                1600);
        }
    }

    void FitWindowToSourceAspect() {
        if (!hwnd_ || src_w_ == 0 || src_h_ == 0) return;
        if (fullscreen_) ToggleFullscreen();

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int cur_cw = std::max(240, static_cast<int>(rc.right - rc.left));
        int target_cw = cur_cw;
        int target_ch = static_cast<int>((static_cast<int64_t>(target_cw) * src_h_ + src_w_ / 2) / src_w_);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            int max_h = (mi.rcWork.bottom - mi.rcWork.top) * 86 / 100;
            if (target_ch > max_h) {
                target_ch = max_h;
                target_cw = static_cast<int>((static_cast<int64_t>(target_ch) * src_w_ + src_h_ / 2) / src_h_);
            }
        }

        DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        RECT adj{0, 0, target_cw, target_ch};
        AdjustWindowRectEx(&adj, style, FALSE, ex_style);
        SetWindowPos(
            hwnd_,
            nullptr,
            0,
            0,
            adj.right - adj.left,
            adj.bottom - adj.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        force_refresh_ = true;
    }

    void ShowContextMenu(int screen_x, int screen_y) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(
            menu,
            MF_STRING,
            kCmdShot,
            T(L"📸 一鍵原畫質無損截圖 (PNG + 複製剪貼簿)\tF12 / Ctrl+S",
              L"📸 Lossless 1:1 Screenshot (PNG + Clipboard)\tF12 / Ctrl+S"));
        AppendMenuW(
            menu,
            MF_STRING,
            kCmdOpenDir,
            T(L"📂 開啟截圖儲存資料夾", L"📂 Open Screenshots Folder"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            menu,
            MF_STRING | (show_hud_ ? MF_CHECKED : 0),
            kCmdToggleHud,
            T(L"📊 顯示即時串流健康度 HUD (FPS / Mbps / 延遲 ms)\tTab / F3",
              L"📊 Show Stream Health HUD (FPS / Mbps / Latency)\tTab / F3"));
        AppendMenuW(
            menu,
            MF_STRING | (fullscreen_ ? MF_CHECKED : 0),
            kCmdFullscreen,
            T(L"⛶ 無邊框全螢幕模式\tF11 / 雙擊", L"⛶ Borderless Fullscreen\tF11 / DblClick"));
        AppendMenuW(
            menu,
            MF_STRING | (topmost_ ? MF_CHECKED : 0),
            kCmdTopmost,
            T(L"📌 視窗永遠置頂 (Always on Top)\tCtrl+T", L"📌 Always on Top\tCtrl+T"));
        AppendMenuW(
            menu,
            MF_STRING | ((has_live_frame_ && src_w_ > 0) ? 0 : MF_GRAYED),
            kCmdFitAspect,
            T(L"📐 依手機原生比例自動貼合視窗大小", L"📐 Fit Window to Device Aspect Ratio"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdClose, T(L"關閉獨立預覽視窗", L"Close Live Monitor"));

        if (screen_x == -1 && screen_y == -1) {
            POINT pt{40, 40};
            ClientToScreen(hwnd_, &pt);
            screen_x = pt.x;
            screen_y = pt.y;
        }
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen_x, screen_y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void TickFrame(bool force) {
        if (!hwnd_ || IsIconic(hwnd_)) return;

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        uint32_t cw = static_cast<uint32_t>(std::max<LONG>(2, rc.right - rc.left));
        uint32_t ch = static_cast<uint32_t>(std::max<LONG>(2, rc.bottom - rc.top));

        if (!shm_.IsOpen()) {
            shm_.Open(kDefaultShmName);
        }

        bool updated = false;
        uint32_t fit_w = 0, fit_h = 0, sw = 0, sh = 0;
        bool alive = shm_.ReadLatestPreviewBGRA32(
            cw,
            ch,
            force || force_refresh_,
            last_seq_,
            frame_bgra_,
            fit_w,
            fit_h,
            sw,
            sh,
            &updated);
        force_refresh_ = false;

        uint64_t now_tick = GetTickCount64();
        bool toast_expired = (!toast_text_.empty() && now_tick >= toast_until_ms_);
        if (toast_expired) {
            toast_text_.clear();
        }

        bool hud_changed = false;
        if (show_hud_ && (force || now_tick >= next_hud_sample_ms_)) {
            next_hud_sample_ms_ = now_tick + 450ULL;
            shm_.ReadStreamHealth(hud_snap_);
            std::wstring next_hud = FormatLiveMonitorHudBadge(hud_snap_, lang_);
            if (next_hud != hud_text_) {
                hud_text_ = std::move(next_hud);
                hud_changed = true;
            }
        }

        if (alive) {
            bool dim_changed = (!has_live_frame_ || sw != src_w_ || sh != src_h_);
            has_live_frame_ = true;
            fit_w_ = fit_w;
            fit_h_ = fit_h;
            src_w_ = sw;
            src_h_ = sh;
            if (dim_changed) {
                UpdateTitleBar();
            }
            if (updated || dim_changed || toast_expired || hud_changed) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else if (has_live_frame_ || toast_expired || hud_changed) {
            has_live_frame_ = false;
            last_seq_ = 0;
            hud_text_.clear();
            UpdateTitleBar();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        LiveMonitorWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<LiveMonitorWindow*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<LiveMonitorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->WndProc(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
                force_refresh_ = true;
                TickFrame(true);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;

            case WM_TIMER:
                if (wParam == kTimerId) {
                    TickFrame(false);
                }
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd_, &ps);
                RECT rc{};
                GetClientRect(hwnd_, &rc);
                std::wstring badge;
                bool is_toast = false;
                if (!toast_text_.empty() && GetTickCount64() < toast_until_ms_) {
                    badge = toast_text_;
                    is_toast = true;
                } else if (show_hud_ && has_live_frame_ && !hud_text_.empty()) {
                    badge = hud_text_;
                }
                DrawPreviewCanvasGdi(
                    hdc,
                    rc,
                    has_live_frame_,
                    frame_bgra_,
                    fit_w_,
                    fit_h_,
                    false,
                    badge,
                    is_toast,
                    T(L"📺 等待 iPhone / iPad 投影訊號中...", L"📺 Waiting for iPhone / iPad Video Signal..."),
                    T(L"於主控制台點擊「🚀 智慧一鍵啟動」後即顯示高畫質畫面（F11 全螢幕｜Tab 健康度 HUD｜F12 原畫質截圖）",
                      L"Click 'Smart Start' in the main window to begin (F11 Fullscreen | Tab Health HUD | F12 Screenshot)"),
                    font_bold_,
                    font_normal_);
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_LBUTTONDBLCLK:
                ToggleFullscreen();
                return 0;

            case WM_CONTEXTMENU:
                ShowContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;

            case WM_SYSKEYDOWN:
                if (wParam == VK_RETURN) {
                    ToggleFullscreen();
                    return 0;
                }
                break;

            case WM_KEYDOWN: {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (wParam == VK_F11) {
                    ToggleFullscreen();
                    return 0;
                }
                if (wParam == VK_ESCAPE && fullscreen_) {
                    ToggleFullscreen();
                    return 0;
                }
                if (wParam == VK_TAB || wParam == VK_F3) {
                    ToggleHud();
                    return 0;
                }
                if (wParam == VK_F12 || (ctrl && wParam == 'S')) {
                    if (on_screenshot_) on_screenshot_(hwnd_);
                    return 0;
                }
                if (ctrl && wParam == 'T') {
                    ToggleTopmost();
                    return 0;
                }
                break;
            }

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                if (id == kCmdShot) {
                    if (on_screenshot_) on_screenshot_(hwnd_);
                    return 0;
                }
                if (id == kCmdOpenDir) {
                    if (on_open_folder_) on_open_folder_();
                    return 0;
                }
                if (id == kCmdToggleHud) {
                    ToggleHud();
                    return 0;
                }
                if (id == kCmdFullscreen) {
                    ToggleFullscreen();
                    return 0;
                }
                if (id == kCmdTopmost) {
                    ToggleTopmost();
                    return 0;
                }
                if (id == kCmdFitAspect) {
                    FitWindowToSourceAspect();
                    return 0;
                }
                if (id == kCmdClose) {
                    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    return 0;
                }
                break;
            }

            case WM_CLOSE:
                CloseMonitor();
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_big_ = nullptr;
    HICON icon_small_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_normal_ = nullptr;
    AppLanguage lang_ = AppLanguage::ZhTW;
    HBRUSH dark_bg_brush_ = nullptr;

    std::function<void(HWND)> on_screenshot_;
    std::function<void()> on_open_folder_;

    SharedCaptureConsumer shm_;
    uint64_t last_seq_ = 0;
    bool force_refresh_ = true;
    bool has_live_frame_ = false;
    uint32_t fit_w_ = 0;
    uint32_t fit_h_ = 0;
    uint32_t src_w_ = 0;
    uint32_t src_h_ = 0;
    std::vector<uint8_t> frame_bgra_;

    bool fullscreen_ = false;
    bool topmost_ = false;
    bool show_hud_ = true;
    uint64_t next_hud_sample_ms_ = 0;
    StreamHealthSnapshot hud_snap_{};
    std::wstring hud_text_;
    DWORD style_prev_ = 0;
    WINDOWPLACEMENT wp_prev_{};
    std::wstring last_title_;

    std::wstring toast_text_;
    uint64_t toast_until_ms_ = 0;
};

class ControlCenterWindow {
public:
    HWND Hwnd() const { return hwnd_; }
    HWND LiveMonitorHwnd() const { return live_monitor_ ? live_monitor_->Hwnd() : nullptr; }
    void TriggerScreenshotFromHotkey() { TakeFullResolutionScreenshot(hwnd_); }

    bool Create(HINSTANCE hInst, int nCmdShow) {
        hinst_ = hInst;
        base_dir_ = AppDir();
        UnblockDownloadedFiles(base_dir_);
        CleanupOldPortableUpdateFiles(base_dir_);

        lang_pref_ = LoadSavedLanguagePref();
        lang_ = ResolveEffectiveLanguage(lang_pref_, static_cast<uint16_t>(GetUserDefaultUILanguage()));

        UINT dpi = GetSystemDpiSafe(nullptr);
        icon_big_ = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_MAIN_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        icon_small_ = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_MAIN_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        icon_banner_ = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_MAIN_ICON), IMAGE_ICON,
            Scale(64, dpi), Scale(64, dpi), LR_DEFAULTCOLOR));

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &ControlCenterWindow::StaticWndProc;
        wc.hInstance = hInst;
        wc.hIcon = icon_big_ ? icon_big_ : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = icon_small_ ? icon_small_ : wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"JCastForIosGuiWndClass";
        RegisterClassExW(&wc);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int work_w = std::max<int>(640, work.right - work.left);
        int work_h = std::max<int>(560, work.bottom - work.top);
        int win_w = std::min<int>(Scale(820, dpi), work_w);
        int win_h = std::min<int>(Scale(760, dpi), work_h);
        int pos_x = work.left + (work_w - win_w) / 2;
        int pos_y = std::max<int>(work.top, work.top + (work_h - win_h) / 2);

        hwnd_ = CreateWindowExW(
            0,
            wc.lpszClassName,
            T(L"JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L" — 智慧擷取控制台 (故障偵測報錯 × 健康度儀表板 × 即時預覽)",
              L"JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L" — Smart Capture Console (Diagnostics × Health Telemetry × Live Preview)"),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            pos_x, pos_y, win_w, win_h,
            nullptr, nullptr, hInst, this);
        if (!hwnd_) return false;

        if (icon_big_) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big_));
        if (icon_small_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));

        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
        return true;
    }

private:
    const wchar_t* T(const wchar_t* zh, const wchar_t* en) const {
        return LText(lang_, zh, en);
    }

    static UINT GetSystemDpiSafe(HWND hwnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        using GetDpiForSystemFn = UINT(WINAPI*)();
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            if (hwnd) {
                auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(u32, "GetDpiForWindow"));
                if (fn) {
                    UINT d = fn(hwnd);
                    if (d > 0) return d;
                }
            }
            auto sys_fn = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(u32, "GetDpiForSystem"));
            if (sys_fn) {
                UINT d = sys_fn();
                if (d > 0) return d;
            }
        }
        HDC hdc = GetDC(nullptr);
        UINT d = hdc ? static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX)) : 96U;
        if (hdc) ReleaseDC(nullptr, hdc);
        return d > 0 ? d : 96U;
    }

    static int Scale(int px, UINT dpi) {
        return MulDiv(px, static_cast<int>(dpi), 96);
    }

    void CreateFonts(UINT dpi) {
        if (font_normal_) DeleteObject(font_normal_);
        if (font_bold_) DeleteObject(font_bold_);
        if (font_title_) DeleteObject(font_title_);
        if (font_mono_) DeleteObject(font_mono_);
        if (font_metric_) DeleteObject(font_metric_);
        if (font_small_) DeleteObject(font_small_);

        auto make_font = [&](int pt_tenths, int weight, const wchar_t* face) {
            int height = -MulDiv(pt_tenths, static_cast<int>(dpi), 720);
            return CreateFontW(
                height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        };

        font_normal_ = make_font(98, FW_NORMAL, L"Microsoft JhengHei UI");
        font_bold_ = make_font(105, FW_SEMIBOLD, L"Microsoft JhengHei UI");
        font_title_ = make_font(125, FW_BOLD, L"Microsoft JhengHei UI");
        font_mono_ = make_font(92, FW_NORMAL, L"Consolas");
        font_metric_ = make_font(132, FW_BOLD, L"Microsoft JhengHei UI");
        font_small_ = make_font(85, FW_NORMAL, L"Microsoft JhengHei UI");
    }

    void PopulateLanguageCombo() {
        if (!combo_language_) return;
        SendMessageW(combo_language_, CB_RESETCONTENT, 0, 0);
        SendMessageW(
            combo_language_,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(T(L"🌐 自動 (跟隨系統)", L"🌐 Auto (System)")));
        SendMessageW(combo_language_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"繁體中文"));
        SendMessageW(combo_language_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        int sel = 0;
        if (lang_pref_ == AppLanguagePref::ZhTW) sel = 1;
        else if (lang_pref_ == AppLanguagePref::EnUS) sel = 2;
        SendMessageW(combo_language_, CB_SETCURSEL, sel, 0);
    }

    void PopulateStatusBarCombo(bool preserve_selection) {
        if (!combo_status_bar_) return;
        LRESULT prev_sel = preserve_selection ? SendMessageW(combo_status_bar_, CB_GETCURSEL, 0, 0) : 0;
        if (prev_sel < 0 || prev_sel > 1) prev_sel = 0;
        SendMessageW(combo_status_bar_, CB_RESETCONTENT, 0, 0);
        SendMessageW(
            combo_status_bar_,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(T(
                L"乾淨直播狀態列（推薦：時間固定 9:41、滿格電量與訊號）",
                L"Clean Streaming Status Bar (Recommended: 9:41, Full Battery & Signal)")));
        SendMessageW(
            combo_status_bar_,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(T(
                L"保留手機真實狀態列（顯示真實時間、電量與通知圖示）",
                L"Keep Native Device Status Bar (Real Time, Battery & Notifications)")));
        SendMessageW(combo_status_bar_, CB_SETCURSEL, static_cast<WPARAM>(prev_sel), 0);
    }

    void BuildControls() {
        UINT dpi = GetSystemDpiSafe(hwnd_);
        CreateFonts(dpi);

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int cw = rc.right - rc.left;
        int margin = Scale(18, dpi);
        int content_w = cw - margin * 2;

        // Top banner: y = 10 .. 130
        banner_rect_ = {margin, Scale(10, dpi), cw - margin, Scale(130, dpi)};

        // Group 1: Preferences (y = 138 .. 294)
        int grp1_y = Scale(138, dpi);
        int grp1_h = Scale(156, dpi);
        grp_pref_ = CreateWindowExW(
            0,
            L"BUTTON",
            T(L"擷取畫質上限（依 USB 速率自動開放選項）、音訊與語言設定",
              L"Capture Resolution Ceiling (Auto-Gated by USB Speed), Audio & Language"),
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp1_y, content_w, grp1_h, hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(grp_pref_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int lbl_w = Scale(158, dpi);
        int combo_w = content_w - lbl_w - Scale(28, dpi);

        // Row 0: Max Selectable Resolution & Adaptive Aspect Ratio
        int row0_y = grp1_y + Scale(26, dpi);
        lbl_res_ = CreateWindowExW(
            0,
            L"STATIC",
            T(L"最大解析度與比例選項：", L"Max Resolution & Aspect:"),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row0_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_res_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_res_mode_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row0_y, combo_w, Scale(240, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboResMode)), hinst_, nullptr);
        SendMessageW(combo_res_mode_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        PopulateResolutionOptionsForSpeed(UsbLinkSpeed::Unknown, true, false);

        // Row 1: Status bar style
        int row1_y = grp1_y + Scale(58, dpi);
        lbl_bar_ = CreateWindowExW(
            0,
            L"STATIC",
            T(L"手機頂部狀態列樣式：", L"iOS Status Bar Style:"),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row1_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_bar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_status_bar_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row1_y, combo_w, Scale(200, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboStatusBar)), hinst_, nullptr);
        SendMessageW(combo_status_bar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        PopulateStatusBarCombo(false);

        // Row 2: PC Audio Routing / Monitoring
        int row2_y = grp1_y + Scale(90, dpi);
        lbl_aud_ = CreateWindowExW(
            0,
            L"STATIC",
            T(L"電腦端音訊監聽／轉發：", L"PC Audio Monitoring:"),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row2_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_aud_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_audio_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row2_y, combo_w, Scale(240, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboAudioOut)), hinst_, nullptr);
        SendMessageW(combo_audio_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        PopulateAudioEndpoints(false);

        // Row 3: Left = GPU Hardware Decode | Right = Interface Language Selector
        int row3_y = grp1_y + Scale(122, dpi);
        int inner_pref_w = content_w - Scale(28, dpi);
        int lang_combo_w = Scale(166, dpi);
        int lang_lbl_w = Scale(126, dpi);
        int lang_gap = Scale(6, dpi);
        int chk_w = std::max(Scale(200, dpi), inner_pref_w - lang_combo_w - lang_lbl_w - lang_gap - Scale(10, dpi));

        chk_hw_decode_ = CreateWindowExW(
            0,
            L"BUTTON",
            T(L"啟用 GPU 硬體超低延遲解碼 (D3D11VA，建議開啟)",
              L"Enable GPU Hardware Decode (D3D11VA, recommended)"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin + Scale(14, dpi), row3_y + Scale(1, dpi), chk_w, Scale(24, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkHwDecode)), hinst_, nullptr);
        SendMessageW(chk_hw_decode_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        SendMessageW(chk_hw_decode_, BM_SETCHECK, BST_CHECKED, 0);

        int lang_combo_x = (margin + Scale(14, dpi) + inner_pref_w) - lang_combo_w;
        int lang_lbl_x = lang_combo_x - lang_gap - lang_lbl_w;
        lbl_lang_ = CreateWindowExW(
            0,
            L"STATIC",
            L"🌐 語言 / Language:",
            WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
            lang_lbl_x, row3_y + Scale(1, dpi), lang_lbl_w, Scale(24, dpi),
            hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_lang_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_language_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            lang_combo_x, row3_y, lang_combo_w, Scale(160, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboLanguage)), hinst_, nullptr);
        SendMessageW(combo_language_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        PopulateLanguageCombo();

        // Group 2: One-Click Action Buttons (y = 302 .. 416)
        int grp2_y = Scale(302, dpi);
        int grp2_h = Scale(114, dpi);
        grp_act_ = CreateWindowExW(
            0,
            L"BUTTON",
            T(L"一鍵自動化操作", L"One-Click Actions"),
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp2_y, content_w, grp2_h, hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(grp_act_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int btn_pad = Scale(14, dpi);
        int inner_w = content_w - btn_pad * 2;
        int main_btn_y = grp2_y + Scale(24, dpi);
        btn_smart_start_ = CreateWindowExW(
            0,
            L"BUTTON",
            T(L"🚀 智慧一鍵啟動（自動檢查安裝驅動 ➔ 開始 2K+ 原生投影）",
              L"🚀 Smart Start (Auto-Verify Drivers -> Begin 2K+ Native Capture)"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            margin + btn_pad, main_btn_y, inner_w, Scale(40, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnSmartStart)), hinst_, nullptr);
        SendMessageW(btn_smart_start_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int sub_btn_y = main_btn_y + Scale(46, dpi);
        int gap = Scale(7, dpi);
        int avail_btn_w = inner_w - gap * 5;
        int w_mock = avail_btn_w * 18 / 100;
        int w_rep = avail_btn_w * 19 / 100;
        int w_diag = avail_btn_w * 19 / 100;
        int w_obs = avail_btn_w * 14 / 100;
        int w_upd = avail_btn_w * 17 / 100;
        int w_uninst = avail_btn_w - w_mock - w_rep - w_diag - w_obs - w_upd;

        int cur_x = margin + btn_pad;
        btn_mock_ = CreateWindowExW(
            0, L"BUTTON", T(L"🧪 免手機自測", L"🧪 Mock Test"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_mock, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnMockTest)), hinst_, nullptr);
        SendMessageW(btn_mock_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_mock + gap;

        btn_repair_ = CreateWindowExW(
            0, L"BUTTON", T(L"🔧 安裝／修復驅動", L"🔧 Repair Driver"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_rep, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnRepairDriver)), hinst_, nullptr);
        SendMessageW(btn_repair_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_rep + gap;

        btn_diagnostics_ = CreateWindowExW(
            0, L"BUTTON", T(L"🩺 診斷報錯", L"🩺 Diagnostics"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_diag, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnDiagnostics)), hinst_, nullptr);
        SendMessageW(btn_diagnostics_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);
        cur_x += w_diag + gap;

        btn_obs_guide_ = CreateWindowExW(
            0, L"BUTTON", T(L"📋 OBS 教學", L"📋 OBS Guide"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_obs, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnCopyObsGuide)), hinst_, nullptr);
        SendMessageW(btn_obs_guide_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_obs + gap;

        btn_update_ = CreateWindowExW(
            0, L"BUTTON", T(L"🔄 檢查更新", L"🔄 Updates"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_upd, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnCheckUpdate)), hinst_, nullptr);
        SendMessageW(btn_update_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_upd + gap;

        btn_uninstall_ = CreateWindowExW(
            0, L"BUTTON", T(L"🗑 完整移除", L"🗑 Uninstall"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_uninst, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnUninstall)), hinst_, nullptr);
        SendMessageW(btn_uninstall_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        // Group 3: Live Stream Health Telemetry Dashboard (FPS / Bitrate Mbps / Decode Latency ms / Health Grade)
        telemetry_rect_ = {margin, Scale(424, dpi), cw - margin, Scale(496, dpi)};

        // Group 4: Split Lower Section — Left: Live Diagnostics Log | Right: Low-Load Live Preview & 1:1 Lossless Screenshot
        int split_gap = Scale(14, dpi);
        int left_w = (content_w - split_gap) * 53 / 100;
        int right_x = margin + left_w + split_gap;
        int right_w = (cw - margin) - right_x;

        int log_lbl_y = Scale(504, dpi);
        lbl_log_ = CreateWindowExW(
            0,
            L"STATIC",
            T(L"即時狀態、解析度比例與碼率監控日誌：", L"Live Status, Resolution & Bitrate Log:"),
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin, log_lbl_y, left_w, Scale(22, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_log_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        int edit_y = log_lbl_y + Scale(24, dpi);
        int bottom_margin = Scale(12, dpi);
        int edit_h = (rc.bottom - rc.top) - edit_y - bottom_margin;
        edit_log_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            margin, edit_y, left_w, std::max(72, edit_h),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditLog)), hinst_, nullptr);
        SendMessageW(edit_log_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
        SendMessageW(edit_log_, EM_SETLIMITTEXT, 256 * 1024, 0);

        chk_inline_preview_ = CreateWindowExW(
            0,
            L"BUTTON",
            T(L"👁 內建即時預覽（雙擊放大｜F12 原畫質截圖）",
              L"👁 Built-in Preview (DblClick Popout | F12 Snapshot)"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            right_x, log_lbl_y, right_w, Scale(22, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkInlinePreview)), hinst_, nullptr);
        SendMessageW(chk_inline_preview_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        SendMessageW(chk_inline_preview_, BM_SETCHECK, BST_CHECKED, 0);
        inline_preview_enabled_ = true;

        int preview_btn_h = Scale(30, dpi);
        int preview_btn_y = (rc.bottom - rc.top) - bottom_margin - preview_btn_h;
        preview_rect_ = {
            right_x,
            edit_y,
            cw - margin,
            std::max(edit_y + Scale(54, dpi), preview_btn_y - Scale(6, dpi)),
        };

        int p_gap = Scale(6, dpi);
        int p_avail = right_w - p_gap * 2;
        int w_pop = p_avail * 38 / 100;
        int w_shot = p_avail * 40 / 100;
        int w_dir = p_avail - w_pop - w_shot;
        int px = right_x;

        btn_popout_monitor_ = CreateWindowExW(
            0, L"BUTTON", T(L"🖥 獨立大視窗", L"🖥 Popout View"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            px, preview_btn_y, w_pop, preview_btn_h,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnPopoutMonitor)), hinst_, nullptr);
        SendMessageW(btn_popout_monitor_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        px += w_pop + p_gap;

        btn_screenshot_ = CreateWindowExW(
            0, L"BUTTON", T(L"📸 原畫質截圖", L"📸 Screenshot"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            px, preview_btn_y, w_shot, preview_btn_h,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnScreenshot)), hinst_, nullptr);
        SendMessageW(btn_screenshot_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);
        px += w_shot + p_gap;

        btn_open_shots_ = CreateWindowExW(
            0, L"BUTTON", T(L"📂 圖庫", L"📂 Folder"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            px, preview_btn_y, w_dir, preview_btn_h,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnOpenScreenshots)), hinst_, nullptr);
        SendMessageW(btn_open_shots_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        live_monitor_ = std::make_unique<LiveMonitorWindow>(
            hinst_,
            icon_big_,
            icon_small_,
            font_bold_,
            font_normal_,
            lang_,
            [this](HWND owner) { TakeFullResolutionScreenshot(owner); },
            [this]() { OpenScreenshotsFolder(); });

        AppendLogLine(T(
            L"[歡迎] 歡迎使用 JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR
            L"！支援「🌐 繁體中文 / English 介面語言即時切換」、智慧故障診斷與即時串流健康度儀表板！",
            L"[Welcome] Welcome to JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR
            L"! Features live Language switching (Traditional Chinese / English), Smart Diagnostics & Stream Health Telemetry!"));
        RefreshHealthAndUi(true);
        TickTelemetryDashboard(true);
        SetTimer(hwnd_, kTimerRefreshId, 2000, nullptr);
        SetTimer(hwnd_, kTimerStartupUpdateId, 1500, nullptr);
        SetTimer(hwnd_, kTimerPreviewId, 33, nullptr);
        SetTimer(hwnd_, kTimerTelemetryId, 500, nullptr);
    }

    void ApplyLanguageToUi() {
        if (!hwnd_) return;

        SetWindowTextW(
            hwnd_,
            T(L"JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L" — 智慧擷取控制台 (故障偵測報錯 × 健康度儀表板 × 即時預覽)",
              L"JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L" — Smart Capture Console (Diagnostics × Health Telemetry × Live Preview)"));

        if (grp_pref_) {
            SetWindowTextW(
                grp_pref_,
                T(L"擷取畫質上限（依 USB 速率自動開放選項）、音訊與語言設定",
                  L"Capture Resolution Ceiling (Auto-Gated by USB Speed), Audio & Language"));
        }
        if (lbl_res_) SetWindowTextW(lbl_res_, T(L"最大解析度與比例選項：", L"Max Resolution & Aspect:"));
        if (lbl_bar_) SetWindowTextW(lbl_bar_, T(L"手機頂部狀態列樣式：", L"iOS Status Bar Style:"));
        if (lbl_aud_) SetWindowTextW(lbl_aud_, T(L"電腦端音訊監聽／轉發：", L"PC Audio Monitoring:"));
        if (chk_hw_decode_) {
            SetWindowTextW(
                chk_hw_decode_,
                T(L"啟用 GPU 硬體超低延遲解碼 (D3D11VA，建議開啟)",
                  L"Enable GPU Hardware Decode (D3D11VA, recommended)"));
        }
        if (grp_act_) SetWindowTextW(grp_act_, T(L"一鍵自動化操作", L"One-Click Actions"));
        if (btn_repair_) SetWindowTextW(btn_repair_, T(L"🔧 安裝／修復驅動", L"🔧 Repair Driver"));
        if (btn_obs_guide_) SetWindowTextW(btn_obs_guide_, T(L"📋 OBS 教學", L"📋 OBS Guide"));
        if (btn_uninstall_) SetWindowTextW(btn_uninstall_, T(L"🗑 完整移除", L"🗑 Uninstall"));
        if (lbl_log_) {
            SetWindowTextW(
                lbl_log_,
                T(L"即時狀態、解析度比例與碼率監控日誌：", L"Live Status, Resolution & Bitrate Log:"));
        }
        if (chk_inline_preview_) {
            SetWindowTextW(
                chk_inline_preview_,
                T(L"👁 內建即時預覽（雙擊放大｜F12 原畫質截圖）",
                  L"👁 Built-in Preview (DblClick Popout | F12 Snapshot)"));
        }
        if (btn_popout_monitor_) SetWindowTextW(btn_popout_monitor_, T(L"🖥 獨立大視窗", L"🖥 Popout View"));
        if (btn_screenshot_) SetWindowTextW(btn_screenshot_, T(L"📸 原畫質截圖", L"📸 Screenshot"));
        if (btn_open_shots_) SetWindowTextW(btn_open_shots_, T(L"📂 圖庫", L"📂 Folder"));

        if (btn_update_) {
            if (checking_update_.load(std::memory_order_acquire)) {
                SetWindowTextW(btn_update_, T(L"⏳ 檢查中...", L"⏳ Checking..."));
            } else if (cached_update_.update_available) {
                std::wstring up_lbl =
                    std::wstring(T(L"⬆️ 升級 ", L"⬆️ Update ")) + Utf8ToWide(cached_update_.latest.tag_name);
                SetWindowTextW(btn_update_, up_lbl.c_str());
            } else {
                SetWindowTextW(btn_update_, T(L"🔄 檢查更新", L"🔄 Updates"));
            }
        }

        PopulateLanguageCombo();
        PopulateResolutionOptionsForSpeed(detected_usb_speed_, false, true);
        PopulateStatusBarCombo(true);
        PopulateAudioEndpoints(true);
        UpdateDiagnosticsButtonBadge();
        RefreshHealthAndUi(false);

        if (live_monitor_) {
            live_monitor_->SetLanguage(lang_);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void PopulateResolutionOptionsForSpeed(UsbLinkSpeed speed, bool is_initial, bool preserve_current = false) {
        if (!combo_res_mode_) return;

        uint32_t prev_max_dim = 0;
        std::wstring prev_mode_tag;
        LRESULT cur_sel = SendMessageW(combo_res_mode_, CB_GETCURSEL, 0, 0);
        if (!is_initial && cur_sel >= 0 && static_cast<size_t>(cur_sel) < res_options_.size()) {
            prev_max_dim = res_options_[static_cast<size_t>(cur_sel)].max_dim;
            prev_mode_tag = res_options_[static_cast<size_t>(cur_sel)].mode_tag;
        }

        const uint32_t max_cap = MaxSelectableDimForUsbSpeed(speed);
        std::vector<ResOptionSpec> next_opts;

        if (max_cap >= 3840U) {
            next_opts.push_back({
                3840U,
                L"native",
                T(L"🌟 原生極限無上限（最高 3840p 自適應比例 — USB 3.x 高速接口已解鎖）",
                  L"🌟 Native Uncapped (Up to 3840p Adaptive Aspect — USB 3.x Unlocked)"),
                L" --res native --max-dim 3840",
            });
            next_opts.push_back({
                2560U,
                L"native",
                T(L"💎 2K 高畫質上限（最高 2560p 自適應比例）",
                  L"💎 2K High Quality Cap (Up to 2560p Adaptive Aspect)"),
                L" --res native --max-dim 2560",
            });
            next_opts.push_back({
                1920U,
                L"native",
                T(L"✨ 1080p 標準上限（最高 1920p 自適應比例、低負載）",
                  L"✨ 1080p Standard Cap (Up to 1920p Adaptive Aspect, Low Load)"),
                L" --res native --max-dim 1920",
            });
            next_opts.push_back({
                1280U,
                L"native",
                T(L"⚡ 720p 流暢上限（最高 1280p 自適應比例）",
                  L"⚡ 720p Smooth Cap (Up to 1280p Adaptive Aspect)"),
                L" --res native --max-dim 1280",
            });
            next_opts.push_back({
                2560U,
                L"1440p",
                T(L"🖥 固定 2K 16:9 畫布 (2560×1440，直橫屏旋轉自動補黑邊)",
                  L"🖥 Fixed 2K 16:9 Canvas (2560×1440, Auto Letterbox on Rotation)"),
                L" --res 1440p --max-dim 2560",
            });
            next_opts.push_back({
                1920U,
                L"1080p",
                T(L"🖥 固定 1080p 16:9 畫布 (1920×1080，傳統直播相容模式)",
                  L"🖥 Fixed 1080p 16:9 Canvas (1920×1080, Classic Stream Compatibility)"),
                L" --res 1080p --max-dim 1920",
            });
        } else if (max_cap >= 2560U) {
            if (speed == UsbLinkSpeed::High_480Mbps) {
                next_opts.push_back({
                    2560U,
                    L"native",
                    T(L"💎 2K 高畫質上限（最高 2560p 自適應比例 — USB 2.0 接口最高上限）",
                      L"💎 2K High Quality Cap (Up to 2560p Adaptive — USB 2.0 Max Tier)"),
                    L" --res native --max-dim 2560",
                });
                next_opts.push_back({
                    1920U,
                    L"native",
                    T(L"✨ 1080p 標準上限（最高 1920p 自適應比例 — USB 2.0 穩定推薦）",
                      L"✨ 1080p Standard Cap (Up to 1920p Adaptive — Recommended for USB 2.0)"),
                    L" --res native --max-dim 1920",
                });
            } else {
                next_opts.push_back({
                    2560U,
                    L"native",
                    T(L"💎 2K 高畫質上限（最高 2560p 自適應 — 插入 USB 3.x 接口將解鎖 3840p）",
                      L"💎 2K High Quality Cap (Up to 2560p Adaptive — Plug USB 3.x to Unlock 3840p)"),
                    L" --res native --max-dim 2560",
                });
                next_opts.push_back({
                    1920U,
                    L"native",
                    T(L"✨ 1080p 標準上限（最高 1920p 自適應比例、低負載）",
                      L"✨ 1080p Standard Cap (Up to 1920p Adaptive Aspect, Low Load)"),
                    L" --res native --max-dim 1920",
                });
            }
            next_opts.push_back({
                1280U,
                L"native",
                T(L"⚡ 720p 流暢上限（最高 1280p 自適應比例）",
                  L"⚡ 720p Smooth Cap (Up to 1280p Adaptive Aspect)"),
                L" --res native --max-dim 1280",
            });
            next_opts.push_back({
                2560U,
                L"1440p",
                T(L"🖥 固定 2K 16:9 畫布 (2560×1440，直橫屏旋轉自動補黑邊)",
                  L"🖥 Fixed 2K 16:9 Canvas (2560×1440, Auto Letterbox on Rotation)"),
                L" --res 1440p --max-dim 2560",
            });
            next_opts.push_back({
                1920U,
                L"1080p",
                T(L"🖥 固定 1080p 16:9 畫布 (1920×1080，傳統直播相容模式)",
                  L"🖥 Fixed 1080p 16:9 Canvas (1920×1080, Classic Stream Compatibility)"),
                L" --res 1080p --max-dim 1920",
            });
        } else {
            next_opts.push_back({
                1280U,
                L"native",
                T(L"⚡ 720p 流暢上限（最高 1280p 自適應比例 — USB 1.1 接口最高上限）",
                  L"⚡ 720p Smooth Cap (Up to 1280p Adaptive — USB 1.1 Max Tier)"),
                L" --res native --max-dim 1280",
            });
            next_opts.push_back({
                960U,
                L"native",
                T(L"📱 540p 省頻寬上限（最高 960p 自適應比例）",
                  L"📱 540p Low-Bandwidth Cap (Up to 960p Adaptive Aspect)"),
                L" --res native --max-dim 960",
            });
            next_opts.push_back({
                1280U,
                L"720p",
                T(L"🖥 固定 720p 16:9 畫布 (1280×720)", L"🖥 Fixed 720p 16:9 Canvas (1280×720)"),
                L" --res 720p --max-dim 1280",
            });
        }

        res_options_ = std::move(next_opts);
        SendMessageW(combo_res_mode_, CB_RESETCONTENT, 0, 0);
        for (const auto& opt : res_options_) {
            SendMessageW(combo_res_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(opt.label.c_str()));
        }

        int target_idx = 0;
        if (!is_initial && (user_manually_picked_res_ || preserve_current) && prev_max_dim > 0) {
            for (size_t i = 0; i < res_options_.size(); ++i) {
                if (res_options_[i].max_dim == prev_max_dim && res_options_[i].mode_tag == prev_mode_tag) {
                    target_idx = static_cast<int>(i);
                    break;
                }
            }
        }
        SendMessageW(combo_res_mode_, CB_SETCURSEL, target_idx, 0);
    }

    void PopulateAudioEndpoints(bool preserve_selection = false) {
        std::wstring prev_choice;
        if (preserve_selection && combo_audio_) {
            LRESULT cur = SendMessageW(combo_audio_, CB_GETCURSEL, 0, 0);
            if (cur >= 0 && static_cast<size_t>(cur) < audio_endpoint_matches_.size()) {
                prev_choice = audio_endpoint_matches_[static_cast<size_t>(cur)];
            }
        }

        SendMessageW(combo_audio_, CB_RESETCONTENT, 0, 0);
        audio_endpoint_matches_.clear();

        SendMessageW(
            combo_audio_,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(T(
                L"自動偵測 VB-Audio Virtual Cable（預設，供 Discord/Zoom 收音）",
                L"Auto-Detect VB-Audio Virtual Cable (Default for Discord / Zoom)")));
        audio_endpoint_matches_.push_back(L"__AUTO__");

        SendMessageW(
            combo_audio_,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(T(
                L"僅輸出至 OBS「JCast for iOS (Audio)」（不在電腦喇叭播放）",
                L"OBS 'JCast for iOS (Audio)' Only (Mute PC Speakers)")));
        audio_endpoint_matches_.push_back(L"__NONE__");

        auto eps = WasapiAudioRouter::EnumerateRenderEndpoints();
        for (const auto& ep : eps) {
            std::wstring wname = Utf8ToWide(ep.friendly_name_utf8);
            std::wstring label = std::wstring(T(L"🎧 同步播放／轉發至：", L"🎧 Monitor / Route to: ")) + wname;
            SendMessageW(combo_audio_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            audio_endpoint_matches_.push_back(wname);
        }

        int sel_idx = 0;
        if (!prev_choice.empty()) {
            for (size_t i = 0; i < audio_endpoint_matches_.size(); ++i) {
                if (audio_endpoint_matches_[i] == prev_choice) {
                    sel_idx = static_cast<int>(i);
                    break;
                }
            }
        }
        SendMessageW(combo_audio_, CB_SETCURSEL, sel_idx, 0);
    }

    void AppendLogLine(const std::wstring& line) {
        if (!edit_log_) return;
        std::wstring formatted = line;
        if (formatted.empty() || formatted.back() != L'\n') {
            formatted += L"\r\n";
        } else if (formatted.size() >= 2 && formatted[formatted.size() - 2] != L'\r') {
            formatted.pop_back();
            formatted += L"\r\n";
        }

        int len = GetWindowTextLengthW(edit_log_);
        if (len > 180000) {
            SetWindowTextW(edit_log_, L"");
            len = 0;
        }
        SendMessageW(edit_log_, EM_SETSEL, len, len);
        SendMessageW(edit_log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(formatted.c_str()));
        SendMessageW(edit_log_, EM_SCROLLCARET, 0, 0);
    }

    void PostLogFromWorker(const std::wstring& msg) {
        auto* heap_str = new std::wstring(msg);
        if (!PostMessageW(hwnd_, kWmAppendLog, 0, reinterpret_cast<LPARAM>(heap_str))) {
            delete heap_str;
        }
    }

    void RefreshHealthAndUi(bool force_usb_speed_reprobe = false) {
        health_ = ProbeSystemHealth(base_dir_);
        bool running = host_running_.load(std::memory_order_acquire);

        // STRICT RULE: Never probe or alter selectable resolution options while actively transmitting!
        if (!running) {
            if (health_.ios_usb == IosUsbState::Unplugged) {
                if (detected_usb_speed_ != UsbLinkSpeed::Unknown) {
                    detected_usb_speed_ = UsbLinkSpeed::Unknown;
                    PopulateResolutionOptionsForSpeed(UsbLinkSpeed::Unknown, false);
                }
            } else if (force_usb_speed_reprobe ||
                       prev_ios_usb_ == IosUsbState::Unplugged ||
                       detected_usb_speed_ == UsbLinkSpeed::Unknown) {
                UsbLinkSpeed new_spd = UsbIosTransport::ProbeConnectedIosLinkSpeed();
                if (new_spd != detected_usb_speed_) {
                    detected_usb_speed_ = new_spd;
                    PopulateResolutionOptionsForSpeed(new_spd, false);
                    uint32_t cap = MaxSelectableDimForUsbSpeed(new_spd);
                    AppendLogLine(
                        std::wstring(T(L"[USB 速率辨識] 已偵測到連接埠速率：", L"[USB Speed] Detected link speed: ")) +
                        Utf8ToWide(UsbLinkSpeedName(new_spd)) +
                        T(L" ➔ 已開放最高 ", L" -> Unlocked up to ") + std::to_wstring(cap) +
                        T(L"p 解析度選項（開始傳輸後將全程鎖定所選解析度，不會中途自動變動）。",
                          L"p options (locked during streaming; never auto-changes mid-stream)."));
                }
            }
        }
        prev_ios_usb_ = health_.ios_usb;

        if (running) {
            if (running_mock_) {
                SetWindowTextW(btn_smart_start_, T(L"■ 停止自測訊號", L"■ Stop Mock Test Signal"));
                SetWindowTextW(btn_mock_, T(L"■ 停止自測", L"■ Stop Mock"));
            } else {
                SetWindowTextW(
                    btn_smart_start_,
                    T(L"■ 停止 iPhone 投影擷取（安全切回一般充電/傳輸模式）",
                      L"■ Stop iPhone Capture (Safely Restore Normal USB Mode)"));
                SetWindowTextW(btn_mock_, T(L"🧪 免手機自測", L"🧪 Mock Test"));
                EnableWindow(btn_mock_, FALSE);
            }
            EnableWindow(combo_res_mode_, FALSE);
            EnableWindow(combo_status_bar_, FALSE);
            EnableWindow(combo_audio_, FALSE);
            EnableWindow(chk_hw_decode_, FALSE);
            EnableWindow(btn_repair_, FALSE);
            EnableWindow(btn_update_, FALSE);
        } else {
            bool needs_setup = (health_.dshow != DshowRegState::Ready) ||
                               !health_.libusb_service_ok ||
                               (health_.ios_usb == IosUsbState::PluggedNeedFilter);
            if (needs_setup) {
                SetWindowTextW(
                    btn_smart_start_,
                    T(L"🚀 智慧一鍵啟動（自動安裝驅動與 JCast 虛擬擷取卡 ➔ 開始投影）",
                      L"🚀 Smart Start (Auto-Install Driver & Virtual Capture Card -> Begin Capture)"));
            } else {
                SetWindowTextW(
                    btn_smart_start_,
                    T(L"▶ 開始 iPhone / iPad 有線投影擷取（鎖定所選解析度 @ 60fps）",
                      L"▶ Start Wired iPhone / iPad Capture (Locked Resolution @ 60fps)"));
            }
            SetWindowTextW(btn_mock_, T(L"🧪 免手機自測", L"🧪 Mock Test"));
            EnableWindow(btn_mock_, TRUE);
            EnableWindow(combo_res_mode_, TRUE);
            EnableWindow(combo_status_bar_, TRUE);
            EnableWindow(combo_audio_, TRUE);
            EnableWindow(chk_hw_decode_, TRUE);
            EnableWindow(btn_repair_, TRUE);
            EnableWindow(btn_update_, checking_update_.load(std::memory_order_acquire) ? FALSE : TRUE);
        }

        InvalidateRect(hwnd_, &banner_rect_, FALSE);
    }

    void PaintStatusBanner(HDC hdc) {
        int w = banner_rect_.right - banner_rect_.left;
        int h = banner_rect_.bottom - banner_rect_.top;
        if (w <= 0 || h <= 0) return;

        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

        bool running = host_running_.load(std::memory_order_acquire);
        COLORREF bg_color = RGB(240, 246, 255);
        COLORREF accent_color = RGB(24, 100, 210);
        std::wstring headline;

        if (running) {
            bg_color = RGB(232, 248, 238);
            accent_color = RGB(22, 136, 72);
            headline = running_mock_
                           ? T(L"🟢 JCast 正在輸出自測動態彩條與 48kHz 測試音（請開啟 OBS 預覽）",
                               L"🟢 JCast Mock Test Active — Animated Color Bars + 48kHz Tone")
                           : T(L"🟢 JCast 有線投影執行中（已鎖定所選解析度 @ 60fps + 48kHz 音訊）",
                               L"🟢 JCast Wired Capture Active (Locked Resolution @ 60fps + 48kHz Audio)");
        } else if (health_.ios_usb == IosUsbState::PluggedZadigConflict) {
            bg_color = RGB(255, 240, 240);
            accent_color = RGB(196, 43, 28);
            headline = T(L"⚠️ 偵測到父裝置驅動遭 Zadig 覆蓋（請於裝置管理員還原 Apple 複合裝置驅動）",
                         L"⚠️ Apple USB Composite Driver Overwritten by Zadig (Restore in Device Manager)");
        } else if (health_.dshow != DshowRegState::Ready || !health_.libusb_service_ok ||
                   health_.ios_usb == IosUsbState::PluggedNeedFilter) {
            bg_color = RGB(255, 248, 232);
            accent_color = RGB(180, 105, 14);
            headline = T(L"⚡ 準備開始：請點擊下方「智慧一鍵啟動」自動完成驅動掛載與高畫質投影",
                         L"⚡ Ready to Setup: Click 'Smart Start' below to attach driver & start capture");
        } else if (health_.ios_usb == IosUsbState::PluggedReady) {
            bg_color = RGB(234, 247, 240);
            accent_color = RGB(20, 130, 75);
            headline = T(L"✅ iPhone 與驅動皆已就緒！請確認解析度選項後點擊「開始有線投影擷取」",
                         L"✅ iOS Device & Drivers Ready! Pick your resolution and click Start Capture");
        } else {
            bg_color = RGB(242, 245, 250);
            accent_color = RGB(45, 95, 185);
            headline = T(L"🔌 JCast for iOS 已就緒 — 請插上已點「信任」的 iPhone/iPad 自動辨識 USB 速率",
                         L"🔌 JCast for iOS Ready — Plug in a Trusted iPhone/iPad to Auto-Detect USB Speed");
        }

        RECT local_rc{0, 0, w, h};
        HBRUSH bg_brush = CreateSolidBrush(bg_color);
        FillRect(mem_dc, &local_rc, bg_brush);
        DeleteObject(bg_brush);

        UINT dpi = GetSystemDpiSafe(hwnd_);
        RECT stripe{0, 0, Scale(6, dpi), h};
        HBRUSH acc_brush = CreateSolidBrush(accent_color);
        FillRect(mem_dc, &stripe, acc_brush);
        FrameRect(mem_dc, &local_rc, acc_brush);
        DeleteObject(acc_brush);

        int icon_sz = Scale(64, dpi);
        int right_limit = icon_banner_ ? (w - icon_sz - Scale(26, dpi)) : (w - Scale(12, dpi));
        if (icon_banner_) {
            DrawIconEx(
                mem_dc,
                w - icon_sz - Scale(16, dpi),
                (h - icon_sz) / 2,
                icon_banner_,
                icon_sz,
                icon_sz,
                0,
                nullptr,
                DI_NORMAL);
        }

        SetBkMode(mem_dc, TRANSPARENT);
        HGDIOBJ old_font = SelectObject(mem_dc, font_title_);
        SetTextColor(mem_dc, accent_color);
        RECT title_rc{Scale(18, dpi), Scale(12, dpi), right_limit, Scale(42, dpi)};
        DrawTextW(mem_dc, headline.c_str(), -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mem_dc, font_normal_);
        SetTextColor(mem_dc, RGB(40, 46, 56));

        std::wstring dshow_txt;
        if (health_.dshow == DshowRegState::Ready) {
            dshow_txt = T(L"• DirectShow 虛擬擷取卡 (JCast for iOS)：✅ 已安裝",
                          L"• DirectShow Virtual Capture Card (JCast for iOS): ✅ Installed");
        } else if (health_.dshow == DshowRegState::PathMismatch) {
            dshow_txt = T(L"• DirectShow 虛擬擷取卡 (JCast for iOS)：⚠️ 資料夾已移動（點一鍵啟動自動更新）",
                          L"• DirectShow Virtual Capture Card (JCast for iOS): ⚠️ Folder Moved (Auto-fixes on Start)");
        } else {
            dshow_txt = T(L"• DirectShow 虛擬擷取卡 (JCast for iOS)：⏳ 尚未註冊（點一鍵啟動自動完成）",
                          L"• DirectShow Virtual Capture Card (JCast for iOS): ⏳ Not Registered (Auto-fixes on Start)");
        }

        std::wstring usb_drv_txt = health_.libusb_service_ok
                                       ? T(L"• WHQL 簽章過濾驅動 (libusb0)：✅ 已就緒",
                                           L"• WHQL Filter Driver (libusb0): ✅ Ready")
                                       : T(L"• WHQL 簽章過濾驅動 (libusb0)：⏳ 待安裝",
                                           L"• WHQL Filter Driver (libusb0): ⏳ Pending Setup");
        if (cached_update_.update_available) {
            usb_drv_txt += std::wstring(T(L"   ｜   🎉 發現新版 ", L"   |   🎉 Update ")) +
                           Utf8ToWide(cached_update_.latest.tag_name) +
                           T(L" 可一鍵升級！", L" Available!");
        } else {
            usb_drv_txt += std::wstring(T(L"   ｜   目前版本：v", L"   |   Version: v")) + IOS_VCAM_VER_DISPLAY_WSTR;
        }

        std::wstring speed_badge = FormatUsbSpeedBadgeWide(detected_usb_speed_, lang_);
        std::wstring speed_suffix = speed_badge.empty() ? L"" : (L" [" + speed_badge + L"]");

        std::wstring phone_txt;
        switch (health_.ios_usb) {
            case IosUsbState::PluggedReady:
                phone_txt = std::wstring(T(L"• iPhone / iPad USB：✅ 已連線", L"• iPhone / iPad USB: ✅ Connected")) +
                            speed_suffix;
                break;
            case IosUsbState::PluggedNeedFilter:
                phone_txt = std::wstring(T(L"• iPhone / iPad USB：📱 已偵測（點啟動掛載過濾層）",
                                           L"• iPhone / iPad USB: 📱 Detected (Click Start to attach filter)")) +
                            speed_suffix;
                break;
            case IosUsbState::PluggedZadigConflict:
                phone_txt = std::wstring(T(L"• iPhone / iPad USB：❌ 偵測到 ", L"• iPhone / iPad USB: ❌ Conflict with ")) +
                            health_.ios_service_name +
                            T(L" 覆蓋原廠驅動", L" driver");
                break;
            case IosUsbState::Unplugged:
                phone_txt = T(L"• iPhone / iPad USB：🔌 尚未插入 USB（插入後自動辨識 USB 3.x / 2.0 速率並更新可選上限）",
                              L"• iPhone / iPad USB: 🔌 Not Connected (Auto-detects USB 3.x / 2.0 speed on plug-in)");
                break;
        }

        RECT line1{Scale(20, dpi), Scale(46, dpi), right_limit, Scale(68, dpi)};
        RECT line2{Scale(20, dpi), Scale(70, dpi), right_limit, Scale(92, dpi)};
        RECT line3{Scale(20, dpi), Scale(94, dpi), right_limit, Scale(116, dpi)};
        DrawTextW(mem_dc, dshow_txt.c_str(), -1, &line1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextW(mem_dc, usb_drv_txt.c_str(), -1, &line2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextW(mem_dc, phone_txt.c_str(), -1, &line3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mem_dc, old_font);
        BitBlt(hdc, banner_rect_.left, banner_rect_.top, w, h, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
    }

    bool RunDriverSetupElevated(const wchar_t* arg_flag) {
        std::filesystem::path exe = base_dir_ / L"ios_vcam_driver_setup.exe";
        if (!std::filesystem::exists(exe)) {
            AppendLogLine(T(L"[錯誤] 找不到 ios_vcam_driver_setup.exe，請確認程式資料夾檔案完整！",
                            L"[Error] Missing ios_vcam_driver_setup.exe. Please verify all app files are extracted!"));
            MessageBoxW(
                hwnd_,
                T(L"找不到 ios_vcam_driver_setup.exe，請確認完整安裝或解壓縮 JCast for iOS 資料夾！",
                  L"ios_vcam_driver_setup.exe was not found. Please reinstall or extract the full JCast for iOS folder!"),
                T(L"缺少檔案", L"Missing File"),
                MB_ICONERROR | MB_OK);
            return false;
        }

        AppendLogLine(
            std::wstring(T(L"[系統] 正在請求系統管理員權限執行 ios_vcam_driver_setup.exe ",
                           L"[System] Requesting administrator elevation for ios_vcam_driver_setup.exe ")) +
            arg_flag + L" ...");

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = hwnd_;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpParameters = arg_flag;
        sei.lpDirectory = base_dir_.c_str();
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExW(&sei)) {
            DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                AppendLogLine(T(L"[取消] 使用者取消了管理員權限授權 (UAC)。",
                                L"[Cancelled] Administrator elevation (UAC) was cancelled by user."));
            } else {
                AppendLogLine(
                    std::wstring(T(L"[錯誤] 無法啟動安裝輔助程式 (錯誤碼 ",
                                   L"[Error] Failed to launch driver setup helper (error ")) +
                    std::to_wstring(err) + L").");
            }
            return false;
        }

        if (sei.hProcess) {
            while (WaitForSingleObject(sei.hProcess, 50) == WAIT_TIMEOUT) {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            DWORD exit_code = 1;
            GetExitCodeProcess(sei.hProcess, &exit_code);
            CloseHandle(sei.hProcess);
            RefreshHealthAndUi(true);
            if (exit_code == 0) {
                AppendLogLine(T(L"[成功] 驅動過濾層與 JCast for iOS 虛擬擷取卡設定完成！",
                                L"[Success] Driver filter and JCast for iOS virtual capture card configured!"));
                return true;
            }
            AppendLogLine(
                std::wstring(T(L"[提醒] 設定程式回傳代碼 ", L"[Notice] Driver setup returned exit code ")) +
                std::to_wstring(exit_code) +
                T(L"，請確認檔案完整或重試。", L". Please verify files and retry."));
            return false;
        }
        RefreshHealthAndUi(true);
        return true;
    }

    std::wstring GetLogTextWide() const {
        if (!edit_log_) return {};
        int len = GetWindowTextLengthW(edit_log_);
        if (len <= 0) return {};
        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
        int copied = GetWindowTextW(edit_log_, buf.data(), len + 1);
        if (copied <= 0) return {};
        buf.resize(static_cast<size_t>(copied));
        return buf;
    }

    void UpdateDiagnosticsButtonBadge() {
        if (btn_diagnostics_) {
            if (recent_faults_.empty()) {
                SetWindowTextW(btn_diagnostics_, T(L"🩺 診斷報錯", L"🩺 Diagnostics"));
            } else {
                std::wstring lbl = std::wstring(T(L"⚠️ 診斷報錯 (", L"⚠️ Issues (")) +
                                   std::to_wstring(recent_faults_.size()) + L")";
                SetWindowTextW(btn_diagnostics_, lbl.c_str());
            }
        }
        if (hwnd_) {
            InvalidateRect(hwnd_, &telemetry_rect_, FALSE);
            InvalidateRect(hwnd_, &banner_rect_, FALSE);
        }
    }

    void OpenDiagnosticsModal() {
        if (!hwnd_ || shutting_down_.load(std::memory_order_acquire)) return;
        UINT dpi = GetSystemDpiSafe(hwnd_);
        DiagnosticsModalWindow dlg(
            hinst_,
            hwnd_,
            base_dir_,
            dpi,
            lang_,
            font_normal_,
            font_bold_,
            font_title_,
            font_mono_,
            icon_small_,
            detected_usb_speed_,
            telem_snap_,
            recent_faults_,
            GetLogTextWide(),
            [this]() {
                health_ = ProbeSystemHealth(base_dir_);
                return health_;
            },
            [this]() {
                if (host_running_.load(std::memory_order_acquire)) {
                    StopHostProcess();
                }
                return RunDriverSetupElevated(L"--install");
            },
            [this]() {
                recent_faults_.clear();
                UpdateDiagnosticsButtonBadge();
            });
        dlg.RunModal();
    }

    void RecordDiagnosticFault(
        DiagCode code,
        DiagSeverity sev,
        const std::wstring& detail,
        bool auto_open_modal = false) {
        if (code == DiagCode::None) return;

        bool duplicate_tail = (!recent_faults_.empty() &&
                               recent_faults_.back().code == code &&
                               recent_faults_.back().detail_wide == detail);
        if (duplicate_tail) {
            recent_faults_.back().timestamp_str = CurrentLocalTimeShortWide();
            recent_faults_.back().severity = std::max(recent_faults_.back().severity, sev);
        } else {
            constexpr size_t kMaxStoredFaults = 25;
            if (recent_faults_.size() >= kMaxStoredFaults) {
                recent_faults_.erase(recent_faults_.begin());
            }
            recent_faults_.push_back(DiagEventRecord{
                code,
                sev,
                CurrentLocalTimeShortWide(),
                detail,
            });

            const auto& spec = LookupDiagSpec(code, lang_);
            AppendLogLine(
                std::wstring(T(L"[異常偵測 ", L"[Fault Detected ")) + Utf8ToWide(spec.code_str) + L"] " +
                Utf8ToWide(spec.title_utf8) + L" — " + detail +
                T(L"（點「🩺 診斷報錯」檢視排除步驟或一鍵回報）",
                  L" (Click 'Diagnostics' for fix steps & bug reporting)"));
        }

        UpdateDiagnosticsButtonBadge();
        if (auto_open_modal && !shutting_down_.load(std::memory_order_acquire)) {
            OpenDiagnosticsModal();
        }
    }

    void StartHostProcess(bool mock_mode) {
        if (host_running_.load(std::memory_order_acquire)) {
            return;
        }

        health_ = ProbeSystemHealth(base_dir_);
        if (!health_.missing_core_files.empty()) {
            std::wstring missing_list;
            for (size_t i = 0; i < health_.missing_core_files.size(); ++i) {
                if (i > 0) missing_list += L", ";
                missing_list += health_.missing_core_files[i];
            }
            RecordDiagnosticFault(
                DiagCode::E101_MissingCoreFiles,
                DiagSeverity::Critical,
                std::wstring(T(L"程式目錄缺少必要元件：", L"Missing required app files: ")) + missing_list +
                    T(L"，請重新完整解壓縮或執行安裝程式。", L". Please extract the full archive or reinstall."),
                true);
            return;
        }

        if (!mock_mode && !health_.mf_h264_ok) {
            RecordDiagnosticFault(
                DiagCode::E103_MissingMediaFoundation,
                DiagSeverity::Critical,
                T(L"系統缺少 mfplat.dll 或 msmpeg2vdec.dll（Windows N / KN 版本請先安裝媒體功能套件）。",
                  L"Missing mfplat.dll or msmpeg2vdec.dll (Windows N/KN editions require Media Feature Pack)."),
                true);
            return;
        }

        if (!mock_mode && health_.ios_usb == IosUsbState::PluggedZadigConflict) {
            RecordDiagnosticFault(
                DiagCode::E201_ZadigDriverConflict,
                DiagSeverity::Critical,
                std::wstring(T(L"偵測到父複合裝置遭 ", L"Composite USB parent overwritten by ")) +
                    health_.ios_service_name +
                    T(L" 覆蓋原廠驅動，無法啟用 Apple 影音通道。", L"; Apple media channel cannot activate."),
                true);
            return;
        }

        if (!mock_mode && !health_.apple_service_installed) {
            RecordDiagnosticFault(
                DiagCode::E102_MissingAppleService,
                DiagSeverity::Warning,
                T(L"尚未偵測到 Apple 裝置 / iTunes 配對服務；若手機未跳出「信任這部電腦」請先安裝 Apple 裝置。",
                  L"Apple Devices / iTunes pairing service not detected; install Apple Devices if Trust prompt does not appear."),
                false);
        }

        std::filesystem::path host_exe = base_dir_ / L"ios_vcam_host.exe";
        if (mock_mode) {
            if (health_.dshow != DshowRegState::Ready) {
                if (!RunDriverSetupElevated(L"--install")) return;
            }
        } else {
            bool needs_install = (health_.dshow != DshowRegState::Ready) ||
                                 !health_.libusb_service_ok ||
                                 (health_.ios_usb == IosUsbState::PluggedNeedFilter);
            if (needs_install) {
                if (!RunDriverSetupElevated(L"--install")) return;
            }
            if (health_.ios_usb == IosUsbState::Unplugged) {
                AppendLogLine(T(
                    L"[提示] 目前尚未插入 iPhone，已進入等待連線模式；插上手機並點「信任」後若為首次插入，再點一次「安裝／修復驅動」即可掛載過濾層。",
                    L"[Tip] No iPhone detected yet — waiting for USB connection. After plugging in and tapping 'Trust', click 'Repair Driver' once if first time."));
            } else {
                RefreshHealthAndUi(true);
            }
        }

        std::wstring event_name = L"Local\\IosUsbVcamGuiStop_" + std::to_wstring(GetCurrentProcessId()) +
                                  L"_" + std::to_wstring(GetTickCount64());
        HANDLE hStopEvt = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
        if (!hStopEvt) {
            AppendLogLine(T(L"[錯誤] 無法建立跨行程控制事件。", L"[Error] Failed to create IPC stop event."));
            return;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE read_pipe = nullptr, write_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            CloseHandle(hStopEvt);
            return;
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        std::wstring cmd = L"\"" + host_exe.wstring() + L"\" --stop-event " + event_name;

        LRESULT res_idx = SendMessageW(combo_res_mode_, CB_GETCURSEL, 0, 0);
        std::wstring chosen_res_label = T(L"預設自適應上限", L"Default Adaptive Cap");
        if (res_idx >= 0 && static_cast<size_t>(res_idx) < res_options_.size()) {
            cmd += res_options_[static_cast<size_t>(res_idx)].cli_args;
            chosen_res_label = res_options_[static_cast<size_t>(res_idx)].label;
        } else {
            cmd += L" --res native --max-dim 2560";
        }

        if (mock_mode) {
            cmd += L" --mock";
        } else {
            LRESULT bar_idx = SendMessageW(combo_status_bar_, CB_GETCURSEL, 0, 0);
            cmd += (bar_idx == 1) ? L" --mode clean" : L" --mode valeria";
            if (SendMessageW(chk_hw_decode_, BM_GETCHECK, 0, 0) != BST_CHECKED) {
                cmd += L" --sw-decode";
            }
        }

        LRESULT aud_idx = SendMessageW(combo_audio_, CB_GETCURSEL, 0, 0);
        if (aud_idx >= 0 && static_cast<size_t>(aud_idx) < audio_endpoint_matches_.size()) {
            const std::wstring& choice = audio_endpoint_matches_[static_cast<size_t>(aud_idx)];
            if (choice == L"__NONE__") {
                cmd += L" --no-audio-route";
            } else if (choice != L"__AUTO__") {
                cmd += L" --audio-out \"" + choice + L"\"";
            }
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');

        user_stopping_host_.store(false, std::memory_order_release);
        session_critical_fault_seen_ = false;

        BOOL ok = CreateProcessW(
            host_exe.c_str(),
            cmd_buf.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            base_dir_.c_str(),
            &si,
            &pi);

        CloseHandle(write_pipe);

        if (!ok) {
            DWORD err = GetLastError();
            CloseHandle(read_pipe);
            CloseHandle(hStopEvt);
            RecordDiagnosticFault(
                DiagCode::E503_HostUnexpectedExit,
                DiagSeverity::Critical,
                std::wstring(T(L"無法啟動背景擷取引擎 ios_vcam_host.exe（Windows 錯誤碼 ",
                               L"Failed to launch capture engine ios_vcam_host.exe (Win32 error ")) +
                    std::to_wstring(err) + L").",
                true);
            return;
        }

        CloseHandle(pi.hThread);
        {
            std::lock_guard<std::mutex> lk(proc_mtx_);
            host_process_ = pi.hProcess;
            host_stop_event_ = hStopEvt;
        }
        running_mock_ = mock_mode;
        host_running_.store(true, std::memory_order_release);
        fps_history_.clear();
        mbps_history_.clear();
        RefreshHealthAndUi(false);
        TickTelemetryDashboard(true);

        AppendLogLine(
            mock_mode
                ? T(L"=== 已啟動 JCast 免手機自測模式（彩條 + 48kHz 測試音） ===",
                    L"=== Started JCast Mock Test Mode (Color Bars + 48kHz Tone) ===")
                : (std::wstring(T(L"=== 已啟動 JCast 有線投影｜本次鎖定選項：",
                                  L"=== Started JCast Wired Capture | Locked Option: ")) +
                   chosen_res_label + L" ==="));

        if (log_reader_thread_.joinable()) {
            log_reader_thread_.join();
        }
        log_reader_thread_ = std::thread([this, read_pipe, hProc = pi.hProcess]() {
            auto dispatch_line = [this](const std::string& raw_line) {
                if (raw_line.empty()) return;
                ParsedDiagEvent pev;
                if (ParseHostDiagLine(raw_line, pev)) {
                    auto* heap_ev = new ParsedDiagEvent(std::move(pev));
                    if (!PostMessageW(hwnd_, kWmHostDiagEvent, 0, reinterpret_cast<LPARAM>(heap_ev))) {
                        delete heap_ev;
                    }
                } else {
                    PostLogFromWorker(Utf8ToWide(raw_line));
                }
            };

            char buf[1024];
            std::string pending;
            DWORD bytes_read = 0;
            while (ReadFile(read_pipe, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
                pending.append(buf, bytes_read);
                size_t nl = 0;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, nl);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    pending.erase(0, nl + 1);
                    dispatch_line(line);
                }
            }
            if (!pending.empty()) {
                if (pending.back() == '\r') pending.pop_back();
                dispatch_line(pending);
            }
            CloseHandle(read_pipe);
            WaitForSingleObject(hProc, 3000);
            DWORD exit_code = 0;
            if (!GetExitCodeProcess(hProc, &exit_code) || exit_code == STILL_ACTIVE) {
                exit_code = 0;
            }
            bool manual_stop = user_stopping_host_.load(std::memory_order_acquire);
            PostMessageW(hwnd_, kWmHostExited, static_cast<WPARAM>(exit_code), manual_stop ? 1 : 0);
        });
    }

    void StopHostProcess() {
        user_stopping_host_.store(true, std::memory_order_release);

        HANDLE hProc = nullptr;
        HANDLE hEvt = nullptr;
        {
            std::lock_guard<std::mutex> lk(proc_mtx_);
            hProc = host_process_;
            hEvt = host_stop_event_;
            host_process_ = nullptr;
            host_stop_event_ = nullptr;
        }

        if (hEvt) {
            SetEvent(hEvt);
        }
        if (hProc) {
            if (WaitForSingleObject(hProc, 3500) == WAIT_TIMEOUT) {
                TerminateProcess(hProc, 0);
            }
        }
        if (log_reader_thread_.joinable()) {
            log_reader_thread_.join();
        }
        if (hProc) {
            CloseHandle(hProc);
        }
        if (hEvt) {
            CloseHandle(hEvt);
        }
        host_running_.store(false, std::memory_order_release);
        running_mock_ = false;
        fps_history_.clear();
        mbps_history_.clear();
        RefreshHealthAndUi(true);
        TickInlinePreview(true);
        TickTelemetryDashboard(true);
    }

    void ShowAndCopyObsGuide() {
        const wchar_t* guide = T(
            L"【JCast for iOS — OBS Studio 滿版自適應比例設定步驟】\r\n\r\n"
            L"1. 先在本控制台插上手機（自動辨識 USB 速率並開放最大解析度選項），選好解析度後點擊「智慧一鍵啟動」。\r\n"
            L"   💡 若只想把電腦當 iPhone/iPad 外接大螢幕，直接點右下角「🖥 獨立大視窗」（按 F11 全螢幕、F12 原畫質截圖），完全不必開 OBS！\r\n"
            L"2. 開啟 OBS Studio，新增「視訊擷取裝置」➔ 選擇：JCast for iOS (Video)\r\n"
            L"3.「解析度/FPS 類型」保持「裝置預設值」：OBS 會自動抓到您鎖定的 iPhone/iPad 原生比例（滿版零黑邊）！\r\n"
            L"   （若您先開了 OBS 才連手機，只要在 OBS 來源點一下「停用 ➔ 啟用」就會自動刷新成原生比例）\r\n"
            L"4. 往下捲動到底部勾選「使用自訂音訊裝置」➔ 選擇：JCast for iOS (Audio)\r\n\r\n"
            L"（裝置名稱已複製到剪貼簿：JCast for iOS (Video)）",
            L"[JCast for iOS — OBS Studio Native Aspect Ratio Setup Guide]\r\n\r\n"
            L"1. Plug in your iPhone/iPad here (USB speed & resolution tiers auto-detect), pick a max resolution, and click 'Smart Start'.\r\n"
            L"   💡 Tip: To use your PC purely as a low-latency external monitor, click '🖥 Popout View' (F11 Fullscreen, F12 Screenshot) — no OBS needed!\r\n"
            L"2. In OBS Studio, add a 'Video Capture Device' -> select: JCast for iOS (Video)\r\n"
            L"3. Leave 'Resolution/FPS Type' on 'Device Default' so OBS automatically adapts to your device's exact native aspect ratio (zero black bars).\r\n"
            L"   (If OBS was opened before plugging in, click Deactivate -> Activate once to refresh.)\r\n"
            L"4. Scroll down and check 'Use custom audio device' -> select: JCast for iOS (Audio)\r\n\r\n"
            L"(Device name copied to clipboard: JCast for iOS (Video))");

        CopyWideTextToClipboard(hwnd_, L"JCast for iOS (Video)");
        MessageBoxW(
            hwnd_,
            guide,
            T(L"JCast for iOS — OBS 設定教學", L"JCast for iOS — OBS Setup Guide"),
            MB_ICONINFORMATION | MB_OK);
    }

    void OpenUpdateDialog(const UpdateCheckResult& info) {
        UINT dpi = GetSystemDpiSafe(hwnd_);
        UpdateModalWindow dlg(
            hinst_,
            hwnd_,
            base_dir_,
            info,
            dpi,
            lang_,
            font_normal_,
            font_bold_,
            font_title_,
            font_mono_,
            icon_small_,
            [this]() {
                if (host_running_.load(std::memory_order_acquire)) {
                    StopHostProcess();
                }
            });
        if (dlg.RunModal()) {
            AppendLogLine(T(L"[軟體更新] 已啟動自動更新程序，正在關閉目前視窗以完成替換...",
                            L"[Updater] Update launcher started — closing window to apply update..."));
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }

    void StartAsyncUpdateCheck(bool manual_trigger) {
        if (checking_update_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (manual_trigger) {
            SetWindowTextW(btn_update_, T(L"⏳ 檢查中...", L"⏳ Checking..."));
            EnableWindow(btn_update_, FALSE);
            AppendLogLine(T(L"[更新檢查] 正在連線至 GitHub Releases API 檢查最新版本...",
                            L"[Updater] Checking GitHub Releases API for latest version..."));
        }

        if (update_check_thread_.joinable()) {
            update_check_thread_.join();
        }

        HWND target_hwnd = hwnd_;
        update_check_thread_ = std::thread([this, target_hwnd, manual_trigger]() {
            UpdateCheckResult res = CheckGitHubLatestRelease(
                IOS_VCAM_GITHUB_OWNER, IOS_VCAM_GITHUB_REPO, IOS_VCAM_VER_DISPLAY_STR);
            if (shutting_down_.load(std::memory_order_acquire)) {
                checking_update_.store(false, std::memory_order_release);
                return;
            }
            auto* heap_res = new UpdateCheckResult(std::move(res));
            if (!PostMessageW(
                    target_hwnd,
                    kWmUpdateCheckDone,
                    manual_trigger ? 1 : 0,
                    reinterpret_cast<LPARAM>(heap_res))) {
                delete heap_res;
                checking_update_.store(false, std::memory_order_release);
            }
        });
    }

    void OnUpdateCheckCompleted(bool manual_trigger, const UpdateCheckResult& res) {
        checking_update_.store(false, std::memory_order_release);
        if (!host_running_.load(std::memory_order_acquire)) {
            EnableWindow(btn_update_, TRUE);
        }

        if (!res.ok) {
            std::wstring restore_lbl = cached_update_.update_available
                                           ? (std::wstring(T(L"⬆️ 升級 ", L"⬆️ Update ")) +
                                              Utf8ToWide(cached_update_.latest.tag_name))
                                           : std::wstring(T(L"🔄 檢查更新", L"🔄 Updates"));
            SetWindowTextW(btn_update_, restore_lbl.c_str());

            if (manual_trigger) {
                std::wstring werr = Utf8ToWide(res.error_message);
                AppendLogLine(std::wstring(T(L"[更新檢查失敗] ", L"[Update Check Failed] ")) + werr);
                std::wstring prompt =
                    std::wstring(T(L"無法從 GitHub Releases API 取得版本資訊：\r\n",
                                   L"Failed to query GitHub Releases API:\r\n")) +
                    werr +
                    T(L"\r\n\r\n是否直接以瀏覽器開啟 GitHub Releases 發布頁面查看？",
                      L"\r\n\r\nOpen the GitHub Releases page in your browser instead?");
                if (MessageBoxW(
                        hwnd_,
                        prompt.c_str(),
                        T(L"檢查更新失敗", L"Update Check Failed"),
                        MB_ICONWARNING | MB_YESNO) == IDYES) {
                    ShellExecuteW(
                        hwnd_,
                        L"open",
                        L"https://github.com/" IOS_VCAM_GITHUB_OWNER_W L"/" IOS_VCAM_GITHUB_REPO_W L"/releases/latest",
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
            }
            return;
        }

        cached_update_ = res;
        if (!res.update_available) {
            SetWindowTextW(btn_update_, T(L"🔄 檢查更新", L"🔄 Updates"));
            InvalidateRect(hwnd_, &banner_rect_, FALSE);
            AppendLogLine(
                std::wstring(T(L"[更新檢查] 目前已是最新版本 (v", L"[Updater] You are up to date (v")) +
                IOS_VCAM_VER_DISPLAY_WSTR +
                T(L"，GitHub 最新發布：", L", GitHub latest: ") +
                Utf8ToWide(res.latest.tag_name) + L").");

            if (manual_trigger) {
                std::wstring msg =
                    std::wstring(T(L"您目前使用的 JCast for iOS v", L"JCast for iOS v")) +
                    IOS_VCAM_VER_DISPLAY_WSTR +
                    T(L" 已是最新版本！\r\n\r\n• GitHub 最新版本：", L" is already the latest version!\r\n\r\n• Latest on GitHub: ") +
                    Utf8ToWide(res.latest.tag_name) + L"\r\n" +
                    (res.latest.published_at.empty()
                         ? L""
                         : (std::wstring(T(L"• 發布時間：", L"• Published: ")) +
                            Utf8ToWide(res.latest.published_at) + L"\r\n")) +
                    T(L"\r\n是否開啟 GitHub Releases 網頁查看完整歷史更新日誌？",
                      L"\r\nOpen GitHub Releases in your browser to view the full changelog?");
                if (MessageBoxW(
                        hwnd_,
                        msg.c_str(),
                        T(L"已是最新版本", L"Up to Date"),
                        MB_ICONINFORMATION | MB_YESNO) == IDYES) {
                    OpenSafeGitHubWebUrl(hwnd_, res.latest.html_url);
                }
            }
            return;
        }

        std::wstring up_lbl = std::wstring(T(L"⬆️ 升級 ", L"⬆️ Update ")) + Utf8ToWide(res.latest.tag_name);
        SetWindowTextW(btn_update_, up_lbl.c_str());
        InvalidateRect(hwnd_, &banner_rect_, FALSE);
        AppendLogLine(
            std::wstring(T(L"[發現新版本] 🎉 GitHub 已發布 JCast for iOS ",
                           L"[Update Available] 🎉 JCast for iOS ")) +
            Utf8ToWide(res.latest.tag_name) +
            T(L"！點擊「", L" is available! Click '") + up_lbl +
            T(L"」即可檢視更新日誌並一鍵自動升級。", L"' to view release notes and auto-upgrade."));

        if (manual_trigger && !host_running_.load(std::memory_order_acquire)) {
            OpenUpdateDialog(cached_update_);
        }
    }

    void ShowInlineToast(const std::wstring& text, uint64_t duration_ms = 2400) {
        inline_toast_text_ = text;
        inline_toast_until_ms_ = GetTickCount64() + duration_ms;
        if (hwnd_) {
            InvalidateRect(hwnd_, &preview_rect_, FALSE);
        }
    }

    void TickInlinePreview(bool force) {
        if (!hwnd_ || IsIconic(hwnd_)) return;

        bool toast_expired = (!inline_toast_text_.empty() && GetTickCount64() >= inline_toast_until_ms_);
        if (toast_expired) {
            inline_toast_text_.clear();
        }

        if (!inline_preview_enabled_) {
            if (inline_has_frame_ || toast_expired) {
                inline_has_frame_ = false;
                inline_last_seq_ = 0;
                inline_bgra_.clear();
                InvalidateRect(hwnd_, &preview_rect_, FALSE);
            }
            return;
        }

        uint32_t pw = static_cast<uint32_t>(std::max<LONG>(4, (preview_rect_.right - preview_rect_.left) - 2));
        uint32_t ph = static_cast<uint32_t>(std::max<LONG>(4, (preview_rect_.bottom - preview_rect_.top) - 2));

        if (!preview_shm_.IsOpen()) {
            preview_shm_.Open(kDefaultShmName);
        }

        bool updated = false;
        uint32_t fit_w = 0, fit_h = 0, sw = 0, sh = 0, kbps = 0;
        bool alive = preview_shm_.ReadLatestPreviewBGRA32(
            pw,
            ph,
            force || inline_force_refresh_,
            inline_last_seq_,
            inline_bgra_,
            fit_w,
            fit_h,
            sw,
            sh,
            &updated,
            &kbps);
        inline_force_refresh_ = false;

        if (alive) {
            bool state_changed = (!inline_has_frame_ || sw != inline_src_w_ || sh != inline_src_h_);
            inline_has_frame_ = true;
            inline_fit_w_ = fit_w;
            inline_fit_h_ = fit_h;
            inline_src_w_ = sw;
            inline_src_h_ = sh;
            inline_bitrate_kbps_ = kbps;
            if (updated || state_changed || toast_expired) {
                InvalidateRect(hwnd_, &preview_rect_, FALSE);
            }
        } else if (inline_has_frame_ || toast_expired) {
            inline_has_frame_ = false;
            inline_last_seq_ = 0;
            inline_src_w_ = 0;
            inline_src_h_ = 0;
            inline_bitrate_kbps_ = 0;
            InvalidateRect(hwnd_, &preview_rect_, FALSE);
        }
    }

    void TickTelemetryDashboard(bool force) {
        if (!hwnd_ || IsIconic(hwnd_)) return;

        if (!telemetry_shm_.IsOpen()) {
            telemetry_shm_.Open(kDefaultShmName);
        }

        StreamHealthSnapshot prev = telem_snap_;
        bool ok = telemetry_shm_.ReadStreamHealth(telem_snap_);
        if (!ok && host_running_.load(std::memory_order_acquire)) {
            telem_snap_ = StreamHealthSnapshot{};
            telem_snap_.active = true;
            telem_snap_.waiting_first_frame = true;
            telem_snap_.grade = StreamHealthGrade::WaitingStream;
        }

        constexpr size_t kMaxSparkSamples = 28;
        if (telem_snap_.active && !telem_snap_.waiting_first_frame) {
            fps_history_.push_back(telem_snap_.fps);
            if (fps_history_.size() > kMaxSparkSamples) {
                fps_history_.erase(fps_history_.begin());
            }
            mbps_history_.push_back(telem_snap_.bitrate_mbps);
            if (mbps_history_.size() > kMaxSparkSamples) {
                mbps_history_.erase(mbps_history_.begin());
            }
        } else if (!telem_snap_.active && (!fps_history_.empty() || !mbps_history_.empty())) {
            fps_history_.clear();
            mbps_history_.clear();
        }

        if (force || telem_snap_.active || prev.active != telem_snap_.active ||
            prev.grade != telem_snap_.grade) {
            InvalidateRect(hwnd_, &telemetry_rect_, FALSE);
            if (inline_preview_enabled_ && inline_has_frame_) {
                InvalidateRect(hwnd_, &preview_rect_, FALSE);
            }
        }
    }

    void PaintTelemetryDashboard(HDC dst_hdc) {
        int w = telemetry_rect_.right - telemetry_rect_.left;
        int h = telemetry_rect_.bottom - telemetry_rect_.top;
        if (w <= 8 || h <= 8) return;

        const bool en = (lang_ == AppLanguage::EnUS);
        UINT dpi = GetSystemDpiSafe(hwnd_);
        HDC mem_dc = CreateCompatibleDC(dst_hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(dst_hdc, w, h);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

        RECT local_rc{0, 0, w, h};
        HBRUSH bg_br = CreateSolidBrush(RGB(16, 20, 29));
        FillRect(mem_dc, &local_rc, bg_br);
        DeleteObject(bg_br);

        SetBkMode(mem_dc, TRANSPARENT);
        HGDIOBJ old_font = SelectObject(mem_dc, font_small_);

        const StreamHealthSnapshot& s = telem_snap_;
        const bool streaming = s.active && !s.waiting_first_frame;
        const int card_w = w / 4;
        const int pad_x = Scale(11, dpi);
        const int accent_h = std::max(2, Scale(3, dpi));

        auto draw_card_frame = [&](int idx, COLORREF accent) {
            int x0 = idx * card_w;
            int x1 = (idx == 3) ? w : ((idx + 1) * card_w);
            RECT top_bar{x0 + 1, 1, x1 - 1, 1 + accent_h};
            HBRUSH acc_br = CreateSolidBrush(accent);
            FillRect(mem_dc, &top_bar, acc_br);
            DeleteObject(acc_br);
            if (idx > 0) {
                RECT div_rc{x0, Scale(8, dpi), x0 + 1, h - Scale(8, dpi)};
                HBRUSH div_br = CreateSolidBrush(RGB(36, 46, 64));
                FillRect(mem_dc, &div_rc, div_br);
                DeleteObject(div_br);
            }
        };

        // --- Card 0: Real-time FPS + Sparkline ---
        COLORREF fps_col = RGB(100, 116, 139);
        std::wstring fps_val = L"--.- fps";
        std::wstring fps_sub = T(L"目標 60fps 低抖動輸出", L"Target 60fps low-jitter output");
        if (s.waiting_first_frame) {
            fps_col = RGB(251, 191, 36);
            fps_val = T(L"等待中...", L"Waiting...");
            fps_sub = T(L"等待手機傳送首張影格", L"Waiting for first frame");
        } else if (streaming) {
            wchar_t vbuf[48] = {0};
            std::swprintf(vbuf, 48, L"%.1f fps", s.fps);
            fps_val = vbuf;
            if (s.grade == StreamHealthGrade::StaticVfr) {
                fps_col = RGB(96, 165, 250);
            } else if (s.fps >= 45.0) {
                fps_col = RGB(74, 222, 128);
            } else if (s.fps >= 22.0) {
                fps_col = RGB(251, 191, 36);
            } else {
                fps_col = RGB(248, 113, 113);
            }
            wchar_t sbuf[80] = {0};
            std::swprintf(
                sbuf,
                80,
                en ? L"Total %llu f | Drop %u" : L"累計 %llu 幀 ｜ 掉幀 %u",
                static_cast<unsigned long long>(s.total_frames),
                s.dropped_frames);
            fps_sub = sbuf;
        }
        draw_card_frame(0, streaming ? fps_col : RGB(51, 65, 85));
        {
            int x0 = 0, x1 = card_w;
            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(148, 163, 184));
            RECT hdr_rc{x0 + pad_x, Scale(6, dpi), x1 - Scale(64, dpi), Scale(23, dpi)};
            DrawTextW(mem_dc, T(L"🎞 即時幀率", L"🎞 Live FPS"), -1, &hdr_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT spk_rc{x1 - Scale(60, dpi), Scale(6, dpi), x1 - Scale(8, dpi), Scale(23, dpi)};
            DrawSparklineGdi(mem_dc, spk_rc, fps_history_, 65.0, fps_col);

            SelectObject(mem_dc, font_metric_);
            SetTextColor(mem_dc, fps_col);
            RECT val_rc{x0 + pad_x, Scale(23, dpi), x1 - pad_x, Scale(50, dpi)};
            DrawTextW(mem_dc, fps_val.c_str(), -1, &val_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(130, 145, 168));
            RECT sub_rc{x0 + pad_x, Scale(50, dpi), x1 - Scale(6, dpi), h - Scale(3, dpi)};
            DrawTextW(mem_dc, fps_sub.c_str(), -1, &sub_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // --- Card 1: Real-time Bitrate Mbps + Sparkline ---
        COLORREF mbps_col = streaming ? RGB(56, 189, 248) : RGB(100, 116, 139);
        std::wstring mbps_val = L"--.- Mbps";
        std::wstring mbps_sub = T(L"USB 動態高畫質 H.264", L"Dynamic USB H.264 stream");
        if (s.waiting_first_frame) {
            mbps_val = L"0.0 Mbps";
            mbps_sub = T(L"握手協調串流通道中", L"Negotiating USB stream");
        } else if (streaming) {
            wchar_t vbuf[48] = {0};
            std::swprintf(vbuf, 48, L"%.1f Mbps", s.bitrate_mbps);
            mbps_val = vbuf;
            std::wstring tot_str = FormatBytesCompactWide(s.total_video_bytes);
            wchar_t sbuf[96] = {0};
            std::swprintf(
                sbuf,
                96,
                en ? L"Peak %.1fM | Total %ls" : L"峰值 %.1fM ｜ 累計 %ls",
                s.peak_bitrate_mbps,
                tot_str.c_str());
            mbps_sub = sbuf;
        }
        draw_card_frame(1, streaming ? mbps_col : RGB(51, 65, 85));
        {
            int x0 = card_w, x1 = card_w * 2;
            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(148, 163, 184));
            RECT hdr_rc{x0 + pad_x, Scale(6, dpi), x1 - Scale(64, dpi), Scale(23, dpi)};
            DrawTextW(mem_dc, T(L"📡 傳輸碼率", L"📡 Bitrate"), -1, &hdr_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT spk_rc{x1 - Scale(60, dpi), Scale(6, dpi), x1 - Scale(8, dpi), Scale(23, dpi)};
            double spk_max = std::max(12.0, s.peak_bitrate_mbps * 1.15);
            DrawSparklineGdi(mem_dc, spk_rc, mbps_history_, spk_max, RGB(56, 189, 248));

            SelectObject(mem_dc, font_metric_);
            SetTextColor(mem_dc, mbps_col);
            RECT val_rc{x0 + pad_x, Scale(23, dpi), x1 - pad_x, Scale(50, dpi)};
            DrawTextW(mem_dc, mbps_val.c_str(), -1, &val_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(130, 145, 168));
            RECT sub_rc{x0 + pad_x, Scale(50, dpi), x1 - Scale(6, dpi), h - Scale(3, dpi)};
            DrawTextW(mem_dc, mbps_sub.c_str(), -1, &sub_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // --- Card 2: Decode & Pipeline Latency (ms) + Engine Pill ---
        COLORREF lat_col = RGB(100, 116, 139);
        std::wstring lat_val = L"--.- ms";
        std::wstring lat_sub = T(L"H.264 解碼 + 畫布處理", L"H.264 decode + canvas");
        if (streaming) {
            wchar_t vbuf[48] = {0};
            std::swprintf(vbuf, 48, L"%.1f ms", s.pipeline_latency_ms);
            lat_val = vbuf;
            if (s.pipeline_latency_ms <= 6.5) {
                lat_col = RGB(74, 222, 128);
            } else if (s.pipeline_latency_ms <= 14.0) {
                lat_col = RGB(251, 191, 36);
            } else {
                lat_col = RGB(248, 113, 113);
            }
            int budget_pct = std::clamp(static_cast<int>(s.pipeline_latency_ms * 100.0 / 16.667 + 0.5), 1, 999);
            wchar_t sbuf[96] = {0};
            std::swprintf(
                sbuf,
                96,
                en ? L"Dec %.1fms | 60fps %d%%" : L"純解碼 %.1fms ｜ 60幀預算 %d%%",
                s.decode_latency_ms,
                budget_pct);
            lat_sub = sbuf;
        }
        draw_card_frame(2, streaming ? lat_col : RGB(51, 65, 85));
        {
            int x0 = card_w * 2, x1 = card_w * 3;
            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(148, 163, 184));
            RECT hdr_rc{x0 + pad_x, Scale(6, dpi), x1 - Scale(66, dpi), Scale(23, dpi)};
            DrawTextW(mem_dc, T(L"⚡ 解碼延遲", L"⚡ Latency"), -1, &hdr_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (streaming) {
                const wchar_t* eng_txt = s.mock_source
                                             ? T(L"自測", L"Mock")
                                             : (s.hw_accelerated ? L"D3D11VA" : T(L"CPU軟解", L"CPU SW"));
                COLORREF pill_bg = s.mock_source
                                       ? RGB(14, 58, 78)
                                       : (s.hw_accelerated ? RGB(14, 68, 44) : RGB(84, 52, 12));
                COLORREF pill_fg = s.mock_source
                                       ? RGB(125, 211, 252)
                                       : (s.hw_accelerated ? RGB(110, 231, 183) : RGB(253, 211, 77));
                RECT pill_rc{x1 - Scale(64, dpi), Scale(6, dpi), x1 - Scale(8, dpi), Scale(23, dpi)};
                HBRUSH pb = CreateSolidBrush(pill_bg);
                FillRect(mem_dc, &pill_rc, pb);
                DeleteObject(pb);
                SetTextColor(mem_dc, pill_fg);
                DrawTextW(mem_dc, eng_txt, -1, &pill_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            SelectObject(mem_dc, font_metric_);
            SetTextColor(mem_dc, lat_col);
            RECT val_rc{x0 + pad_x, Scale(23, dpi), x1 - pad_x, Scale(50, dpi)};
            DrawTextW(mem_dc, lat_val.c_str(), -1, &val_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(130, 145, 168));
            RECT sub_rc{x0 + pad_x, Scale(50, dpi), x1 - Scale(6, dpi), h - Scale(3, dpi)};
            DrawTextW(mem_dc, lat_sub.c_str(), -1, &sub_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // --- Card 3: Overall Stream Health Grade, Fault Alert & Click-to-Diagnose ---
        COLORREF grd_col = RGB(148, 163, 184);
        std::wstring grd_val = T(L"⚪ 待機未啟動", L"⚪ Idle / Standby");
        std::wstring grd_sub = T(L"點擊開啟 8 項環境自檢與報錯", L"Click for 8-point self-check");
        switch (s.grade) {
            case StreamHealthGrade::Excellent:
                grd_col = RGB(74, 222, 128);
                grd_val = s.mock_source
                              ? T(L"🟢 極佳 (自測)", L"🟢 Optimal (Mock)")
                              : T(L"🟢 極佳 · 低延遲", L"🟢 Optimal · Low Lag");
                break;
            case StreamHealthGrade::StaticVfr:
                grd_col = RGB(96, 165, 250);
                grd_val = T(L"🔵 靜態省電 VFR", L"🔵 Static Idle VFR");
                break;
            case StreamHealthGrade::Warning:
                grd_col = RGB(251, 191, 36);
                grd_val = (s.pipeline_latency_ms > 14.0 || s.decode_latency_ms > 12.0)
                              ? T(L"🟡 解碼負載偏高", L"🟡 High Decode Load")
                              : T(L"🟡 幀率/頻寬受限", L"🟡 FPS / BW Limited");
                break;
            case StreamHealthGrade::WaitingStream:
                grd_col = RGB(251, 191, 36);
                grd_val = T(L"⏳ 等待手機訊號", L"⏳ Waiting for iOS");
                grd_sub = T(L"請解鎖手機並點信任電腦", L"Unlock & tap Trust on phone");
                break;
            case StreamHealthGrade::Idle:
            default:
                break;
        }
        if (streaming) {
            if (s.grade == StreamHealthGrade::StaticVfr) {
                grd_sub = T(L"畫面靜止自動降頻 (滑動即60幀)", L"Static screen VFR (60fps on move)");
            } else if (s.grade == StreamHealthGrade::Warning && !s.hw_accelerated) {
                grd_sub = T(L"建議勾選開啟 D3D11VA 硬解", L"Enable D3D11VA GPU decode");
            } else {
                wchar_t sbuf[96] = {0};
                std::swprintf(
                    sbuf,
                    96,
                    en ? L"%u×%u | Aud %llu pkts" : L"%u×%u ｜ 音訊 %llu 包",
                    s.frame_width,
                    s.frame_height,
                    static_cast<unsigned long long>(s.total_audio_packets));
                grd_sub = sbuf;
            }
        }

        bool highlight_fault_on_card = false;
        if (!recent_faults_.empty()) {
            const auto& latest_f = recent_faults_.back();
            bool stream_runtime_fault =
                (latest_f.code == DiagCode::E402_GpuFallbackToSoftware ||
                 latest_f.code == DiagCode::E403_HighDecodeLatency ||
                 latest_f.code == DiagCode::E501_AudioEndpointLost);
            if (!streaming || stream_runtime_fault || latest_f.severity >= DiagSeverity::Error) {
                highlight_fault_on_card = true;
                const auto& fspec = LookupDiagSpec(latest_f.code, lang_);
                grd_col = (latest_f.severity >= DiagSeverity::Error)
                              ? RGB(248, 113, 113)
                              : RGB(251, 191, 36);
                grd_val = std::wstring((latest_f.severity >= DiagSeverity::Error)
                                           ? T(L"❌ 錯誤 ", L"❌ Error ")
                                           : T(L"⚠️ 異常 ", L"⚠️ Alert ")) +
                          Utf8ToWide(fspec.code_str);
                grd_sub = T(L"點擊開啟診斷中心查看解法", L"Click to view fix & report");
            }
        }

        draw_card_frame(3, (s.active || highlight_fault_on_card) ? grd_col : RGB(51, 65, 85));
        {
            int x0 = card_w * 3, x1 = w;
            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(148, 163, 184));
            RECT hdr_rc{x0 + pad_x, Scale(6, dpi), x1 - pad_x, Scale(23, dpi)};
            DrawTextW(
                mem_dc,
                T(L"🩺 健康度診斷 (點擊自檢)", L"🩺 Health (Click Diag)"),
                -1,
                &hdr_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(mem_dc, font_bold_);
            SetTextColor(mem_dc, grd_col);
            RECT val_rc{x0 + pad_x, Scale(24, dpi), x1 - Scale(6, dpi), Scale(49, dpi)};
            DrawTextW(mem_dc, grd_val.c_str(), -1, &val_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(mem_dc, font_small_);
            SetTextColor(mem_dc, RGB(130, 145, 168));
            RECT sub_rc{x0 + pad_x, Scale(50, dpi), x1 - Scale(6, dpi), h - Scale(3, dpi)};
            DrawTextW(mem_dc, grd_sub.c_str(), -1, &sub_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        HBRUSH border_br = CreateSolidBrush(RGB(52, 65, 88));
        FrameRect(mem_dc, &local_rc, border_br);
        DeleteObject(border_br);

        SelectObject(mem_dc, old_font);
        BitBlt(dst_hdc, telemetry_rect_.left, telemetry_rect_.top, w, h, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
    }

    RECT GetTelemetryDiagCardRect() const {
        int w = telemetry_rect_.right - telemetry_rect_.left;
        return RECT{
            telemetry_rect_.left + (w * 3 / 4),
            telemetry_rect_.top,
            telemetry_rect_.right,
            telemetry_rect_.bottom,
        };
    }

    void PaintInlinePreview(HDC hdc) {
        std::wstring badge;
        bool is_toast = false;
        if (!inline_toast_text_.empty() && GetTickCount64() < inline_toast_until_ms_) {
            badge = inline_toast_text_;
            is_toast = true;
        } else if (inline_preview_enabled_ && inline_has_frame_ && inline_src_w_ > 0 && inline_src_h_ > 0) {
            if (telem_snap_.active && !telem_snap_.waiting_first_frame) {
                wchar_t bbuf[96] = {0};
                std::swprintf(
                    bbuf,
                    96,
                    L"● LIVE %u×%u · %.0ffps · %.1fms",
                    inline_src_w_,
                    inline_src_h_,
                    telem_snap_.fps,
                    telem_snap_.pipeline_latency_ms);
                badge = bbuf;
            } else {
                badge = L"● LIVE " + std::to_wstring(inline_src_w_) + L"×" + std::to_wstring(inline_src_h_);
            }
        }

        std::wstring idle_title = inline_preview_enabled_
                                      ? T(L"🎬 低負載即時預覽待命中", L"🎬 Low-Load Live Preview Standby")
                                      : T(L"⏸ 已暫停內建預覽（0% 額外負載）", L"⏸ Inline Preview Paused (0% Extra Load)");
        std::wstring idle_sub = inline_preview_enabled_
                                    ? T(L"啟動投影後自動顯示（雙擊畫面開啟大視窗）",
                                        L"Auto-starts with capture (Double-click to pop out)")
                                    : T(L"OBS 虛擬擷取卡與「📸 原畫質截圖」仍正常運作",
                                        L"OBS virtual camera & F12 Snapshot remain active");

        DrawPreviewCanvasGdi(
            hdc,
            preview_rect_,
            inline_preview_enabled_ && inline_has_frame_,
            inline_bgra_,
            inline_fit_w_,
            inline_fit_h_,
            true,
            badge,
            is_toast,
            idle_title,
            idle_sub,
            font_bold_,
            font_normal_);
    }

    void TakeFullResolutionScreenshot(HWND source_hwnd) {
        if (!preview_shm_.IsOpen()) {
            preview_shm_.Open(kDefaultShmName);
        }

        std::vector<uint8_t> full_bgra;
        uint32_t w = 0, h = 0;
        if (!preview_shm_.CaptureLatestFullResBGRA32(full_bgra, w, h)) {
            AppendLogLine(T(
                L"[截圖提示] 目前尚無擷取畫面，請先點擊「🚀 智慧一鍵啟動」或「🧪 免手機自測」後再截圖。",
                L"[Screenshot] No active video frame yet. Click 'Smart Start' or 'Mock Test' first."));
            if (live_monitor_ && source_hwnd == live_monitor_->Hwnd()) {
                live_monitor_->ShowToast(
                    T(L"⚠️ 尚無投影訊號可截圖，請先啟動投影", L"⚠️ No live signal to capture yet"), 2200);
            } else {
                ShowInlineToast(T(L"⚠️ 尚無投影訊號可截圖", L"⚠️ No live signal yet"), 2200);
            }
            return;
        }

        std::filesystem::path dir = GetDefaultScreenshotDirectory(base_dir_);
        std::filesystem::path png_path = BuildTimestampedScreenshotPath(dir, w, h);

        std::string err;
        bool png_ok = SaveTopDownBGRA32AsPng(full_bgra.data(), w, h, png_path, &err);
        HWND clip_owner = (source_hwnd && IsWindow(source_hwnd)) ? source_hwnd : hwnd_;
        bool clip_ok = CopyTopDownBGRA32ToClipboard(clip_owner, full_bgra.data(), w, h);

        if (png_ok) {
            std::wstring toast =
                std::wstring(T(L"📸 已截圖 ", L"📸 Captured ")) + std::to_wstring(w) + L"×" + std::to_wstring(h) +
                (clip_ok ? T(L" (PNG + 已複製剪貼簿)", L" (PNG + Clipboard)")
                         : T(L" (已存 PNG)", L" (PNG Saved)"));
            ShowInlineToast(toast, 2500);
            if (live_monitor_ && live_monitor_->IsOpen()) {
                live_monitor_->ShowToast(toast, 2500);
            }
            AppendLogLine(
                std::wstring(T(L"[原畫質截圖] 📸 已儲存 1:1 無損 PNG (",
                               L"[Screenshot] 📸 Saved 1:1 lossless PNG (")) +
                std::to_wstring(w) + L"×" + std::to_wstring(h) + L")" +
                (clip_ok ? T(L" 並複製至剪貼簿 (可直接 Ctrl+V 貼上)", L" & copied to clipboard (Ctrl+V ready)") : L"") +
                L": " + png_path.wstring());
        } else {
            AppendLogLine(
                std::wstring(T(L"[截圖失敗] 無法寫入 PNG 檔案：", L"[Screenshot Failed] Unable to save PNG: ")) +
                Utf8ToWide(err));
            ShowInlineToast(T(L"❌ 截圖儲存失敗", L"❌ Screenshot save failed"), 2500);
            if (live_monitor_ && live_monitor_->IsOpen()) {
                live_monitor_->ShowToast(T(L"❌ 截圖儲存失敗", L"❌ Screenshot save failed"), 2500);
            }
        }
    }

    void OpenScreenshotsFolder() {
        std::filesystem::path dir = GetDefaultScreenshotDirectory(base_dir_);
        ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ControlCenterWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ControlCenterWindow*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ControlCenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->WndProc(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE:
                BuildControls();
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd_, &ps);
                PaintStatusBanner(hdc);
                PaintTelemetryDashboard(hdc);
                PaintInlinePreview(hdc);
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_SETCURSOR: {
                if (reinterpret_cast<HWND>(wParam) == hwnd_ && LOWORD(lParam) == HTCLIENT) {
                    POINT pt{};
                    RECT diag_rc = GetTelemetryDiagCardRect();
                    if (GetCursorPos(&pt) && ScreenToClient(hwnd_, &pt) &&
                        (PtInRect(&preview_rect_, pt) || PtInRect(&diag_rc, pt))) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
                break;
            }

            case WM_LBUTTONDOWN: {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT diag_rc = GetTelemetryDiagCardRect();
                if (PtInRect(&diag_rc, pt)) {
                    OpenDiagnosticsModal();
                    return 0;
                }
                break;
            }

            case WM_LBUTTONDBLCLK: {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (PtInRect(&preview_rect_, pt) && live_monitor_) {
                    live_monitor_->ShowOrActivate(GetSystemDpiSafe(hwnd_));
                    return 0;
                }
                break;
            }

            case WM_CTLCOLORSTATIC: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                HWND ctrl = reinterpret_cast<HWND>(lParam);
                if (ctrl != edit_log_) {
                    SetBkMode(hdc, TRANSPARENT);
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
                }
                break;
            }

            case WM_TIMER:
                if (wParam == kTimerRefreshId) {
                    RefreshHealthAndUi(false);
                } else if (wParam == kTimerStartupUpdateId) {
                    KillTimer(hwnd_, kTimerStartupUpdateId);
                    if (!host_running_.load(std::memory_order_acquire)) {
                        StartAsyncUpdateCheck(false);
                    }
                } else if (wParam == kTimerPreviewId) {
                    TickInlinePreview(false);
                } else if (wParam == kTimerTelemetryId) {
                    TickTelemetryDashboard(false);
                }
                return 0;

            case WM_DEVICECHANGE:
                if (wParam == DBT_DEVNODES_CHANGED) {
                    RefreshHealthAndUi(true);
                }
                return TRUE;

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                int code = HIWORD(wParam);
                if (code == CBN_SELCHANGE && id == kIdComboResMode) {
                    user_manually_picked_res_ = true;
                    return 0;
                }
                if (code == CBN_SELCHANGE && id == kIdComboLanguage) {
                    LRESULT sel = SendMessageW(combo_language_, CB_GETCURSEL, 0, 0);
                    AppLanguagePref next_pref = AppLanguagePref::Auto;
                    if (sel == 1) next_pref = AppLanguagePref::ZhTW;
                    else if (sel == 2) next_pref = AppLanguagePref::EnUS;
                    lang_pref_ = next_pref;
                    SaveLanguagePref(lang_pref_);
                    lang_ = ResolveEffectiveLanguage(lang_pref_, static_cast<uint16_t>(GetUserDefaultUILanguage()));
                    ApplyLanguageToUi();
                    AppendLogLine(T(
                        L"[語言設定] 🌐 已切換介面語言為「繁體中文」（設定已自動儲存）。",
                        L"[Language] 🌐 Interface language switched to English (preference saved)."));
                    return 0;
                }
                if (code == BN_CLICKED) {
                    if (id == kIdChkInlinePreview) {
                        inline_preview_enabled_ =
                            (SendMessageW(chk_inline_preview_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        inline_force_refresh_ = true;
                        TickInlinePreview(true);
                        InvalidateRect(hwnd_, &preview_rect_, FALSE);
                        return 0;
                    }
                    if (id == kIdBtnPopoutMonitor) {
                        if (live_monitor_) {
                            live_monitor_->ShowOrActivate(GetSystemDpiSafe(hwnd_));
                        }
                        return 0;
                    }
                    if (id == kIdBtnScreenshot) {
                        TakeFullResolutionScreenshot(hwnd_);
                        return 0;
                    }
                    if (id == kIdBtnOpenScreenshots) {
                        OpenScreenshotsFolder();
                        return 0;
                    }
                    if (id == kIdBtnSmartStart) {
                        if (host_running_.load(std::memory_order_acquire)) {
                            AppendLogLine(T(L"[操作] 正在停止投影擷取並恢復標準 USB 模式...",
                                            L"[Action] Stopping capture and restoring normal USB mode..."));
                            StopHostProcess();
                            AppendLogLine(T(L"[完成] 已停止投影擷取。", L"[Done] Capture stopped."));
                        } else {
                            StartHostProcess(false);
                        }
                        return 0;
                    }
                    if (id == kIdBtnMockTest) {
                        if (host_running_.load(std::memory_order_acquire)) {
                            StopHostProcess();
                            AppendLogLine(T(L"[完成] 已停止自測訊號。", L"[Done] Mock test signal stopped."));
                        } else {
                            StartHostProcess(true);
                        }
                        return 0;
                    }
                    if (id == kIdBtnRepairDriver) {
                        RunDriverSetupElevated(L"--install");
                        return 0;
                    }
                    if (id == kIdBtnDiagnostics) {
                        OpenDiagnosticsModal();
                        return 0;
                    }
                    if (id == kIdBtnCopyObsGuide) {
                        ShowAndCopyObsGuide();
                        return 0;
                    }
                    if (id == kIdBtnCheckUpdate) {
                        if (cached_update_.update_available) {
                            OpenUpdateDialog(cached_update_);
                        } else {
                            StartAsyncUpdateCheck(true);
                        }
                        return 0;
                    }
                    if (id == kIdBtnUninstall) {
                        if (MessageBoxW(
                                hwnd_,
                                T(L"確定要移除 Apple USB 過濾層並反註冊 JCast for iOS 虛擬擷取卡嗎？",
                                  L"Remove the Apple USB filter driver and unregister the JCast for iOS virtual capture card?"),
                                T(L"確認解除安裝", L"Confirm Uninstall"),
                                MB_ICONQUESTION | MB_YESNO) == IDYES) {
                            if (host_running_.load(std::memory_order_acquire)) {
                                StopHostProcess();
                            }
                            if (RunDriverSetupElevated(L"--uninstall")) {
                                AppendLogLine(T(L"[完成] 已完整移除驅動過濾層與虛擬擷取卡註冊。",
                                                L"[Done] Driver filter and virtual capture card removed cleanly."));
                            }
                        }
                        return 0;
                    }
                }
                break;
            }

            case kWmAppendLog: {
                auto* str = reinterpret_cast<std::wstring*>(lParam);
                if (str) {
                    AppendLogLine(*str);
                    delete str;
                }
                return 0;
            }

            case kWmHostDiagEvent: {
                auto* ev = reinterpret_cast<ParsedDiagEvent*>(lParam);
                if (ev) {
                    if (ev->severity == DiagSeverity::Critical) {
                        session_critical_fault_seen_ = true;
                    }
                    RecordDiagnosticFault(ev->code, ev->severity, Utf8ToWide(ev->detail_utf8), false);
                    delete ev;
                }
                return 0;
            }

            case kWmHostExited: {
                uint32_t exit_code = static_cast<uint32_t>(wParam);
                bool manual_stop = (lParam != 0);
                if (host_running_.load(std::memory_order_acquire)) {
                    StopHostProcess();
                    AppendLogLine(T(L"[狀態] 擷取背景程序已結束。", L"[Status] Background capture process exited."));
                }
                if (!manual_stop && !shutting_down_.load(std::memory_order_acquire) && exit_code != 0) {
                    if (!session_critical_fault_seen_) {
                        RecordDiagnosticFault(
                            DiagCode::E503_HostUnexpectedExit,
                            DiagSeverity::Critical,
                            Utf8ToWide(DescribeProcessExitCode(exit_code, lang_)),
                            true);
                    } else {
                        OpenDiagnosticsModal();
                    }
                }
                return 0;
            }

            case kWmUpdateCheckDone: {
                auto* res = reinterpret_cast<UpdateCheckResult*>(lParam);
                if (res) {
                    OnUpdateCheckCompleted(wParam != 0, *res);
                    delete res;
                }
                return 0;
            }

            case WM_CLOSE:
                shutting_down_.store(true, std::memory_order_release);
                KillTimer(hwnd_, kTimerRefreshId);
                KillTimer(hwnd_, kTimerStartupUpdateId);
                KillTimer(hwnd_, kTimerPreviewId);
                KillTimer(hwnd_, kTimerTelemetryId);
                if (live_monitor_) {
                    live_monitor_->CloseMonitor();
                }
                preview_shm_.Close();
                telemetry_shm_.Close();
                if (host_running_.load(std::memory_order_acquire)) {
                    StopHostProcess();
                }
                if (update_check_thread_.joinable()) {
                    update_check_thread_.join();
                }
                DestroyWindow(hwnd_);
                return 0;

            case WM_DESTROY:
                if (font_normal_) DeleteObject(font_normal_);
                if (font_bold_) DeleteObject(font_bold_);
                if (font_title_) DeleteObject(font_title_);
                if (font_mono_) DeleteObject(font_mono_);
                if (font_metric_) DeleteObject(font_metric_);
                if (font_small_) DeleteObject(font_small_);
                if (icon_big_) DestroyIcon(icon_big_);
                if (icon_small_) DestroyIcon(icon_small_);
                if (icon_banner_) DestroyIcon(icon_banner_);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    std::filesystem::path base_dir_;
    AppLanguagePref lang_pref_ = AppLanguagePref::Auto;
    AppLanguage lang_ = AppLanguage::ZhTW;
    SystemHealth health_;
    IosUsbState prev_ios_usb_ = IosUsbState::Unplugged;
    UsbLinkSpeed detected_usb_speed_ = UsbLinkSpeed::Unknown;
    bool user_manually_picked_res_ = false;
    std::vector<ResOptionSpec> res_options_;

    HICON icon_big_ = nullptr;
    HICON icon_small_ = nullptr;
    HICON icon_banner_ = nullptr;

    RECT banner_rect_{};
    RECT telemetry_rect_{};
    RECT preview_rect_{};
    HWND grp_pref_ = nullptr;
    HWND lbl_res_ = nullptr;
    HWND lbl_bar_ = nullptr;
    HWND lbl_aud_ = nullptr;
    HWND lbl_lang_ = nullptr;
    HWND grp_act_ = nullptr;
    HWND lbl_log_ = nullptr;
    HWND combo_res_mode_ = nullptr;
    HWND combo_status_bar_ = nullptr;
    HWND combo_audio_ = nullptr;
    HWND combo_language_ = nullptr;
    HWND chk_hw_decode_ = nullptr;
    HWND chk_inline_preview_ = nullptr;
    HWND btn_smart_start_ = nullptr;
    HWND btn_mock_ = nullptr;
    HWND btn_repair_ = nullptr;
    HWND btn_diagnostics_ = nullptr;
    HWND btn_obs_guide_ = nullptr;
    HWND btn_update_ = nullptr;
    HWND btn_uninstall_ = nullptr;
    HWND btn_popout_monitor_ = nullptr;
    HWND btn_screenshot_ = nullptr;
    HWND btn_open_shots_ = nullptr;
    HWND edit_log_ = nullptr;

    HFONT font_normal_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_mono_ = nullptr;
    HFONT font_metric_ = nullptr;
    HFONT font_small_ = nullptr;

    std::vector<std::wstring> audio_endpoint_matches_;
    std::vector<DiagEventRecord> recent_faults_;
    bool session_critical_fault_seen_ = false;

    SharedCaptureConsumer telemetry_shm_;
    StreamHealthSnapshot telem_snap_{};
    std::vector<double> fps_history_;
    std::vector<double> mbps_history_;

    SharedCaptureConsumer preview_shm_;
    bool inline_preview_enabled_ = true;
    bool inline_force_refresh_ = true;
    bool inline_has_frame_ = false;
    uint64_t inline_last_seq_ = 0;
    uint32_t inline_fit_w_ = 0;
    uint32_t inline_fit_h_ = 0;
    uint32_t inline_src_w_ = 0;
    uint32_t inline_src_h_ = 0;
    uint32_t inline_bitrate_kbps_ = 0;
    std::vector<uint8_t> inline_bgra_;
    std::wstring inline_toast_text_;
    uint64_t inline_toast_until_ms_ = 0;
    std::unique_ptr<LiveMonitorWindow> live_monitor_;

    std::mutex proc_mtx_;
    HANDLE host_process_ = nullptr;
    HANDLE host_stop_event_ = nullptr;
    std::atomic<bool> host_running_{false};
    std::atomic<bool> user_stopping_host_{false};
    bool running_mock_ = false;
    std::thread log_reader_thread_;

    std::atomic<bool> checking_update_{false};
    std::atomic<bool> shutting_down_{false};
    UpdateCheckResult cached_update_;
    std::thread update_check_thread_;
};

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetDllDirectoryW(L"");
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
        auto fn = reinterpret_cast<SetDpiCtxFn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        }
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ControlCenterWindow app;
    if (!app.Create(hInstance, nCmdShow)) {
        CoUninitialize();
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND active_wnd = GetActiveWindow();
        if (active_wnd == app.Hwnd() && IsWindowEnabled(app.Hwnd()) &&
            msg.message == WM_KEYDOWN && msg.wParam == VK_F12) {
            app.TriggerScreenshotFromHotkey();
            continue;
        }
        if (active_wnd != nullptr && active_wnd == app.LiveMonitorHwnd()) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (!IsDialogMessageW(active_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
