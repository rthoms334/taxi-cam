#pragma once
// Display choices saved before panel marks name each display by its
// panel.cfg texture= name (for example $EFIS_LEFT). The bridge now names
// displays by panel-mark key (section and first gauge, mark_identity.hpp),
// so such a name is converted once from the aircraft's own panel.cfg files:
// the [VCockpitNN] section with that texture=, and the gauge whose block
// names the texture.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "../shared/mark_identity.hpp"
#include "../shared/panel_marks.hpp"

namespace taxi_camera::standalone::legacy_names {
struct Section {
  std::string section, texture, gauge;  // gauge: the first visible one, which names the texture.
  std::vector<std::string> visible;     // Every gauge whose block can be read.
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
// A number list such as "2048,2048" or " 30,   33, 958, 971".
inline std::vector<double> numbers(std::string_view text) {
  std::vector<double> out;
  std::string item;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ',') {
      const auto value = trim(item);
      out.push_back(value.empty() ? 0 : std::atof(value.c_str()));
      item.clear();
    } else
      item += text[i];
  }
  return out;
}
// The helper's block for a gauge (taxi-cam-marks.js): its top-right corner a
// cell in, in page pixels. Empty (zero width) when it does not fit: a gauge
// too small for one carries no mark.
struct Block {
  double left{}, top{}, width{}, height{};
};
inline Block block_for(const std::string& section, const std::string& gauge, double x, double y, double width, double height) {
  panel_marks::Mark m;
  m.cell = panel_marks::kCell;
  std::strncpy(m.name.data(), section.c_str(), m.name.size() - 1);
  std::strncpy(m.gauge_name.data(), gauge.c_str(), m.gauge_name.size() - 1);
  std::strncpy(m.info.data(), "c=0000", m.info.size() - 1);
  const auto rows = panel_marks::rows_for(panel_marks::encode_payload(m).size());
  const double block_w = (panel_marks::kColumns + 2) * panel_marks::kCell, block_h = (rows + 2) * panel_marks::kCell;
  if (width < block_w + 2 * panel_marks::kCell || height < block_h + 2 * panel_marks::kCell)
    return {};
  return {x + width - block_w - panel_marks::kCell, y + panel_marks::kCell, block_w, block_h};
}
// The [VCockpitNN] sections of a panel.cfg with their texture= and the gauge
// whose mark names the texture: the first htmlgaugeNN (in NN order, as the
// page lists them) large enough to carry a block.
inline std::vector<Section> parse_panel_cfg(std::string_view text) {
  std::vector<Section> out;
  struct Gauge {
    int index;
    std::string name;
    double x, y, width, height;
  };
  Section current;
  std::vector<Gauge> gauges;
  std::vector<double> logical, display;
  bool in_vcockpit = false;
  const auto flush = [&] {
    if (!in_vcockpit || current.texture.empty())
      return;
    std::sort(gauges.begin(), gauges.end(), [](const Gauge& a, const Gauge& b) { return a.index < b.index; });
    const double rx = logical.size() >= 2 && display.size() >= 2 && logical[0] > 0 ? display[0] / logical[0] : 1;
    const double ry = logical.size() >= 2 && display.size() >= 2 && logical[1] > 0 ? display[1] / logical[1] : 1;
    // Blocks are drawn in gauge order; one under a later block is never read
    // (the Fenix PFD panel's WXR gauges lie under PFD_Captain's).
    std::vector<Block> blocks;
    for (const auto& g : gauges)
      blocks.push_back(block_for(current.section, g.name, std::round(g.x * rx), std::round(g.y * ry), std::round(g.width * rx),
                                 std::round(g.height * ry)));
    for (std::size_t i = 0; i < gauges.size(); ++i) {
      const auto& a = blocks[i];
      bool covered = !a.width;
      for (std::size_t j = i + 1; j < gauges.size() && !covered; ++j) {
        const auto& b = blocks[j];
        covered = b.width && a.left < b.left + b.width && b.left < a.left + a.width && a.top < b.top + b.height && b.top < a.top + a.height;
      }
      if (covered)
        continue;
      if (current.gauge.empty())
        current.gauge = gauges[i].name;
      current.visible.push_back(gauges[i].name);
    }
    out.push_back(current);
  };
  std::size_t start = 0;
  while (start <= text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos)
      end = text.size();
    auto line = trim(text.substr(start, end - start));
    start = end + 1;
    for (const char* comment : {"//", ";"})
      if (const auto at = line.find(comment); at != std::string::npos)
        line = trim(line.substr(0, at));
    if (line.empty())
      continue;
    if (line.front() == '[') {
      flush();
      current = {};
      gauges.clear();
      logical.clear();
      display.clear();
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
    else if (key == "size_mm")
      logical = numbers(value);
    else if (key == "pixel_size")
      display = numbers(value);
    else if (key.rfind("htmlgauge", 0) == 0 && key.size() > 9) {
      const auto comma = value.find(',');
      const auto rect = comma == std::string::npos ? std::vector<double>{} : numbers(std::string_view(value).substr(comma + 1));
      gauges.push_back({std::atoi(key.c_str() + 9), gauge_name(trim(std::string_view(value).substr(0, comma))),
                        rect.size() >= 4 ? rect[0] : 0, rect.size() >= 4 ? rect[1] : 0, rect.size() >= 4 ? rect[2] : 0,
                        rect.size() >= 4 ? rect[3] : 0});
    }
  }
  flush();
  return out;
}
// A section's key (mark_identity.hpp): its first visible gauge, ranked
// among the sections where that gauge is visible, in the order read.
inline mark_identity::Key section_key(const std::vector<Section>& sections, std::size_t index) {
  const auto& s = sections[index];
  if (s.gauge.empty())
    return {};
  std::size_t rank = 0, count = 0;
  for (std::size_t i = 0; i < sections.size(); ++i)
    if (std::find(sections[i].visible.begin(), sections[i].visible.end(), s.gauge) != sections[i].visible.end()) {
      if (i == index)
        rank = count;
      ++count;
    }
  return mark_identity::make_key(s.gauge.c_str(), static_cast<unsigned>(rank), static_cast<unsigned>(std::max<std::size_t>(count, 1)));
}
// The key for a saved name among an aircraft's sections: a panel.cfg
// texture= name, or a "VCockpitNN/gauge" key from a test build. Empty when
// nothing names it, two name it differently, or its panel has no gauge a
// mark can be read from.
inline mark_identity::Key key_for(const std::vector<Section>& sections, std::string_view name) {
  mark_identity::Key found{};
  std::string section, gauge;
  if (const auto slash = name.find('/'); name.rfind("VCockpit", 0) == 0 && slash != std::string_view::npos) {
    section = std::string(name.substr(0, slash));
    gauge = std::string(name.substr(slash + 1));
  }
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const auto& s = sections[i];
    const bool named =
        gauge.empty() ? s.texture == name : s.section == section && std::find(s.visible.begin(), s.visible.end(), gauge) != s.visible.end();
    if (!named)
      continue;
    const auto key = section_key(sections, i);
    if (!key[0] || (found[0] && found != key))
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
  // The loaded preset's own folder: packages can share an airplane folder
  // (the Fenix A320 and its A319/A321 package both have FNX_32X).
  std::filesystem::path preset;
  {
    std::string text(sim_path);
    std::replace(text.begin(), text.end(), '/', '\\');
    preset = std::filesystem::path(text).parent_path();
    if (preset.filename() == L"config" || preset.filename() == L"Config" || preset.filename() == L"CONFIG")
      preset = preset.parent_path();
  }
  std::vector<std::filesystem::path> roots{community};
  const auto packages = community.parent_path();
  for (const wchar_t* official : {L"Official\\OneStore", L"Official\\Steam"})
    roots.push_back(packages / official);
  std::vector<std::filesystem::path> exact, shared;
  std::error_code error;
  for (const auto& root : roots) {
    for (std::filesystem::directory_iterator package(root, error), end; !error && package != end; package.increment(error)) {
      const auto airplane = package->path() / folder;
      if (!std::filesystem::is_directory(airplane, error))
        continue;
      (std::filesystem::is_directory(package->path() / preset, error) ? exact : shared).push_back(airplane);
    }
    error.clear();
  }
  for (const auto* list : {&exact, &shared}) {
    for (const auto& airplane : *list) {
      for (std::filesystem::recursive_directory_iterator file(airplane, error), last; !error && file != last; file.increment(error)) {
        auto name = file->path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        if (name != L"panel.cfg")
          continue;
        std::ifstream in(file->path(), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        // Variant panel files repeat sections (FSLabs: panel, Panel.FSL and
        // Panel.FSLACF): the same section once.
        for (const auto& section : parse_panel_cfg(text.str()))
          if (std::none_of(out.begin(), out.end(), [&](const Section& o) {
                return o.section == section.section && o.texture == section.texture && o.visible == section.visible;
              }))
            out.push_back(section);
      }
      error.clear();
    }
    if (!out.empty())
      return out;
  }
  return out;
}
}  // namespace taxi_camera::standalone::legacy_names
