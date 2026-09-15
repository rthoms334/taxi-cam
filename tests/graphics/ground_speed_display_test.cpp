#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#include "../../src/camera/taxi_speed_cutoff.hpp"
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

  // Quantizing the display must not delay the raw-speed cutoff until 61 kt.
  taxi_camera::native_camera::TaxiSpeedCutoff cutoff;
  assert(cutoff.update(100, true, 59.99, true, 1, 100) == 0);
  assert(cutoff.update(101, true, 60.0, true, 1, 101) == 0);
  assert(cutoff.update(102, true, 60.01, true, 1, 102) == 1);
  assert(cutoff.inhibited());
  std::puts("Ground-speed display: PASS; fractional truncation, invalid/overflow samples, raw-speed cutoff");
}
