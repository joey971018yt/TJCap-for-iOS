#include "ios_vcam/app_updater.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#endif

namespace ios_vcam {

namespace {

std::string ToLowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool EndsWithIgnoreCase(std::string_view str, std::string_view suffix) {
    if (str.size() < suffix.size()) return false;
    return ToLowerAscii(str.substr(str.size() - suffix.size())) == ToLowerAscii(suffix);
}

void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | ((cp >> 6) & 0x1FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | ((cp >> 12) & 0x0FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0x10FFFFU) {
        out.push_back(static_cast<char>(0xF0U | ((cp >> 18) & 0x07U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    std::variant<std::monostate, bool, uint64_t, double, std::string, JsonArray, JsonObject> v;

    bool IsBool() const { return std::holds_alternative<bool>(v); }
    bool IsUint() const { return std::holds_alternative<uint64_t>(v); }
    bool IsString() const { return std::holds_alternative<std::string>(v); }
    bool IsArray() const { return std::holds_alternative<JsonArray>(v); }
    bool IsObject() const { return std::holds_alternative<JsonObject>(v); }

    bool AsBool(bool def = false) const {
        return IsBool() ? std::get<bool>(v) : def;
    }
    uint64_t AsUint(uint64_t def = 0) const {
        if (IsUint()) return std::get<uint64_t>(v);
        if (std::holds_alternative<double>(v)) {
            double d = std::get<double>(v);
            return d >= 0.0 ? static_cast<uint64_t>(d) : def;
        }
        return def;
    }
    std::string AsString(std::string_view def = {}) const {
        return IsString() ? std::get<std::string>(v) : std::string(def);
    }
    const JsonObject* AsObject() const {
        return IsObject() ? &std::get<JsonObject>(v) : nullptr;
    }
    const JsonArray* AsArray() const {
        return IsArray() ? &std::get<JsonArray>(v) : nullptr;
    }
};

class MiniJsonParser {
public:
    explicit MiniJsonParser(std::string_view src) : src_(src) {}

    bool ParseRoot(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return pos_ == src_.size();
    }

private:
    static constexpr int kMaxDepth = 32;

    void SkipWs() {
        // Skip optional UTF-8 BOM at start
        if (pos_ == 0 && src_.size() >= 3 &&
            static_cast<unsigned char>(src_[0]) == 0xEF &&
            static_cast<unsigned char>(src_[1]) == 0xBB &&
            static_cast<unsigned char>(src_[2]) == 0xBF) {
            pos_ += 3;
        }
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    static int HexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool ParseHex4(uint16_t& out) {
        if (pos_ + 4 > src_.size()) return false;
        uint16_t val = 0;
        for (int i = 0; i < 4; ++i) {
            int hv = HexVal(src_[pos_++]);
            if (hv < 0) return false;
            val = static_cast<uint16_t>((val << 4) | hv);
        }
        out = val;
        return true;
    }

    bool ParseString(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= src_.size()) return false;
            char esc = src_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint16_t u1 = 0;
                    if (!ParseHex4(u1)) return false;
                    uint32_t cp = u1;
                    if (u1 >= 0xD800U && u1 <= 0xDBFFU) {
                        if (pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint16_t u2 = 0;
                            if (!ParseHex4(u2)) return false;
                            if (u2 >= 0xDC00U && u2 <= 0xDFFFU) {
                                cp = 0x10000U + ((static_cast<uint32_t>(u1 - 0xD800U) << 10) |
                                                 static_cast<uint32_t>(u2 - 0xDC00U));
                            }
                        }
                    }
                    AppendUtf8Codepoint(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        size_t start = pos_;
        bool neg = false;
        if (pos_ < src_.size() && src_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            return false;
        }
        uint64_t uval = 0;
        bool overflow = false;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            uint64_t digit = static_cast<uint64_t>(src_[pos_] - '0');
            if (uval > (UINT64_MAX - digit) / 10ULL) {
                overflow = true;
            } else {
                uval = uval * 10ULL + digit;
            }
            ++pos_;
        }
        bool is_float = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            is_float = true;
            ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return false;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return false;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }

        if (!neg && !is_float && !overflow) {
            out.v = uval;
            return true;
        }
        std::string num_str(src_.substr(start, pos_ - start));
        char* end_ptr = nullptr;
        double d = std::strtod(num_str.c_str(), &end_ptr);
        if (!end_ptr || end_ptr == num_str.c_str()) return false;
        out.v = d;
        return true;
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        SkipWs();
        if (pos_ >= src_.size()) return false;

        char c = src_[pos_];
        if (c == '"') {
            std::string s;
            if (!ParseString(s)) return false;
            out.v = std::move(s);
            return true;
        }
        if (c == '{') {
            ++pos_;
            SkipWs();
            JsonObject obj;
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                out.v = std::move(obj);
                return true;
            }
            while (pos_ < src_.size()) {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (pos_ >= src_.size() || src_[pos_] != ':') return false;
                ++pos_;
                JsonValue val;
                if (!ParseValue(val, depth + 1)) return false;
                obj[std::move(key)] = std::move(val);
                SkipWs();
                if (pos_ < src_.size() && src_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < src_.size() && src_[pos_] == '}') {
                    ++pos_;
                    out.v = std::move(obj);
                    return true;
                }
                return false;
            }
            return false;
        }
        if (c == '[') {
            ++pos_;
            SkipWs();
            JsonArray arr;
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                out.v = std::move(arr);
                return true;
            }
            while (pos_ < src_.size()) {
                JsonValue elem;
                if (!ParseValue(elem, depth + 1)) return false;
                arr.push_back(std::move(elem));
                SkipWs();
                if (pos_ < src_.size() && src_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < src_.size() && src_[pos_] == ']') {
                    ++pos_;
                    out.v = std::move(arr);
                    return true;
                }
                return false;
            }
            return false;
        }
        if (src_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out.v = true;
            return true;
        }
        if (src_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out.v = false;
            return true;
        }
        if (src_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out.v = std::monostate{};
            return true;
        }
        return ParseNumber(out);
    }

    std::string_view src_;
    size_t pos_ = 0;
};

std::string ExtractSha256Hex(std::string_view digest_field) {
    std::string low = ToLowerAscii(digest_field);
    constexpr std::string_view kPrefix = "sha256:";
    if (low.rfind(kPrefix, 0) != 0) return {};
    std::string hex = low.substr(kPrefix.size());
    if (hex.size() != 64) return {};
    for (char c : hex) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return {};
    }
    return hex;
}

#ifdef _WIN32
std::wstring Utf8ToWideLocal(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8Local(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

struct WinHttpHandleGuard {
    HINTERNET h = nullptr;
    explicit WinHttpHandleGuard(HINTERNET handle = nullptr) : h(handle) {}
    ~WinHttpHandleGuard() {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandleGuard(const WinHttpHandleGuard&) = delete;
    WinHttpHandleGuard& operator=(const WinHttpHandleGuard&) = delete;
    operator HINTERNET() const { return h; }
};

HINTERNET OpenConfiguredWinHttpSession(const std::wstring& user_agent) {
    HINTERNET hSession = WinHttpOpen(
        user_agent.c_str(),
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        hSession = WinHttpOpen(
            user_agent.c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }
    if (hSession) {
        WinHttpSetTimeouts(hSession, 8000, 10000, 15000, 30000);
    }
    return hSession;
}
#endif

}  // namespace

SemVer ParseSemVer(std::string_view ver_str) {
    SemVer sv;
    size_t i = 0;
    while (i < ver_str.size() && std::isspace(static_cast<unsigned char>(ver_str[i]))) ++i;
    if (i < ver_str.size() && (ver_str[i] == 'v' || ver_str[i] == 'V')) ++i;

    int parts[4] = {0, 0, 0, 0};
    int part_idx = 0;
    bool any_digit = false;

    while (i < ver_str.size() && part_idx < 4) {
        if (!std::isdigit(static_cast<unsigned char>(ver_str[i]))) break;
        any_digit = true;
        long val = 0;
        while (i < ver_str.size() && std::isdigit(static_cast<unsigned char>(ver_str[i]))) {
            val = std::min<long>(999999L, val * 10L + (ver_str[i] - '0'));
            ++i;
        }
        parts[part_idx++] = static_cast<int>(val);
        if (i < ver_str.size() && ver_str[i] == '.') {
            ++i;
        } else {
            break;
        }
    }

    if (!any_digit) return sv;
    if (i < ver_str.size() && ver_str[i] == '-') {
        sv.is_prerelease = true;
    }

    sv.major = parts[0];
    sv.minor = parts[1];
    sv.patch = parts[2];
    sv.build = parts[3];
    sv.valid = true;
    return sv;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return (a.major < b.major) ? -1 : 1;
    if (a.minor != b.minor) return (a.minor < b.minor) ? -1 : 1;
    if (a.patch != b.patch) return (a.patch < b.patch) ? -1 : 1;
    if (a.build != b.build) return (a.build < b.build) ? -1 : 1;
    if (a.is_prerelease != b.is_prerelease) {
        // Stable release (is_prerelease == false) is newer than pre-release (is_prerelease == true)
        return a.is_prerelease ? -1 : 1;
    }
    return 0;
}

bool IsRemoteVersionNewer(std::string_view current_version, std::string_view remote_tag) {
    SemVer local_v = ParseSemVer(current_version);
    SemVer remote_v = ParseSemVer(remote_tag);
    if (!local_v.valid || !remote_v.valid) return false;
    return CompareSemVer(remote_v, local_v) > 0;
}

bool ParseGitHubReleaseJson(
    std::string_view json_utf8,
    GitHubReleaseInfo& out_info,
    std::string* out_error) {
    out_info = GitHubReleaseInfo{};

    JsonValue root_val;
    MiniJsonParser parser(json_utf8);
    if (!parser.ParseRoot(root_val) || !root_val.IsObject()) {
        if (out_error) *out_error = "無法解析 GitHub Releases JSON 回應格式。";
        return false;
    }

    const JsonObject& root = *root_val.AsObject();
    auto get_str = [&](const JsonObject& o, const char* key) -> std::string {
        auto it = o.find(key);
        return (it != o.end()) ? it->second.AsString() : std::string{};
    };
    auto get_bool = [&](const JsonObject& o, const char* key) -> bool {
        auto it = o.find(key);
        return (it != o.end()) ? it->second.AsBool(false) : false;
    };
    auto get_uint = [&](const JsonObject& o, const char* key) -> uint64_t {
        auto it = o.find(key);
        return (it != o.end()) ? it->second.AsUint(0) : 0ULL;
    };

    out_info.tag_name = get_str(root, "tag_name");
    if (out_info.tag_name.empty()) {
        std::string api_msg = get_str(root, "message");
        if (out_error) {
            *out_error = api_msg.empty()
                             ? "GitHub Releases 回應缺少 tag_name 欄位。"
                             : ("GitHub API 回應：" + api_msg);
        }
        return false;
    }

    out_info.title = get_str(root, "name");
    if (out_info.title.empty()) out_info.title = out_info.tag_name;
    out_info.html_url = get_str(root, "html_url");
    out_info.published_at = get_str(root, "published_at");
    out_info.body_markdown = get_str(root, "body");
    out_info.draft = get_bool(root, "draft");
    out_info.prerelease = get_bool(root, "prerelease");

    auto assets_it = root.find("assets");
    if (assets_it != root.end() && assets_it->second.IsArray()) {
        for (const JsonValue& item : *assets_it->second.AsArray()) {
            const JsonObject* ao = item.AsObject();
            if (!ao) continue;

            ReleaseAssetInfo asset;
            asset.name = get_str(*ao, "name");
            asset.browser_download_url = get_str(*ao, "browser_download_url");
            asset.size_bytes = get_uint(*ao, "size");
            asset.sha256_hex = ExtractSha256Hex(get_str(*ao, "digest"));

            if (asset.name.empty() || asset.browser_download_url.empty()) continue;

            std::string low_name = ToLowerAscii(asset.name);
            if (EndsWithIgnoreCase(low_name, ".exe")) {
                if (low_name.find("setup") != std::string::npos || out_info.setup_exe.browser_download_url.empty()) {
                    out_info.setup_exe = asset;
                }
            } else if (EndsWithIgnoreCase(low_name, ".zip")) {
                if (low_name.find("portable") != std::string::npos ||
                    out_info.portable_zip.browser_download_url.empty()) {
                    out_info.portable_zip = asset;
                }
            }
            out_info.assets.push_back(std::move(asset));
        }
    }

    return true;
}

UpdateCheckResult CheckGitHubLatestRelease(
    std::string_view owner,
    std::string_view repo,
    std::string_view current_version) {
    UpdateCheckResult res;
    res.current_version = std::string(current_version);

#ifdef _WIN32
    std::wstring ua = L"TJCap-for-iOS/" + Utf8ToWideLocal(current_version) + L" (Windows NT; Win64; x64)";
    WinHttpHandleGuard session(OpenConfiguredWinHttpSession(ua));
    if (!session) {
        res.error_message = "無法初始化 Windows WinHTTP 網路服務。";
        return res;
    }

    WinHttpHandleGuard connect(WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) {
        res.error_message = "無法連線至 api.github.com，請檢查網路連線。";
        return res;
    }

    std::wstring path = L"/repos/" + Utf8ToWideLocal(owner) + L"/" + Utf8ToWideLocal(repo) + L"/releases/latest";
    WinHttpHandleGuard request(WinHttpOpenRequest(
        connect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request) {
        res.error_message = "無法建立 HTTPS 更新查詢請求。";
        return res;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    const wchar_t* headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";

    if (!WinHttpSendRequest(
            request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        DWORD err = GetLastError();
        res.error_message = "連線至 GitHub Releases API 失敗 (WinHTTP 錯誤碼 " + std::to_string(err) + ")。";
        return res;
    }

    DWORD status_code = 0;
    DWORD status_sz = sizeof(status_code);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code,
        &status_sz,
        WINHTTP_NO_HEADER_INDEX);
    res.http_status = static_cast<int>(status_code);

    std::string body;
    body.reserve(16 * 1024);
    constexpr size_t kMaxApiJsonBytes = 4 * 1024 * 1024;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        if (body.size() + avail > kMaxApiJsonBytes) {
            res.error_message = "GitHub API 回應超過安全大小上限。";
            return res;
        }
        size_t old_sz = body.size();
        body.resize(old_sz + avail);
        DWORD read_bytes = 0;
        if (!WinHttpReadData(request, body.data() + old_sz, avail, &read_bytes)) {
            res.error_message = "讀取 GitHub Releases 回應資料時發生中斷。";
            return res;
        }
        body.resize(old_sz + read_bytes);
    }

    if (status_code == 404) {
        res.error_message = "專案目前尚未建立正式版 GitHub Release (HTTP 404)。";
        return res;
    }
    if (status_code == 403 || status_code == 429) {
        res.error_message = "GitHub API 匿名查詢頻率暫時達上限 (HTTP " + std::to_string(status_code) +
                            ")，請稍後再試或點擊開啟 GitHub 網頁查看。";
        return res;
    }
    if (status_code != 200) {
        res.error_message = "GitHub API 回傳非預期狀態碼 HTTP " + std::to_string(status_code) + "。";
        return res;
    }

    std::string parse_err;
    if (!ParseGitHubReleaseJson(body, res.latest, &parse_err)) {
        res.error_message = parse_err;
        return res;
    }

    res.ok = true;
    if (!res.latest.draft && !res.latest.prerelease) {
        res.update_available = IsRemoteVersionNewer(current_version, res.latest.tag_name);
    }
    return res;
#else
    (void)owner;
    (void)repo;
    res.error_message = "WinHTTP backend is only active on Windows.";
    return res;
#endif
}

bool DownloadReleaseAssetHttps(
    const ReleaseAssetInfo& asset,
    const std::filesystem::path& dest_file,
    const std::atomic<bool>* cancel_flag,
    const DownloadProgressFn& on_progress,
    std::string* out_error) {
#ifdef _WIN32
    if (asset.browser_download_url.empty()) {
        if (out_error) *out_error = "更新檔案下載網址為空。";
        return false;
    }

    std::wstring wurl = Utf8ToWideLocal(asset.browser_download_url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        if (out_error) *out_error = "無法解析更新檔案網址格式。";
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        if (out_error) *out_error = "基於安全考量，已拒絕非 HTTPS 加密之更新網址。";
        return false;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::string host_low = ToLowerAscii(WideToUtf8Local(host));
    bool trusted_host = (host_low == "github.com") ||
                        (host_low == "api.github.com") ||
                        EndsWithIgnoreCase(host_low, ".githubusercontent.com");
    if (!trusted_host) {
        if (out_error) *out_error = "基於安全考量，已拒絕非 GitHub 官方網域之更新下載來源。";
        return false;
    }

    std::wstring path_query(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) {
        path_query.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }

    WinHttpHandleGuard session(OpenConfiguredWinHttpSession(L"TJCap-for-iOS-Updater/2.0.0"));
    if (!session) {
        if (out_error) *out_error = "無法初始化 WinHTTP 下載工作階段。";
        return false;
    }

    WinHttpHandleGuard connect(WinHttpConnect(session, host.c_str(), uc.nPort, 0));
    if (!connect) {
        if (out_error) *out_error = "無法連線至 GitHub 檔案伺服器。";
        return false;
    }

    WinHttpHandleGuard request(WinHttpOpenRequest(
        connect,
        L"GET",
        path_query.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request) {
        if (out_error) *out_error = "無法建立 HTTPS 下載請求。";
        return false;
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    const wchar_t* dl_headers = L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(
            request, dl_headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        if (out_error) *out_error = "下載連線中斷或伺服器無回應 (錯誤碼 " + std::to_string(GetLastError()) + ")。";
        return false;
    }

    DWORD status_code = 0;
    DWORD status_sz = sizeof(status_code);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code,
        &status_sz,
        WINHTTP_NO_HEADER_INDEX);
    if (status_code != 200) {
        if (out_error) *out_error = "下載伺服器回傳 HTTP " + std::to_string(status_code) + "。";
        return false;
    }

    uint64_t total_bytes = asset.size_bytes;
    wchar_t cl_buf[64] = {0};
    DWORD cl_sz = sizeof(cl_buf) - sizeof(wchar_t);
    if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX,
            cl_buf,
            &cl_sz,
            WINHTTP_NO_HEADER_INDEX)) {
        uint64_t parsed_cl = static_cast<uint64_t>(_wtoi64(cl_buf));
        if (parsed_cl > 0) total_bytes = parsed_cl;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_file.parent_path(), ec);
    std::filesystem::path part_file = dest_file;
    part_file += L".part";

    std::ofstream ofs(part_file, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (out_error) *out_error = "無法於暫存資料夾建立更新檔案。";
        return false;
    }

    // Initialize streaming SHA-256 hash via Windows BCrypt
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> hash_obj;
    bool hashing_ok = false;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && hAlg) {
        DWORD obj_len = 0, cb_res = 0;
        if (BCryptGetProperty(
                hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb_res, 0) == 0 &&
            obj_len > 0) {
            hash_obj.resize(obj_len);
            if (BCryptCreateHash(hAlg, &hHash, hash_obj.data(), obj_len, nullptr, 0, 0) == 0) {
                hashing_ok = true;
            }
        }
    }

    auto cleanup_crypto = [&]() {
        if (hHash) {
            BCryptDestroyHash(hHash);
            hHash = nullptr;
        }
        if (hAlg) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            hAlg = nullptr;
        }
    };

    std::vector<uint8_t> chunk(64 * 1024);
    uint64_t downloaded = 0;
    if (on_progress) on_progress(0, total_bytes);

    while (true) {
        if (cancel_flag && cancel_flag->load(std::memory_order_relaxed)) {
            ofs.close();
            cleanup_crypto();
            std::filesystem::remove(part_file, ec);
            if (out_error) *out_error = "使用者已取消下載。";
            return false;
        }

        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            ofs.close();
            cleanup_crypto();
            std::filesystem::remove(part_file, ec);
            if (out_error) *out_error = "下載資料流讀取失敗。";
            return false;
        }
        if (avail == 0) {
            break;  // EOF
        }

        DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(chunk.size()));
        DWORD read_now = 0;
        if (!WinHttpReadData(request, chunk.data(), to_read, &read_now)) {
            ofs.close();
            cleanup_crypto();
            std::filesystem::remove(part_file, ec);
            if (out_error) *out_error = "讀取更新封包時連線中斷。";
            return false;
        }
        if (read_now == 0) break;

        ofs.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(read_now));
        if (!ofs.good()) {
            ofs.close();
            cleanup_crypto();
            std::filesystem::remove(part_file, ec);
            if (out_error) *out_error = "寫入更新檔案失敗（請檢查磁碟空間）。";
            return false;
        }

        if (hashing_ok) {
            BCryptHashData(hHash, chunk.data(), read_now, 0);
        }

        downloaded += read_now;
        if (on_progress) {
            on_progress(downloaded, total_bytes);
        }
    }

    ofs.flush();
    ofs.close();

    if (asset.size_bytes > 0 && downloaded != asset.size_bytes) {
        cleanup_crypto();
        std::filesystem::remove(part_file, ec);
        if (out_error) {
            *out_error = "下載檔案大小不符（預期 " + std::to_string(asset.size_bytes) +
                         " bytes，實際收到 " + std::to_string(downloaded) + " bytes）。";
        }
        return false;
    }

    if (!asset.sha256_hex.empty() && hashing_ok) {
        uint8_t raw_digest[32] = {0};
        if (BCryptFinishHash(hHash, raw_digest, sizeof(raw_digest), 0) == 0) {
            char hex_buf[65] = {0};
            for (size_t i = 0; i < 32; ++i) {
                std::snprintf(hex_buf + i * 2, 3, "%02x", raw_digest[i]);
            }
            if (asset.sha256_hex != std::string_view(hex_buf, 64)) {
                cleanup_crypto();
                std::filesystem::remove(part_file, ec);
                if (out_error) *out_error = "SHA-256 數位雜湊校驗失敗！檔案可能於傳輸過程損毀，已拒絕執行。";
                return false;
            }
        }
    }
    cleanup_crypto();

    if (!MoveFileExW(part_file.c_str(), dest_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        std::filesystem::remove(part_file, ec);
        if (out_error) *out_error = "無法完成更新暫存檔案重新命名。";
        return false;
    }

    std::wstring ads = dest_file.wstring() + L":Zone.Identifier";
    DeleteFileW(ads.c_str());
    return true;
#else
    (void)asset;
    (void)dest_file;
    (void)cancel_flag;
    (void)on_progress;
    if (out_error) *out_error = "Unsupported OS";
    return false;
#endif
}

bool IsInstalledAppMode(const std::filesystem::path& app_dir) {
    std::error_code ec;
    return std::filesystem::exists(app_dir / "unins000.exe", ec);
}

void CleanupOldPortableUpdateFiles(const std::filesystem::path& app_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(app_dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(app_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string fname = entry.path().filename().string();
        if (EndsWithIgnoreCase(fname, ".old_upd")) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

#ifdef _WIN32
bool LaunchInstallerUpdate(
    const std::filesystem::path& setup_exe_path,
    void* parent_hwnd,
    std::string* out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(setup_exe_path, ec)) {
        if (out_error) *out_error = "找不到已下載的安裝程式檔案。";
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.hwnd = static_cast<HWND>(parent_hwnd);
    sei.lpVerb = L"runas";
    sei.lpFile = setup_exe_path.c_str();
    sei.lpParameters = L"/SP- /CLOSEAPPLICATIONS";
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            if (out_error) *out_error = "使用者取消了安裝程式管理員授權 (UAC)。";
        } else {
            if (out_error) *out_error = "無法啟動更新安裝程式 (錯誤碼 " + std::to_string(err) + ")。";
        }
        return false;
    }
    return true;
}

bool LaunchPortableZipSelfUpdate(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& app_dir,
    uint32_t current_pid,
    std::string* out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(zip_path, ec)) {
        if (out_error) *out_error = "找不到已下載的便攜版更新壓縮檔。";
        return false;
    }

    std::filesystem::path work_dir = zip_path.parent_path();
    std::filesystem::path ps1_path = work_dir / L"tjcap_apply_portable_update.ps1";

    auto escape_ps_single = [](const std::wstring& s) {
        std::wstring out;
        for (wchar_t c : s) {
            if (c == L'\'') out += L"''";
            else out.push_back(c);
        }
        return out;
    };

    std::wstring zip_esc = escape_ps_single(zip_path.wstring());
    std::wstring app_esc = escape_ps_single(app_dir.wstring());
    std::wstring stage_esc = escape_ps_single((work_dir / L"extracted_stage").wstring());

    std::string script_utf8 =
        "$ErrorActionPreference = 'Continue'\r\n"
        "$targetPid = " + std::to_string(current_pid) + "\r\n"
        "for ($i = 0; $i -lt 40; $i++) {\r\n"
        "    if (-not (Get-Process -Id $targetPid -ErrorAction SilentlyContinue)) { break }\r\n"
        "    Start-Sleep -Milliseconds 250\r\n"
        "}\r\n"
        "Stop-Process -Name 'ios_vcam_host' -Force -ErrorAction SilentlyContinue\r\n"
        "Start-Sleep -Milliseconds 300\r\n"
        "$zipPath = '" + WideToUtf8Local(zip_esc) + "'\r\n"
        "$appDir  = '" + WideToUtf8Local(app_esc) + "'\r\n"
        "$stage   = '" + WideToUtf8Local(stage_esc) + "'\r\n"
        "if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }\r\n"
        "Expand-Archive -LiteralPath $zipPath -DestinationPath $stage -Force\r\n"
        "$srcRoot = $stage\r\n"
        "if (Test-Path -LiteralPath (Join-Path $stage 'TJCap-for-iOS')) {\r\n"
        "    $srcRoot = Join-Path $stage 'TJCap-for-iOS'\r\n"
        "} elseif (Test-Path -LiteralPath (Join-Path $stage 'JCast-for-iOS')) {\r\n"
        "    $srcRoot = Join-Path $stage 'JCast-for-iOS'\r\n"
        "} else {\r\n"
        "    $topDirs = @(Get-ChildItem -LiteralPath $stage -Directory)\r\n"
        "    $topFiles = @(Get-ChildItem -LiteralPath $stage -File)\r\n"
        "    if ($topDirs.Count -eq 1 -and $topFiles.Count -eq 0) {\r\n"
        "        $srcRoot = $topDirs[0].FullName\r\n"
        "    }\r\n"
        "}\r\n"
        "Get-ChildItem -LiteralPath $srcRoot -Recurse -File | ForEach-Object {\r\n"
        "    $rel = $_.FullName.Substring($srcRoot.Length).TrimStart('\\','/')\r\n"
        "    $dst = Join-Path $appDir $rel\r\n"
        "    $dstParent = Split-Path -Parent $dst\r\n"
        "    if (-not (Test-Path -LiteralPath $dstParent)) {\r\n"
        "        New-Item -ItemType Directory -Force -Path $dstParent | Out-Null\r\n"
        "    }\r\n"
        "    try {\r\n"
        "        Copy-Item -LiteralPath $_.FullName -Destination $dst -Force -ErrorAction Stop\r\n"
        "    } catch {\r\n"
        "        $oldPath = $dst + '.old_upd'\r\n"
        "        Remove-Item -LiteralPath $oldPath -Force -ErrorAction SilentlyContinue\r\n"
        "        Move-Item -LiteralPath $dst -Destination $oldPath -Force -ErrorAction SilentlyContinue\r\n"
        "        Copy-Item -LiteralPath $_.FullName -Destination $dst -Force -ErrorAction SilentlyContinue\r\n"
        "    }\r\n"
        "}\r\n"
        "Remove-Item -LiteralPath (Join-Path $appDir 'JCast for iOS.exe') -Force -ErrorAction SilentlyContinue\r\n"
        "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue\r\n"
        "Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue\r\n"
        "$exeToLaunch = Join-Path $appDir 'TJCap for iOS.exe'\r\n"
        "if (-not (Test-Path -LiteralPath $exeToLaunch)) {\r\n"
        "    $exeToLaunch = Join-Path $appDir 'ios_vcam_gui.exe'\r\n"
        "}\r\n"
        "if (Test-Path -LiteralPath $exeToLaunch) {\r\n"
        "    Start-Process -FilePath $exeToLaunch -WorkingDirectory $appDir\r\n"
        "}\r\n";

    {
        std::ofstream pf(ps1_path, std::ios::binary | std::ios::trunc);
        if (!pf.is_open()) {
            if (out_error) *out_error = "無法寫入便攜版自動更新腳本。";
            return false;
        }
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        pf.write(reinterpret_cast<const char*>(bom), 3);
        pf.write(script_utf8.data(), static_cast<std::streamsize>(script_utf8.size()));
    }

    wchar_t sys_dir[MAX_PATH] = {0};
    UINT sys_len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    std::filesystem::path ps_exe =
        (sys_len > 0)
            ? (std::filesystem::path(std::wstring(sys_dir, sys_len)) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe")
            : std::filesystem::path(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");

    std::wstring cmd = L"\"" + ps_exe.wstring() +
                       L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                       ps1_path.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    if (!CreateProcessW(
            ps_exe.c_str(),
            cmd_buf.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            work_dir.c_str(),
            &si,
            &pi)) {
        if (out_error) *out_error = "無法啟動 PowerShell 自動替換程序 (錯誤碼 " + std::to_string(GetLastError()) + ")。";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
#endif

}  // namespace ios_vcam
