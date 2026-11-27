#include "ios_vcam/app_lang.h"

#include <cctype>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ios_vcam {

namespace {

std::string ToLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

const char* LanguagePrefToConfigValue(AppLanguagePref pref) {
    switch (pref) {
        case AppLanguagePref::ZhTW:
            return "zh-TW";
        case AppLanguagePref::EnUS:
            return "en-US";
        case AppLanguagePref::Auto:
        default:
            return "auto";
    }
}

AppLanguagePref ParseLanguagePrefValue(std::string_view value) {
    std::string low = ToLowerAscii(value);
    if (low == "en" || low == "en-us" || low == "en_us" || low == "english") {
        return AppLanguagePref::EnUS;
    }
    if (low == "zh" || low == "zh-tw" || low == "zh_tw" || low == "zh-hant" || low == "zh-hk" || low == "chinese") {
        return AppLanguagePref::ZhTW;
    }
    return AppLanguagePref::Auto;
}

AppLanguage ResolveEffectiveLanguage(AppLanguagePref pref, uint16_t windows_lang_id) {
    if (pref == AppLanguagePref::ZhTW) return AppLanguage::ZhTW;
    if (pref == AppLanguagePref::EnUS) return AppLanguage::EnUS;

    // Primary Windows LANGID: 0x04 == LANG_CHINESE
    const uint16_t primary_lang = static_cast<uint16_t>(windows_lang_id & 0x03FFU);
    return (primary_lang == 0x04U) ? AppLanguage::ZhTW : AppLanguage::EnUS;
}

#ifdef _WIN32
namespace {
constexpr const wchar_t* kSettingsRegSubkey = L"Software\\JCastForiOS";
constexpr const wchar_t* kLanguageRegValue = L"Language";
}  // namespace

AppLanguagePref LoadSavedLanguagePref() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegSubkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return AppLanguagePref::Auto;
    }
    wchar_t buf[64] = {0};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    AppLanguagePref pref = AppLanguagePref::Auto;
    if (RegQueryValueExW(key, kLanguageRegValue, nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb) == ERROR_SUCCESS &&
        type == REG_SZ) {
        std::string ascii;
        for (size_t i = 0; i < 63 && buf[i] != L'\0'; ++i) {
            if (buf[i] < 128) ascii.push_back(static_cast<char>(buf[i]));
        }
        pref = ParseLanguagePrefValue(ascii);
    }
    RegCloseKey(key);
    return pref;
}

bool SaveLanguagePref(AppLanguagePref pref) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsRegSubkey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const char* val_u8 = LanguagePrefToConfigValue(pref);
    std::wstring val_w;
    for (const char* p = val_u8; *p; ++p) {
        val_w.push_back(static_cast<wchar_t>(*p));
    }
    DWORD cb = static_cast<DWORD>((val_w.size() + 1) * sizeof(wchar_t));
    LONG rc = RegSetValueExW(
        key,
        kLanguageRegValue,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(val_w.c_str()),
        cb);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

AppLanguage DetectSystemDefaultLanguage() {
    const uint16_t lang_id = static_cast<uint16_t>(GetUserDefaultUILanguage());
    return ResolveEffectiveLanguage(AppLanguagePref::Auto, lang_id);
}
#endif

}  // namespace ios_vcam
