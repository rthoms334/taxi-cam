#pragma once
// Display identity by panel marks (claudedocs/panel-mark-identification.md).
// Every cockpit panel page paints a block per gauge naming its panel section
// and gauge (panel_marks.hpp); the bridge reads each panel texture once and
// keeps here what it found. A display is named by a key: "VCockpit02/EFIS"
// (section and gauge) or "VCockpit01" (any gauge of that section). Section
// names repeat across MSFS 2024 attachments, so catalog keys name a gauge.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#include "panel_marks.hpp"

namespace taxi_camera::mark_identity {
inline constexpr std::size_t KeySize = 32;
using Key = std::array<char, KeySize>;

// "section/gauge", cut to fit; a section alone without a gauge.
inline Key make_key(const char* section, const char* gauge) noexcept {
  Key key{};
  std::size_t at = 0;
  const auto put = [&](const char* text) {
    for (; text && *text && at + 1 < key.size(); ++text)
      key[at++] = *text;
  };
  put(section);
  if (gauge && *gauge && at + 1 < key.size()) {
    key[at++] = '/';
    put(gauge);
  }
  return key;
}
// A display key as the marks write it; anything else (a panel.cfg texture=
// name saved by an older Taxi Cam) has to be converted first.
inline bool is_key(const char* name) noexcept {
  return name && std::strncmp(name, "VCockpit", 8) == 0 && name[8] >= '0' && name[8] <= '9';
}

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
inline bool matches(const char* key, const Gauge& gauge) noexcept {
  if (!key || !*key)
    return false;
  const char* slash = std::strchr(key, '/');
  const std::size_t section = slash ? static_cast<std::size_t>(slash - key) : std::strlen(key);
  if (std::strlen(gauge.section.data()) != section || std::strncmp(key, gauge.section.data(), section) != 0)
    return false;
  // A key cut short to fit matches the gauge it was made from.
  return !slash || make_key(gauge.section.data(), gauge.name.data()) == make_key(key, nullptr);
}

struct Texture {
  std::uint64_t id{};
  bool copied{};  // Marks inherited from the texture the simulator copied into this one.
  std::vector<Gauge> gauges;
  // The texture's own key: its first gauge.
  Key key() const noexcept {
    if (gauges.empty())
      return {};
    const auto first = std::min_element(gauges.begin(), gauges.end(), [](const Gauge& a, const Gauge& b) { return a.index < b.index; });
    return make_key(first->section.data(), first->name.data());
  }
  bool has(const char* key) const noexcept {
    return std::any_of(gauges.begin(), gauges.end(), [&](const Gauge& gauge) { return matches(key, gauge); });
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
  // The live texture showing the display named by key: of several (a source
  // and its copies), the end of the copy chain, then the newest. Keys name a
  // gauge whose block can be read: a gauge under a later gauge's block never
  // is (legacy_display_names.hpp picks the same one).
  std::uint64_t resolve(const char* key, const std::pair<std::uint64_t, std::uint64_t>* feeds, std::size_t count) const noexcept {
    std::uint64_t best = 0;
    bool best_end = false;
    for (const auto& texture : textures_) {
      if (!texture.has(key))
        continue;
      bool end = true;
      for (std::size_t i = 0; i < count && end; ++i)
        if (feeds[i].first == texture.id) {
          const auto* to = find(feeds[i].second);
          end = !(to && to->has(key));
        }
      if (!best || (end && !best_end) || (end == best_end && texture.id > best)) {
        best = texture.id;
        best_end = end;
      }
    }
    return best;
  }

 private:
  static bool same(const Gauge& a, const Gauge& b) noexcept {
    return a.section == b.section && a.name == b.name && a.index == b.index && a.x == b.x && a.y == b.y && a.width == b.width &&
           a.height == b.height && a.panel_width == b.panel_width && a.panel_height == b.panel_height;
  }
  std::vector<Texture> textures_;
};
}  // namespace taxi_camera::mark_identity
