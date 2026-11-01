#pragma once
#include <cstring>
#include "protocol.hpp"

namespace taxi_camera::standalone {
// What the guided setup saves once the user has picked each side's display by
// picture: the layout of the variant in Status, read from its panel map.
enum class LayoutLearning { learned, no_variant, not_clean, not_paired, unknown_panel, invalid };
// Panels within this many creations of a display become its anchors. Replayed
// loads (2026-10-31) paired every real and simulated aircraft switch right
// with 2; 1 and 3 each failed one case.
inline constexpr std::size_t AnchorReach = 2;
// names[side] is the panel texture= name chosen for each of the first sides.
// Only a clean, complete map is learned from: after a switch to another
// aircraft, leftover textures can move names one texture off, and saved names
// would then route the wrong displays on every later load. A cockpit test
// (require_clean false) is never saved, so any complete map will do.
inline LayoutLearning learn_layout(const Status& status,
                                   const std::array<const char*, MaxDisplaySides>& names,
                                   unsigned sides,
                                   profiles::DiscoveredLayout& out,
                                   bool require_clean = true) noexcept {
  profiles::VariantKey key{};
  if (!profiles::variant_key(std::string_view(status.aircraft_path, strnlen(status.aircraft_path, sizeof(status.aircraft_path))), key))
    return LayoutLearning::no_variant;
  if (require_clean && !status.panel_clean)
    return LayoutLearning::not_clean;
  const auto count = std::min<std::size_t>(status.panel_count, MaxPanelTextures);
  if (status.panel_state != static_cast<std::uint32_t>(PanelMapState::paired) || !count)
    return LayoutLearning::not_paired;
  if (sides < 2 || sides > MaxDisplaySides)
    return LayoutLearning::invalid;
  const auto same = [](const PanelTexture& panel, const char* name) {
    return name && std::strncmp(panel.name, name, sizeof(panel.name)) == 0;
  };
  // The map lists panels highest index first, the order their textures were
  // created. A repeated name is its lowest index, the texture created last.
  std::array<std::size_t, MaxDisplaySides> at{};
  profiles::DiscoveredLayout l{};
  std::memcpy(l.key, key.data(), sizeof(l.key));
  l.sides = sides;
  for (unsigned side = 0; side < sides; ++side) {
    bool found = false;
    for (std::size_t i = 0; i < count; ++i)
      if (same(status.panels[i], names[side]))
        at[side] = i, found = true;
    if (!found || !names[side][0] || std::strlen(names[side]) >= profiles::MaxPanelName)
      return LayoutLearning::unknown_panel;
    // Any panel can be a display; each side keeps its own panel's shape.
    const auto& panel = status.panels[at[side]];
    std::strncpy(l.textures[side], names[side], profiles::MaxPanelName - 1);
    l.shapes[side] = {panel.width, panel.height, panel.mips};
  }
  std::size_t anchors = 0;
  for (unsigned side = 0; side < sides; ++side)
    for (std::size_t i = at[side] > AnchorReach ? at[side] - AnchorReach : 0; i <= at[side] + AnchorReach && i < count; ++i) {
      const auto& panel = status.panels[i];
      bool skip = !panel.name[0] || anchors == profiles::MaxPanelAnchors;
      for (unsigned s = 0; !skip && s < sides; ++s)
        skip = same(panel, names[s]);
      // A repeated name has no single shape to check.
      for (std::size_t j = 0; !skip && j < count; ++j)
        skip = j != i && same(status.panels[j], panel.name);
      for (std::size_t a = 0; !skip && a < anchors; ++a)
        skip = std::strncmp(l.anchors[a].name, panel.name, profiles::MaxPanelName) == 0;
      if (skip)
        continue;
      std::strncpy(l.anchors[anchors].name, panel.name, profiles::MaxPanelName - 1);
      l.anchors[anchors].width = panel.width;
      l.anchors[anchors].height = panel.height;
      ++anchors;
    }
  if (!profiles::valid_layout(l))
    return LayoutLearning::invalid;
  out = l;
  return LayoutLearning::learned;
}
}  // namespace taxi_camera::standalone
