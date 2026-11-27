#include "ios_vcam/app_resource.h"
#include "ios_vcam/app_updater.h"
#include "ios_vcam/dshow_guids.h"
#include "ios_vcam/usb_ios_transport.h"
#include "ios_vcam/wasapi_renderer.h"

#include <windows.h>
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
constexpr int kIdComboResMode = 1010;
constexpr int kIdComboStatusBar = 1011;
constexpr int kIdComboAudioOut = 1012;
constexpr int kIdChkHwDecode = 1013;
constexpr int kIdEditLog = 1020;

constexpr UINT kWmAppendLog = WM_APP + 1;
constexpr UINT kWmHostExited = WM_APP + 2;
constexpr UINT kWmUpdateCheckDone = WM_APP + 3;
constexpr UINT_PTR kTimerRefreshId = 1;
constexpr UINT_PTR kTimerStartupUpdateId = 2;

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
    IosUsbState ios_usb = IosUsbState::Unplugged;
    std::wstring ios_service_name;
};

struct ResOptionSpec {
    uint32_t max_dim = 2560;
    std::wstring mode_tag;  // e.g. L"native", L"1440p", L"1080p", L"720p"
    std::wstring label;
    std::wstring cli_args;
};

std::wstring FormatUsbSpeedBadgeWide(UsbLinkSpeed speed) {
    switch (speed) {
        case UsbLinkSpeed::SuperPlus_20Gbps:
            return L"⚡ USB 3.2 Gen2x2 (20Gbps)｜已解鎖最高 3840p";
        case UsbLinkSpeed::SuperPlus_10Gbps:
            return L"⚡ USB 3.2 Gen2 (10Gbps)｜已解鎖最高 3840p";
        case UsbLinkSpeed::Super_5Gbps:
            return L"⚡ USB 3.0 (5Gbps)｜已解鎖最高 3840p";
        case UsbLinkSpeed::High_480Mbps:
            return L"🔌 USB 2.0 (480Mbps)｜最高可選 2K (2560p)";
        case UsbLinkSpeed::Full_12Mbps:
            return L"⚠️ USB 1.1 (12Mbps)｜最高可選 720p (1280p)";
        case UsbLinkSpeed::Low_1_5Mbps:
            return L"⚠️ USB 1.0 (1.5Mbps)｜最高可選 720p (1280p)";
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

        std::wstring title = L"JCast for iOS — 發現新版本 " + Utf8ToWide(info_.latest.tag_name);
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

    void BuildDialogControls() {
        RECT rc{};
        GetClientRect(dlg_hwnd_, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        int pad = S(16);
        int inner_w = cw - pad * 2;

        std::wstring hdr = L"🎉 發現新版本：" + Utf8ToWide(info_.latest.title);
        HWND lbl_hdr = CreateWindowExW(
            0, L"STATIC", hdr.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            pad, S(14), inner_w, S(26), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_hdr, WM_SETFONT, reinterpret_cast<WPARAM>(font_title_), TRUE);

        std::wstring pkg_desc;
        if (!target_asset_.browser_download_url.empty()) {
            pkg_desc = (use_setup_exe_ ? L"安裝版自動升級 (" : L"便攜版原地自動更新 (") +
                       Utf8ToWide(target_asset_.name) + L", " + FormatBytesMb(target_asset_.size_bytes) +
                       (target_asset_.sha256_hex.empty() ? L")" : L", 含 SHA-256 驗證)");
        } else {
            pkg_desc = L"請開啟 GitHub Releases 網頁下載更新檔案";
        }
        std::wstring sub = L"目前版本：v" IOS_VCAM_VER_DISPLAY_WSTR L"  ➔  最新版本：" +
                           Utf8ToWide(info_.latest.tag_name) + L"  ｜  " + pkg_desc;
        HWND lbl_sub = CreateWindowExW(
            0, L"STATIC", sub.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            pad, S(44), inner_w, S(22), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_sub, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        HWND lbl_notes = CreateWindowExW(
            0, L"STATIC", L"版本更新日誌 (GitHub Release Notes)：",
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
                                   ? L"（此版本未附帶詳細文字說明，請點擊「開啟 GitHub 發布頁」查看。）"
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
            0, L"STATIC", L"準備就緒 — 點擊下方按鈕即可自動從 GitHub 下載並完成升級。",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            pad, prog_y + S(24), inner_w, S(20), dlg_hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        int btn_y = ch - S(50);
        int gap = S(10);
        int w_dl = (inner_w - gap * 2) * 46 / 100;
        int w_web = (inner_w - gap * 2) * 30 / 100;
        int w_can = inner_w - gap * 2 - w_dl - w_web;

        btn_download_ = CreateWindowExW(
            0, L"BUTTON", L"🚀 一鍵下載並自動更新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            pad, btn_y, w_dl, S(36),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnDownload)), hinst_, nullptr);
        SendMessageW(btn_download_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);
        if (target_asset_.browser_download_url.empty()) {
            EnableWindow(btn_download_, FALSE);
        }

        btn_web_ = CreateWindowExW(
            0, L"BUTTON", L"🌐 開啟 GitHub 發布頁",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            pad + w_dl + gap, btn_y, w_web, S(36),
            dlg_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnOpenWeb)), hinst_, nullptr);
        SendMessageW(btn_web_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        btn_cancel_ = CreateWindowExW(
            0, L"BUTTON", L"稍後再說",
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
        SetWindowTextW(btn_cancel_, L"取消下載");
        SendMessageW(progress_bar_, PBM_SETPOS, 0, 0);
        SetWindowTextW(lbl_status_, L"正在連線至 GitHub Releases 伺服器...");

        if (dl_thread_.joinable()) {
            dl_thread_.join();
        }

        ReleaseAssetInfo asset_copy = target_asset_;
        std::filesystem::path dst_copy = downloaded_file_;
        HWND target_hwnd = dlg_hwnd_;

        dl_thread_ = std::thread([this, asset_copy, dst_copy, target_hwnd]() {
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
                        msg = L"正在下載更新包... " + std::to_wstring(pct) + L"% (" +
                              FormatBytesMb(done) + L" / " + FormatBytesMb(total) + L")";
                    } else {
                        msg = L"正在下載更新包... 已接收 " + FormatBytesMb(done);
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
            SetWindowTextW(lbl_status_, L"正在取消下載，請稍候...");
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
                    SetWindowTextW(lbl_status_, L"✅ 下載與雜湊驗證完成！正在啟動自動升級程序...");
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
                    SetWindowTextW(btn_cancel_, L"關閉");
                } else {
                    std::wstring fail_txt = L"❌ " + (err_msg.empty() ? L"下載失敗" : err_msg);
                    SetWindowTextW(lbl_status_, fail_txt.c_str());
                    EnableWindow(btn_download_, TRUE);
                    EnableWindow(btn_cancel_, TRUE);
                    SetWindowTextW(btn_cancel_, L"關閉");
                    if (!cancel_flag_.load(std::memory_order_relaxed)) {
                        MessageBoxW(dlg_hwnd_, fail_txt.c_str(), L"下載更新失敗", MB_ICONWARNING | MB_OK);
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

class ControlCenterWindow {
public:
    bool Create(HINSTANCE hInst, int nCmdShow) {
        hinst_ = hInst;
        base_dir_ = AppDir();
        UnblockDownloadedFiles(base_dir_);
        CleanupOldPortableUpdateFiles(base_dir_);

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
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ControlCenterWindow::StaticWndProc;
        wc.hInstance = hInst;
        wc.hIcon = icon_big_ ? icon_big_ : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = icon_small_ ? icon_small_ : wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"JCastForIosGuiWndClass";
        RegisterClassExW(&wc);

        int win_w = Scale(780, dpi);
        int win_h = Scale(680, dpi);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int pos_x = work.left + ((work.right - work.left) - win_w) / 2;
        int pos_y = work.top + ((work.bottom - work.top) - win_h) / 2;

        hwnd_ = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L" — 一鍵智慧擷取控制台 (USB 速率辨識 × 2K+ 原生自適應)",
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
    }

    void BuildControls() {
        UINT dpi = GetSystemDpiSafe(hwnd_);
        CreateFonts(dpi);

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int cw = rc.right - rc.left;
        int margin = Scale(18, dpi);
        int content_w = cw - margin * 2;

        // Top banner: y = 14 .. 136
        banner_rect_ = {margin, Scale(14, dpi), cw - margin, Scale(136, dpi)};

        // Group 1: Preferences (y = 148 .. 302)
        int grp1_y = Scale(148, dpi);
        int grp1_h = Scale(154, dpi);
        HWND grp_pref = CreateWindowExW(
            0, L"BUTTON", L"擷取畫質上限（依 USB 速率自動開放選項）、比例與音訊設定",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp1_y, content_w, grp1_h, hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(grp_pref, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int lbl_w = Scale(158, dpi);
        int combo_w = content_w - lbl_w - Scale(28, dpi);

        // Row 0: Max Selectable Resolution & Adaptive Aspect Ratio
        int row0_y = grp1_y + Scale(28, dpi);
        HWND lbl_res = CreateWindowExW(
            0, L"STATIC", L"最大解析度與比例選項：",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row0_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_res, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_res_mode_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row0_y, combo_w, Scale(240, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboResMode)), hinst_, nullptr);
        SendMessageW(combo_res_mode_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        PopulateResolutionOptionsForSpeed(UsbLinkSpeed::Unknown, true);

        // Row 1: Status bar style
        int row1_y = grp1_y + Scale(60, dpi);
        HWND lbl_bar = CreateWindowExW(
            0, L"STATIC", L"手機頂部狀態列樣式：",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row1_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_bar, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_status_bar_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row1_y, combo_w, Scale(200, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboStatusBar)), hinst_, nullptr);
        SendMessageW(combo_status_bar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        SendMessageW(combo_status_bar_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"乾淨直播狀態列（推薦：時間固定 9:41、滿格電量與訊號）"));
        SendMessageW(combo_status_bar_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"保留手機真實狀態列（顯示真實時間、電量與通知圖示）"));
        SendMessageW(combo_status_bar_, CB_SETCURSEL, 0, 0);

        // Row 2: PC Audio Routing / Monitoring
        int row2_y = grp1_y + Scale(92, dpi);
        HWND lbl_aud = CreateWindowExW(
            0, L"STATIC", L"電腦端音訊監聽／轉發：",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row2_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_aud, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_audio_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row2_y, combo_w, Scale(240, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboAudioOut)), hinst_, nullptr);
        SendMessageW(combo_audio_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        PopulateAudioEndpoints();

        // Row 3: GPU Hardware Decode
        int row3_y = grp1_y + Scale(124, dpi);
        chk_hw_decode_ = CreateWindowExW(
            0, L"BUTTON", L"啟用 GPU 硬體超低延遲解碼 (D3D11VA，2K+60fps 建議保持開啟)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin + Scale(14, dpi), row3_y, content_w - Scale(28, dpi), Scale(22, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkHwDecode)), hinst_, nullptr);
        SendMessageW(chk_hw_decode_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        SendMessageW(chk_hw_decode_, BM_SETCHECK, BST_CHECKED, 0);

        // Group 2: One-Click Action Buttons (y = 312 .. 430)
        int grp2_y = Scale(312, dpi);
        int grp2_h = Scale(118, dpi);
        HWND grp_act = CreateWindowExW(
            0, L"BUTTON", L"一鍵自動化操作",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp2_y, content_w, grp2_h, hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(grp_act, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int btn_pad = Scale(14, dpi);
        int inner_w = content_w - btn_pad * 2;
        int main_btn_y = grp2_y + Scale(26, dpi);
        btn_smart_start_ = CreateWindowExW(
            0, L"BUTTON", L"🚀 智慧一鍵啟動（自動檢查安裝驅動 ➔ 開始 2K+ 原生投影）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            margin + btn_pad, main_btn_y, inner_w, Scale(42, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnSmartStart)), hinst_, nullptr);
        SendMessageW(btn_smart_start_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int sub_btn_y = main_btn_y + Scale(48, dpi);
        int gap = Scale(8, dpi);
        int avail_btn_w = inner_w - gap * 4;
        int w_mock = avail_btn_w * 23 / 100;
        int w_rep = avail_btn_w * 21 / 100;
        int w_obs = avail_btn_w * 17 / 100;
        int w_upd = avail_btn_w * 22 / 100;
        int w_uninst = avail_btn_w - w_mock - w_rep - w_obs - w_upd;

        int cur_x = margin + btn_pad;
        btn_mock_ = CreateWindowExW(
            0, L"BUTTON", L"🧪 免手機自測",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_mock, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnMockTest)), hinst_, nullptr);
        SendMessageW(btn_mock_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_mock + gap;

        btn_repair_ = CreateWindowExW(
            0, L"BUTTON", L"🔧 安裝／修復驅動",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_rep, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnRepairDriver)), hinst_, nullptr);
        SendMessageW(btn_repair_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_rep + gap;

        btn_obs_guide_ = CreateWindowExW(
            0, L"BUTTON", L"📋 OBS 教學",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_obs, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnCopyObsGuide)), hinst_, nullptr);
        SendMessageW(btn_obs_guide_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_obs + gap;

        btn_update_ = CreateWindowExW(
            0, L"BUTTON", L"🔄 檢查更新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_upd, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnCheckUpdate)), hinst_, nullptr);
        SendMessageW(btn_update_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_upd + gap;

        btn_uninstall_ = CreateWindowExW(
            0, L"BUTTON", L"🗑 完整移除",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_uninst, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnUninstall)), hinst_, nullptr);
        SendMessageW(btn_uninstall_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        // Group 3: Live Diagnostics, Mbps Bitrate & FPS Log
        int log_lbl_y = Scale(440, dpi);
        HWND lbl_log = CreateWindowExW(
            0, L"STATIC",
            L"即時狀態、原生解析度比例與碼率監控（先啟動投影再開 OBS 可自動抓滿版 2K+ 比例）：",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin, log_lbl_y, content_w, Scale(22, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_log, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        int edit_y = log_lbl_y + Scale(24, dpi);
        int edit_h = (rc.bottom - rc.top) - edit_y - Scale(14, dpi);
        edit_log_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            margin, edit_y, content_w, std::max(80, edit_h),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditLog)), hinst_, nullptr);
        SendMessageW(edit_log_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
        SendMessageW(edit_log_, EM_SETLIMITTEXT, 256 * 1024, 0);

        AppendLogLine(
            L"[歡迎] 歡迎使用 JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L"！支援 GitHub Releases 自動更新、USB 速率辨識與 2K+ 原生自適應比例！");
        RefreshHealthAndUi(true);
        SetTimer(hwnd_, kTimerRefreshId, 2000, nullptr);
        SetTimer(hwnd_, kTimerStartupUpdateId, 1500, nullptr);
    }

    void PopulateResolutionOptionsForSpeed(UsbLinkSpeed speed, bool is_initial) {
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
            // USB 3.x (5 / 10 / 20 Gbps): unlock full 3840p native tier + all lower tiers
            next_opts.push_back({
                3840U,
                L"native",
                L"🌟 原生極限無上限（最高 3840p 自適應比例 — USB 3.x 高速接口已解鎖）",
                L" --res native --max-dim 3840",
            });
            next_opts.push_back({
                2560U,
                L"native",
                L"💎 2K 高畫質上限（最高 2560p 自適應比例）",
                L" --res native --max-dim 2560",
            });
            next_opts.push_back({
                1920U,
                L"native",
                L"✨ 1080p 標準上限（最高 1920p 自適應比例、低負載）",
                L" --res native --max-dim 1920",
            });
            next_opts.push_back({
                1280U,
                L"native",
                L"⚡ 720p 流暢上限（最高 1280p 自適應比例）",
                L" --res native --max-dim 1280",
            });
            next_opts.push_back({
                2560U,
                L"1440p",
                L"🖥 固定 2K 16:9 畫布 (2560×1440，直橫屏旋轉自動補黑邊)",
                L" --res 1440p --max-dim 2560",
            });
            next_opts.push_back({
                1920U,
                L"1080p",
                L"🖥 固定 1080p 16:9 畫布 (1920×1080，傳統直播相容模式)",
                L" --res 1080p --max-dim 1920",
            });
        } else if (max_cap >= 2560U) {
            // USB 2.0 (480 Mbps) or Unplugged: cap selectable max at 2K (2560p); 3840p option is gated out
            if (speed == UsbLinkSpeed::High_480Mbps) {
                next_opts.push_back({
                    2560U,
                    L"native",
                    L"💎 2K 高畫質上限（最高 2560p 自適應比例 — USB 2.0 接口最高上限）",
                    L" --res native --max-dim 2560",
                });
                next_opts.push_back({
                    1920U,
                    L"native",
                    L"✨ 1080p 標準上限（最高 1920p 自適應比例 — USB 2.0 穩定推薦）",
                    L" --res native --max-dim 1920",
                });
            } else {
                next_opts.push_back({
                    2560U,
                    L"native",
                    L"💎 2K 高畫質上限（最高 2560p 自適應 — 插入 USB 3.x 接口將解鎖 3840p）",
                    L" --res native --max-dim 2560",
                });
                next_opts.push_back({
                    1920U,
                    L"native",
                    L"✨ 1080p 標準上限（最高 1920p 自適應比例、低負載）",
                    L" --res native --max-dim 1920",
                });
            }
            next_opts.push_back({
                1280U,
                L"native",
                L"⚡ 720p 流暢上限（最高 1280p 自適應比例）",
                L" --res native --max-dim 1280",
            });
            next_opts.push_back({
                2560U,
                L"1440p",
                L"🖥 固定 2K 16:9 畫布 (2560×1440，直橫屏旋轉自動補黑邊)",
                L" --res 1440p --max-dim 2560",
            });
            next_opts.push_back({
                1920U,
                L"1080p",
                L"🖥 固定 1080p 16:9 畫布 (1920×1080，傳統直播相容模式)",
                L" --res 1080p --max-dim 1920",
            });
        } else {
            // USB 1.1 (1.5 / 12 Mbps): cap selectable max at 720p (1280p)
            next_opts.push_back({
                1280U,
                L"native",
                L"⚡ 720p 流暢上限（最高 1280p 自適應比例 — USB 1.1 接口最高上限）",
                L" --res native --max-dim 1280",
            });
            next_opts.push_back({
                960U,
                L"native",
                L"📱 540p 省頻寬上限（最高 960p 自適應比例）",
                L" --res native --max-dim 960",
            });
            next_opts.push_back({
                1280U,
                L"720p",
                L"🖥 固定 720p 16:9 畫布 (1280×720)",
                L" --res 720p --max-dim 1280",
            });
        }

        res_options_ = std::move(next_opts);
        SendMessageW(combo_res_mode_, CB_RESETCONTENT, 0, 0);
        for (const auto& opt : res_options_) {
            SendMessageW(combo_res_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(opt.label.c_str()));
        }

        int target_idx = 0;
        if (!is_initial && user_manually_picked_res_ && prev_max_dim > 0) {
            for (size_t i = 0; i < res_options_.size(); ++i) {
                if (res_options_[i].max_dim == prev_max_dim && res_options_[i].mode_tag == prev_mode_tag) {
                    target_idx = static_cast<int>(i);
                    break;
                }
            }
        }
        SendMessageW(combo_res_mode_, CB_SETCURSEL, target_idx, 0);
    }

    void PopulateAudioEndpoints() {
        SendMessageW(combo_audio_, CB_RESETCONTENT, 0, 0);
        audio_endpoint_matches_.clear();

        SendMessageW(combo_audio_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"自動偵測 VB-Audio Virtual Cable（預設，供 Discord/Zoom 收音）"));
        audio_endpoint_matches_.push_back(L"__AUTO__");

        SendMessageW(combo_audio_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"僅輸出至 OBS「JCast for iOS (Audio)」（不在電腦喇叭播放）"));
        audio_endpoint_matches_.push_back(L"__NONE__");

        auto eps = WasapiAudioRouter::EnumerateRenderEndpoints();
        for (const auto& ep : eps) {
            std::wstring wname = Utf8ToWide(ep.friendly_name_utf8);
            std::wstring label = L"🎧 同步播放／轉發至：" + wname;
            SendMessageW(combo_audio_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            audio_endpoint_matches_.push_back(wname);
        }
        SendMessageW(combo_audio_, CB_SETCURSEL, 0, 0);
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
                        L"[USB 速率辨識] 已偵測到連接埠速率：" +
                        Utf8ToWide(UsbLinkSpeedName(new_spd)) +
                        L" ➔ 已開放最高 " + std::to_wstring(cap) +
                        L"p 解析度選項（開始傳輸後將全程鎖定所選解析度，不會中途自動變動）。");
                }
            }
        }
        prev_ios_usb_ = health_.ios_usb;

        if (running) {
            if (running_mock_) {
                SetWindowTextW(btn_smart_start_, L"■ 停止自測訊號");
                SetWindowTextW(btn_mock_, L"■ 停止自測");
            } else {
                SetWindowTextW(btn_smart_start_, L"■ 停止 iPhone 投影擷取（安全切回一般充電/傳輸模式）");
                SetWindowTextW(btn_mock_, L"🧪 免手機自測");
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
                SetWindowTextW(btn_smart_start_,
                               L"🚀 智慧一鍵啟動（自動安裝驅動與 JCast 虛擬擷取卡 ➔ 開始投影）");
            } else {
                SetWindowTextW(btn_smart_start_, L"▶ 開始 iPhone / iPad 有線投影擷取（鎖定所選解析度 @ 60fps）");
            }
            SetWindowTextW(btn_mock_, L"🧪 免手機自測");
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
                           ? L"🟢 JCast 正在輸出自測動態彩條與 48kHz 測試音（請開啟 OBS 預覽）"
                           : L"🟢 JCast 有線投影執行中（已鎖定所選解析度 @ 60fps + 48kHz 音訊）";
        } else if (health_.ios_usb == IosUsbState::PluggedZadigConflict) {
            bg_color = RGB(255, 240, 240);
            accent_color = RGB(196, 43, 28);
            headline = L"⚠️ 偵測到父裝置驅動遭 Zadig 覆蓋（請於裝置管理員還原 Apple 複合裝置驅動）";
        } else if (health_.dshow != DshowRegState::Ready || !health_.libusb_service_ok ||
                   health_.ios_usb == IosUsbState::PluggedNeedFilter) {
            bg_color = RGB(255, 248, 232);
            accent_color = RGB(180, 105, 14);
            headline = L"⚡ 準備開始：請點擊下方「智慧一鍵啟動」自動完成驅動掛載與高畫質投影";
        } else if (health_.ios_usb == IosUsbState::PluggedReady) {
            bg_color = RGB(234, 247, 240);
            accent_color = RGB(20, 130, 75);
            headline = L"✅ iPhone 與驅動皆已就緒！請確認解析度選項後點擊「開始有線投影擷取」";
        } else {
            bg_color = RGB(242, 245, 250);
            accent_color = RGB(45, 95, 185);
            headline = L"🔌 JCast for iOS 已就緒 — 請插上已點「信任」的 iPhone/iPad 自動辨識 USB 速率";
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
            dshow_txt = L"• DirectShow 虛擬擷取卡 (JCast for iOS)：✅ 已安裝";
        } else if (health_.dshow == DshowRegState::PathMismatch) {
            dshow_txt = L"• DirectShow 虛擬擷取卡 (JCast for iOS)：⚠️ 資料夾已移動（點一鍵啟動自動更新）";
        } else {
            dshow_txt = L"• DirectShow 虛擬擷取卡 (JCast for iOS)：⏳ 尚未註冊（點一鍵啟動自動完成）";
        }

        std::wstring usb_drv_txt = health_.libusb_service_ok
                                       ? L"• WHQL 簽章過濾驅動 (libusb0)：✅ 已就緒"
                                       : L"• WHQL 簽章過濾驅動 (libusb0)：⏳ 待安裝";
        if (cached_update_.update_available) {
            usb_drv_txt += L"   ｜   🎉 發現新版 " + Utf8ToWide(cached_update_.latest.tag_name) + L" 可一鍵升級！";
        } else {
            usb_drv_txt += L"   ｜   目前版本：v" IOS_VCAM_VER_DISPLAY_WSTR;
        }

        std::wstring speed_badge = FormatUsbSpeedBadgeWide(detected_usb_speed_);
        std::wstring speed_suffix = speed_badge.empty() ? L"" : (L" [" + speed_badge + L"]");

        std::wstring phone_txt;
        switch (health_.ios_usb) {
            case IosUsbState::PluggedReady:
                phone_txt = L"• iPhone / iPad USB：✅ 已連線" + speed_suffix;
                break;
            case IosUsbState::PluggedNeedFilter:
                phone_txt = L"• iPhone / iPad USB：📱 已偵測（點啟動掛載過濾層）" + speed_suffix;
                break;
            case IosUsbState::PluggedZadigConflict:
                phone_txt = L"• iPhone / iPad USB：❌ 偵測到 " + health_.ios_service_name +
                            L" 覆蓋原廠驅動";
                break;
            case IosUsbState::Unplugged:
                phone_txt = L"• iPhone / iPad USB：🔌 尚未插入 USB（插入後自動辨識 USB 3.x / 2.0 速率並更新可選上限）";
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
            AppendLogLine(L"[錯誤] 找不到 ios_vcam_driver_setup.exe，請確認程式資料夾檔案完整！");
            MessageBoxW(hwnd_, L"找不到 ios_vcam_driver_setup.exe，請確認完整安裝或解壓縮 JCast for iOS 資料夾！",
                        L"缺少檔案", MB_ICONERROR | MB_OK);
            return false;
        }

        AppendLogLine(std::wstring(L"[系統] 正在請求系統管理員權限執行 ios_vcam_driver_setup.exe ") + arg_flag + L" ...");

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
                AppendLogLine(L"[取消] 使用者取消了管理員權限授權 (UAC)。");
            } else {
                AppendLogLine(L"[錯誤] 無法啟動安裝輔助程式 (錯誤碼 " + std::to_wstring(err) + L")。");
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
                AppendLogLine(L"[成功] 驅動過濾層與 JCast for iOS 虛擬擷取卡設定完成！");
                return true;
            }
            AppendLogLine(L"[提醒] 設定程式回傳代碼 " + std::to_wstring(exit_code) + L"，請確認檔案完整或重試。");
            return false;
        }
        RefreshHealthAndUi(true);
        return true;
    }

    void StartHostProcess(bool mock_mode) {
        if (host_running_.load(std::memory_order_acquire)) {
            return;
        }

        std::filesystem::path host_exe = base_dir_ / L"ios_vcam_host.exe";
        if (!std::filesystem::exists(host_exe)) {
            AppendLogLine(L"[錯誤] 找不到 ios_vcam_host.exe！");
            MessageBoxW(hwnd_, L"找不到 ios_vcam_host.exe，請確認完整安裝或解壓縮 JCast for iOS 資料夾！",
                        L"缺少檔案", MB_ICONERROR | MB_OK);
            return;
        }

        health_ = ProbeSystemHealth(base_dir_);
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
                AppendLogLine(L"[提示] 目前尚未插入 iPhone，已進入等待連線模式；插上手機並點「信任」後若為首次插入，再點一次「安裝／修復驅動」即可掛載過濾層。");
            } else {
                RefreshHealthAndUi(true);
            }
        }

        std::wstring event_name = L"Local\\IosUsbVcamGuiStop_" + std::to_wstring(GetCurrentProcessId()) +
                                  L"_" + std::to_wstring(GetTickCount64());
        HANDLE hStopEvt = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
        if (!hStopEvt) {
            AppendLogLine(L"[錯誤] 無法建立跨行程控制事件。");
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
        std::wstring chosen_res_label = L"預設自適應上限";
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
            CloseHandle(read_pipe);
            CloseHandle(hStopEvt);
            AppendLogLine(L"[錯誤] 啟動 ios_vcam_host.exe 失敗 (錯誤碼 " + std::to_wstring(GetLastError()) + L")。");
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
        RefreshHealthAndUi(false);

        AppendLogLine(mock_mode ? L"=== 已啟動 JCast 免手機自測模式（彩條 + 48kHz 測試音） ==="
                                : (L"=== 已啟動 JCast 有線投影｜本次鎖定選項：" + chosen_res_label + L" ==="));

        if (log_reader_thread_.joinable()) {
            log_reader_thread_.join();
        }
        log_reader_thread_ = std::thread([this, read_pipe, hProc = pi.hProcess]() {
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
                    if (!line.empty()) {
                        PostLogFromWorker(Utf8ToWide(line));
                    }
                }
            }
            if (!pending.empty()) {
                PostLogFromWorker(Utf8ToWide(pending));
            }
            CloseHandle(read_pipe);
            WaitForSingleObject(hProc, 3000);
            PostMessageW(hwnd_, kWmHostExited, 0, 0);
        });
    }

    void StopHostProcess() {
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
            CloseHandle(hProc);
        }
        if (hEvt) {
            CloseHandle(hEvt);
        }
        if (log_reader_thread_.joinable()) {
            log_reader_thread_.join();
        }
        host_running_.store(false, std::memory_order_release);
        running_mock_ = false;
        RefreshHealthAndUi(true);
    }

    void ShowAndCopyObsGuide() {
        const wchar_t* guide =
            L"【JCast for iOS — OBS Studio 滿版自適應比例設定步驟】\r\n\r\n"
            L"1. 先在本控制台插上手機（自動辨識 USB 速率並開放最大解析度選項），選好解析度後點擊「智慧一鍵啟動」。\r\n"
            L"2. 開啟 OBS Studio，新增「視訊擷取裝置」➔ 選擇：JCast for iOS (Video)\r\n"
            L"3.「解析度/FPS 類型」保持「裝置預設值」：OBS 會自動抓到您鎖定的 iPhone/iPad 原生比例（滿版零黑邊）！\r\n"
            L"   （若您先開了 OBS 才連手機，只要在 OBS 來源點一下「停用 ➔ 啟用」就會自動刷新成原生比例）\r\n"
            L"4. 往下捲動到底部勾選「使用自訂音訊裝置」➔ 選擇：JCast for iOS (Audio)\r\n\r\n"
            L"（裝置名稱已複製到剪貼簿：JCast for iOS (Video)）";

        if (OpenClipboard(hwnd_)) {
            EmptyClipboard();
            const wchar_t* clip_txt = L"JCast for iOS (Video)";
            size_t cb = (wcslen(clip_txt) + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, cb);
            if (hMem) {
                void* ptr = GlobalLock(hMem);
                if (ptr) {
                    std::memcpy(ptr, clip_txt, cb);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
            }
            CloseClipboard();
        }
        MessageBoxW(hwnd_, guide, L"JCast for iOS — OBS 設定教學", MB_ICONINFORMATION | MB_OK);
    }

    void OpenUpdateDialog(const UpdateCheckResult& info) {
        UINT dpi = GetSystemDpiSafe(hwnd_);
        UpdateModalWindow dlg(
            hinst_,
            hwnd_,
            base_dir_,
            info,
            dpi,
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
            AppendLogLine(L"[軟體更新] 已啟動自動更新程序，正在關閉目前視窗以完成替換...");
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }

    void StartAsyncUpdateCheck(bool manual_trigger) {
        if (checking_update_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (manual_trigger) {
            SetWindowTextW(btn_update_, L"⏳ 檢查中...");
            EnableWindow(btn_update_, FALSE);
            AppendLogLine(L"[更新檢查] 正在連線至 GitHub Releases API 檢查最新版本...");
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
                                           ? (L"⬆️ 升級 " + Utf8ToWide(cached_update_.latest.tag_name))
                                           : L"🔄 檢查更新";
            SetWindowTextW(btn_update_, restore_lbl.c_str());

            if (manual_trigger) {
                std::wstring werr = Utf8ToWide(res.error_message);
                AppendLogLine(L"[更新檢查失敗] " + werr);
                std::wstring prompt =
                    L"無法從 GitHub Releases API 取得版本資訊：\r\n" + werr +
                    L"\r\n\r\n是否直接以瀏覽器開啟 GitHub Releases 發布頁面查看？";
                if (MessageBoxW(hwnd_, prompt.c_str(), L"檢查更新失敗", MB_ICONWARNING | MB_YESNO) == IDYES) {
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
            SetWindowTextW(btn_update_, L"🔄 檢查更新");
            InvalidateRect(hwnd_, &banner_rect_, FALSE);
            AppendLogLine(
                L"[更新檢查] 目前已是最新版本 (v" IOS_VCAM_VER_DISPLAY_WSTR L"，GitHub 最新發布：" +
                Utf8ToWide(res.latest.tag_name) + L")。");

            if (manual_trigger) {
                std::wstring msg =
                    L"您目前使用的 JCast for iOS v" IOS_VCAM_VER_DISPLAY_WSTR L" 已是最新版本！\r\n\r\n"
                    L"• GitHub 最新版本：" + Utf8ToWide(res.latest.tag_name) + L"\r\n" +
                    (res.latest.published_at.empty()
                         ? L""
                         : (L"• 發布時間：" + Utf8ToWide(res.latest.published_at) + L"\r\n")) +
                    L"\r\n是否開啟 GitHub Releases 網頁查看完整歷史更新日誌？";
                if (MessageBoxW(hwnd_, msg.c_str(), L"已是最新版本", MB_ICONINFORMATION | MB_YESNO) == IDYES) {
                    OpenSafeGitHubWebUrl(hwnd_, res.latest.html_url);
                }
            }
            return;
        }

        std::wstring up_lbl = L"⬆️ 升級 " + Utf8ToWide(res.latest.tag_name);
        SetWindowTextW(btn_update_, up_lbl.c_str());
        InvalidateRect(hwnd_, &banner_rect_, FALSE);
        AppendLogLine(
            L"[發現新版本] 🎉 GitHub 已發布 JCast for iOS " + Utf8ToWide(res.latest.tag_name) +
            L"！點擊「" + up_lbl + L"」即可檢視更新日誌並一鍵自動升級。");

        if (manual_trigger && !host_running_.load(std::memory_order_acquire)) {
            OpenUpdateDialog(cached_update_);
        }
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
                EndPaint(hwnd_, &ps);
                return 0;
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
                if (code == BN_CLICKED) {
                    if (id == kIdBtnSmartStart) {
                        if (host_running_.load(std::memory_order_acquire)) {
                            AppendLogLine(L"[操作] 正在停止投影擷取並恢復標準 USB 模式...");
                            StopHostProcess();
                            AppendLogLine(L"[完成] 已停止投影擷取。");
                        } else {
                            StartHostProcess(false);
                        }
                        return 0;
                    }
                    if (id == kIdBtnMockTest) {
                        if (host_running_.load(std::memory_order_acquire)) {
                            StopHostProcess();
                            AppendLogLine(L"[完成] 已停止自測訊號。");
                        } else {
                            StartHostProcess(true);
                        }
                        return 0;
                    }
                    if (id == kIdBtnRepairDriver) {
                        RunDriverSetupElevated(L"--install");
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
                                L"確定要移除 Apple USB 過濾層並反註冊 JCast for iOS 虛擬擷取卡嗎？",
                                L"確認解除安裝",
                                MB_ICONQUESTION | MB_YESNO) == IDYES) {
                            if (host_running_.load(std::memory_order_acquire)) {
                                StopHostProcess();
                            }
                            if (RunDriverSetupElevated(L"--uninstall")) {
                                AppendLogLine(L"[完成] 已完整移除驅動過濾層與虛擬擷取卡註冊。");
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

            case kWmHostExited: {
                if (host_running_.load(std::memory_order_acquire)) {
                    StopHostProcess();
                    AppendLogLine(L"[狀態] 擷取背景程序已結束。");
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
    SystemHealth health_;
    IosUsbState prev_ios_usb_ = IosUsbState::Unplugged;
    UsbLinkSpeed detected_usb_speed_ = UsbLinkSpeed::Unknown;
    bool user_manually_picked_res_ = false;
    std::vector<ResOptionSpec> res_options_;

    HICON icon_big_ = nullptr;
    HICON icon_small_ = nullptr;
    HICON icon_banner_ = nullptr;

    RECT banner_rect_{};
    HWND combo_res_mode_ = nullptr;
    HWND combo_status_bar_ = nullptr;
    HWND combo_audio_ = nullptr;
    HWND chk_hw_decode_ = nullptr;
    HWND btn_smart_start_ = nullptr;
    HWND btn_mock_ = nullptr;
    HWND btn_repair_ = nullptr;
    HWND btn_obs_guide_ = nullptr;
    HWND btn_update_ = nullptr;
    HWND btn_uninstall_ = nullptr;
    HWND edit_log_ = nullptr;

    HFONT font_normal_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_mono_ = nullptr;

    std::vector<std::wstring> audio_endpoint_matches_;

    std::mutex proc_mtx_;
    HANDLE host_process_ = nullptr;
    HANDLE host_stop_event_ = nullptr;
    std::atomic<bool> host_running_{false};
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
        if (!IsDialogMessageW(GetActiveWindow(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
