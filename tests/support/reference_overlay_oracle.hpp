// Independent CPU image oracle; no production shader or GPU packing reused.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace reference_overlay_oracle {
inline bool glyph(unsigned x, unsigned y, unsigned left, const char* rows) {
  if (x < left || x >= left + 12 || y < 12 || y >= 32)
    return false;
  return rows[((y - 12) / 4) * 3 + (x - left) / 4] == '1';
}
inline bool pixel(unsigned x, unsigned y, std::array<unsigned char, 4>& out, bool guides = true, bool valid = false, unsigned speed = 0) {
  if (x < 140 && y < 48) {
    out = {0, 0, 0, 255};
    if (glyph(x, y, 8, "111100101101111") || glyph(x, y, 24, "111100111001111")) {
      out = {255, 255, 255, 255};
      return true;
    }
    bool number = false;
    if (!valid)
      number = glyph(x, y, 52, "000000111000000") || glyph(x, y, 68, "000000111000000");
    else {
      constexpr const char* digits[]{"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
                                     "111100111001111", "111100111101111", "111001010010010", "111101111101111", "111101111001111"};
      const unsigned count = speed >= 100 ? 3 : speed >= 10 ? 2 : 1;
      unsigned divisor = count == 3 ? 100 : count == 2 ? 10 : 1;
      for (unsigned n = 0; n < count; ++n) {
        number |= glyph(x, y, 52 + 16 * n, digits[(speed / divisor) % 10]);
        divisor /= 10;
      }
    }
    if (number)
      out = {0, 255, 0, 255};
    return true;
  }
  if (y >= 245 && y < 269) {
    out = {0, 0, 0, 255};
    return true;
  }
  if (!guides || y >= 763)
    return false;
  const bool nose = y < 255;
  const double px = std::min(double(x) + 0.5, 768 - (double(x) + 0.5)), py = double(y) + 0.5 - (nose ? 0 : 259);
  if (nose) {
    const double dx = px - .14 * 768, dy = py - .48 * 255;
    if (std::abs(dx) >= 7 || std::abs(dy) >= 7)
      return false;
    out = {255, 0, 255, 255};
    return true;
  }
  const std::array<double, 2> corner{.305 * 768, .75 * 504};
  const std::array<double, 2> upper{.33 * 768, .625 * 504};
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
