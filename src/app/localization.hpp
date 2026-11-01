#pragma once
#include <windows.h>
#include <cwchar>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../shared/ui_text.hpp"
#include "translations/de.hpp"
#include "translations/es.hpp"
#include "translations/fr.hpp"
#include "translations/zh_hans.hpp"

namespace taxi_camera::standalone {
// Companion display language. Bridge telemetry, logs and bug reports stay English.
struct UiLanguage {
  const wchar_t* tag;          // BCP 47, saved as [companion] language.
  const wchar_t* native_name;  // Language menu label, in its own script.
  const wchar_t* typeface;     // Segoe UI Variable has no Han glyphs.
  BYTE charset;
  const wchar_t* sentence_gap;  // Joins whole sentences; Chinese uses none.
  std::span<const Translation> catalog;
};

inline constexpr UiLanguage UiLanguages[]{
    {L"en", L"English", L"Segoe UI Variable", DEFAULT_CHARSET, L" ", {}},
    {L"de", L"Deutsch", L"Segoe UI Variable", DEFAULT_CHARSET, L" ", GermanTranslations},
    {L"es", L"Español", L"Segoe UI Variable", DEFAULT_CHARSET, L" ", SpanishTranslations},
    {L"fr", L"Français", L"Segoe UI Variable", DEFAULT_CHARSET, L" ", FrenchTranslations},
    {L"zh-Hans", L"简体中文", L"Microsoft YaHei UI", GB2312_CHARSET, L"", SimplifiedChineseTranslations},
};
inline constexpr const UiLanguage& EnglishLanguage = UiLanguages[0];
inline constexpr wchar_t LanguageSection[] = L"companion";
inline constexpr wchar_t LanguageKey[] = L"language";

inline const UiLanguage* find_language(std::wstring_view tag) {
  for (const auto& language : UiLanguages)
    if (!tag.empty() && tag.size() == std::wcslen(language.tag) && !_wcsnicmp(tag.data(), language.tag, tag.size()))
      return &language;
  return nullptr;
}

// Matches one Windows display language such as de-DE, zh-CN or zh-Hans-HK by
// dropping subtags until a catalog fits. Traditional Chinese stays English.
inline const UiLanguage* match_language(std::wstring_view name) {
  constexpr std::pair<const wchar_t*, const wchar_t*> scripts[]{{L"zh", L"zh-Hans"},    {L"zh-CN", L"zh-Hans"}, {L"zh-SG", L"zh-Hans"},
                                                                {L"zh-TW", L"zh-Hant"}, {L"zh-HK", L"zh-Hant"}, {L"zh-MO", L"zh-Hant"}};
  std::wstring candidate(name);
  for (const auto& [region, script] : scripts)
    if (candidate.size() == std::wcslen(region) && !_wcsicmp(candidate.c_str(), region))
      candidate = script;
  while (!candidate.empty()) {
    if (const auto* language = find_language(candidate))
      return language;
    const auto dash = candidate.find_last_of(L'-');
    if (dash == std::wstring::npos)
      break;
    candidate.resize(dash);
  }
  return nullptr;
}

// The Windows display languages in preference order, not the regional format.
inline std::vector<std::wstring> windows_display_languages() {
  ULONG count{}, size{};
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size < 2)
    return {};
  std::wstring names(size, L'\0');
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &size))
    return {};
  std::vector<std::wstring> result;
  for (const wchar_t* name = names.c_str(); *name; name += std::wcslen(name) + 1)
    result.emplace_back(name);
  return result;
}

// An explicit choice wins; otherwise the first display language with a catalog.
inline const UiLanguage& resolve_language(std::wstring_view preference, std::span<const std::wstring> display_languages) {
  if (const auto* chosen = find_language(preference))
    return *chosen;
  for (const auto& name : display_languages)
    if (const auto* language = match_language(name))
      return *language;
  return EnglishLanguage;
}

// An empty preference follows Windows, as does a tag this build does not carry.
inline std::wstring load_language(const std::wstring& directory) {
  if (directory.empty())
    return {};
  wchar_t tag[32]{};
  GetPrivateProfileStringW(LanguageSection, LanguageKey, L"", tag, 32, (directory + L"\\settings.ini").c_str());
  const auto* language = find_language(tag);
  return language ? language->tag : L"";
}

inline bool save_language(const std::wstring& directory, std::wstring_view preference) {
  const auto* language = find_language(preference);
  if (directory.empty() || (!preference.empty() && !language))
    return false;
  return WritePrivateProfileStringW(LanguageSection, LanguageKey, language ? language->tag : nullptr,
                                    (directory + L"\\settings.ini").c_str()) != FALSE;
}

// UI thread only. Other threads keep English and leave translation to the display.
inline const UiLanguage* active_language = &EnglishLanguage;
inline std::unordered_map<std::wstring_view, const wchar_t*> active_translations;

inline void select_language(const UiLanguage& language) {
  active_language = &language;
  active_translations.clear();
  for (const auto& entry : language.catalog)
    active_translations.emplace(entry.english, entry.text);
}

inline const wchar_t* tr(const wchar_t* english) {
  if (!english || active_translations.empty())
    return english;
  const auto found = active_translations.find(english);
  return found == active_translations.end() ? english : found->second;
}

// Replaces {0}..{9}. A placeholder without a value stays visible rather than
// reading past the list, so a faulty translation cannot misuse its arguments.
template <class Values>
std::wstring fill(std::wstring_view pattern, const Values& values) {
  std::wstring result;
  result.reserve(pattern.size() + 32);
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 1] >= L'0' && pattern[i + 1] <= L'9' && pattern[i + 2] == L'}') {
      const auto index = static_cast<size_t>(pattern[i + 1] - L'0');
      if (index < values.size()) {
        result += std::wstring_view(values.begin()[index]);
        i += 2;
        continue;
      }
    }
    result += pattern[i];
  }
  return result;
}

inline std::wstring tr_format(const wchar_t* english, std::initializer_list<std::wstring_view> values) {
  return fill(tr(english), values);
}

// Fixed-point numbers keep a decimal point, matching what the edit fields accept.
inline std::wstring decimal(double value, int places) {
  wchar_t text[64];
  std::swprintf(text, 64, L"%.*f", places, value);
  return text;
}

// English sentences and their values, translated when shown. The connection
// worker builds these, so it never reads the interface language, and a language
// change applies to text that is already on screen.
class UiMessage {
 public:
  UiMessage() = default;
  UiMessage(const wchar_t* english) { add(english); }
  UiMessage& add(std::wstring english, std::vector<std::wstring> values = {}) {
    parts_.push_back({std::move(english), std::move(values)});
    return *this;
  }
  std::wstring text() const {
    std::wstring result;
    for (const auto& part : parts_) {
      if (!result.empty())
        result += active_language->sentence_gap;
      result += fill(tr(part.english.c_str()), part.values);
    }
    return result;
  }

 private:
  struct Part {
    std::wstring english;
    std::vector<std::wstring> values;
  };
  std::vector<Part> parts_;
};
}  // namespace taxi_camera::standalone
