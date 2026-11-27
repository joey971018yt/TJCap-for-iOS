#include "ios_vcam/app_resource.h"
#include "ios_vcam/dshow_guids.h"
#include "ios_vcam/wasapi_renderer.h"

#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <dbt.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
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
constexpr int kIdComboResMode = 1010;
constexpr int kIdComboStatusBar = 1011;
constexpr int kIdComboAudioOut = 1012;
constexpr int kIdChkHwDecode = 1013;
constexpr int kIdEditLog = 1020;

constexpr UINT kWmAppendLog = WM_APP + 1;
constexpr UINT kWmHostExited = WM_APP + 2;
constexpr UINT_PTR kTimerRefreshId = 1;

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

class ControlCenterWindow {
public:
    bool Create(HINSTANCE hInst, int nCmdShow) {
        hinst_ = hInst;
        base_dir_ = AppDir();
        UnblockDownloadedFiles(base_dir_);

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
        wc.lpszClassName = L"IosUsbVcamGuiWndClass";
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
            L"iOS USB 擷取卡 v1.3 — 一鍵智慧控制台 (2K+ 原生自適應比例)",
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
            0, L"BUTTON", L"擷取畫質、比例與音訊設定",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, grp1_y, content_w, grp1_h, hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(grp_pref, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

        int lbl_w = Scale(158, dpi);
        int combo_w = content_w - lbl_w - Scale(28, dpi);

        // Row 0: Resolution & Adaptive Aspect Ratio
        int row0_y = grp1_y + Scale(28, dpi);
        HWND lbl_res = CreateWindowExW(
            0, L"STATIC", L"輸出畫質與比例模式：",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            margin + Scale(14, dpi), row0_y, lbl_w, Scale(26, dpi), hwnd_, nullptr, hinst_, nullptr);
        SendMessageW(lbl_res, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);

        combo_res_mode_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            margin + Scale(14, dpi) + lbl_w, row0_y, combo_w, Scale(220, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdComboResMode)), hinst_, nullptr);
        SendMessageW(combo_res_mode_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        SendMessageW(combo_res_mode_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"🌟 2K+ 原生自適應比例（推薦：依 iPhone/iPad 機型自動滿版無黑邊）"));
        SendMessageW(combo_res_mode_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"✨ 1080p 自適應比例（依機型自動滿版無黑邊、輕量低負載）"));
        SendMessageW(combo_res_mode_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"🖥 固定 2K 16:9 畫布 (2560×1440，直橫屏旋轉自動補黑邊)"));
        SendMessageW(combo_res_mode_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"🖥 固定 1080p 16:9 畫布 (1920×1080，傳統直播相容模式)"));
        SendMessageW(combo_res_mode_, CB_SETCURSEL, 0, 0);

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
        int w_mock = (inner_w - gap * 3) * 34 / 100;
        int w_rep = (inner_w - gap * 3) * 24 / 100;
        int w_obs = (inner_w - gap * 3) * 22 / 100;
        int w_uninst = inner_w - gap * 3 - w_mock - w_rep - w_obs;

        int cur_x = margin + btn_pad;
        btn_mock_ = CreateWindowExW(
            0, L"BUTTON", L"🧪 免插手機自測 (2K 彩條+測試音)",
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
            0, L"BUTTON", L"📋 OBS 設定教學",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            cur_x, sub_btn_y, w_obs, Scale(34, dpi),
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnCopyObsGuide)), hinst_, nullptr);
        SendMessageW(btn_obs_guide_, WM_SETFONT, reinterpret_cast<WPARAM>(font_normal_), TRUE);
        cur_x += w_obs + gap;

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

        AppendLogLine(L"[歡迎] 已支援 2K+ 裝置原生解析度與自動比例自適應（iPhone 19.5:9 / iPad 4:3 滿版無黑邊）！");
        RefreshHealthAndUi();
        SetTimer(hwnd_, kTimerRefreshId, 2000, nullptr);
    }

    void PopulateAudioEndpoints() {
        SendMessageW(combo_audio_, CB_RESETCONTENT, 0, 0);
        audio_endpoint_matches_.clear();

        SendMessageW(combo_audio_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"自動偵測 VB-Audio Virtual Cable（預設，供 Discord/Zoom 收音）"));
        audio_endpoint_matches_.push_back(L"__AUTO__");

        SendMessageW(combo_audio_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"僅輸出至 OBS DirectShow 擷取卡（不在電腦喇叭播放）"));
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

    void RefreshHealthAndUi() {
        health_ = ProbeSystemHealth(base_dir_);
        bool running = host_running_.load(std::memory_order_acquire);

        if (running) {
            if (running_mock_) {
                SetWindowTextW(btn_smart_start_, L"■ 停止自測訊號");
                SetWindowTextW(btn_mock_, L"■ 停止自測訊號");
            } else {
                SetWindowTextW(btn_smart_start_, L"■ 停止 iPhone 投影擷取（安全切回一般充電/傳輸模式）");
                SetWindowTextW(btn_mock_, L"🧪 免插手機自測 (2K 彩條+測試音)");
                EnableWindow(btn_mock_, FALSE);
            }
            EnableWindow(combo_res_mode_, FALSE);
            EnableWindow(combo_status_bar_, FALSE);
            EnableWindow(combo_audio_, FALSE);
            EnableWindow(chk_hw_decode_, FALSE);
            EnableWindow(btn_repair_, FALSE);
        } else {
            bool needs_setup = (health_.dshow != DshowRegState::Ready) ||
                               !health_.libusb_service_ok ||
                               (health_.ios_usb == IosUsbState::PluggedNeedFilter);
            if (needs_setup) {
                SetWindowTextW(btn_smart_start_,
                               L"🚀 智慧一鍵啟動（自動安裝驅動與虛擬擷取卡 ➔ 開始 2K+ 自適應投影）");
            } else {
                SetWindowTextW(btn_smart_start_, L"▶ 開始 iPhone / iPad 有線投影擷取 (2K+ 自適應 @ 60fps)");
            }
            SetWindowTextW(btn_mock_, L"🧪 免插手機自測 (2K 彩條+測試音)");
            EnableWindow(btn_mock_, TRUE);
            EnableWindow(combo_res_mode_, TRUE);
            EnableWindow(combo_status_bar_, TRUE);
            EnableWindow(combo_audio_, TRUE);
            EnableWindow(chk_hw_decode_, TRUE);
            EnableWindow(btn_repair_, TRUE);
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
                           ? L"🟢 正在輸出 2K@60fps 動態彩條與 48kHz 測試音（請開啟 OBS 預覽）"
                           : L"🟢 iPhone / iPad 有線投影執行中（2K+ 自適應比例 @ 60fps + 48kHz 音訊）";
        } else if (health_.ios_usb == IosUsbState::PluggedZadigConflict) {
            bg_color = RGB(255, 240, 240);
            accent_color = RGB(196, 43, 28);
            headline = L"⚠️ 偵測到父裝置驅動遭 Zadig 覆蓋（請於裝置管理員還原 Apple 複合裝置驅動）";
        } else if (health_.dshow != DshowRegState::Ready || !health_.libusb_service_ok ||
                   health_.ios_usb == IosUsbState::PluggedNeedFilter) {
            bg_color = RGB(255, 248, 232);
            accent_color = RGB(180, 105, 14);
            headline = L"⚡ 準備開始：請點擊下方「智慧一鍵啟動」自動完成驅動掛載與 2K+ 投影";
        } else if (health_.ios_usb == IosUsbState::PluggedReady) {
            bg_color = RGB(234, 247, 240);
            accent_color = RGB(20, 130, 75);
            headline = L"✅ iPhone 與驅動皆已就緒！點擊「開始有線投影擷取」自動匹配原生 2K+ 比例";
        } else {
            bg_color = RGB(242, 245, 250);
            accent_color = RGB(45, 95, 185);
            headline = L"🔌 驅動已就緒 — 請插上已解鎖並點「信任」的 iPhone/iPad，或點「免插手機自測」";
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
            dshow_txt = L"• DirectShow 2K+ 視訊/音訊虛擬擷取卡：✅ 已安裝";
        } else if (health_.dshow == DshowRegState::PathMismatch) {
            dshow_txt = L"• DirectShow 2K+ 視訊/音訊虛擬擷取卡：⚠️ 資料夾已移動（點一鍵啟動自動更新路徑）";
        } else {
            dshow_txt = L"• DirectShow 2K+ 視訊/音訊虛擬擷取卡：⏳ 尚未註冊（點一鍵啟動自動完成）";
        }

        std::wstring usb_drv_txt = health_.libusb_service_ok
                                       ? L"• WHQL 簽章過濾驅動 (libusb0)：✅ 已就緒"
                                       : L"• WHQL 簽章過濾驅動 (libusb0)：⏳ 待安裝";

        std::wstring phone_txt;
        switch (health_.ios_usb) {
            case IosUsbState::PluggedReady:
                phone_txt = L"• iPhone / iPad USB 狀態：✅ 已連線且 UpperFilter 過濾層已掛載";
                break;
            case IosUsbState::PluggedNeedFilter:
                phone_txt = L"• iPhone / iPad USB 狀態：📱 已偵測到裝置（點一鍵啟動自動掛載過濾層）";
                break;
            case IosUsbState::PluggedZadigConflict:
                phone_txt = L"• iPhone / iPad USB 狀態：❌ 偵測到 " + health_.ios_service_name +
                            L" 覆蓋了原廠 usbccgp/usbaapl64";
                break;
            case IosUsbState::Unplugged:
                phone_txt = L"• iPhone / iPad USB 狀態：🔌 尚未插入 USB（插入後請解鎖螢幕並按「信任這部電腦」）";
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
            AppendLogLine(L"[錯誤] 找不到 ios_vcam_driver_setup.exe，請確認 dist-win64 資料夾檔案完整！");
            MessageBoxW(hwnd_, L"找不到 ios_vcam_driver_setup.exe，請確認完整解壓縮 dist-win64 資料夾！",
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
            RefreshHealthAndUi();
            if (exit_code == 0) {
                AppendLogLine(L"[成功] 驅動過濾層與 DirectShow 虛擬擷取卡設定完成！");
                return true;
            }
            AppendLogLine(L"[提醒] 設定程式回傳代碼 " + std::to_wstring(exit_code) + L"，請確認檔案完整或重試。");
            return false;
        }
        RefreshHealthAndUi();
        return true;
    }

    void StartHostProcess(bool mock_mode) {
        if (host_running_.load(std::memory_order_acquire)) {
            return;
        }

        std::filesystem::path host_exe = base_dir_ / L"ios_vcam_host.exe";
        if (!std::filesystem::exists(host_exe)) {
            AppendLogLine(L"[錯誤] 找不到 ios_vcam_host.exe！");
            MessageBoxW(hwnd_, L"找不到 ios_vcam_host.exe，請確認完整解壓縮 dist-win64 資料夾！",
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
        if (res_idx == 1) cmd += L" --res native1080";
        else if (res_idx == 2) cmd += L" --res 1440p";
        else if (res_idx == 3) cmd += L" --res 1080p";
        else cmd += L" --res native";

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
        RefreshHealthAndUi();

        AppendLogLine(mock_mode ? L"=== 已啟動免手機自測模式 (2K@60fps 彩條 + 48kHz 測試音) ==="
                                : L"=== 已啟動 iPhone / iPad 有線投影擷取 (2K+ 原生自適應) ===");

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
        RefreshHealthAndUi();
    }

    void ShowAndCopyObsGuide() {
        const wchar_t* guide =
            L"【OBS Studio 2K+ 滿版自適應比例設定步驟】\r\n\r\n"
            L"1. 先在本控制台插上手機、打開要直播的畫面方向（橫向或直向），點擊「智慧一鍵啟動」。\r\n"
            L"2. 開啟 OBS Studio，新增「視訊擷取裝置」➔ 選擇：iOS USB Capture Card (Video)\r\n"
            L"3.「解析度/FPS 類型」保持「裝置預設值」：OBS 會自動抓到您 iPhone/iPad 的原生 2K+ 解析度與真實比例（滿版零黑邊）！\r\n"
            L"   （若您先開了 OBS 才連手機，只要在 OBS 來源點一下「停用 ➔ 啟用」就會自動刷新成原生比例）\r\n"
            L"4. 往下捲動到底部勾選「使用自訂音訊裝置」➔ 選擇：iOS USB Capture Card (Audio)\r\n\r\n"
            L"（裝置名稱已複製到剪貼簿：iOS USB Capture Card (Video)）";

        if (OpenClipboard(hwnd_)) {
            EmptyClipboard();
            const wchar_t* clip_txt = L"iOS USB Capture Card (Video)";
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
        MessageBoxW(hwnd_, guide, L"OBS 2K+ 自適應比例設定教學", MB_ICONINFORMATION | MB_OK);
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
                    RefreshHealthAndUi();
                }
                return 0;

            case WM_DEVICECHANGE:
                if (wParam == DBT_DEVNODES_CHANGED) {
                    RefreshHealthAndUi();
                }
                return TRUE;

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                int code = HIWORD(wParam);
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
                    if (id == kIdBtnUninstall) {
                        if (MessageBoxW(
                                hwnd_,
                                L"確定要移除 Apple USB 過濾層並反註冊 DirectShow 虛擬擷取卡嗎？",
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

            case WM_CLOSE:
                KillTimer(hwnd_, kTimerRefreshId);
                if (host_running_.load(std::memory_order_acquire)) {
                    StopHostProcess();
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
};

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
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
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
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
