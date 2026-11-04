#pragma once
#include <cstring>
#include "protocol.hpp"

namespace taxi_camera::standalone {
// What the guided setup saves once the user has picked each side's display by
// picture: the layout of the variant in Status, read from its panel map. The
// map lists every texture whose panel marks the bridge read, by key
// (mark_identity.hpp); each side keeps its own panel's shape.
enum class LayoutLearning { learned, no_variant, not_paired, unknown_panel, invalid };
// names[side] is the panel key chosen for each of the first sides.
inline LayoutLearning learn_layout(const Status& status,
                                   const std::array<const char*, MaxDisplaySides>& names,
                                   unsigned sides,
                                   profiles::DiscoveredLayout& out) noexcept {
  profiles::VariantKey key{};
  if (!profiles::variant_key(std::string_view(status.aircraft_path, strnlen(status.aircraft_path, sizeof(status.aircraft_path))), key))
    return LayoutLearning::no_variant;
  const auto count = std::min<std::size_t>(status.panel_count, MaxPanelTextures);
  if (status.panel_state != static_cast<std::uint32_t>(PanelMapState::marked) || !count)
    return LayoutLearning::not_paired;
  if (sides < 2 || sides > MaxDisplaySides)
    return LayoutLearning::invalid;
  profiles::DiscoveredLayout l{};
  std::memcpy(l.key, key.data(), sizeof(l.key));
  l.sides = sides;
  for (unsigned side = 0; side < sides; ++side) {
    const PanelTexture* panel = nullptr;
    for (std::size_t i = 0; i < count && !panel; ++i)
      if (names[side] && std::strncmp(status.panels[i].name, names[side], sizeof(status.panels[i].name)) == 0)
        panel = &status.panels[i];
    if (!panel || !names[side][0] || std::strlen(names[side]) >= profiles::MaxPanelName)
      return LayoutLearning::unknown_panel;
    std::strncpy(l.textures[side], names[side], profiles::MaxPanelName - 1);
    l.shapes[side] = {panel->width, panel->height, panel->mips};
  }
  if (!profiles::valid_layout(l))
    return LayoutLearning::invalid;
  out = l;
  return LayoutLearning::learned;
}
}  // namespace taxi_camera::standalone
