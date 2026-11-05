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
// bcrypt.h needs windows.h first; this line keeps the two apart.
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace taxi_camera::standalone::marks_package {
inline constexpr wchar_t PackageName[] = L"taxi-cam-panel-marks";
// The folder name used instead when the package whose page ours is built on
// sorts after PackageName: per the MSFS 2024 SDK (Virtual File System
// Information), packages of one order hint mount in alphabetical order and the
// one mounted later wins. Not yet observed against a conflicting package.
inline constexpr wchar_t LatePackageName[] = L"zzzz-taxi-cam-panel-marks";
inline constexpr char HelperName[] = "taxi-cam-marks.js";
inline constexpr wchar_t MarkerName[] = L"taxi-cam-marks.stock.txt";
inline constexpr char PageRelative[] = "html_ui/Pages/VCockpit/Core/VCockpit.html";
inline constexpr char HelperRelative[] = "html_ui/Pages/VCockpit/Core/taxi-cam-marks.js";
// Bumped when the files this writes change shape.
inline constexpr char FormatVersion[] = "2";
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
// Our own hint, used when the page is the stock one.
inline constexpr char OwnOrderHint[] = "PANEL_PATCH";
// "package_order_hint" from a package manifest: letters, digits and '_' only,
// else empty (also when the manifest sets none, which MSFS mounts last).
inline std::string order_hint(std::string_view manifest) {
  const auto key = manifest.find("\"package_order_hint\"");
  if (key == std::string_view::npos)
    return {};
  const auto colon = manifest.find(':', key);
  const auto open = colon == std::string_view::npos ? colon : manifest.find('"', colon + 1);
  const auto close = open == std::string_view::npos ? open : manifest.find('"', open + 1);
  if (close == std::string_view::npos)
    return {};
  const auto value = manifest.substr(open + 1, close - open - 1);
  if (value.empty() || value.size() > 64)
    return {};
  for (const char c : value)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return {};
  return std::string(value);
}
// Whether a manifest lists the cockpit page among its globally overridden
// files: in MSFS 2024 a package's copy of an html_ui file overrides the
// stock one only when it is listed there.
inline bool lists_page(std::string_view manifest) {
  const auto key = manifest.find("\"globally_overriden_base_sim_files\"");
  const auto open = key == std::string_view::npos ? key : manifest.find('[', key);
  const auto close = open == std::string_view::npos ? open : manifest.find(']', open);
  if (close == std::string_view::npos)
    return false;
  std::string list;
  for (const char c : manifest.substr(open, close - open))
    if (c != '\\' && c != '/')
      list += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return list.find("html_uipagesvcockpitcorevcockpit.html") != std::string::npos;
}
// The package's manifest; `hint` empty leaves package_order_hint out, as the
// package our page is built on may (MSFS then mounts both last).
inline std::string manifest_json(std::string_view minimum_version, std::string_view hint = OwnOrderHint) {
  std::string out =
      "{\r\n  \"dependencies\": [],\r\n  \"content_type\": \"MISC\",\r\n  \"title\": \"Taxi Cam panel marks\",\r\n"
      "  \"manufacturer\": \"\",\r\n  \"creator\": \"Taxi Cam\",\r\n  \"package_version\": \"1.0.0\",\r\n"
      "  \"minimum_game_version\": \"";
  out += minimum_version.empty() ? std::string_view{"1.0.0"} : minimum_version;
  out +=
      "\",\r\n  \"release_notes\": { \"neutral\": { \"LastUpdate\": \"\", \"OlderHistory\": \"\" } },\r\n"
      "  \"export_type\": \"Community\",\r\n";
  if (!hint.empty()) {
    out += "  \"package_order_hint\": \"";
    out += hint;
    out += "\",\r\n";
  }
  out += "  \"globally_overriden_base_sim_files\": [ \"\\\\html_ui\\\\pages\\\\VCockpit\\\\Core\\\\VCockpit.html\" ]\r\n}\r\n";
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
// What the marker file records: the page the package was made from (the
// stock page, or another package's copy of it, with that package's order
// hint) and the helper. A difference in any of them rewrites it, so another
// package added, changed or removed later is followed at the next check.
inline std::string marker_text(std::string_view page_hash,
                               std::string_view helper_hash,
                               std::string_view source = "stock",
                               std::string_view hint = OwnOrderHint) {
  std::string out = "format=";
  out += FormatVersion;
  out += "\r\nsource=";
  out += source;
  out += "\r\nhint=";
  out += hint;
  out += "\r\npage=";
  out += page_hash;
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
// Other Community packages that ship their own copy of a stock file every
// cockpit page loads: the page itself or BaseInstrument.js (which runs in
// every page). Names only, sorted; packages directly in the Community
// folder; Taxi Cam's own package (the one with its marker) left out.
inline constexpr char BaseInstrumentRelative[] = "html_ui/Pages/VCockpit/Instruments/Shared/BaseInstrument.js";
struct PageOverrides {
  std::vector<std::wstring> page, base_instrument;
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
    if (detail::exists(folder / BaseInstrumentRelative))
      out.base_instrument.push_back(name);
  }
  std::sort(out.page.begin(), out.page.end());
  std::sort(out.base_instrument.begin(), out.base_instrument.end());
  return out;
}
// The other package whose copy of the cockpit page MSFS would load instead
// of the stock one: among those listing it as a globally overridden file, the
// last in mount order. The MSFS 2024 SDK documents mounting by order hint
// group, then by folder name within a group, the later mount winning; the
// order between groups is not documented. Packages with no hint are taken as
// mounted last (community reports), so they rank after hinted ones; listing
// packages with different hints otherwise cannot be ordered: `ambiguous`.
struct PageSource {
  std::wstring package;  // empty: the stock page
  std::string hint;      // its package_order_hint, or empty when it sets none
  bool ambiguous = false;
};
inline std::wstring lower(std::wstring text) {
  for (auto& c : text)
    c = static_cast<wchar_t>(towlower(c));
  return text;
}
inline PageSource page_source(const std::filesystem::path& community) {
  std::vector<PageSource> listing;
  for (const auto& name : other_page_overrides(community).page) {
    std::string manifest;
    if (detail::read_file(community / name / L"manifest.json", manifest) && lists_page(manifest))
      listing.push_back({name, order_hint(manifest)});
  }
  // Unhinted packages, when any, mount after every hinted one.
  const bool unhinted = std::any_of(listing.begin(), listing.end(), [](const PageSource& p) { return p.hint.empty(); });
  PageSource out;
  for (const auto& candidate : listing) {
    if (unhinted && !candidate.hint.empty())
      continue;
    if (!out.package.empty() && candidate.hint != out.hint)
      out.ambiguous = true;
    if (out.package.empty() || lower(candidate.package) > lower(out.package))
      out = {candidate.package, candidate.hint, out.ambiguous};
  }
  return out;
}
inline std::string narrow(const std::wstring& text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
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
  std::error_code error;
  std::filesystem::remove_all(package, error);
  return !error;
}
// Removes the package Taxi Cam made, under either name (the uninstaller).
// True when none is left.
inline bool remove(const std::filesystem::path& community) {
  if (community.empty())
    return true;
  const bool first = remove_named(community, PackageName);
  return remove_named(community, LatePackageName) && first;
}
// Makes the package current for the cockpit page MSFS would otherwise load
// (the stock page, or another package's copy of it; page_source) and the
// helper: that page plus the helper's script line, with the other package's
// order hint and a folder name after it, so ours mounts after it and both
// add-ons' scripts load. Re-run at every check, it follows that package
// being added, changed or removed. A package folder without the marker file
// is never touched: it is not one Taxi Cam made.
inline Outcome ensure(const std::filesystem::path& simulator, const std::filesystem::path& community, std::string_view helper) {
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
  const auto source = page_source(community);
  std::string hint = OwnOrderHint;
  if (!source.package.empty()) {
    if (!detail::read_file(community / source.package / PageRelative, page))
      return Outcome::failed;
    hint = source.hint;
  }
  const wchar_t* name = !source.package.empty() && lower(source.package) >= lower(PackageName) ? LatePackageName : PackageName;
  const wchar_t* other_name = name == PackageName ? LatePackageName : PackageName;
  // A source that cannot be placed before ours (listing packages whose order
  // cannot be told apart, or one named after even the late name) would still
  // load instead of ours: left to work alone, as an unusable page is below.
  const bool unplaceable = source.ambiguous || (!source.package.empty() && lower(source.package) >= lower(LatePackageName));
  const auto package = community / name;
  const auto marker = package / MarkerName;
  const bool ours = detail::exists(marker);
  if (detail::exists(package) && !ours)
    return Outcome::failed;
  // Keep the page's UTF-8 byte order mark, if it has one.
  const bool bom = page.size() >= 3 && page.compare(0, 3, "\xEF\xBB\xBF") == 0;
  auto patched = patch_page(std::string_view(page).substr(bom ? 3 : 0));
  if (patched.empty() || unplaceable) {
    // Another package's page Taxi Cam cannot add to is left to work alone:
    // a package of ours mounted after it would replace that add-on's page.
    const bool had = ours || detail::exists(community / other_name / MarkerName);
    if (!marks_package::remove(community))
      return Outcome::failed;
    if (!source.package.empty())
      return Outcome::conflict;
    return had ? Outcome::removed : Outcome::page_changed;
  }
  if (bom)
    patched.insert(0, "\xEF\xBB\xBF");
  const auto source_name = source.package.empty() ? std::string("stock") : narrow(source.package);
  const auto expected = marker_text(detail::sha256(page), detail::sha256(helper), source_name, hint);
  // The package under the other name is out of date wherever this one goes.
  if (!remove_named(community, other_name))
    return Outcome::failed;
  if (ours && detail::read_file(marker, recorded) && recorded == expected)
    return Outcome::current;
  std::error_code error;
  const bool fresh = !detail::exists(package);
  const auto core = package / L"html_ui\\Pages\\VCockpit\\Core";
  const auto give_up = [&] {
    std::error_code ignored;
    if (fresh && !detail::exists(marker))
      std::filesystem::remove_all(package, ignored);
    return Outcome::failed;
  };
  std::filesystem::create_directories(core, error);
  if (error)
    return give_up();
  // The marker first, matching no hashes until the end: a write that stops
  // part way leaves a package Taxi Cam still owns, so the next check rewrites
  // it and the uninstaller removes it.
  if (!detail::write_file(marker, "incomplete\n"))
    return give_up();
  if (!detail::write_file(package / PageRelative, patched) || !detail::write_file(package / HelperRelative, helper) ||
      !detail::write_file(package / L"manifest.json", manifest_json(minimum_game_version(manifest), hint)))
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
