#pragma once
// Display choices saved before panel marks name each display by its
// panel.cfg texture= name (for example $EFIS_LEFT). The bridge now names
// displays by panel-mark key (section and first gauge, mark_identity.hpp),
// so such a name is converted once from the aircraft's own panel.cfg files:
// the [VCockpitNN] section with that texture=, and its htmlgauge00, the gauge
// whose block names the texture.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "../shared/mark_identity.hpp"

namespace taxi_camera::standalone::legacy_names {
struct Section {
  std::string section, texture, gauge;
};
// The gauge name a mark carries for an htmlgauge URL: wasm_gauge for a WASM
// gauge, else the page's file name (taxi-cam-marks.js gaugeName).
inline std::string gauge_name(std::string_view url) {
  const auto query = url.find('?');
  for (std::size_t at = query; at != std::string_view::npos && at < url.size();) {
    const auto start = at + 1;
    auto end = url.find('&', start);
    if (end == std::string_view::npos)
      end = url.size();
    const auto pair = url.substr(start, end - start);
    if (pair.rfind("wasm_gauge=", 0) == 0)
      return std::string(pair.substr(11));
    at = end < url.size() ? end : std::string_view::npos;
  }
  auto path = url.substr(0, query);
  const auto slash = path.find_last_of("/\\");
  if (slash != std::string_view::npos)
    path.remove_prefix(slash + 1);
  for (const std::string_view extension : {".html", ".htm", ".HTML", ".HTM"})
    if (path.size() > extension.size() && path.substr(path.size() - extension.size()) == extension) {
      path.remove_suffix(extension.size());
      break;
    }
  return std::string(path);
}
inline std::string trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r");
  return std::string(text.substr(first, last - first + 1));
}
// The [VCockpitNN] sections of a panel.cfg with their texture= and first gauge.
inline std::vector<Section> parse_panel_cfg(std::string_view text) {
  std::vector<Section> out;
  Section current;
  bool in_vcockpit = false;
  const auto flush = [&] {
    if (in_vcockpit && !current.texture.empty())
      out.push_back(current);
  };
  std::size_t start = 0;
  while (start <= text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos)
      end = text.size();
    auto line = trim(text.substr(start, end - start));
    start = end + 1;
    if (const auto comment = line.find("//"); comment != std::string::npos)
      line = trim(line.substr(0, comment));
    if (line.empty() || line[0] == ';')
      continue;
    if (line.front() == '[') {
      flush();
      current = {};
      const auto close = line.find(']');
      current.section = close == std::string::npos ? std::string{} : trim(std::string_view(line).substr(1, close - 1));
      in_vcockpit = current.section.rfind("VCockpit", 0) == 0;
      continue;
    }
    const auto equals = line.find('=');
    if (!in_vcockpit || equals == std::string::npos)
      continue;
    auto key = trim(std::string_view(line).substr(0, equals));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto value = trim(std::string_view(line).substr(equals + 1));
    if (key == "texture")
      current.texture = value;
    else if (key == "htmlgauge00")
      current.gauge = gauge_name(trim(std::string_view(value).substr(0, value.find(','))));
  }
  flush();
  return out;
}
// The key for a texture= name among an aircraft's sections; empty when no
// section names it or two name it differently.
inline mark_identity::Key key_for(const std::vector<Section>& sections, std::string_view texture) {
  mark_identity::Key found{};
  for (const auto& s : sections) {
    if (s.texture != texture)
      continue;
    const auto key = mark_identity::make_key(s.section.c_str(), s.gauge.c_str());
    if (found[0] && found != key)
      return {};
    found = key;
  }
  return found;
}
// The airplane folder of a sim path such as
// "SimObjects\Airplanes\FNX_32X\presets\fnx\FNX_320_CFM_SL\config\aircraft.CFG":
// its first three components.
inline std::filesystem::path airplane_folder(std::string_view sim_path) {
  std::filesystem::path out;
  std::size_t start = 0;
  for (int part = 0; part < 3; ++part) {
    const auto end = sim_path.find_first_of("/\\", start);
    if (end == std::string_view::npos)
      return {};
    out /= std::string(sim_path.substr(start, end - start));
    start = end + 1;
  }
  return out;
}
// Every [VCockpitNN] section of every panel.cfg under the airplane folder
// (its attachments included), in whichever package holds it: Community, then
// the official packages beside it. Empty when the files cannot be read (an
// encrypted package).
inline std::vector<Section> aircraft_sections(const std::filesystem::path& community, std::string_view sim_path) {
  std::vector<Section> out;
  const auto folder = airplane_folder(sim_path);
  if (folder.empty() || community.empty())
    return out;
  std::vector<std::filesystem::path> roots{community};
  const auto packages = community.parent_path();
  for (const wchar_t* official : {L"Official\\OneStore", L"Official\\Steam"})
    roots.push_back(packages / official);
  std::error_code error;
  for (const auto& root : roots) {
    for (std::filesystem::directory_iterator package(root, error), end; !error && package != end; package.increment(error)) {
      const auto airplane = package->path() / folder;
      if (!std::filesystem::is_directory(airplane, error))
        continue;
      for (std::filesystem::recursive_directory_iterator file(airplane, error), last; !error && file != last; file.increment(error)) {
        auto name = file->path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        if (name != L"panel.cfg")
          continue;
        std::ifstream in(file->path(), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const auto sections = parse_panel_cfg(text.str());
        out.insert(out.end(), sections.begin(), sections.end());
      }
      if (!out.empty())
        return out;
    }
    error.clear();
  }
  return out;
}
}  // namespace taxi_camera::standalone::legacy_names
