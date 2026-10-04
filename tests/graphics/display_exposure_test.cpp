#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#include "../../src/graphics/display_exposure.hpp"

int main() {
  using taxi_camera::DisplayExposureController;
  DisplayExposureController controller;
  constexpr float day = -8;
  constexpr auto unlit = DisplayExposureController::UnlitExposureEv;
  constexpr auto boost = DisplayExposureController::NightBoostEv;
  static_assert(boost == 4);
  assert(controller.snapshot().applied_ev == unlit);
  // The first update starts at its target; later ones slew at 1 EV/s.
  auto value = controller.update(1000, day, true, 4000, 1000);
  assert(value.applied_ev == day && value.target_ev == day && value.lighting_valid && value.darkness == 0);
  value = controller.update(1500, day, true, 0.536, 1500);
  assert(value.target_ev == day + boost && value.applied_ev == day + .5f);
  for (unsigned i = 2; i <= 8; ++i)
    value = controller.update(1000 + i * 500, day, true, 0.536, 1000 + i * 500);
  assert(value.applied_ev == day + boost);
  controller.reset();
  value = controller.update(1000, day, true, 0, 1000);
  assert(value.applied_ev == day + boost);  // Zero is a valid darkest reading.
  value = controller.update(2500, day, true, 0, 1000);
  assert(value.lighting_valid && value.target_ev == day + boost && value.darkness == 1);
  // Lighting older than 1.5 s, or none, targets the compositor's default.
  value = controller.update(2501, day, true, 0, 1000);
  assert(!value.lighting_valid && value.target_ev == unlit && value.applied_ev < day + boost && value.applied_ev > day + boost - .01f &&
         value.darkness == 1);  // The last valid darkness is kept for the camera brightness.
  value = controller.update(1000000, day, false, 0, 0);
  assert(value.applied_ev > day + boost - 1.01f);  // A long gap advances at most 1 EV.
  const auto before_reverse = value.applied_ev;
  value = controller.update(999999, day, false, 0, 0);
  assert(value.applied_ev == before_reverse);
  controller.reset();
  value = controller.update(100, day, false, 0, 0);
  assert(!value.lighting_valid && value.applied_ev == unlit && value.target_ev == unlit);

  float previous_target = day + boost;
  for (double ambient : {0., .5, 1., 2., 10., 100., 1000., 4000., 50000.}) {
    controller.reset();
    value = controller.update(100, day, true, ambient, 100);
    assert(value.lighting_valid && value.target_ev <= previous_target && value.target_ev >= day);
    previous_target = value.target_ev;
  }
  assert(previous_target == day);
  for (double invalid : {-1., 1e7 + 1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    value = controller.update(200, day, true, invalid, 200);
    assert(!value.lighting_valid && value.target_ev == unlit && std::isfinite(value.applied_ev));
  }
  value = controller.update(200, day, true, 1, 201);
  assert(!value.lighting_valid);
  controller.reset();
  value = controller.update(300, 3, true, 0, 300);
  assert(value.target_ev == 4 && value.applied_ev == 4);
  controller.reset();
  value = controller.update(300, -99, true, 4000, 300);
  assert(value.applied_ev == -16 && value.target_ev == -16);
  controller.reset();
  value = controller.update(300, std::numeric_limits<float>::quiet_NaN(), true, 4000, 300);
  assert(value.target_ev == unlit && std::isfinite(value.applied_ev));
  controller.reset();
  assert(controller.snapshot().applied_ev == unlit && !controller.snapshot().lighting_valid);
  std::puts("Display exposure: PASS");
}
