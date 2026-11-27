#include "ios_vcam/diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ios_vcam {

namespace {

const DiagCodeSpec kNoneSpec{
    DiagCode::None,
    DiagSeverity::Info,
    "JCAST-OK",
    "系統運作正常",
    "目前未偵測到任何硬體、驅動或串流異常。",
    "無需額外操作。",
    false,
};

const DiagCodeSpec kSpecs[] = {
    {
        DiagCode::E101_MissingCoreFiles,
        DiagSeverity::Critical,
        "JCAST-E101",
        "缺少核心執行檔或驅動元件",
        "程式資料夾內遺失必要元件（如 ios_vcam_host.exe、ios_vcam_dshow.dll、libusb-1.0.dll 或 drivers\\libusb0.sys），常見於未完整解壓縮 Portable.zip 或遭防毒軟體誤隔離。",
        "1. 請確認已將整個 ZIP 壓縮包「完整解壓縮」至同一資料夾（勿直接在壓縮檔內雙擊執行）。\n"
        "2. 若防毒軟體攔截，請將 JCast for iOS 資料夾加入信任排除清單，或改下載 Setup.exe 正式安裝版重新安裝。",
        false,
    },
    {
        DiagCode::E102_MissingAppleService,
        DiagSeverity::Warning,
        "JCAST-E102",
        "未偵測到 Apple 行動裝置配對服務 (AMDS / Apple USB Driver)",
        "電腦尚未安裝 Apple 原廠 USB 驅動與信任配對服務，iPhone / iPad 插上電腦後將無法跳出「信任這部電腦」提示。",
        "1. 請開啟 Microsoft Store 搜尋並安裝官方免費的「Apple 裝置 (Apple Devices)」或「iTunes」。\n"
        "2. 安裝完成後重新插拔 iPhone / iPad，解鎖螢幕並點擊「信任」輸入手機密碼。",
        false,
    },
    {
        DiagCode::E103_MissingMediaFoundation,
        DiagSeverity::Critical,
        "JCAST-E103",
        "系統缺少 Windows Media Foundation H.264 視訊解碼元件",
        "找不到 mfplat.dll 或 msmpeg2vdec.dll，常見於 Windows 10 / 11 N 或 KN 歐洲精簡版系統未安裝媒體功能套件。",
        "1. 開啟 Windows「設定 ➔ 系統 ➔ 選用功能 (Optional Features) ➔ 新增選用功能」。\n"
        "2. 搜尋並安裝「媒體功能套件 (Media Feature Pack)」，完成後重新開機即可正常硬體解碼。",
        false,
    },
    {
        DiagCode::E104_DshowNotRegistered,
        DiagSeverity::Warning,
        "JCAST-E104",
        "DirectShow 虛擬擷取卡尚未註冊或路徑已變更",
        "尚未完成首次虛擬鏡頭註冊，或解壓縮資料夾在註冊後曾被搬移至其他路徑。",
        "1. 點擊主視窗或本診斷視窗中的「🔧 安裝／修復驅動」按鈕，自動以系統管理員權限重新指向目前路徑。\n"
        "2. 若 OBS Studio 已開啟，修復後重新啟動 OBS 即可抓到 JCast for iOS (Video) / (Audio)。",
        true,
    },
    {
        DiagCode::E201_ZadigDriverConflict,
        DiagSeverity::Critical,
        "JCAST-E201",
        "偵測到 iPhone 父複合裝置驅動遭 Zadig (WinUSB / libusbK) 覆蓋",
        "曾使用 Zadig 等工具將 Apple Mobile Device USB Composite Device 原廠驅動強制替換，導致手機失去原廠配對與 QuickTime 多組態切換能力。",
        "1. 開啟 Windows「裝置管理員」➔ 找到該 Apple / WinUSB 裝置 ➔ 右鍵「解除安裝裝置」，勾選「嘗試移除此裝置的驅動程式」。\n"
        "2. 重新拔插 iPhone 讓系統恢復原廠 Apple Composite 驅動後，再點擊本程式「🔧 安裝／修復驅動」以安全 UpperFilter 模式掛載。",
        false,
    },
    {
        DiagCode::E202_UpperFilterMissing,
        DiagSeverity::Warning,
        "JCAST-E202",
        "此 iPhone / iPad 尚未掛載 libusb0 安全過濾層",
        "首次插入此台 iOS 裝置或更換不同型號手機時，尚未在該裝置節點寫入 UpperFilters=libusb0。",
        "1. 保持手機連接在電腦上，點擊「🔧 安裝／修復驅動」（或直接點「🚀 智慧一鍵啟動」）自動完成無損掛載。\n"
        "2. 若掛載後仍提示權限不足，請拔下 USB 線重插一次即可立即生效。",
        true,
    },
    {
        DiagCode::E203_UsbOpenAccessDenied,
        DiagSeverity::Error,
        "JCAST-E203",
        "開啟 iOS USB 裝置遭拒 (LIBUSB_ERROR_ACCESS)",
        "已偵測到 Apple 裝置，但 Windows 拒絕開啟低階 USB 控制代碼；通常為剛安裝完過濾驅動尚未重新插拔，或權限受限。",
        "1. 請將手機端的 USB 傳輸線拔下，等待 2 秒後重新插回電腦。\n"
        "2. 若仍發生，請點擊一次「🔧 安裝／修復驅動」刷新裝置節點。",
        true,
    },
    {
        DiagCode::E204_UsbInterfaceBusy,
        DiagSeverity::Error,
        "JCAST-E204",
        "iOS QuickTime 影音通道正被其他程式佔用 (LIBUSB_ERROR_BUSY)",
        "背景已有另一個投屏工具、macOS/Windows 虛擬機或殘留的 ios_vcam_host.exe 鎖定了 SubClass 0x2A 介面。",
        "1. 請關閉其他手機有線投屏軟體或第三方手機助手。\n"
        "2. 開啟工作管理員確認無多餘的 ios_vcam_host.exe 後，重新拔插 USB 線再啟動投影。",
        false,
    },
    {
        DiagCode::E301_IosNotTrustedOrLocked,
        DiagSeverity::Warning,
        "JCAST-E301",
        "尚未取得手機信任授權或無法切換 QuickTime 影音模式",
        "已送出 QuickTime 隱藏組態啟用指令，但手機未開啟 SubClass 0x2A 影音介面；最常見原因為手機處於鎖屏狀態、尚未點擊「信任這部電腦」，或使用了僅供充電的線材。",
        "1. 請解鎖 iPhone / iPad 保持螢幕亮起，若跳出「要信任這部電腦嗎？」請點選「信任」並輸入鎖屏密碼。\n"
        "2. 若完全沒跳出信任視窗，請確認已安裝「Apple 裝置 / iTunes」且使用的是支援資料傳輸的 USB 線（非純充電線）。",
        false,
    },
    {
        DiagCode::E302_SetConfigFailed,
        DiagSeverity::Error,
        "JCAST-E302",
        "切換 USB QuickTime 組態失敗 (libusb_set_configuration)",
        "切換至高頻寬影音組態時遭匯流排拒絕，常見於過濾驅動未完整生效或外接 USB Hub 供電不足。",
        "1. 點擊「🔧 安裝／修復驅動」重新確認 UpperFilter 狀態。\n"
        "2. 改插電腦主機板後方的原生 USB 3.0 / Type-C 接口（避免透過無獨立供電的集線器或機殼前置延長孔），並重插手機。",
        true,
    },
    {
        DiagCode::E303_HandshakeNoFrames,
        DiagSeverity::Warning,
        "JCAST-E303",
        "USB 影音通道已建立但逾時未收到畫面封包",
        "已成功宣稱 QuickTime 介面並送出音視訊同步握手，但超過 8 秒未收到任何 H.264 影格；通常因手機在握手瞬間鎖屏或 iOS 媒體服務暫時卡住。",
        "1. 請將 iPhone / iPad 解鎖並滑動一下螢幕。\n"
        "2. 若仍無畫面，請點擊「停止投影」，拔插一次 USB 線後再重新點擊「啟動投影」。",
        false,
    },
    {
        DiagCode::E304_UsbTransferIoDrop,
        DiagSeverity::Error,
        "JCAST-E304",
        "USB 傳輸途中發生 I/O 中斷或頻寬溢位",
        "串流期間收到 LIBUSB_ERROR_IO / PIPE / OVERFLOW / NO_DEVICE；除正常拔線外，多因線材接觸不良、機殼前置 USB 孔壓降過大，或老舊 USB 孔跑超高碼率所致。",
        "1. 檢查 Lightning / USB-C 接頭是否鬆動，並優先改插「主機板後方原生 USB 接口」。\n"
        "2. 若使用一般 USB 2.0 線材偶發瞬斷，可於啟動前將最大解析度上限調降至「1080p (1920p)」以降低峰值匯流排負載。",
        false,
    },
    {
        DiagCode::E401_DecoderInitFailed,
        DiagSeverity::Critical,
        "JCAST-E401",
        "H.264 視訊解碼器初始化失敗",
        "顯示卡 D3D11VA 硬體解碼與系統軟體解碼器皆無法建立，可能因顯示卡驅動異常或缺少系統解碼器。",
        "1. 請嘗試取消勾選主視窗的「啟用 GPU 硬體超低延遲解碼 (D3D11VA)」改用 CPU 軟解測試。\n"
        "2. 更新 NVIDIA / AMD / Intel 顯示卡驅動程式；若為 Windows N 版請安裝 Media Feature Pack。",
        false,
    },
    {
        DiagCode::E402_GpuFallbackToSoftware,
        DiagSeverity::Warning,
        "JCAST-E402",
        "GPU 硬體解碼異常，已自動無縫降級為 CPU 軟體解碼",
        "顯示卡 D3D11VA 發生驅動重置 (TDR) 或不支援目前畫面組態；系統已自動切換至 CPU 軟體解碼以確保直播不中斷。",
        "1. 目前串流已自動恢復運作，您可繼續使用。\n"
        "2. 若 CPU 軟解在 2K+ 下延遲超過 14ms，建議稍後更新顯示卡驅動，或將解析度上限選為 1080p。",
        false,
    },
    {
        DiagCode::E403_HighDecodeLatency,
        DiagSeverity::Warning,
        "JCAST-E403",
        "解碼管線延遲偏高（超過 60fps 16.7ms 預算）",
        "單張影格解碼與縮放耗時過長，可能導致 60fps 直播掉幀；常見於未開啟 D3D11VA 硬體解碼卻執行 2K+ 超高解析度。",
        "1. 請確認已勾選「啟用 GPU 硬體超低延遲解碼 (D3D11VA)」。\n"
        "2. 若電腦無獨立／內建硬解加速能力，建議將最大解析度改選為「1080p (1920p)」或「720p (1280p)」。",
        false,
    },
    {
        DiagCode::E501_AudioEndpointLost,
        DiagSeverity::Warning,
        "JCAST-E501",
        "電腦端音訊監聽裝置已斷開或失效",
        "直播途中拔除了耳機／USB 音效卡或藍牙音訊中斷 (AUDCLNT_E_DEVICE_INVALIDATED)。註：OBS 內的「JCast for iOS (Audio)」不受影響仍正常收音！",
        "1. 若僅需 OBS 直播收音，可忽略此提示，OBS 音軌完全正常。\n"
        "2. 若需恢復電腦耳機同步監聽，請重新接上耳機後停止並重新啟動一次投影。",
        false,
    },
    {
        DiagCode::E502_SharedMemInitFailed,
        DiagSeverity::Critical,
        "JCAST-E502",
        "無法建立跨行程高畫質共享記憶體 (IosUsbVcamSharedMemV2)",
        "系統可用虛擬記憶體（分頁檔 Commit Charge）不足，或遭舊版高權限行程鎖定同名區段。",
        "1. 關閉佔用大量記憶體的閒置程式，確認 Windows 虛擬記憶體（分頁檔）未被手動停用。\n"
        "2. 重新啟動 JCast for iOS 與 OBS Studio。",
        false,
    },
    {
        DiagCode::E503_HostUnexpectedExit,
        DiagSeverity::Critical,
        "JCAST-E503",
        "背景擷取核心程序 (ios_vcam_host.exe) 異常終止",
        "擷取引擎在非使用者手動停止的情況下提前結束並回傳非零錯誤碼。",
        "1. 請檢視下方「程序結束代碼說明」與近期錯誤紀錄。\n"
        "2. 點擊「📋 複製診斷報告」或「🐞 開啟 GitHub 回報問題」將完整脫敏診斷單提供給開發者快速排查。",
        false,
    },
};

std::string ToLowerCopy(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

const char* SeverityTag(DiagSeverity sev) {
    switch (sev) {
        case DiagSeverity::Info:
            return "INFO";
        case DiagSeverity::Warning:
            return "WARN";
        case DiagSeverity::Critical:
            return "CRIT";
        case DiagSeverity::Error:
        default:
            return "ERR";
    }
}

DiagSeverity ParseSeverityTag(std::string_view sv) {
    if (sv == "INFO") return DiagSeverity::Info;
    if (sv == "WARN") return DiagSeverity::Warning;
    if (sv == "CRIT") return DiagSeverity::Critical;
    return DiagSeverity::Error;
}

std::string UrlEncodeRfc3986(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool IsHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

const DiagCodeSpec& LookupDiagSpec(DiagCode code) {
    for (const auto& s : kSpecs) {
        if (s.code == code) {
            return s;
        }
    }
    return kNoneSpec;
}

DiagCode ParseDiagCodeToken(std::string_view token) {
    if (token.rfind("JCAST-", 0) == 0) {
        token.remove_prefix(6);
    }
    if (token.size() >= 4 && (token[0] == 'E' || token[0] == 'e')) {
        int num = std::atoi(std::string(token.substr(1)).c_str());
        for (const auto& s : kSpecs) {
            if (static_cast<int>(s.code) == num) {
                return s.code;
            }
        }
    }
    return DiagCode::None;
}

DiagCode ClassifyUsbOrTransportError(std::string_view raw_err) {
    std::string low = ToLowerCopy(raw_err);
    if (low.find("libusb-1.0.dll") != std::string::npos &&
        (low.find("missing") != std::string::npos || low.find("not found") != std::string::npos ||
         low.find("failed to load") != std::string::npos)) {
        return DiagCode::E101_MissingCoreFiles;
    }
    if (low.find("busy") != std::string::npos || low.find("code -6") != std::string::npos ||
        low.find("(-6)") != std::string::npos) {
        return DiagCode::E204_UsbInterfaceBusy;
    }
    if (low.find("libusb_open") != std::string::npos || low.find("access") != std::string::npos ||
        low.find("code -3") != std::string::npos || low.find("(-3)") != std::string::npos) {
        return DiagCode::E203_UsbOpenAccessDenied;
    }
    if (low.find("set_configuration") != std::string::npos) {
        return DiagCode::E302_SetConfigFailed;
    }
    if (low.find("subclass 0x2a") != std::string::npos || low.find("re-enumeration") != std::string::npos) {
        return DiagCode::E301_IosNotTrustedOrLocked;
    }
    if (low.find("bulk in") != std::string::npos || low.find("overflow") != std::string::npos ||
        low.find("pipe") != std::string::npos || low.find("libusb_error_io") != std::string::npos ||
        low.find("(-1)") != std::string::npos || low.find("(-4)") != std::string::npos ||
        low.find("(-8)") != std::string::npos || low.find("(-9)") != std::string::npos) {
        return DiagCode::E304_UsbTransferIoDrop;
    }
    return DiagCode::None;
}

std::string FormatHostDiagLine(DiagCode code, DiagSeverity sev, std::string_view detail_utf8) {
    const auto& spec = LookupDiagSpec(code);
    const char* short_code = spec.code_str;
    if (std::strncmp(short_code, "JCAST-", 6) == 0) {
        short_code += 6;
    }
    std::string out = "[JCAST-DIAG|";
    out += short_code;
    out += "|";
    out += SeverityTag(sev);
    out += "] ";
    out.append(detail_utf8.data(), detail_utf8.size());
    return out;
}

bool ParseHostDiagLine(std::string_view line_utf8, ParsedDiagEvent& out_event) {
    constexpr std::string_view kPrefix = "[JCAST-DIAG|";
    if (line_utf8.rfind(kPrefix, 0) != 0) {
        return false;
    }
    size_t close_br = line_utf8.find(']', kPrefix.size());
    if (close_br == std::string_view::npos) {
        return false;
    }
    std::string_view header = line_utf8.substr(kPrefix.size(), close_br - kPrefix.size());
    size_t pipe_pos = header.find('|');
    if (pipe_pos == std::string_view::npos) {
        return false;
    }
    std::string_view code_tok = header.substr(0, pipe_pos);
    std::string_view sev_tok = header.substr(pipe_pos + 1);

    DiagCode code = ParseDiagCodeToken(code_tok);
    if (code == DiagCode::None) {
        return false;
    }

    size_t msg_start = close_br + 1;
    while (msg_start < line_utf8.size() && line_utf8[msg_start] == ' ') {
        ++msg_start;
    }

    out_event.code = code;
    out_event.severity = ParseSeverityTag(sev_tok);
    out_event.detail_utf8.assign(line_utf8.substr(msg_start));
    return true;
}

std::string DescribeProcessExitCode(uint32_t exit_code) {
    char hex_buf[32] = {0};
    std::snprintf(hex_buf, sizeof(hex_buf), "0x%08X (%u)", exit_code, exit_code);
    std::string s = hex_buf;

    switch (exit_code) {
        case 0U:
            return s + " — 正常結束";
        case 1U:
            return s + " — 初始化錯誤（請查看上方對應 JCAST-E 錯誤碼，如缺少檔案、USB 驅動或共享記憶體失敗）";
        case 0xC0000135U:
            return s + " — STATUS_DLL_NOT_FOUND（系統缺少必要系統 DLL 或未完整解壓縮程式資料夾）";
        case 0xC000007BU:
            return s + " — STATUS_INVALID_IMAGE_FORMAT（32/64 位元 DLL 混用或檔案損毀）";
        case 0xC0000005U:
            return s + " — STATUS_ACCESS_VIOLATION（記憶體存取違規，可能為顯示卡驅動或外部元件異常）";
        case 0xC0000409U:
            return s + " — STATUS_STACK_BUFFER_OVERRUN / FAST_FAIL（安全防護攔截異常狀態）";
        default:
            return s + " — 非預期結束代碼";
    }
}

std::string RedactSensitiveReportText(std::string_view raw_utf8) {
    std::string s(raw_utf8);

    // 1. Mask Windows/macOS/Linux user profile directory names: Users\<name>\ or Users/<name>/
    std::string low = ToLowerCopy(s);
    size_t search_pos = 0;
    while (search_pos < s.size()) {
        low = ToLowerCopy(s);
        size_t u_pos = low.find("users", search_pos);
        if (u_pos == std::string::npos) {
            break;
        }
        size_t sep1 = u_pos + 5;
        if (sep1 < s.size() && (s[sep1] == '\\' || s[sep1] == '/')) {
            char slash = s[sep1];
            size_t name_start = sep1 + 1;
            size_t name_end = name_start;
            while (name_end < s.size() && s[name_end] != '\\' && s[name_end] != '/' &&
                   s[name_end] != '\r' && s[name_end] != '\n' && s[name_end] != '"') {
                ++name_end;
            }
            if (name_end > name_start && name_end < s.size() && (s[name_end] == '\\' || s[name_end] == '/')) {
                std::string token = s.substr(name_start, name_end - name_start);
                if (token != "<USER>" && token != "Public" && token != "Default") {
                    s.replace(name_start, name_end - name_start, "<USER>");
                    search_pos = name_start + 6;
                    continue;
                }
            }
            (void)slash;
        }
        search_pos = u_pos + 5;
    }

    // 2. Mask explicit "Serial=<value>" fields while keeping first 4 chars for model-family debugging
    search_pos = 0;
    while (search_pos < s.size()) {
        size_t spos = s.find("Serial=", search_pos);
        if (spos == std::string::npos) {
            break;
        }
        size_t val_start = spos + 7;
        size_t val_end = val_start;
        while (val_end < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[val_end])) || s[val_end] == '-')) {
            ++val_end;
        }
        size_t len = val_end - val_start;
        std::string val = s.substr(val_start, len);
        if (len > 4 && val != "N/A") {
            std::string masked = val.substr(0, 4) + "****-REDACTED";
            s.replace(val_start, len, masked);
            search_pos = val_start + masked.size();
        } else {
            search_pos = val_end;
        }
    }

    // 3. Mask standalone 25-char dashed UDIDs (00008130-001A2B3C4D5E6F70) and classic 40-char hex UDIDs
    for (size_t i = 0; i + 25 <= s.size(); ++i) {
        // Pattern A: 8 hex + '-' + 16 hex (25 chars, iPhone XS / iPad Pro 2018+)
        if (s[i + 8] == '-') {
            bool ok = true;
            for (size_t k = 0; k < 25; ++k) {
                if (k == 8) continue;
                if (!IsHexChar(s[i + k])) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                std::string masked = s.substr(i, 4) + "****-REDACTED";
                s.replace(i, 25, masked);
                i += masked.size() - 1;
                continue;
            }
        }
        // Pattern B: 40 contiguous hex chars (classic Apple UDID)
        if (i + 40 <= s.size()) {
            bool prev_hex = (i > 0 && IsHexChar(s[i - 1]));
            bool next_hex = (i + 40 < s.size() && IsHexChar(s[i + 40]));
            if (!prev_hex && !next_hex) {
                bool ok40 = true;
                for (size_t k = 0; k < 40; ++k) {
                    if (!IsHexChar(s[i + k])) {
                        ok40 = false;
                        break;
                    }
                }
                if (ok40) {
                    std::string masked = s.substr(i, 4) + "****-REDACTED";
                    s.replace(i, 40, masked);
                    i += masked.size() - 1;
                }
            }
        }
    }

    return s;
}

std::string BuildGitHubNewIssueUrl(
    std::string_view owner,
    std::string_view repo,
    std::string_view app_version,
    DiagCode primary_code,
    std::string_view short_summary_utf8) {
    const auto& spec = LookupDiagSpec(primary_code);
    std::string title = "[診斷回報] JCast for iOS v" + std::string(app_version);
    if (primary_code != DiagCode::None) {
        title += " — ";
        title += spec.code_str;
        title += " ";
        title += spec.title_utf8;
    }

    std::string body =
        "### 問題簡述\n"
        "（請在此補充您遇到的操作情境，例如手機型號、iOS 版本、使用的線材）\n\n"
        "### 自動偵測摘要\n";
    body += "- 版本：`v" + std::string(app_version) + "`\n";
    if (primary_code != DiagCode::None) {
        body += std::string("- 主要錯誤碼：`") + spec.code_str + "` (" + spec.title_utf8 + ")\n";
    }
    if (!short_summary_utf8.empty()) {
        body += "- 狀態摘要：" + RedactSensitiveReportText(short_summary_utf8) + "\n";
    }
    body +=
        "\n### 完整系統診斷報告（已自動隱私脫敏）\n"
        "> 💡 **診斷報告已自動複製到您的剪貼簿，請直接在下方空白處按 `Ctrl + V` 貼上：**\n\n"
        "```text\n"
        "（請在此按 Ctrl+V 貼上剪貼簿中的診斷報告）\n"
        "```\n";

    std::string url = "https://github.com/";
    url.append(owner.data(), owner.size());
    url.push_back('/');
    url.append(repo.data(), repo.size());
    url += "/issues/new?title=" + UrlEncodeRfc3986(title) + "&body=" + UrlEncodeRfc3986(body);
    return url;
}

}  // namespace ios_vcam
