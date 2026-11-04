#pragma once
#include <algorithm>
#include <array>
#include "../graphics/display_keep.hpp"
#include "../profiles/catalog.hpp"

namespace taxi_camera::standalone::display_keep {
// The panel marks for an aircraft (display_keep.hpp): current is the profile
// in use, base its built-in profile (the same one unless the user changed a
// display). The built-in profile's display corners stay put when the camera
// area moves; the current areas' corners follow it. A set-up aircraft's gauge
// is unknown, so its area corners stand in for it, with the texture's own
// corner.
inline unsigned profile_marks(const profiles::AircraftProfile* current,
                              const profiles::AircraftProfile* base,
                              std::array<Point, MaxMarks>& out) noexcept {
  unsigned count = 0;
  const auto add = [&](unsigned x, unsigned y) noexcept {
    for (unsigned i = 0; i < count; ++i)
      if (out[i].x == x && out[i].y == y)
        return;
    if (count < out.size())
      out[count++] = {x, y};
  };
  if (!current)
    return 0;
  const unsigned sides = std::min<unsigned>(current->sides, MaxDisplaySides);
  for (unsigned side = 0; side < sides; ++side) {
    if (current->fit_texture) {
      add(0, 0);
      add(current->areas[side].left, current->areas[side].top);
      continue;
    }
    for (const auto* p : {base, current})
      if (p && !p->fit_texture) {
        const auto region = profiles::display_rect(*p, side), content = profiles::display_content_rect(*p, side);
        add(region.left, region.top);
        add(content.left, content.top);
      }
  }
  return count;
}
}  // namespace taxi_camera::standalone::display_keep
