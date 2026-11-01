#include "../../src/app/localization.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}

std::wstring read_utf8(const std::wstring& path) {
  const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Could not open a companion source file");
  LARGE_INTEGER size{};
  std::string bytes;
  DWORD read{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
  if (ok) {
    bytes.resize(static_cast<size_t>(size.QuadPart));
    ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  }
  CloseHandle(file);
  if (!ok || read != bytes.size())
    throw std::runtime_error("Could not read a companion source file");
  std::wstring text(bytes.size(), L'\0');
  text.resize(static_cast<size_t>(
      MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), static_cast<int>(text.size()))));
  return text;
}

// Reads one or more adjacent L"..." literals, as the compiler would join them.
bool read_literal(const std::wstring& source, size_t& at, std::wstring& value) {
  bool found = false;
  for (;;) {
    while (at < source.size() && iswspace(source[at]))
      ++at;
    if (source.compare(at, 2, L"L\"") != 0)
      return found;
    found = true;
    for (at += 2; at < source.size() && source[at] != L'"'; ++at) {
      if (source[at] != L'\\') {
        value += source[at];
        continue;
      }
      const wchar_t escape = source[++at];
      if (escape == L'n')
        value += L'\n';
      else if (escape == L't')
        value += L'\t';
      else if (escape == L'r')
        value += L'\r';
      else if (escape == L'x' || escape == L'u') {
        unsigned code{};
        while (at + 1 < source.size() && iswxdigit(source[at + 1])) {
          const wchar_t digit = towlower(source[++at]);
          code = code * 16 + (digit <= L'9' ? digit - L'0' : digit - L'a' + 10);
        }
        value += static_cast<wchar_t>(code);
      } else
        value += escape;
    }
    ++at;
  }
}

// Every literal passed straight to tr(), tr_format() or ui_text() in the companion sources.
std::set<std::wstring> marked_wording(const std::wstring& root) {
  std::set<std::wstring> wording;
  for (const wchar_t* folder : {L"\\src\\app\\", L"\\src\\shared\\"}) {
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((root + folder + L"*").c_str(), &entry);
    require(search != INVALID_HANDLE_VALUE, "List companion sources");
    do {
      const std::wstring name = entry.cFileName;
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (!name.ends_with(L".cpp") && !name.ends_with(L".hpp")))
        continue;
      const auto source = read_utf8(root + folder + name);
      for (const wchar_t* marker : {L"tr(", L"tr_format(", L"ui_text("}) {
        const std::wstring_view call = marker;
        for (size_t at = source.find(call); at != std::wstring::npos; at = source.find(call, at + 1)) {
          if (at && (iswalnum(source[at - 1]) || source[at - 1] == L'_'))
            continue;
          size_t cursor = at + call.size();
          std::wstring value;
          if (read_literal(source, cursor, value))
            wording.insert(value);
        }
      }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  return wording;
}

std::multiset<std::wstring> placeholders(std::wstring_view text) {
  std::multiset<std::wstring> result;
  for (size_t i = 0; i + 2 < text.size(); ++i)
    if (text[i] == L'{' && text[i + 1] >= L'0' && text[i + 1] <= L'9' && text[i + 2] == L'}')
      result.insert(std::wstring(text.substr(i, 3)));
  return result;
}

std::string utf8(const std::wstring& text) {
  std::string result(text.size() * 4 + 1, '\0');
  result.resize(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                                                        static_cast<int>(result.size()), nullptr, nullptr)));
  return result;
}

void print(const char* label, const wchar_t* tag, const std::wstring& text) {
  std::fprintf(stderr, "%s [%ls]: %s\n", label, tag, utf8(text).c_str());
}

// --missing <tag> prints untranslated wording as catalog entries to fill in.
void print_missing(const std::wstring& root, const char* tag) {
  const auto* language = find_language(std::wstring(tag, tag + std::strlen(tag)));
  require(language, "Name a language from UiLanguages");
  std::set<std::wstring> keys;
  for (const auto& entry : language->catalog)
    keys.insert(entry.english);
  for (const auto& english : marked_wording(root)) {
    if (keys.contains(english))
      continue;
    std::wstring literal;
    for (const wchar_t c : english)
      literal += c == L'\n' ? L"\\n" : c == L'\t' ? L"\\t" : c == L'"' ? L"\\\"" : c == L'\\' ? L"\\\\" : std::wstring(1, c);
    std::printf("    {L\"%s\", L\"\"},\n", utf8(literal).c_str());
  }
}

void catalogs(const std::wstring& root, std::wstring_view only = {}) {
  const auto wording = marked_wording(root);
  require(wording.size() > 300, "Companion wording is marked for translation");
  require(wording.contains(L"Save changes") && wording.contains(L"Waiting for Microsoft Flight Simulator 2024"),
          "Source scan finds marked control and status wording");
  for (const auto& language : UiLanguages) {
    if (!only.empty() && only != language.tag)
      continue;
    std::set<std::wstring> keys;
    unsigned stale{};
    for (const auto& entry : language.catalog) {
      require(entry.english && entry.english[0] && entry.text && entry.text[0], "Catalog entry has both sides");
      require(keys.insert(entry.english).second, "Catalog key is not defined twice");
      require(placeholders(entry.english) == placeholders(entry.text), "Translation keeps every {n} value");
      if (!wording.contains(entry.english)) {
        print("stale key", language.tag, entry.english);
        ++stale;
      }
    }
    // An outdated key silently shows English, so it has to be updated with the source.
    require(!stale, "Every catalog key matches current English wording");
    if (language.catalog.empty())
      continue;
    unsigned missing{};
    for (const auto& english : wording)
      if (!keys.contains(english))
        ++missing;
    std::fprintf(stderr, "%ls: %zu of %zu strings translated\n", language.tag, wording.size() - missing, wording.size());
    require(!missing, "Shipped languages translate all marked wording");
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const bool missing = argc == 4 && !std::strcmp(argv[2], "--missing"), check = argc == 4 && !std::strcmp(argv[2], "--check");
    require(argc == 2 || missing || check, "Pass the repository root, optionally --missing <tag> or --check <tag>");
    std::wstring root(argv[1], argv[1] + std::strlen(argv[1]));
    if (missing) {
      print_missing(root, argv[3]);
      return 0;
    }
    if (check) {
      // One catalog's keys, values and coverage, for a translator working alone.
      catalogs(root, std::wstring(argv[3], argv[3] + std::strlen(argv[3])));
      std::printf("Catalog checks passed: %u\n", checks);
      return 0;
    }
    // Windows display languages, in preference order, decide the default.
    const std::vector<std::wstring> german{L"de-DE", L"en-US"}, british{L"en-GB", L"de-DE"}, none{};
    require(resolve_language(L"", german).tag == std::wstring(L"de"), "German display language selects German");
    require(resolve_language(L"", british).tag == std::wstring(L"en"), "English first keeps English");
    require(resolve_language(L"", none).tag == std::wstring(L"en"), "No display language falls back to English");
    require(resolve_language(L"fr", german).tag == std::wstring(L"fr"), "An explicit choice wins over Windows");
    require(resolve_language(L"xx", german).tag == std::wstring(L"de"), "An unknown choice follows Windows");
    const std::vector<std::wstring> regional{L"es-MX"}, canadian{L"fr-CA"}, unknown{L"pt-BR", L"es-ES"};
    require(resolve_language(L"", regional).tag == std::wstring(L"es"), "Regional Spanish selects Spanish");
    require(resolve_language(L"", canadian).tag == std::wstring(L"fr"), "Canadian French selects French");
    require(resolve_language(L"", unknown).tag == std::wstring(L"es"), "A language without a catalog falls through to the next");
    for (const wchar_t* simplified : {L"zh-CN", L"zh-SG", L"zh-Hans-HK", L"zh", L"ZH-cn"})
      require(match_language(simplified) && match_language(simplified)->tag == std::wstring(L"zh-Hans"),
              "Simplified Chinese is recognised");
    for (const wchar_t* traditional : {L"zh-TW", L"zh-HK", L"zh-MO", L"zh-Hant-TW"})
      require(!match_language(traditional), "Traditional Chinese keeps English until it has a catalog");
    require(!match_language(L"") && !match_language(L"zxx-CN"), "Other names have no catalog");
    require(!windows_display_languages().empty(), "Windows reports a display language");

    // Preferences store the tag, so adding a language never renumbers the others.
    wchar_t temp[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temp), "Find the temporary folder");
    const auto directory = std::wstring(temp) + L"taxi-cam-localization-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated preference folder");
    require(load_language(directory).empty(), "Missing preference follows Windows");
    require(save_language(directory, L"de") && load_language(directory) == L"de", "German preference reloads");
    require(save_language(directory, L"ZH-hans") && load_language(directory) == L"zh-Hans", "Tag is stored in its canonical form");
    require(save_language(directory, L"") && load_language(directory).empty(), "Following Windows removes the stored tag");
    require(!save_language(directory, L"xx") && !save_language(L"", L"de"), "Unknown tags and missing folders are refused");
    require(WritePrivateProfileStringW(LanguageSection, LanguageKey, L"2", (directory + L"\\settings.ini").c_str()),
            "Write an unknown stored value");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, (directory + L"\\settings.ini").c_str());
    require(load_language(directory).empty(), "An unknown stored value follows Windows");
    DeleteFileW((directory + L"\\settings.ini").c_str());
    require(RemoveDirectoryW(directory.c_str()), "Remove isolated preference folder");

    // Values fill by position, and a missing value cannot read past the list.
    require(fill(std::wstring_view(L"{1} then {0}"), std::initializer_list<std::wstring_view>{L"a", L"b"}) == L"b then a",
            "Translations can reorder values");
    require(fill(std::wstring_view(L"{0} {2} {x} {"), std::initializer_list<std::wstring_view>{L"a"}) == L"a {2} {x} {",
            "Unknown placeholders stay visible");
    select_language(*find_language(L"de"));
    require(tr(L"Not catalogue wording") == std::wstring(L"Not catalogue wording") && tr(nullptr) == nullptr,
            "Unknown wording passes through");
    UiMessage message(L"Not catalogue wording.");
    message.add(L"Not catalogue value {0}.", {L"5"});
    require(message.text() == L"Not catalogue wording. Not catalogue value 5.", "Message parts join as sentences");
    select_language(*find_language(L"zh-Hans"));
    require(message.text() == L"Not catalogue wording.Not catalogue value 5.", "Chinese joins sentences without a space");
    select_language(EnglishLanguage);
    require(tr(L"Language") == std::wstring(L"Language"), "English shows the source wording");

    catalogs(root);
    std::printf("Localization checks passed: %u\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Localization test failed: %s\n", error.what());
    return 1;
  }
}
