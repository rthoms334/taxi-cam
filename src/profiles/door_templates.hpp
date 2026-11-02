#pragma once
#include <array>
#include <cstring>
#include <string_view>
#include "discovered.hpp"

namespace taxi_camera::profiles {
// Shipped starting points for the door camera of set-up aircraft, by airplane
// folder (variant_family). They are examples, not aircraft code: the panel
// the door camera fills, added as the aircraft's lower display, its display
// area on that panel, a starting mount, and a momentary cockpit L:var that
// turns it on. A variant takes them once, while its settings have no door
// camera yet; the user's own choices are kept from then on.
struct DoorTemplate {
  std::string_view family;
  const char* panel;  // panel.cfg texture= name
  DiscoveredLayout::Shape shape;
  DisplayRect area;  // On the panel; empty for all of it.
  std::array<double, 6> mount;
  const char* lvar;
};
// Fenix A319/A320/A321: the pedestal VIDEO button (Cockpit_Behavior.xml,
// S_PED_COCKPIT_DOOR_VIDEO, momentary) shows the cabin on the lower ECAM
// ($A320_ECAM_Lower, 768 x 768, five mips in the 2026-11-01 setup). The area
// leaves the ECAM's bottom status lines (TAT, SAT, time, GW) showing below
// the picture, as set on the user's A320 on 2026-11-02. The mount starts on
// the centreline about where the aircraft's cameras.cfg puts its cabin,
// looking aft; adjust it with the live view.
inline constexpr std::array<DoorTemplate, 1> DoorTemplates{{{"SimObjects/Airplanes/FNX_32X",
                                                             "$A320_ECAM_Lower",
                                                             {768, 768, 5},
                                                             {0, 0, 768, 666},
                                                             {0, 2, 9, -10, 180, 1.2},
                                                             "S_PED_COCKPIT_DOOR_VIDEO"}}};
inline const DoorTemplate* door_template(std::string_view variant) noexcept {
  const auto family = variant_family(variant);
  const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c == '\\' ? '/' : c; };
  for (const auto& t : DoorTemplates) {
    if (family.size() != t.family.size())
      continue;
    bool same = true;
    for (std::size_t i = 0; same && i < family.size(); ++i)
      same = fold(family[i]) == fold(t.family[i]);
    if (same)
      return &t;
  }
  return nullptr;
}
// Gives a layout the template's panel as its lower display (side 2); a layout
// that already has a lower display keeps it. The panel stops being an anchor.
// False, leaving the layout unchanged, when the result is not a valid layout.
inline bool add_door_display(DiscoveredLayout& layout, const DoorTemplate& t) noexcept {
  if (layout.sides >= MaxDisplaySides)
    return true;
  if (layout.sides != MaxDisplaySides - 1 || std::strlen(t.panel) >= MaxPanelName)
    return false;
  auto next = layout;
  next.sides = MaxDisplaySides;
  std::strcpy(next.textures[2], t.panel);
  next.shapes[2] = t.shape;
  std::size_t kept = 0;
  for (const auto& anchor : layout.anchors)
    if (anchor.name[0] && std::strcmp(anchor.name, t.panel) != 0)
      next.anchors[kept++] = anchor;
  for (; kept < MaxPanelAnchors; ++kept)
    next.anchors[kept] = {};
  if (!valid_layout(next))
    return false;
  layout = next;
  return true;
}
}  // namespace taxi_camera::profiles
