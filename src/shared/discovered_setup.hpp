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
namespace detail {
inline const PanelTexture* find_gauge_panel(const Status& status, std::size_t count, const char* key) noexcept {
  mark_identity::ParsedKey parsed;
  if (!mark_identity::parse_key(key, parsed))
    return nullptr;
  // Match MarkIdentity Store::resolve: only full KeySize-1 keys were cut to fit
  // and match by prefix; PanelGauge::name is capped at 23 chars, so shorter keys
  // require an exact untruncated gauge name.
  const bool prefix = std::strlen(key) == mark_identity::KeySize - 1;
  const auto gauge_count = std::min<std::size_t>(status.gauge_count, MaxPanelGauges);
  const PanelTexture* carriers[MaxPanelTextures]{};
  std::size_t carrier_count = 0;
  for (std::size_t g = 0; g < gauge_count; ++g) {
    const auto& gauge = status.gauges[g];
    if (gauge.panel >= count)
      continue;
    const auto* end = static_cast<const char*>(std::memchr(gauge.name, '\0', sizeof(gauge.name)));
    const std::size_t len = end ? static_cast<std::size_t>(end - gauge.name) : sizeof(gauge.name);
    const bool match = prefix ? len == sizeof(gauge.name) - 1 && parsed.gauge.size() >= len &&
                                    std::string_view(parsed.gauge).compare(0, len, gauge.name, len) == 0
                              : len < sizeof(gauge.name) - 1 && parsed.gauge == std::string_view(gauge.name, len);
    if (!match)
      continue;
    const PanelTexture* carrier = &status.panels[gauge.panel];
    bool seen = false;
    for (std::size_t c = 0; c < carrier_count && !seen; ++c)
      seen = carriers[c] == carrier || (carrier->name[0] && std::strncmp(carriers[c]->name, carrier->name, sizeof(carrier->name)) == 0);
    if (!seen && carrier_count < MaxPanelTextures)
      carriers[carrier_count++] = carrier;
  }
  if (carrier_count != parsed.count)
    return nullptr;
  if (parsed.count == 1)
    return carriers[0];
  // Ranked keys (e.g. @A350WXR_EFIS#0/2) follow panel order when every carrier's
  // published key shares the same first-gauge family; otherwise accept only if
  // every carrier has the same display shape.
  bool ranked_family = true;
  std::string first_gauge;
  const PanelTexture* ranked = nullptr;
  for (std::size_t c = 0; c < carrier_count && ranked_family; ++c) {
    mark_identity::ParsedKey pk;
    if (!mark_identity::parse_key(carriers[c]->name, pk) || pk.count != parsed.count) {
      ranked_family = false;
      break;
    }
    if (c == 0)
      first_gauge = pk.gauge;
    else if (pk.gauge != first_gauge)
      ranked_family = false;
    if (pk.rank == parsed.rank)
      ranked = carriers[c];
  }
  if (ranked_family)
    return ranked;
  for (std::size_t c = 1; c < carrier_count; ++c)
    if (carriers[c]->width != carriers[0]->width || carriers[c]->height != carriers[0]->height || carriers[c]->mips != carriers[0]->mips)
      return nullptr;
  return carriers[0];
}
}  // namespace detail

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
    if (!names[side] || !names[side][0] || std::strlen(names[side]) >= profiles::MaxPanelName)
      return LayoutLearning::unknown_panel;
    const PanelTexture* panel = nullptr;
    for (std::size_t i = 0; i < count && !panel; ++i)
      if (std::strncmp(status.panels[i].name, names[side], sizeof(status.panels[i].name)) == 0)
        panel = &status.panels[i];
    if (!panel)
      panel = detail::find_gauge_panel(status, count, names[side]);
    if (!panel)
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
