// Independent CPU image oracle; no production shader or GPU packing reused.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace reference_overlay_oracle {
// Validate font layout and colour independently of the production vector paths.
// Only these bounded glyph cells admit antialiased pixels; all padding, the
// opaque panel, guides and source imagery still use the exact pixel oracle.
inline unsigned number_count(bool valid, unsigned speed) {
  return !valid || speed >= 10 ? 2 : 1;
}
inline int font_cell(unsigned x, unsigned y, bool valid = false, unsigned speed = 0) {
  if (y < 20 || y >= 40)
    return -1;
  for (unsigned index = 0; index < 2 + number_count(valid, speed); ++index) {
    const unsigned left = index < 2 ? 24 + 16 * index : 88 + 16 * (index - 2);
    if (x >= left && x < left + 12)
      return static_cast<int>(index);
  }
  return -1;
}
struct FontCoverage {
  std::array<unsigned, 4> lit{}, core{}, antialiased{};
  bool observe(int cell, const unsigned char* pixel) {
    if (cell < 0 || cell >= static_cast<int>(lit.size()) || pixel[3] != 255)
      return false;
    const bool label = cell < 2;
    // Approved common GS colour is RGB 22/109/19. Compare each channel's
    // coverage against green, allowing one byte of independent UNORM rounding.
    if ((label && (pixel[0] != pixel[1] || pixel[1] != pixel[2])) ||
        (!label && (pixel[1] > 109 || std::abs(int(pixel[0]) - int(std::lround(pixel[1] * 22. / 109))) > 1 ||
                    std::abs(int(pixel[2]) - int(std::lround(pixel[1] * 19. / 109))) > 1)))
      return false;
    const unsigned coverage = label ? pixel[1] : static_cast<unsigned>(std::lround(pixel[1] * 255. / 109));
    lit[cell] += coverage > 12;
    core[cell] += coverage >= 216;
    antialiased[cell] += coverage > 0 && coverage < 240;
    return true;
  }
  bool complete(bool valid = false, unsigned speed = 0) const {
    for (unsigned index = 0; index < 2 + number_count(valid, speed); ++index)
      if (lit[index] < 12 || lit[index] > 180 || core[index] < 4 || antialiased[index] < 4)
        return false;
    return true;
  }
};
inline bool pixel(unsigned x,
                  unsigned y,
                  std::array<unsigned char, 4>& out,
                  bool guides = true,
                  bool valid = false,
                  unsigned speed = 0,
                  bool round_nose = false) {
  const unsigned count = number_count(valid, speed);
  const unsigned panel_width = count == 1 ? 96 : 112;
  if (x >= 16 && x < 16 + panel_width && y >= 12 && y < 48) {
    out = {0, 0, 0, 255};
    return true;
  }
  if (y >= 251 && y < 263) {
    out = {0, 0, 0, 255};
    return true;
  }
  if (!guides || y >= 763)
    return false;
  const bool nose = y < 255;
  const double px = std::min(double(x) + 0.5, 768 - (double(x) + 0.5)), py = double(y) + 0.5 - (nose ? 0 : 259);
  if (nose) {
    const double dx = px - .14 * 768, dy = py - .48 * 255;
    if (round_nose ? dx * dx + dy * dy > 36 : std::abs(dx) >= 7 || std::abs(dy) >= 7)
      return false;
    out = {255, 0, 255, 255};
    return true;
  }
  const std::array<double, 2> corner{.305 * 768, .75 * 504};
  const std::array<double, 2> upper{.33 * 768, .64 * 504};
  const std::array<double, 2> inner{.365 * 768, .758 * 504};
  auto distance = [&](const auto& a, const auto& b) {
    const double dx = b[0] - a[0], dy = b[1] - a[1], t = std::clamp(((px - a[0]) * dx + (py - a[1]) * dy) / (dx * dx + dy * dy), 0., 1.);
    return std::hypot(px - a[0] - t * dx, py - a[1] - t * dy);
  };
  if (std::min(distance(upper, corner), distance(corner, inner)) <= 2) {
    out = {255, 0, 255, 255};
    return true;
  }
  return false;
}
}  // namespace reference_overlay_oracle
