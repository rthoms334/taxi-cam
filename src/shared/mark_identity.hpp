#pragma once
// Display identity by panel marks (claudedocs/panel-mark-identification.md).
// Every cockpit panel page paints a block per gauge naming its panel and
// gauge (panel_marks.hpp); the bridge reads each panel texture once and keeps
// here what it found.
//
// A display is named by a key built from its gauge, the one stable name: the
// panel names a page reports ("VCockpit02") are MSFS's running numbers over
// every attachment's panels in load order, and change between loads (the
// Fenix A320's FAP EFB loaded first once and moved every cockpit panel up by
// one, 2026-11-04). "@PFD_Captain" is the texture carrying that gauge; where
// several textures carry the same gauge (the A350's two EFIS panels),
// "@EFIS#1/2" is the second of the two, in panel number order, which a shift
// of the whole list keeps. Such a key only resolves while exactly that many
// textures carry the gauge.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "panel_marks.hpp"

namespace taxi_camera::mark_identity {
inline constexpr std::size_t KeySize = 32;
using Key = std::array<char, KeySize>;

// "@gauge", or "@gauge#rank/count" for one of count textures carrying the
// gauge; the gauge name is cut to fit.
inline Key make_key(const char* gauge, unsigned rank = 0, unsigned count = 1) noexcept {
  Key key{};
  char suffix[16]{};
  if (count > 1)
    std::snprintf(suffix, sizeof(suffix), "#%u/%u", rank, count);
  const std::size_t room = key.size() - 2 - std::strlen(suffix);
  std::size_t at = 0;
  key[at++] = '@';
  for (std::size_t i = 0; gauge && gauge[i] && i < room; ++i)
    key[at++] = gauge[i];
  for (std::size_t i = 0; suffix[i]; ++i)
    key[at++] = suffix[i];
  return key;
}
// A display key; anything else (a panel.cfg texture= name, or a
// "VCockpitNN/gauge" key from a test build) has to be converted first.
inline bool is_key(const char* name) noexcept {
  return name && name[0] == '@' && name[1];
}
struct ParsedKey {
  std::string gauge;
  unsigned rank = 0, count = 1;
};
inline bool parse_key(const char* key, ParsedKey& out) {
  if (!is_key(key))
    return false;
  out = {};
  std::string text(key + 1);
  if (const auto hash = text.rfind('#'); hash != std::string::npos) {
    unsigned rank = 0, count = 0;
    char extra = 0;
    if (std::sscanf(text.c_str() + hash, "#%u/%u%c", &rank, &count, &extra) == 2 && count > 1 && rank < count) {
      out.rank = rank;
      out.count = count;
      text.resize(hash);
    }
  }
  out.gauge = text;
  return !out.gauge.empty();
}
// The number in a panel name ("VCockpit02" is 2), for ordering.
inline unsigned panel_number(const char* section) noexcept {
  while (section && *section && (*section < '0' || *section > '9'))
    ++section;
  return section ? static_cast<unsigned>(std::strtoul(section, nullptr, 10)) : 0;
}
// Whether Taxi Cam's display package is not loaded: no panel texture has
// carried a mark, MissingMarkPictures pictures came back without one, and
// they were of a cockpit, MissingMarkTextures panel textures listed at the
// same time each pictured without a mark. A cockpit draws many panel pages
// at once (the Fenix A320 in a flight: 20 listed, 14 read, 2026-11-05). The
// MSFS 2024 main menu shows the aircraft without its cockpit pages: it drew
// one display-shaped texture at a time, recreating it, and its 10 pictures
// without marks gave the verdict while the package was loaded (2026-11-05).
// empty_textures is the most textures listed at once with such a picture.
//
// The pages paint their marks only after the bridge asks for them, so the
// first pictures of a load carry none: the iniBuilds A350 gave 8 pictures
// without marks of 3 textures within 1.0 s of the request and its first mark
// 0.45 s later (2026-11-05). The verdict therefore also waits for the scan to
// settle (scan_settled): textures listed, each read or pictured MarkTries
// times a second apart, or 10 s since the marks were last asked for.
// Once given, it holds until a texture is read or the aircraft session
// changes (reset), so a texture created later does not take it back.
inline constexpr unsigned MissingMarkPictures = 8, MissingMarkTextures = 3;
inline bool package_inactive(bool any_read, unsigned empty_pictures, unsigned empty_textures) noexcept {
  return !any_read && empty_pictures >= MissingMarkPictures && empty_textures >= MissingMarkTextures;
}
class PackageVerdict {
 public:
  bool observe(bool any_read, unsigned empty_pictures, unsigned empty_textures, bool scan_settled) noexcept {
    if (any_read)
      inactive_ = false;
    else if (scan_settled && package_inactive(any_read, empty_pictures, empty_textures))
      inactive_ = true;
    return inactive_;
  }
  void reset() noexcept { inactive_ = false; }
  bool inactive() const noexcept { return inactive_; }

 private:
  bool inactive_ = false;
};

struct Gauge {
  std::array<char, 48> section{}, name{};
  std::uint8_t index{};
  // Gauge rectangle and panel size in page pixels.
  std::uint16_t x{}, y{}, width{}, height{}, panel_width{}, panel_height{};
  // The gauge rectangle in a texture of this size (texture_rect).
  bool rect_in(unsigned texture_width, unsigned texture_height, panel_marks::Rect& out) const noexcept {
    panel_marks::Mark m;
    m.x = x, m.y = y, m.width = width, m.height = height, m.panel_width = panel_width, m.panel_height = panel_height;
    return panel_marks::texture_rect(m, texture_width, texture_height, out);
  }
};
struct Texture {
  std::uint64_t id{};
  bool copied{};  // Marks inherited from the texture the simulator copied into this one.
  std::vector<Gauge> gauges;
  // Its identifying gauge: the first one read.
  const Gauge* first() const noexcept {
    return gauges.empty()
               ? nullptr
               : &*std::min_element(gauges.begin(), gauges.end(), [](const Gauge& a, const Gauge& b) { return a.index < b.index; });
  }
  // The gauge of that name; a name cut short to fit a key (prefix) matches
  // the gauge it was cut from.
  const Gauge* gauge(const std::string& name, bool prefix = false) const noexcept {
    for (const auto& g : gauges)
      if (prefix ? std::strncmp(g.name.data(), name.c_str(), name.size()) == 0 : name == g.name.data())
        return &g;
    return nullptr;
  }
};

class Store {
 public:
  void clear() noexcept { textures_.clear(); }
  const std::vector<Texture>& textures() const noexcept { return textures_; }
  const Texture* find(std::uint64_t id) const noexcept {
    for (const auto& texture : textures_)
      if (texture.id == id)
        return &texture;
    return nullptr;
  }
  // A picture's marks: gauge blocks only. Returns true when they are new.
  bool read(std::uint64_t id, const panel_marks::Mark* marks, std::size_t count) {
    Texture texture{id, false, {}};
    for (std::size_t i = 0; i < count; ++i) {
      if (marks[i].kind != panel_marks::Kind::gauge)
        continue;
      Gauge gauge;
      gauge.section = marks[i].name;
      gauge.name = marks[i].gauge_name;
      gauge.index = marks[i].gauge;
      gauge.x = marks[i].x, gauge.y = marks[i].y, gauge.width = marks[i].width, gauge.height = marks[i].height;
      gauge.panel_width = marks[i].panel_width, gauge.panel_height = marks[i].panel_height;
      texture.gauges.push_back(gauge);
    }
    if (texture.gauges.empty())
      return false;
    std::sort(texture.gauges.begin(), texture.gauges.end(), [](const Gauge& a, const Gauge& b) { return a.index < b.index; });
    for (auto& existing : textures_)
      if (existing.id == id) {
        const bool changed = existing.copied || existing.gauges.size() != texture.gauges.size() ||
                             !std::equal(existing.gauges.begin(), existing.gauges.end(), texture.gauges.begin(), same);
        existing = std::move(texture);
        return changed;
      }
    textures_.push_back(std::move(texture));
    return true;
  }
  // Forget textures that are gone.
  template <class Alive>
  void retain(Alive alive) {
    std::erase_if(textures_, [&](const Texture& texture) { return !alive(texture.id); });
  }
  // The simulator copies some displays into another texture and shows that
  // one (FlyByWire A380X, FSLabs): the copy carries the same picture, so it
  // takes the source's marks. feeds are (source, destination) copies.
  // Returns how many textures took marks.
  template <class Alive>
  std::size_t inherit(const std::pair<std::uint64_t, std::uint64_t>* feeds, std::size_t count, Alive alive) {
    std::size_t taken = 0;
    for (bool more = true; more;) {
      more = false;
      for (std::size_t i = 0; i < count; ++i) {
        const auto [from, to] = feeds[i];
        const auto* source = find(from);
        if (!source || find(to) || !alive(to))
          continue;
        Texture copy{to, true, source->gauges};
        textures_.push_back(std::move(copy));
        more = true;
        ++taken;
      }
    }
    return taken;
  }
  // The textures carrying a gauge, one per panel (a recreated panel's newest
  // texture; copies follow their source), in panel number order.
  std::vector<const Texture*> owners(const std::string& gauge, bool prefix = false) const {
    std::vector<const Texture*> out;
    for (const auto& texture : textures_) {
      const auto* g = texture.copied ? nullptr : texture.gauge(gauge, prefix);
      if (!g)
        continue;
      const auto panel = panel_number(g->section.data());
      auto same_panel = std::find_if(out.begin(), out.end(),
                                     [&](const Texture* t) { return panel_number(t->gauge(gauge, prefix)->section.data()) == panel; });
      if (same_panel == out.end())
        out.push_back(&texture);
      else if (texture.id > (*same_panel)->id)
        *same_panel = &texture;
    }
    std::sort(out.begin(), out.end(), [&](const Texture* a, const Texture* b) {
      return panel_number(a->gauge(gauge, prefix)->section.data()) < panel_number(b->gauge(gauge, prefix)->section.data());
    });
    return out;
  }
  // A texture's key: its identifying gauge, ranked among the textures that
  // carry it. A copy has its source's. None when its own panel is not among
  // them (a copy whose source is gone): any key would name another texture.
  Key key_of(const Texture& texture) const {
    const auto* first = texture.first();
    if (!first)
      return {};
    const std::string gauge = first->name.data();
    const auto list = owners(gauge);
    const auto panel = panel_number(first->section.data());
    for (std::size_t i = 0; i < list.size(); ++i)
      if (panel_number(list[i]->gauge(gauge)->section.data()) == panel)
        return make_key(gauge.c_str(), static_cast<unsigned>(i), static_cast<unsigned>(list.size()));
    return {};
  }
  // The live texture showing the display named by key: the gauge's texture
  // (or the ranked one of exactly that many), then along the simulator's
  // copies to the texture it shows.
  std::uint64_t resolve(const char* key, const std::pair<std::uint64_t, std::uint64_t>* feeds, std::size_t count) const {
    ParsedKey parsed;
    if (!parse_key(key, parsed))
      return 0;
    // A key as long as a key can be was cut to fit: its gauge name is a prefix.
    const bool prefix = std::strlen(key) == KeySize - 1;
    const auto list = owners(parsed.gauge, prefix);
    if (list.size() != parsed.count)
      return 0;
    std::uint64_t id = list[parsed.rank]->id;
    // Only into a copy that carries the gauge (one with marks of its own is
    // another display), and the newest such copy.
    for (std::size_t step = 0; step < count; ++step) {
      std::uint64_t next = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const auto* to = feeds[i].first == id ? find(feeds[i].second) : nullptr;
        if (to && to->copied && to->gauge(parsed.gauge, prefix) && to->id > next)
          next = to->id;
      }
      if (!next)
        break;
      id = next;
    }
    return id;
  }

 private:
  static bool same(const Gauge& a, const Gauge& b) noexcept {
    return a.section == b.section && a.name == b.name && a.index == b.index && a.x == b.x && a.y == b.y && a.width == b.width &&
           a.height == b.height && a.panel_width == b.panel_width && a.panel_height == b.panel_height;
  }
  std::vector<Texture> textures_;
};
}  // namespace taxi_camera::mark_identity
