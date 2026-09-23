#pragma once

#include <array>
#include <cstdint>

namespace taxi_camera {

struct CalibrationRect {
  std::int32_t left;
  std::int32_t top;
  std::int32_t right;
  std::int32_t bottom;
  std::array<float, 4> color;
};

constexpr std::uint32_t upper_height(std::uint32_t height) {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * 763 / 1024);
}

// These colors and moving markers are a calibration signal, never a camera feed.
// Coordinates are half-open; no rectangle includes any pixel of the lower display.
// Bars covering exactly width x bottom from the origin, for a display that is
// a whole rectangle (single-display profiles) rather than an upper region.
inline std::array<CalibrationRect, 5> calibration_area_rectangles(std::uint32_t width, std::uint32_t bottom_rows, std::uint64_t frame) {
  const auto right = static_cast<std::int32_t>(width);
  const auto bottom = static_cast<std::int32_t>(bottom_rows);
  const auto split = bottom / 3;
  const auto bar_width = static_cast<std::int32_t>(width / 32 > 0 ? width / 32 : 1);
  const auto travel = width > static_cast<std::uint32_t>(bar_width) ? width - bar_width : 1;
  const auto x = static_cast<std::int32_t>((frame * 3) % travel);
  return {{{0, 0, right, split, {0.02f, 0.16f, 0.38f, 1.0f}},
           {0, split, right, bottom, {0.12f, 0.32f, 0.03f, 1.0f}},
           {x, 0, x + bar_width, split, {0.0f, 0.9f, 1.0f, 1.0f}},
           {right - x - bar_width, split, right - x, bottom, {1.0f, 0.8f, 0.0f, 1.0f}},
           {0, split, right, split + 1, {1.0f, 1.0f, 1.0f, 1.0f}}}};
}

inline std::array<CalibrationRect, 5> calibration_rectangles(std::uint32_t width, std::uint32_t height, std::uint64_t frame) {
  return calibration_area_rectangles(width, upper_height(height), frame);
}

}  // namespace taxi_camera
