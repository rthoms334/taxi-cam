#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "../../src/graphics/calibration.hpp"

int main() {
  static_assert(taxi_camera::upper_height(1024) == 763);
  static_assert(taxi_camera::upper_height(2048) == 1526);
  constexpr std::array<std::uint32_t, 8> dimensions{32, 33, 767, 768, 1023, 1024, 2048, 16384};
  constexpr std::array<std::uint64_t, 5> frames{0, 1, 100, 100000, std::numeric_limits<std::uint64_t>::max()};
  for (const auto width : dimensions) {
    for (const auto height : dimensions) {
      for (const auto frame : frames) {
        const auto rectangles = taxi_camera::calibration_rectangles(width, height, frame);
        for (const auto& rectangle : rectangles) {
          assert(rectangle.left >= 0 && rectangle.right > rectangle.left);
          assert(rectangle.top >= 0 && rectangle.bottom > rectangle.top);
          assert(static_cast<std::uint32_t>(rectangle.right) <= width);
          assert(static_cast<std::uint32_t>(rectangle.bottom) <= taxi_camera::upper_height(height));
          for (const auto channel : rectangle.color) {
            assert(std::isfinite(channel) && channel >= 0.0f && channel <= 1.0f);
          }
        }
        // Background regions completely cover the upper display without gaps.
        assert(rectangles[0].left == 0 && rectangles[1].left == 0);
        assert(rectangles[0].right == static_cast<std::int32_t>(width));
        assert(rectangles[1].right == static_cast<std::int32_t>(width));
        assert(rectangles[0].top == 0 && rectangles[0].bottom == rectangles[1].top);
        assert(rectangles[1].bottom == static_cast<std::int32_t>(taxi_camera::upper_height(height)));
      }
    }
  }
  const auto first = taxi_camera::calibration_rectangles(768, 1024, 0);
  const auto next = taxi_camera::calibration_rectangles(768, 1024, 1);
  assert(first[2].left != next[2].left && first[3].left != next[3].left);
  // Whole-gauge area (PMDG 777 958 x 971): bars fill exactly the gauge.
  for (const auto frame : frames) {
    const auto gauge = taxi_camera::calibration_area_rectangles(958, 971, frame);
    for (const auto& rectangle : gauge)
      assert(rectangle.left >= 0 && rectangle.top >= 0 && rectangle.right <= 958 && rectangle.bottom <= 971 &&
             rectangle.right > rectangle.left && rectangle.bottom > rectangle.top);
    assert(gauge[0].top == 0 && gauge[1].bottom == 971 && gauge[0].right == 958 && gauge[0].bottom == gauge[1].top);
  }
  std::puts("PASS: 320 size/frame combinations preserve lower display, cover upper display, and produce valid animated rectangles.");
}
