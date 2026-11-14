#pragma once
// The Community package that loads the panel-mark helper into every cockpit
// panel page (claudedocs/panel-mark-identification.md). It overrides one
// stock file, made from the installed one every time it changes; Taxi Cam
// never ships a copy, as one left behind by a simulator update could break
// every cockpit display:
// - page mode: html_ui/Pages/VCockpit/Core/VCockpit.html, the stock page plus
//   one script line after VCockpit.js. A stock page whose VCockpit.js line is
//   not exactly as expected removes the package instead.
// - script mode, when another Community package replaces that page (issue
//   184: vRAAS's panel-raas, the same order hint as ours and sorting before
//   us, still won in MSFS 2024, so which copy of the page loads cannot be
//   predicted): html_ui/Pages/VCockpit/Core/VCockpit.js, the stock script
//   with the helper added at its end. Every copy of the page loads it, so
//   our helper loads whichever page loads (seen with a stand-in made of
//   vRAAS's page and manifest, 2026-11-05; not with vRAAS itself). The
//   script is 23.5 KB of the simulator's logic, so this is used only when
//   the page is taken: a simulator update to it runs our copy for one
//   session until the next check rewrites it.
//   A copy of VCockpit.js in any other Community package rules that script
//   out, listed in its manifest or not (issue 200: with vRAAS taking the
//   page, no mark was read on a Steam PC while YourControls and
//   fscopilot-bridge, each with an unlisted copy, were installed, and marks
//   were read with both removed; a Store reporter saw the same with
//   YourControls, not checked in the log). Ours then replaces the
//   next script every copy of the page loads that no other package holds,
//   Instruments/Shared/Utils/XMLLogic.js (ScriptCarriers); not yet run in
//   the simulator.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// bcrypt.h needs windows.h first; this line keeps the two apart.
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace taxi_camera::standalone::marks_package {
inline constexpr wchar_t PackageName[] = L"taxi-cam-panel-marks";
// A folder name 1.3.3 test builds used to mount after another package's page
// (never shipped in a release); only removed now.
inline constexpr wchar_t LatePackageName[] = L"zzzz-taxi-cam-panel-marks";
inline constexpr char HelperName[] = "taxi-cam-marks.js";
inline constexpr wchar_t MarkerName[] = L"taxi-cam-marks.stock.txt";
inline constexpr char PageRelative[] = "html_ui/Pages/VCockpit/Core/VCockpit.html";
inline constexpr char HelperRelative[] = "html_ui/Pages/VCockpit/Core/taxi-cam-marks.js";
inline constexpr char ScriptRelative[] = "html_ui/Pages/VCockpit/Core/VCockpit.js";
inline constexpr char LogicRelative[] = "html_ui/Pages/VCockpit/Instruments/Shared/Utils/XMLLogic.js";
// The stock scripts ours can replace in script mode, in order of choice:
// where the file is, the `src` of the stock page's line that loads it, and
// how our manifest lists it.
struct Carrier {
  const char* relative;
  std::string_view src, listed;
  // The file's own name, for the marker and the logs.
  std::string_view name() const noexcept {
    const std::string_view path(relative);
    return path.substr(path.rfind('/') + 1);
  }
};
inline constexpr Carrier ScriptCarriers[] = {
    {ScriptRelative, "VCockpit.js", "\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.js"},
    {LogicRelative, "/Pages/VCockpit/Instruments/Shared/Utils/XMLLogic.js",
     "\\\\html_ui\\\\pages\\\\VCockpit\\\\Instruments\\\\Shared\\\\Utils\\\\XMLLogic.js"},
};
// Bumped when the files this writes change shape.
inline constexpr char FormatVersion[] = "3";
enum class Mode { page, script };
inline const char* mode_name(Mode mode) noexcept {
  return mode == Mode::script ? "script" : "page";
}
// taxi-cam-marks.js, built into taxi-cam.exe (app.rc).
inline constexpr int HelperResource = 201;
inline std::string_view helper_text() noexcept {
  const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(HelperResource), MAKEINTRESOURCEW(10));  // RT_RCDATA
  const HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
  const void* data = loaded ? LockResource(loaded) : nullptr;
  return data ? std::string_view(static_cast<const char*>(data), SizeofResource(nullptr, found)) : std::string_view{};
}

// A page's `<script ... src="VCockpit.js">` lines (or another script's, by
// its `src`): how many, and for the last one where the next line starts, its
// indent and its line ending.
struct ScriptLine {
  std::size_t count = 0, next = std::string_view::npos;
  std::string_view indent, ending;
};
inline ScriptLine script_lines(std::string_view stock, std::string_view src = ScriptCarriers[0].src) {
  std::string script = R"(<script type="text/javascript" src=")";
  script += src;
  script += R"("></script>)";
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
  return {count, found, indent, ending};
}
// Whether a page loads VCockpit.js (or that script) exactly once, by the
// stock line.
inline bool loads_script(std::string_view page, std::string_view src = ScriptCarriers[0].src) {
  return script_lines(page, src).count == 1;
}
// The stock page with the helper's script line after the VCockpit.js line,
// keeping that line's indent and line ending. Empty when the page has no or
// several such lines, or already names the helper.
inline std::string patch_page(std::string_view stock) {
  if (stock.find(HelperName) != std::string_view::npos)
    return {};
  const auto line = script_lines(stock);
  if (line.count != 1)
    return {};
  const auto found = line.next;
  std::string out(stock.substr(0, found));
  if (found == stock.size() && !out.empty() && out.back() != '\n')
    out += line.ending;
  out += line.indent;
  out += R"(<script type="text/javascript" src="/Pages/VCockpit/Core/)";
  out += HelperName;
  out += R"("></script>)";
  out += line.ending;
  out += stock.substr(found);
  return out;
}
// A stock script (VCockpit.js, XMLLogic.js) with the helper added at its
// end, on lines of its own. The helper waits for the page's panel by itself,
// so it runs the same from a script the page loads before VCockpit.js.
// Empty when the script is empty or already carries the helper.
inline constexpr std::string_view ScriptBanner = "// Taxi Cam panel marks (taxi-cam-marks.js), added by Taxi Cam.";
inline std::string patch_script(std::string_view stock, std::string_view helper) {
  if (stock.empty() || helper.empty() || stock.find(ScriptBanner) != std::string_view::npos)
    return {};
  std::string out(stock);
  if (out.back() != '\n')
    out += "\r\n";
  out += ScriptBanner;
  out += "\r\n";
  out += helper;
  if (out.back() != '\n')
    out += "\r\n";
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
// Our order hint, the SDK's for third-party packages that override panel
// files.
inline constexpr char OwnOrderHint[] = "PANEL_PATCH";
// Whether a manifest lists `relative` (an html_ui path) among its globally
// overridden files: in MSFS 2024 a package's copy of an html_ui file
// overrides the stock one only when it is listed there. Case and slashes
// are ignored.
inline bool lists_file(std::string_view manifest, std::string_view relative) {
  const auto key = manifest.find("\"globally_overriden_base_sim_files\"");
  const auto open = key == std::string_view::npos ? key : manifest.find('[', key);
  const auto close = open == std::string_view::npos ? open : manifest.find(']', open);
  if (close == std::string_view::npos)
    return false;
  const auto fold = [](std::string_view text) {
    std::string out;
    for (const char c : text)
      if (c != '\\' && c != '/')
        out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return out;
  };
  return fold(manifest.substr(open, close - open)).find(fold(relative)) != std::string::npos;
}
inline bool lists_page(std::string_view manifest) {
  return lists_file(manifest, PageRelative);
}
// The package's manifest, overriding the page or a script (VCockpit.js
// unless another is named).
inline std::string manifest_json(std::string_view minimum_version, Mode mode = Mode::page, const Carrier* carrier = nullptr) {
  std::string out =
      "{\r\n  \"dependencies\": [],\r\n  \"content_type\": \"MISC\",\r\n  \"title\": \"Taxi Cam panel marks\",\r\n"
      "  \"manufacturer\": \"\",\r\n  \"creator\": \"Taxi Cam\",\r\n  \"package_version\": \"1.0.0\",\r\n"
      "  \"minimum_game_version\": \"";
  out += minimum_version.empty() ? std::string_view{"1.0.0"} : minimum_version;
  out +=
      "\",\r\n  \"release_notes\": { \"neutral\": { \"LastUpdate\": \"\", \"OlderHistory\": \"\" } },\r\n"
      "  \"export_type\": \"Community\",\r\n  \"package_order_hint\": \"";
  out += OwnOrderHint;
  out += "\",\r\n  \"globally_overriden_base_sim_files\": [ \"";
  out += mode == Mode::script ? (carrier ? carrier : &ScriptCarriers[0])->listed
                              : std::string_view{"\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.html"};
  out += "\" ]\r\n}\r\n";
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
// What the marker file records: the mode, the stock file the package was
// made from and the helper. A difference in any of them rewrites it, so
// another package taking the page or giving it back, or a simulator update,
// is followed at the next check. A script other than VCockpit.js is named
// too; VCockpit.js is not, so a package 1.3.4 wrote stays current.
inline std::string marker_text(std::string_view stock_hash,
                               std::string_view helper_hash,
                               Mode mode = Mode::page,
                               const Carrier* carrier = nullptr) {
  std::string out = "format=";
  out += FormatVersion;
  out += "\r\nmode=";
  out += mode_name(mode);
  if (mode == Mode::script && carrier && carrier != &ScriptCarriers[0]) {
    out += "\r\ncarrier=";
    out += carrier->name();
  }
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
// Whether nothing of that name is left. Something deleted while another
// program still had it open stays listed until that program lets go.
inline bool absent(const std::filesystem::path& path) noexcept {
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    return false;
  const auto error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
// Deletes a folder or file and waits until it has really gone. Windows can
// refuse a folder's delete (not empty) or keep it listed while something still
// has a file in it open: 0.1 to 0.4% of deletes here, at any file age, cleared
// within 18 ms (2026-11-09). A delete that stops there leaves the package
// without its page, or an empty folder that is no longer recognisably ours.
inline bool remove_tree(const std::filesystem::path& path) {
  constexpr unsigned Tries = 20;
  constexpr DWORD RetryMs = 25;
  for (unsigned attempt = 1;; ++attempt) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (!error && absent(path))
      return true;
    if (attempt == Tries)
      return false;
    Sleep(RetryMs);
  }
}
}  // namespace detail

// The Community folder named by one simulator's UserCfg.opt, or empty.
inline std::filesystem::path community_from(const std::filesystem::path& options) {
  std::string text;
  if (!detail::read_file(options, text))
    return {};
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
  return {};
}
// Each simulator keeps its UserCfg.opt in its own place: the Microsoft Store
// build under LOCALAPPDATA\Packages, the Steam build under APPDATA.
inline std::filesystem::path store_options() {
  return detail::environment(L"LOCALAPPDATA") + L"\\Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\UserCfg.opt";
}
inline std::filesystem::path steam_options() {
  return detail::environment(L"APPDATA") + L"\\Microsoft Flight Simulator 2024\\UserCfg.opt";
}
// A Steam install lives under steamapps (steamapps\common\MSFS2024).
inline bool steam_simulator(const std::filesystem::path& simulator) {
  auto text = simulator.wstring();
  for (auto& c : text)
    c = static_cast<wchar_t>(towlower(c));
  return text.find(L"\\steamapps\\") != std::wstring::npos || text.find(L"/steamapps/") != std::wstring::npos;
}
// The Community folder of the simulator installed at `simulator` (its folder
// or executable), or empty. A PC can have both builds, each with its own
// Community folder, so it is never the other build's: Steam's for a Steam
// install; the Store's otherwise, or Steam's where the Store build has no
// settings (an install in an unusual place).
inline std::filesystem::path community_folder(const std::filesystem::path& simulator) {
  if (steam_simulator(simulator))
    return community_from(steam_options());
  const auto store = community_from(store_options());
  return store.empty() ? community_from(steam_options()) : store;
}
// Every simulator's Community folder on this PC, for the uninstaller.
inline std::vector<std::filesystem::path> community_folders() {
  std::vector<std::filesystem::path> out;
  for (const auto& options : {store_options(), steam_options()})
    if (auto community = community_from(options); !community.empty() && std::find(out.begin(), out.end(), community) == out.end())
      out.push_back(std::move(community));
  return out;
}
// The stock vcockpits package under a simulator folder, or empty.
inline std::filesystem::path stock_package(const std::filesystem::path& simulator) {
  for (const auto& candidate : {simulator / L"Packages\\asobo-vcockpits-core", simulator / L"Content\\Packages\\asobo-vcockpits-core"})
    if (detail::exists(candidate / PageRelative))
      return candidate;
  return {};
}
// A stock file of the cockpit pages, as installed: in the stock vcockpits
// package, else in the one other stock package beside it that has it. Empty
// when none or several have it.
inline std::filesystem::path stock_file(const std::filesystem::path& stock, const char* relative) {
  if (detail::exists(stock / relative))
    return stock / relative;
  std::filesystem::path found;
  std::error_code error;
  std::filesystem::directory_iterator entries(stock.parent_path(), std::filesystem::directory_options::skip_permission_denied, error);
  for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
    const auto candidate = entries->path() / relative;
    if (!detail::exists(candidate))
      continue;
    if (!found.empty())
      return {};
    found = candidate;
  }
  return found;
}
// Other Community packages that ship their own copy of a stock file every
// cockpit page loads: the page, its VCockpit.js, XMLLogic.js, or
// BaseInstrument.js (which runs in every page). Names only, sorted; packages
// directly in the Community folder; Taxi Cam's own package (the one with its
// marker) left out.
inline constexpr char BaseInstrumentRelative[] = "html_ui/Pages/VCockpit/Instruments/Shared/BaseInstrument.js";
struct PageOverrides {
  std::vector<std::wstring> page, script, logic, base_instrument;
  // The packages with a copy of that script, listed in their manifest or not.
  const std::vector<std::wstring>& holders(const Carrier& carrier) const noexcept {
    return &carrier == &ScriptCarriers[0] ? script : logic;
  }
};
inline PageOverrides other_page_overrides(const std::filesystem::path& community) {
  PageOverrides out;
  if (community.empty())
    return out;
  std::error_code error;
  std::filesystem::directory_iterator entries(community, std::filesystem::directory_options::skip_permission_denied, error);
  for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
    const auto& folder = entries->path();
    if (detail::exists(folder / MarkerName))
      continue;
    const auto name = folder.filename().wstring();
    if (detail::exists(folder / PageRelative))
      out.page.push_back(name);
    if (detail::exists(folder / ScriptRelative))
      out.script.push_back(name);
    if (detail::exists(folder / LogicRelative))
      out.logic.push_back(name);
    if (detail::exists(folder / BaseInstrumentRelative))
      out.base_instrument.push_back(name);
  }
  for (auto* list : {&out.page, &out.script, &out.logic, &out.base_instrument})
    std::sort(list->begin(), list->end());
  return out;
}
// The other packages whose manifest lists the stock page or its script as
// globally overridden. The page counts as taken only when listed (an unlisted
// copy of the page has not been seen to load). A script does not need
// listing to get in the way: ensure() goes by every copy
// (PageOverrides::holders), and `script` here is for the logs.
struct Takers {
  std::vector<std::wstring> page, script;
};
inline std::vector<std::wstring> listing(const std::filesystem::path& community,
                                         const std::vector<std::wstring>& names,
                                         std::string_view relative) {
  std::vector<std::wstring> out;
  for (const auto& name : names) {
    std::string manifest;
    if (detail::read_file(community / name / L"manifest.json", manifest) && lists_file(manifest, relative))
      out.push_back(name);
  }
  return out;
}
inline Takers takers(const std::filesystem::path& community) {
  const auto overrides = other_page_overrides(community);
  return {listing(community, overrides.page, PageRelative), listing(community, overrides.script, ScriptRelative)};
}
// Ours replaces the page while no other package does, else a script.
inline Mode mode_for(const Takers& taken) noexcept {
  return taken.page.empty() ? Mode::page : Mode::script;
}
// The other packages between Taxi Cam and the cockpit pages, for the
// companion's messages: those taking the page, and those holding a script
// ours had to pass over, up to the first script no package holds (all of
// them when every script is held). None while the page is free: ours is on
// the page then, where another package's copy of a script does no harm.
// Sorted, each once.
inline std::vector<std::wstring> in_the_way(const std::filesystem::path& community) {
  const auto overrides = other_page_overrides(community);
  auto out = listing(community, overrides.page, PageRelative);
  if (out.empty())
    return out;
  for (const auto& carrier : ScriptCarriers) {
    const auto& holders = overrides.holders(carrier);
    if (holders.empty())
      break;
    out.insert(out.end(), holders.begin(), holders.end());
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
// The script the package Taxi Cam wrote replaces, by its file name; empty in
// page mode, without a package, or for a marker that names no mode.
inline std::string written_carrier(const std::filesystem::path& community) {
  std::string marker;
  if (community.empty() || !detail::read_file(community / PackageName / MarkerName, marker) ||
      marker.find("\r\nmode=script\r\n") == std::string::npos)
    return {};
  constexpr std::string_view key = "\r\ncarrier=";
  const auto at = marker.find(key);
  if (at == std::string::npos)
    return std::string(ScriptCarriers[0].name());
  const auto end = marker.find("\r\n", at + key.size());
  return marker.substr(at + key.size(), end == std::string::npos ? end : end - at - key.size());
}

enum class Outcome { current, written, removed, no_community, no_simulator, page_changed, failed, conflict };
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
    case Outcome::conflict:
      return "conflict";
  }
  return "failed";
}
// Removes the package Taxi Cam made under one of its names. True when no
// folder of that name is left, or the folder there is not Taxi Cam's.
inline bool remove_named(const std::filesystem::path& community, const wchar_t* name) {
  const auto package = community / name;
  if (!detail::exists(package / MarkerName))
    return !detail::exists(package);
  return detail::remove_tree(package);
}
// Removes the package Taxi Cam made, under either name (the uninstaller).
// True when none is left.
inline bool remove(const std::filesystem::path& community) {
  if (community.empty())
    return true;
  const bool first = remove_named(community, PackageName);
  return remove_named(community, LatePackageName) && first;
}
// A simulator may be running but when it started is unknown: its mounted
// files are kept, as no check can tell they are no longer mounted.
inline constexpr std::uint64_t StartUnknown = ~std::uint64_t{0};
// When the running simulator started, as a FILETIME; 0 while none runs. The
// earliest of several; StartUnknown when the process list or one's start
// can't be read.
inline std::uint64_t simulator_started() noexcept {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return StartUnknown;
  std::uint64_t earliest = 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (_wcsicmp(entry.szExeFile, L"FlightSimulator2024.exe") != 0)
      continue;
    FILETIME created{}, exited{}, kernel{}, user{};
    std::uint64_t started = 0;
    if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
      if (GetProcessTimes(process, &created, &exited, &kernel, &user))
        started = std::uint64_t{created.dwHighDateTime} << 32 | created.dwLowDateTime;
      CloseHandle(process);
    }
    if (!started) {
      earliest = StartUnknown;
      break;
    }
    earliest = earliest ? std::min(earliest, started) : started;
  }
  CloseHandle(snapshot);
  return earliest;
}
inline std::string_view strip_bom(std::string_view text) noexcept {
  return text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text;
}
// Makes the package current: the stock page plus the helper's script line
// while no other package replaces the page, else a stock script plus the
// helper (mode_for): the first of ScriptCarriers that every taking page
// loads and no other package has a copy of. Re-run at every check, it
// follows another package taking the page or giving it back, and simulator
// updates. A package
// folder without the marker file is never touched: it is not one Taxi Cam
// made. `running_since`: when the running simulator started (FILETIME, 0
// when none runs, StartUnknown when one may run from an unknown time);
// found by itself when not given.
inline Outcome ensure(const std::filesystem::path& simulator,
                      const std::filesystem::path& community,
                      std::string_view helper,
                      std::optional<std::uint64_t> running_since = std::nullopt) {
  if (helper.empty())
    return Outcome::failed;
  if (community.empty())
    return Outcome::no_community;
  const auto stock = simulator.empty() ? std::filesystem::path{} : stock_package(simulator);
  if (stock.empty())
    return Outcome::no_simulator;
  std::string page, manifest, recorded;
  if (!detail::read_file(stock / PageRelative, page))
    return Outcome::failed;
  detail::read_file(stock / L"manifest.json", manifest);
  const auto package = community / PackageName;
  const auto marker = package / MarkerName;
  const bool ours = detail::exists(marker);
  if (detail::exists(package) && !ours)
    return Outcome::failed;
  const bool had = ours || detail::exists(community / LatePackageName / MarkerName);
  // A file Taxi Cam cannot add to leaves the package out.
  const auto leave_out = [&](Outcome why) {
    if (!marks_package::remove(community))
      return Outcome::failed;
    return why == Outcome::page_changed && had ? Outcome::removed : why;
  };
  const auto overrides = other_page_overrides(community);
  const Takers taken{listing(community, overrides.page, PageRelative), listing(community, overrides.script, ScriptRelative)};
  const auto mode = mode_for(taken);
  const Carrier* carrier = nullptr;
  std::string source, patched;
  if (mode == Mode::page) {
    source = page;
    patched = patch_page(strip_bom(page));
    if (patched.empty())
      return leave_out(Outcome::page_changed);
    // Keep the page's UTF-8 byte order mark, if it has one.
    if (strip_bom(page).size() != page.size())
      patched.insert(0, "\xEF\xBB\xBF");
  } else {
    // Whichever package's page loads has to load the script, and no other
    // package may have a copy of it, listed or not (issue 200). The first
    // script that passes carries the helper; with none, the add-ons in the
    // way are left to work alone (conflict), unless only the simulator's
    // own files ruled the scripts out (page_changed).
    std::vector<std::string> pages;
    for (const auto& name : taken.page) {
      std::string other;
      detail::read_file(community / name / PageRelative, other);
      pages.push_back(std::move(other));
    }
    bool blocked = false;
    for (const auto& candidate : ScriptCarriers) {
      if (!overrides.holders(candidate).empty() || !std::all_of(pages.begin(), pages.end(), [&](const std::string& other) {
            return loads_script(strip_bom(other), candidate.src);
          })) {
        blocked = true;
        continue;
      }
      const auto installed = stock_file(stock, candidate.relative);
      if (installed.empty() || !detail::read_file(installed, source))
        continue;
      patched = patch_script(source, helper);
      if (patched.empty())
        continue;
      carrier = &candidate;
      break;
    }
    if (!carrier)
      return leave_out(blocked ? Outcome::conflict : Outcome::page_changed);
  }
  // The 1.3.3 test builds' later-named folder is out of date wherever ours is.
  if (!remove_named(community, LatePackageName))
    return Outcome::failed;
  std::vector<std::pair<const char*, std::string_view>> files;
  if (mode == Mode::page)
    files = {{PageRelative, patched}, {HelperRelative, helper}};
  else
    files = {{carrier->relative, patched}};
  // Our files this mode doesn't use (the other mode's, or an older format's)
  // go, best effort, only where no running simulator has them mounted: with
  // none running, or once the package was written before the running one
  // started (it mounted this mode then). A running simulator mounted the
  // package as it was when it started, so a rewrite now keeps the old
  // mode's files for the rest of that session. A file another program holds
  // open (a virus scanner) is tried again at the next check. One left
  // behind replaces nothing: our manifest and layout.json don't list it.
  const auto started = running_since ? *running_since : simulator_started();
  const auto clear_unused = [&] {
    for (const char* relative : {PageRelative, HelperRelative, ScriptRelative, LogicRelative})
      if (std::none_of(files.begin(), files.end(), [&](const auto& file) { return std::string_view(file.first) == relative; })) {
        std::error_code ignored;
        std::filesystem::remove(package / relative, ignored);
      }
  };
  const auto expected = marker_text(detail::sha256(source), detail::sha256(helper), mode, carrier);
  // The marker alone doesn't say the files are there: a delete that gave up
  // part way leaves it behind without them.
  const bool complete = std::all_of(files.begin(), files.end(), [&](const auto& file) { return detail::exists(package / file.first); });
  if (ours && complete && detail::read_file(marker, recorded) && recorded == expected) {
    if (!started || (started != StartUnknown && detail::file_time(marker) < started))
      clear_unused();
    return Outcome::current;
  }
  std::error_code error;
  const bool fresh = !detail::exists(package);
  const auto give_up = [&] {
    if (fresh && !detail::exists(marker))
      detail::remove_tree(package);
    return Outcome::failed;
  };
  // Each file's folder (the scripts are not all in Core).
  for (const auto& file : files) {
    std::filesystem::create_directories((package / file.first).parent_path(), error);
    if (error)
      return give_up();
  }
  // The marker first, matching no hashes until the end: a write that stops
  // part way leaves a package Taxi Cam still owns, so the next check rewrites
  // it and the uninstaller removes it.
  if (!detail::write_file(marker, "incomplete\n"))
    return give_up();
  if (!started)
    clear_unused();
  for (const auto& [relative, text] : files)
    if (!detail::write_file(package / relative, text))
      return Outcome::failed;
  if (!detail::write_file(package / L"manifest.json", manifest_json(minimum_game_version(manifest), mode, carrier)))
    return Outcome::failed;
  std::vector<LayoutEntry> entries;
  for (const auto& [relative, text] : files) {
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
