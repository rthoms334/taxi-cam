#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>
#include "catalog.hpp"

namespace taxi_camera::profiles {
// The display layout of an aircraft the user set up: its variant key
// (aircraft_variant.hpp), the panel.cfg texture= name and shape of the panel
// each display side was set up on, and the shapes of panels created next to
// those, which keep name pairing on the right textures after an aircraft
// switch. Each camera fills whichever panel texture its side is routed to,
// at that texture's size. Travels in the companion's Settings and is saved
// per variant.
inline constexpr std::size_t MaxPanelName = 32, MaxPanelAnchors = 8;
struct DiscoveredLayout {
  char key[MaxVariantKey]{};
  std::uint32_t sides{};
  char textures[MaxDisplaySides][MaxPanelName]{};
  struct Shape {
    std::uint32_t width{}, height{}, mips{};
  };
  Shape shapes[MaxDisplaySides]{};
  DisplayInsets padding{};
  struct Anchor {
    char name[MaxPanelName]{};
    std::uint32_t width{}, height{};
  };
  Anchor anchors[MaxPanelAnchors]{};
};
// Compared bytewise: no padding, so equal layouts have equal bytes.
static_assert(std::has_unique_object_representations_v<DiscoveredLayout>);
static_assert(std::is_trivially_copyable_v<DiscoveredLayout>);
inline bool same_layout(const DiscoveredLayout& a, const DiscoveredLayout& b) noexcept {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}
// Panel textures are created R8G8B8A8_UNORM (format 28), the only format
// aircraft discovery pairs with panel names.
inline constexpr std::uint32_t DiscoveredFormat = 28;
// Starting camera mounts until the user positions the cameras: the A350-900's.
inline constexpr std::array<std::array<double, 6>, 3> DiscoveredMounts = A359.mounts;
namespace discovered_detail {
template <std::size_t N>
inline bool terminated_name(const char (&text)[N], bool allow_empty) noexcept {
  const auto* end = static_cast<const char*>(std::memchr(text, 0, N));
  if (!end || (end == text && !allow_empty))
    return false;
  for (const char* c = text; c < end; ++c)
    if (*c < 32 || *c > 126)
      return false;
  for (const char* c = end; c < text + N; ++c)
    if (*c)
      return false;  // Unused bytes stay zero so equal layouts compare equal.
  return true;
}
}  // namespace discovered_detail
inline bool valid_layout(const DiscoveredLayout& l) noexcept {
  using discovered_detail::terminated_name;
  VariantKey own{};
  if (!terminated_name(l.key, false) || !variant_key(l.key, own) || std::strcmp(own.data(), l.key) != 0)
    return false;
  if (l.sides < 2 || l.sides > MaxDisplaySides)
    return false;
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const auto& shape = l.shapes[side];
    if (side >= l.sides) {
      if (l.textures[side][0] || shape.width || shape.height || shape.mips || !terminated_name(l.textures[side], true))
        return false;
      continue;
    }
    // The camera inset must fit the panel the side was set up on.
    const auto content = inset_rect({0, 0, shape.width, shape.height}, l.padding);
    if (!terminated_name(l.textures[side], false) || !panel_texture_shape(shape.width, shape.height, shape.mips, DiscoveredFormat) ||
        content.right <= content.left || content.bottom <= content.top)
      return false;
  }
  bool ended = false;
  for (const auto& anchor : l.anchors) {
    if (!terminated_name(anchor.name, true))
      return false;
    if (!anchor.name[0]) {
      ended = true;
      if (anchor.width || anchor.height)
        return false;
      continue;
    }
    if (ended || !anchor.width || anchor.width > MaxPanelEdge || !anchor.height || anchor.height > MaxPanelEdge)
      return false;
    for (unsigned side = 0; side < l.sides; ++side)
      if (std::strcmp(anchor.name, l.textures[side]) == 0)
        return false;
  }
  return true;
}
// One published profile and the storage its pointers refer to.
struct DiscoveredProfile {
  DiscoveredLayout layout;
  wchar_t name[MaxVariantComponent + 1]{};
  AircraftProfile profile{};
};
// Shortcuts and previews only, the generic Airbus guide layout and starting
// mounts; names and panel shapes come from the layout, and each side fits the
// texture routed to it (AircraftProfile::fit_texture).
inline void build_discovered(DiscoveredProfile& out) noexcept {
  const auto& l = out.layout;
  auto& p = out.profile;
  p.id = DiscoveredProfileId;
  p.key = l.key;
  const auto preset = variant_name(l.key);
  std::size_t n = 0;
  for (; n < preset.size() && n < MaxVariantComponent; ++n)
    out.name[n] = static_cast<wchar_t>(preset[n]);
  out.name[n] = 0;
  p.name = out.name;
  p.taxi_lvars = p.taxi_events = p.pfd_labels = {"", "", ""};
  p.mounts = DiscoveredMounts;
  // Side 0's panel stands for the display shape where one is still read.
  p.width = l.shapes[0].width;
  p.height = l.shapes[0].height;
  p.mips = l.shapes[0].mips;
  p.taxi_control = TaxiControl::manual_only;
  p.aircraft_types = {"", "", ""};
  p.package_markers = {"", "", ""};
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const auto& shape = l.shapes[side];
    p.display_regions[side] = {0, 0, shape.width, shape.height};
    p.side_shapes[side] = {shape.width, shape.height, shape.mips};
    p.panel_textures[side] = l.textures[side];
  }
  p.fit_texture = true;
  p.composition = A350Etacs;
  p.formats = {28, 29};
  p.camera_padding = l.padding;
  p.pfd_detection = PfdDetectionPolicy::named;
  p.sides = l.sides;
  for (std::size_t i = 0; i < MaxPanelAnchors; ++i)
    p.panel_shapes[i] = l.anchors[i].name[0] ? AircraftProfile::PanelShape{l.anchors[i].name, l.anchors[i].width, l.anchors[i].height}
                                             : AircraftProfile::PanelShape{};
  // The cameras take side 0's panel shape, from the A380 panes made for its
  // camera area; every other side shows that picture at its shape.
  shape_cameras(p, A380.camera_panes, display_content_rect(A380, 0), 1, {1, 1, 1});
}
static_assert(MaxPanelAnchors == std::tuple_size_v<decltype(AircraftProfile::panel_shapes)>);
namespace discovered_detail {
inline std::mutex publish_mutex;
inline const DiscoveredProfile* published{};
inline std::atomic<std::uint64_t> generation{};
}  // namespace discovered_detail
// True when this layout is the published one.
inline bool discovered_current(const DiscoveredLayout& layout) noexcept {
  const std::lock_guard lock(discovered_detail::publish_mutex);
  return discovered_detail::published && same_layout(discovered_detail::published->layout, layout);
}
// Makes find(DiscoveredProfileId) return this layout's profile. An unchanged
// layout keeps its profile; a new one gets a new profile and generation.
// nullptr for an invalid layout or when memory runs out.
inline const AircraftProfile* publish_discovered(const DiscoveredLayout& layout) noexcept {
  if (!valid_layout(layout))
    return nullptr;
  const std::lock_guard lock(discovered_detail::publish_mutex);
  if (const auto* current = discovered_detail::published; current && same_layout(current->layout, layout))
    return &current->profile;
  auto* next = new (std::nothrow) DiscoveredProfile{};  // Never freed: older readers keep their profile.
  if (!next)
    return nullptr;
  next->layout = layout;
  build_discovered(*next);
  discovered_detail::published = next;
  discovered_detail::generation.fetch_add(1, std::memory_order_relaxed);
  discovered_profile.store(&next->profile, std::memory_order_release);
  return &next->profile;
}
inline std::uint64_t discovered_generation() noexcept {
  return discovered_detail::generation.load(std::memory_order_relaxed);
}
// The user's display choices for a profile, saved with it: per side, another
// panel to route by name (empty keeps the profile's) and a display area in
// that texture's pixels (empty keeps the profile's region; on a set-up
// aircraft, the whole panel). Travels in Settings for the selected profile.
struct DisplayOverride {
  char textures[MaxDisplaySides][MaxPanelName]{};
  DisplayRect areas[MaxDisplaySides]{};
};
static_assert(std::has_unique_object_representations_v<DisplayOverride>);
inline bool empty_area(const DisplayRect& r) noexcept {
  return !r.left && !r.top && !r.right && !r.bottom;
}
inline bool valid_override(unsigned sides, bool fit_texture, const DisplayInsets& padding, const DisplayOverride& o) noexcept {
  using discovered_detail::terminated_name;
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const auto& a = o.areas[side];
    if (!terminated_name(o.textures[side], true) || (side >= sides && (o.textures[side][0] || !empty_area(a))))
      return false;
    if (empty_area(a))
      continue;
    // A real rectangle on a texture, leaving room for the camera inset. A
    // catalog profile keeps a whole display: no area narrower than 64 pixels.
    if (a.right <= a.left || a.bottom <= a.top || a.right > MaxPanelEdge || a.bottom > MaxPanelEdge ||
        (!fit_texture && (a.right - a.left < 64 || a.bottom - a.top < 64)))
      return false;
    const auto content = inset_rect(a, padding);
    if (content.right <= content.left || content.bottom <= content.top)
      return false;
  }
  return true;
}
struct OverrideProfile {
  DisplayOverride choices;
  const AircraftProfile* base{};
  AircraftProfile profile{};
};
namespace discovered_detail {
inline const OverrideProfile* override_published{};
}
// p (a copy of base) with valid display choices: other panels by name (kept
// in `choices`, which must outlive p), display areas, and the cameras shaped
// for side 0's camera area (shape_cameras) from the ones base was made for.
inline void apply_choices(AircraftProfile& p, const AircraftProfile& base, const DisplayOverride& choices) noexcept {
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    if (choices.textures[side][0])
      p.panel_textures[side] = choices.textures[side];
    if (empty_area(choices.areas[side]))
      continue;
    if (base.fit_texture)
      p.areas[side] = choices.areas[side];
    else
      p.display_regions[side] = choices.areas[side];
  }
  // base's own camera area per side: its display region, or a set-up
  // aircraft's whole panel (whose sides all show side 0's shape).
  const auto own = [&](unsigned side) {
    return base.fit_texture ? inset_rect({0, 0, base.side_shapes[side][0], base.side_shapes[side][1]}, base.camera_padding)
                            : display_content_rect(base, side);
  };
  const auto aspect = [](DisplayRect r) {
    return r.right > r.left && r.bottom > r.top ? double(r.right - r.left) / double(r.bottom - r.top) : 1.0;
  };
  std::array<float, MaxDisplaySides> ratios{1, 1, 1};
  if (!base.fit_texture)
    for (unsigned side = 1; side < base.sides && side < MaxDisplaySides; ++side)
      ratios[side] = static_cast<float>(aspect(own(side)) / aspect(own(0)));
  shape_cameras(p, base.camera_panes, own(0), base.composition.ui_aspect, ratios);
}
inline bool override_current(const AircraftProfile* base, const DisplayOverride& choices) noexcept {
  const std::lock_guard lock(discovered_detail::publish_mutex);
  const auto* current = discovered_detail::override_published;
  return current && base && current->base == base && std::memcmp(&current->choices, &choices, sizeof(choices)) == 0;
}
// Makes find(base->id) return base with these display choices. The same base
// and choices keep their profile; nullptr when they are invalid.
inline const AircraftProfile* publish_override(const AircraftProfile* base, const DisplayOverride& choices) noexcept {
  if (!base || !valid_override(base->sides, base->fit_texture, base->camera_padding, choices))
    return nullptr;
  const std::lock_guard lock(discovered_detail::publish_mutex);
  if (const auto* current = discovered_detail::override_published;
      current && current->base == base && std::memcmp(&current->choices, &choices, sizeof(choices)) == 0)
    return &current->profile;
  auto* next = new (std::nothrow) OverrideProfile{};  // Never freed: older readers keep their profile.
  if (!next)
    return nullptr;
  next->choices = choices;
  next->base = base;
  next->profile = *base;
  apply_choices(next->profile, *base, next->choices);
  discovered_detail::override_published = next;
  override_profile.store(&next->profile, std::memory_order_release);
  return &next->profile;
}
}  // namespace taxi_camera::profiles
