#pragma once

#include <cstdint>
#include <string_view>

namespace ios_vcam {

enum class AppLanguage : uint8_t {
    ZhTW = 0,
    EnUS = 1,
};

enum class AppLanguagePref : uint8_t {
    Auto = 0,
    ZhTW = 1,
    EnUS = 2,
};

// Serializes preference for persistent config/registry storage ("auto", "zh-TW", "en-US")
const char* LanguagePrefToConfigValue(AppLanguagePref pref);

// Parses stored config string into AppLanguagePref (unknown/empty defaults to Auto)
AppLanguagePref ParseLanguagePrefValue(std::string_view value);

// Resolves effective UI language given user preference and Windows UI LANGID (primary LANGID = lang_id & 0x3FF)
AppLanguage ResolveEffectiveLanguage(AppLanguagePref pref, uint16_t windows_lang_id);

#ifdef _WIN32
// Loads saved language preference from HKCU\Software\TJCapForiOS (falls back to legacy JCastForiOS if unset)
AppLanguagePref LoadSavedLanguagePref();

// Saves language preference to HKCU\Software\TJCapForiOS (no UAC elevation required)
bool SaveLanguagePref(AppLanguagePref pref);

// Detects effective language from current Windows user UI language
AppLanguage DetectSystemDefaultLanguage();
#endif

// Concise bilingual string selectors
inline const wchar_t* LText(AppLanguage lang, const wchar_t* zh_tw, const wchar_t* en_us) {
    return (lang == AppLanguage::EnUS) ? en_us : zh_tw;
}

inline const char* LTextUtf8(AppLanguage lang, const char* zh_tw, const char* en_us) {
    return (lang == AppLanguage::EnUS) ? en_us : zh_tw;
}

}  // namespace ios_vcam
