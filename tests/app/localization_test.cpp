#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "../../src/app/localization.hpp"
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}

// A translated printf format has to keep every conversion the caller supplies.
unsigned conversions(const wchar_t* text) {
  unsigned count{};
  for (const wchar_t* p = text; (p = std::wcschr(p, L'%')) != nullptr; ++p)
    if (p[1] != L'%')
      ++count;
  return count;
}

const wchar_t* lookup(const wchar_t* english) {
  apply_language(UiLanguage::simplified_chinese, L"en-US");
  const auto* value = tr(english);
  apply_language(UiLanguage::english, L"zh-CN");
  return value;
}
}  // namespace

int main() {
  using namespace taxi_camera::standalone;
  try {
    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read isolated fixture root");
    const auto directory = std::wstring(cwd) + L"\\build\\localization-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated preference directory");

    require(std::size(kChineseCatalog) > 100, "Catalog carries the interface wording");
    for (size_t i = 0; i < std::size(kChineseCatalog); ++i) {
      const auto& message = kChineseCatalog[i];
      require(message.english[0] && message.chinese[0], "Catalog entry has both sides");
      require(conversions(message.english) == conversions(message.chinese), "Catalog entry keeps every format conversion");
      bool unique = true;
      for (size_t j = 0; j < i; ++j)
        if (std::wcscmp(kChineseCatalog[j].english, message.english) == 0)
          unique = false;
      if (!unique)
        std::fwprintf(stderr, L"duplicate key: %ls\n", message.english);
      require(unique, "Catalog key is not defined twice");
      require(std::wcscmp(message.english, message.chinese) != 0, "Catalog entry is translated");
    }

    require(lookup(L"Save changes") == std::wstring(L"保存更改"), "Known control resolves to Chinese");
    require(lookup(L"Not a catalogue entry") == std::wstring(L"Not a catalogue entry"), "Unknown wording passes through");
    require(lookup(L"") == std::wstring(L""), "Empty label passes through");
    require(tr(nullptr) == nullptr, "Missing label cannot break lookup");

    apply_language(UiLanguage::english, L"zh-CN");
    require(tr(L"Save changes") == std::wstring(L"Save changes"), "English preference ignores a Chinese system locale");
    apply_language(UiLanguage::simplified_chinese, L"en-US");
    require(tr(L"Save changes") == std::wstring(L"保存更改"), "Chinese preference wins on an English system");
    apply_language(UiLanguage::follow_windows, L"zh-CN");
    require(tr(L"Save changes") == std::wstring(L"保存更改"), "Follow Windows resolves a Chinese locale");
    apply_language(UiLanguage::follow_windows, L"zh-Hans-HK");
    require(tr(L"Save changes") == std::wstring(L"保存更改"), "Follow Windows accepts an explicit Hans tag");
    apply_language(UiLanguage::follow_windows, L"zh-TW");
    require(tr(L"Save changes") == std::wstring(L"Save changes"), "Traditional-only locales keep the English interface");
    apply_language(UiLanguage::follow_windows, L"en-GB");
    require(tr(L"Save changes") == std::wstring(L"Save changes"), "Follow Windows resolves an English locale");
    apply_language(UiLanguage::follow_windows, L"");
    require(tr(L"Save changes") == std::wstring(L"Save changes"), "Missing locale name falls back to English");
    apply_language(UiLanguage::english, L"en-US");

    require(locale_is_simplified_chinese(L"zh") && locale_is_simplified_chinese(L"zh-SG"), "Simplified locales are recognized");
    require(!locale_is_simplified_chinese(L"zh-HK") && !locale_is_simplified_chinese(L"zxx-CN"), "Other locales stay English");
    require(!locale_is_simplified_chinese(nullptr), "Missing locale name cannot crash detection");
    require(ui_locale_name().size() > 1 || ui_locale_name().empty(), "Windows reports a usable locale name");

    require(load_language(directory) == UiLanguage::follow_windows, "Missing preference follows Windows");
    require(save_language(directory, UiLanguage::simplified_chinese) && load_language(directory) == UiLanguage::simplified_chinese,
            "Persisted preference reloads as Chinese");
    require(save_language(directory, UiLanguage::english) && load_language(directory) == UiLanguage::english,
            "Persisted preference reloads as English");
    require(!save_language(directory, static_cast<UiLanguage>(9)), "Unknown language value is refused");
    require(!save_language(L"", UiLanguage::english), "Missing directory cannot write preference");
    require(load_language(L"") == UiLanguage::follow_windows, "Missing directory fails open to the default");
    require(WritePrivateProfileStringW(LanguageSection, LanguageKey, L"255", (directory + L"\\settings.ini").c_str()),
            "Write an out-of-range language value");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, (directory + L"\\settings.ini").c_str());
    require(load_language(directory) == UiLanguage::follow_windows, "Out-of-range language value falls back");
    require(DeleteFileW((directory + L"\\settings.ini").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND, "Remove preference file");

    apply_language(UiLanguage::simplified_chinese, L"zh-CN");
    require(std::wstring(menu_label(UiLanguage::english)) == L"English", "English option names its own language");
    require(std::wstring(menu_label(UiLanguage::simplified_chinese)) == L"简体中文", "Chinese option names its own language");
    require(std::wstring(menu_label(UiLanguage::follow_windows)) == L"跟随 Windows", "Follow option follows the menu language");
    apply_language(UiLanguage::english, L"en-US");
    require(std::wstring(menu_label(UiLanguage::follow_windows)) == L"Follow Windows", "Follow option stays English here");
    require(RemoveDirectoryW(directory.c_str()), "Remove isolated fixture");

    std::printf("PASS localization: %u catalogue, resolution and preference checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL localization: %s\n", error.what());
    return 1;
  }
}
