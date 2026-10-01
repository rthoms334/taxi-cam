#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#include "../../src/graphics/ground_speed_display.hpp"

int main() {
  using taxi_camera::ground_speed_display;
  const auto expect = [](float input, std::uint32_t knots) {
    const auto display = ground_speed_display(input, true);
    assert(display.valid && display.knots == knots);
  };
  expect(0.f, 0);
  expect(-0.f, 0);
  expect(.9f, 0);
  expect(9.9f, 9);
  expect(12.5f, 12);
  expect(12.9f, 12);
  expect(std::nextafter(13.f, 0.f), 12);
  expect(13.f, 13);
  expect(std::nextafter(13.f, 14.f), 13);
  expect(59.99f, 59);
  expect(60.f, 60);
  expect(60.01f, 60);
  expect(99.f, 99);
  expect(std::nextafter(99.5f, 0.f), 99);
  const float invalid[]{-.01f,
                        99.5f,
                        100.f,
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()};
  for (float input : invalid) {
    const auto display = ground_speed_display(input, true);
    assert(!display.valid && display.knots == 0);
  }
  const auto unavailable = ground_speed_display(12.9f, false);
  assert(!unavailable.valid && unavailable.knots == 0);

  std::puts("Ground-speed display: PASS; fractional truncation, invalid/overflow samples");
}
