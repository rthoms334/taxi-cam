#pragma once
#include <algorithm>
#include <array>
#include "../graphics/display_keep.hpp"
#include "../profiles/catalog.hpp"

namespace taxi_camera::standalone::display_keep {
// The panel marks for an aircraft (display_keep.hpp): current is the profile
// in use, base its built-in profile (the same one unless the user changed a
// display). Gauge marks are the built-in profile's display corners, which
// stay put when the camera area moves; area marks are the current areas'
// corners. A set-up aircraft's gauge is unknown, so its area corners stand in
// for it and count as gauge marks, with the texture's own corner. Gauge marks
// come first, so a corner listed twice stays a gauge mark.
inline unsigned profile_marks(const profiles::AircraftProfile* current,
                              const profiles::AircraftProfile* base,
                              std::array<Point, MaxMarks>& out) noexcept {
  unsigned count = 0;
  const auto add = [&](unsigned x, unsigned y, bool gauge) noexcept {
    for (unsigned i = 0; i < count; ++i)
      if (out[i].x == x && out[i].y == y)
        return;
    if (count < out.size())
      out[count++] = {x, y, gauge};
  };
  if (!current)
    return 0;
  const unsigned sides = std::min<unsigned>(current->sides, MaxDisplaySides);
  for (const bool gauge : {true, false})
    for (unsigned side = 0; side < sides; ++side) {
      if (current->fit_texture) {
        if (gauge) {
          add(0, 0, true);
          add(current->areas[side].left, current->areas[side].top, true);
        }
        continue;
      }
      if (const auto* p = gauge ? base : current; p && !p->fit_texture) {
        const auto region = profiles::display_rect(*p, side), content = profiles::display_content_rect(*p, side);
        add(region.left, region.top, gauge);
        add(content.left, content.top, gauge);
      }
    }
  return count;
}
}  // namespace taxi_camera::standalone::display_keep
