#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ios_vcam {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int build = 0;
    bool is_prerelease = false;
    bool valid = false;
};

struct ReleaseAssetInfo {
    std::string name;
    std::string browser_download_url;
    uint64_t size_bytes = 0;
    std::string sha256_hex;  // Lowercase 64-char SHA-256 hex extracted from GitHub API "digest": "sha256:..."
};

struct GitHubReleaseInfo {
    std::string tag_name;
    std::string title;
    std::string html_url;
    std::string published_at;
    std::string body_markdown;
    bool draft = false;
    bool prerelease = false;

    std::vector<ReleaseAssetInfo> assets;
    ReleaseAssetInfo setup_exe;
    ReleaseAssetInfo portable_zip;
};

struct UpdateCheckResult {
    bool ok = false;
    bool update_available = false;
    int http_status = 0;
    std::string current_version;
    std::string error_message;
    GitHubReleaseInfo latest;
};

// Pure C++20 cross-platform SemVer & GitHub Releases REST API JSON parsers (unit-tested on Linux & Windows)
SemVer ParseSemVer(std::string_view ver_str);
int CompareSemVer(const SemVer& a, const SemVer& b);
bool IsRemoteVersionNewer(std::string_view current_version, std::string_view remote_tag);

bool ParseGitHubReleaseJson(
    std::string_view json_utf8,
    GitHubReleaseInfo& out_info,
    std::string* out_error = nullptr);

// Queries https://api.github.com/repos/<owner>/<repo>/releases/latest via native WinHTTP (TLS 1.2/1.3)
UpdateCheckResult CheckGitHubLatestRelease(
    std::string_view owner,
    std::string_view repo,
    std::string_view current_version);

using DownloadProgressFn = std::function<void(uint64_t downloaded_bytes, uint64_t total_bytes)>;

// Downloads a release asset strictly over HTTPS via WinHTTP and verifies SHA-256 digest via Windows BCrypt
bool DownloadReleaseAssetHttps(
    const ReleaseAssetInfo& asset,
    const std::filesystem::path& dest_file,
    const std::atomic<bool>* cancel_flag,
    const DownloadProgressFn& on_progress,
    std::string* out_error = nullptr);

// Returns true if running from an Inno Setup installation folder (unins000.exe exists)
bool IsInstalledAppMode(const std::filesystem::path& app_dir);

// Cleans up any leftover *.old_upd files from a previous Portable in-place self-update
void CleanupOldPortableUpdateFiles(const std::filesystem::path& app_dir);

#ifdef _WIN32
// Launches downloaded Setup.exe installer with elevation (/SP- /CLOSEAPPLICATIONS)
bool LaunchInstallerUpdate(
    const std::filesystem::path& setup_exe_path,
    void* parent_hwnd,
    std::string* out_error = nullptr);

// Spawns background PowerShell self-updater to replace files in app_dir from Portable.zip after current_pid exits
bool LaunchPortableZipSelfUpdate(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& app_dir,
    uint32_t current_pid,
    std::string* out_error = nullptr);
#endif

}  // namespace ios_vcam
