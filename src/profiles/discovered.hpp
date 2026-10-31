#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>
#include "catalog.hpp"

namespace taxi_camera::profiles {
// The display layout of an aircraft the user set up: its variant key
// (aircraft_variant.hpp), the panel.cfg texture= name and region of each
// display side, and the shapes of panels created next to those displays,
// which keep name pairing on the right textures after an aircraft switch.
// Travels in the companion's Settings and is saved per variant.
inline constexpr std::size_t MaxPanelName = 32, MaxPanelAnchors = 8;
struct DiscoveredLayout {
  char key[MaxVariantKey]{};
  std::uint32_t sides{}, width{}, height{}, mips{}, format{};
  char textures[MaxDisplaySides][MaxPanelName]{};
  DisplayRect regions[MaxDisplaySides]{};
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
// Panel textures are multi-mip R8G8B8A8_UNORM render targets (format 28), the
// only shape aircraft discovery tracks.
inline constexpr std::uint32_t DiscoveredFormat = 28, MaxDiscoveredEdge = 16384;
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
  if (l.sides < 2 || l.sides > MaxDisplaySides || !l.width || l.width > MaxDiscoveredEdge || !l.height || l.height > MaxDiscoveredEdge ||
      l.mips < 2 || l.mips > 12 || l.format != DiscoveredFormat)
    return false;
  AircraftProfile shape{};
  shape.sides = l.sides;
  shape.camera_padding = l.padding;
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const auto& r = l.regions[side];
    if (side >= l.sides) {
      if (l.textures[side][0] || r.left || r.top || r.right || r.bottom || !terminated_name(l.textures[side], true))
        return false;
      continue;
    }
    shape.display_regions[side] = r;
    // Every side is one patch size (scene_frame_output.cpp checks side 0's).
    const auto& first = l.regions[0];
    if (!terminated_name(l.textures[side], false) || r.right > l.width || r.bottom > l.height ||
        r.right - r.left != first.right - first.left || r.bottom - r.top != first.bottom - first.top)
      return false;
    const auto content = display_content_rect(shape, side);
    if (content.right <= content.left || content.bottom <= content.top)
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
    if (ended || !anchor.width || anchor.width > MaxDiscoveredEdge || !anchor.height || anchor.height > MaxDiscoveredEdge)
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
// mounts; the display shape, names and regions come from the layout.
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
  p.width = l.width;
  p.height = l.height;
  p.mips = l.mips;
  p.taxi_control = TaxiControl::manual_only;
  p.aircraft_types = {"", "", ""};
  p.package_markers = {"", "", ""};
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    p.display_regions[side] = l.regions[side];
    p.panel_textures[side] = l.textures[side];
  }
  p.composition = A350Etacs;
  p.formats = {l.format};
  p.camera_padding = l.padding;
  p.pfd_detection = PfdDetectionPolicy::named;
  p.sides = l.sides;
  for (std::size_t i = 0; i < MaxPanelAnchors; ++i)
    p.panel_shapes[i] = l.anchors[i].name[0] ? AircraftProfile::PanelShape{l.anchors[i].name, l.anchors[i].width, l.anchors[i].height}
                                             : AircraftProfile::PanelShape{};
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
}  // namespace taxi_camera::profiles
