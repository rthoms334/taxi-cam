#pragma once
// The Community package that loads the panel-mark helper into every cockpit
// panel page (claudedocs/panel-mark-identification.md). It overrides one
// stock file, html_ui/Pages/VCockpit/Core/VCockpit.html, with the simulator's
// own copy plus one script line after VCockpit.js, made from the installed
// stock page every time it changes. Taxi Cam never ships a copy of the page:
// a copy left behind by a simulator update could break every cockpit display,
// so a stock page whose VCockpit.js line is not exactly as expected removes
// the package instead.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace taxi_camera::standalone::marks_package {
inline constexpr wchar_t PackageName[] = L"taxi-cam-panel-marks";
inline constexpr char HelperName[] = "taxi-cam-marks.js";
inline constexpr wchar_t MarkerName[] = L"taxi-cam-marks.stock.txt";
inline constexpr char PageRelative[] = "html_ui/Pages/VCockpit/Core/VCockpit.html";
inline constexpr char HelperRelative[] = "html_ui/Pages/VCockpit/Core/taxi-cam-marks.js";
// Bumped when the files this writes change shape.
inline constexpr char FormatVersion[] = "1";
// taxi-cam-marks.js, built into taxi-cam.exe (app.rc).
inline constexpr int HelperResource = 201;
inline std::string_view helper_text() noexcept {
  const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(HelperResource), MAKEINTRESOURCEW(10));  // RT_RCDATA
  const HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
  const void* data = loaded ? LockResource(loaded) : nullptr;
  return data ? std::string_view(static_cast<const char*>(data), SizeofResource(nullptr, found)) : std::string_view{};
}

// The stock page with the helper's script line after the VCockpit.js line,
// keeping that line's indent and line ending. Empty when the page has no or
// several such lines, or already names the helper.
inline std::string patch_page(std::string_view stock) {
  constexpr std::string_view script = R"(<script type="text/javascript" src="VCockpit.js"></script>)";
  if (stock.find(HelperName) != std::string_view::npos)
    return {};
  std::size_t found = std::string_view::npos, count = 0, start = 0;
  std::string_view indent, ending;
  while (start < stock.size()) {
    auto end = stock.find('\n', start);
    const bool last = end == std::string_view::npos;
    if (last)
      end = stock.size();
    auto line = stock.substr(start, end - start);
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr)
      line.remove_suffix(1);
    const auto first = line.find_first_not_of(" \t");
    auto content = first == std::string_view::npos ? std::string_view{} : line.substr(first);
    while (!content.empty() && (content.back() == ' ' || content.back() == '\t'))
      content.remove_suffix(1);
    if (content == script) {
      ++count;
      found = last ? stock.size() : end + 1;
      indent = line.substr(0, first);
      ending = last ? std::string_view{"\r\n"} : cr ? std::string_view{"\r\n"} : std::string_view{"\n"};
    }
    start = last ? stock.size() : end + 1;
  }
  if (count != 1)
    return {};
  std::string out(stock.substr(0, found));
  if (found == stock.size() && !out.empty() && out.back() != '\n')
    out += ending;
  out += indent;
  out += R"(<script type="text/javascript" src="/Pages/VCockpit/Core/)";
  out += HelperName;
  out += R"("></script>)";
  out += ending;
  out += stock.substr(found);
  return out;
}
// "minimum_game_version" from a package manifest, or empty.
inline std::string minimum_game_version(std::string_view manifest) {
  const auto key = manifest.find("\"minimum_game_version\"");
  if (key == std::string_view::npos)
    return {};
  const auto open = manifest.find('"', manifest.find(':', key) + 1);
  const auto close = open == std::string_view::npos ? open : manifest.find('"', open + 1);
  if (close == std::string_view::npos)
    return {};
  const auto value = manifest.substr(open + 1, close - open - 1);
  for (const char c : value)
    if (!(c >= '0' && c <= '9') && c != '.')
      return {};
  return std::string(value);
}
inline std::string manifest_json(std::string_view minimum_version) {
  std::string out =
      "{\r\n  \"dependencies\": [],\r\n  \"content_type\": \"MISC\",\r\n  \"title\": \"Taxi Cam panel marks\",\r\n"
      "  \"manufacturer\": \"\",\r\n  \"creator\": \"Taxi Cam\",\r\n  \"package_version\": \"1.0.0\",\r\n"
      "  \"minimum_game_version\": \"";
  out += minimum_version.empty() ? std::string_view{"1.0.0"} : minimum_version;
  out +=
      "\",\r\n  \"release_notes\": { \"neutral\": { \"LastUpdate\": \"\", \"OlderHistory\": \"\" } },\r\n"
      "  \"export_type\": \"Community\",\r\n  \"package_order_hint\": \"PANEL_PATCH\",\r\n"
      "  \"globally_overriden_base_sim_files\": [ \"\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.html\" ]\r\n}\r\n";
  return out;
}
struct LayoutEntry {
  std::string path;
  std::uint64_t size{}, date{};
};
inline std::string layout_json(const std::vector<LayoutEntry>& entries) {
  std::string out = "{\r\n  \"content\": [";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    char line[512];
    std::snprintf(line, sizeof(line), "%s\r\n    { \"path\": \"%s\", \"size\": %llu, \"date\": %llu }", i ? "," : "",
                  entries[i].path.c_str(), static_cast<unsigned long long>(entries[i].size),
                  static_cast<unsigned long long>(entries[i].date));
    out += line;
  }
  out += "\r\n  ]\r\n}\r\n";
  return out;
}
// What the marker file records: the stock page and helper the package was
// made from. A difference in either rewrites it.
inline std::string marker_text(std::string_view stock_hash, std::string_view helper_hash) {
  std::string out = "format=";
  out += FormatVersion;
  out += "\r\nstock=";
  out += stock_hash;
  out += "\r\nhelper=";
  out += helper_hash;
  out += "\r\n";
  return out;
}

namespace detail {
inline bool read_file(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return in.good() || in.eof();
}
inline bool write_file(const std::filesystem::path& path, std::string_view data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}
inline std::string sha256(std::string_view data) {
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  std::array<unsigned char, 32> digest{};
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) >= 0 &&
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
  if (hash)
    BCryptDestroyHash(hash);
  if (algorithm)
    BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok)
    return {};
  std::string out;
  for (const auto b : digest) {
    char two[3];
    std::snprintf(two, sizeof(two), "%02X", b);
    out += two;
  }
  return out;
}
inline std::uint64_t file_time(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    return 0;
  return std::uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32 | data.ftLastWriteTime.dwLowDateTime;
}
inline std::wstring environment(const wchar_t* name) {
  wchar_t value[MAX_PATH]{};
  const auto n = GetEnvironmentVariableW(name, value, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(value, n) : std::wstring{};
}
inline bool exists(const std::filesystem::path& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
}
}  // namespace detail

// The Community folder from the simulator's UserCfg.opt (Microsoft Store,
// then Steam), or empty.
inline std::filesystem::path community_folder() {
  const std::filesystem::path options[]{
      detail::environment(L"LOCALAPPDATA") + L"\\Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\UserCfg.opt",
      detail::environment(L"APPDATA") + L"\\Microsoft Flight Simulator 2024\\UserCfg.opt"};
  for (const auto& option : options) {
    std::string text;
    if (!detail::read_file(option, text))
      continue;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
      const auto key = line.find("InstalledPackagesPath");
      const auto open = key == std::string::npos ? key : line.find('"', key);
      const auto close = open == std::string::npos ? open : line.find('"', open + 1);
      if (close == std::string::npos)
        continue;
      const auto folder = line.substr(open + 1, close - open - 1);
      const int wide = MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), nullptr, 0);
      std::wstring path(static_cast<std::size_t>(wide > 0 ? wide : 0), L'\0');
      if (wide > 0)
        MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), path.data(), wide);
      const auto community = std::filesystem::path(path) / L"Community";
      if (detail::exists(community))
        return community;
    }
  }
  return {};
}
// The folder holding FlightSimulator2024.exe: the running simulator's, else
// the Microsoft Store default, else Steam's. Empty when none is found.
inline std::filesystem::path simulator_folder() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::filesystem::path found;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && found.empty(); more = Process32NextW(snapshot, &entry)) {
      if (_wcsicmp(entry.szExeFile, L"FlightSimulator2024.exe") != 0)
        continue;
      if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size))
          found = std::filesystem::path(path).parent_path();
        CloseHandle(process);
      }
    }
    CloseHandle(snapshot);
    if (!found.empty())
      return found;
  }
  std::vector<std::filesystem::path> candidates{L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content"};
  HKEY key{};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
    wchar_t steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    if (RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, reinterpret_cast<BYTE*>(steam), &size) == ERROR_SUCCESS) {
      const std::filesystem::path root(steam);
      candidates.push_back(root / L"steamapps\\common\\Microsoft Flight Simulator 2024");
      // Other Steam libraries.
      std::string vdf;
      if (detail::read_file(root / L"steamapps\\libraryfolders.vdf", vdf)) {
        std::istringstream lines(vdf);
        for (std::string line; std::getline(lines, line);) {
          const auto key_at = line.find("\"path\"");
          const auto open = key_at == std::string::npos ? key_at : line.find('"', key_at + 6);
          const auto close = open == std::string::npos ? open : line.find('"', open + 1);
          if (close == std::string::npos)
            continue;
          std::string folder;
          for (std::size_t i = open + 1; i < close; ++i)
            if (!(line[i] == '\\' && i + 1 < close && line[i + 1] == '\\'))
              folder += line[i];
          candidates.push_back(std::filesystem::path(folder) / L"steamapps\\common\\Microsoft Flight Simulator 2024");
        }
      }
    }
    RegCloseKey(key);
  }
  for (const auto& candidate : candidates)
    if (detail::exists(candidate / L"FlightSimulator2024.exe"))
      return candidate;
  return {};
}
// The stock vcockpits package under a simulator folder, or empty.
inline std::filesystem::path stock_package(const std::filesystem::path& simulator) {
  for (const auto& candidate : {simulator / L"Packages\\asobo-vcockpits-core", simulator / L"Content\\Packages\\asobo-vcockpits-core"})
    if (detail::exists(candidate / PageRelative))
      return candidate;
  return {};
}

enum class Outcome { current, written, removed, no_community, no_simulator, page_changed, failed };
inline const char* outcome_name(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::current:
      return "current";
    case Outcome::written:
      return "written";
    case Outcome::removed:
      return "removed";
    case Outcome::no_community:
      return "no_community";
    case Outcome::no_simulator:
      return "no_simulator";
    case Outcome::page_changed:
      return "page_changed";
    case Outcome::failed:
      return "failed";
  }
  return "failed";
}
// Removes the package Taxi Cam made (the uninstaller). True when none is left.
inline bool remove(const std::filesystem::path& community) {
  if (community.empty())
    return true;
  const auto package = community / PackageName;
  if (!detail::exists(package / MarkerName))
    return !detail::exists(package);
  std::error_code error;
  std::filesystem::remove_all(package, error);
  return !error;
}
// Makes the package current for this simulator's stock page and helper.
// A package folder without the marker file is never touched: it is not one
// Taxi Cam made.
inline Outcome ensure(const std::filesystem::path& simulator, const std::filesystem::path& community, std::string_view helper) {
  if (helper.empty())
    return Outcome::failed;
  if (community.empty())
    return Outcome::no_community;
  const auto stock = simulator.empty() ? std::filesystem::path{} : stock_package(simulator);
  if (stock.empty())
    return Outcome::no_simulator;
  const auto package = community / PackageName;
  const auto marker = package / MarkerName;
  std::string page, manifest, recorded;
  if (!detail::read_file(stock / PageRelative, page))
    return Outcome::failed;
  detail::read_file(stock / L"manifest.json", manifest);
  const bool ours = detail::exists(marker);
  if (detail::exists(package) && !ours)
    return Outcome::failed;
  // Keep the page's UTF-8 byte order mark, if it has one.
  const bool bom = page.size() >= 3 && page.compare(0, 3, "\xEF\xBB\xBF") == 0;
  auto patched = patch_page(std::string_view(page).substr(bom ? 3 : 0));
  if (patched.empty()) {
    std::error_code ignored;
    if (ours && std::filesystem::remove_all(package, ignored) > 0)
      return Outcome::removed;
    return Outcome::page_changed;
  }
  if (bom)
    patched.insert(0, "\xEF\xBB\xBF");
  const auto expected = marker_text(detail::sha256(page), detail::sha256(helper));
  if (ours && detail::read_file(marker, recorded) && recorded == expected)
    return Outcome::current;
  std::error_code error;
  const auto core = package / L"html_ui\\Pages\\VCockpit\\Core";
  std::filesystem::create_directories(core, error);
  if (error)
    return Outcome::failed;
  if (!detail::write_file(package / PageRelative, patched) || !detail::write_file(package / HelperRelative, helper) ||
      !detail::write_file(package / L"manifest.json", manifest_json(minimum_game_version(manifest))))
    return Outcome::failed;
  std::vector<LayoutEntry> entries;
  for (const char* relative : {PageRelative, HelperRelative}) {
    const auto path = package / relative;
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    entries.push_back({relative, size_error ? 0 : size, detail::file_time(path)});
  }
  if (!detail::write_file(package / L"layout.json", layout_json(entries)) || !detail::write_file(marker, expected))
    return Outcome::failed;
  return Outcome::written;
}
}  // namespace taxi_camera::standalone::marks_package
