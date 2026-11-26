#pragma once
#include <algorithm>
#include <cstring>
#include <string_view>
#include "mark_identity.hpp"
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
    if (!panel && names[side]) {
      // Previously saved upper-gauge keys (e.g. @PFD_Captain when gauge 0 is WXR)
      // match when exactly one panel in the map carries that gauge.
      mark_identity::ParsedKey parsed;
      if (mark_identity::parse_key(names[side], parsed) && parsed.count == 1) {
        const auto gauge_count = std::min<std::size_t>(status.gauge_count, MaxPanelGauges);
        for (std::size_t g = 0; g < gauge_count; ++g) {
          const auto& gauge = status.gauges[g];
          if (gauge.panel >= count)
            continue;
          const auto* end = static_cast<const char*>(std::memchr(gauge.name, '\0', sizeof(gauge.name)));
          const std::size_t len = end ? static_cast<std::size_t>(end - gauge.name) : sizeof(gauge.name);
          const bool match = len == sizeof(gauge.name) - 1
                                 ? parsed.gauge.size() >= len && std::string_view(parsed.gauge).compare(0, len, gauge.name, len) == 0
                                 : parsed.gauge == std::string_view(gauge.name, len);
          if (!match)
            continue;
          if (panel && panel != &status.panels[gauge.panel]) {
            panel = nullptr;
            break;
          }
          panel = &status.panels[gauge.panel];
        }
      }
    }
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
