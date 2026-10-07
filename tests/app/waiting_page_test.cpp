#include <cassert>
#include <cstdio>
#include "../../src/shared/waiting_page.hpp"

using taxi_camera::profiles::WaitingPageMinimumMs;
using taxi_camera::standalone::WaitingPageTimer;

int main() {
  // The stale limit covers six feed intervals and never drops under a second.
  using taxi_camera::profiles::WaitingPageStaleMs;
  using taxi_camera::standalone::waiting_stale_ms;
  static_assert(waiting_stale_ms(2) == 3000);
  static_assert(waiting_stale_ms(5) == 1200);
  static_assert(waiting_stale_ms(10) == WaitingPageStaleMs);
  static_assert(waiting_stale_ms(60) == WaitingPageStaleMs);
  static_assert(waiting_stale_ms(0) == 6000);
  WaitingPageTimer timer;
  assert(timer.observe(1000, 0) == 0);
  // Each side starts its own minimum time when it is first admitted.
  assert(timer.observe(1000, 1) == 1);
  assert(timer.observe(1500, 3) == 3);
  assert(timer.observe(1000 + WaitingPageMinimumMs - 1, 3) == 3);
  assert(timer.observe(1000 + WaitingPageMinimumMs, 3) == 2);
  assert(timer.observe(1500 + WaitingPageMinimumMs, 3) == 0);
  // OFF resets that side only; the next ON shows the page again.
  assert(timer.observe(5000, 2) == 0);
  assert(timer.observe(5100, 3) == 1);
  assert(timer.observe(5100 + WaitingPageMinimumMs, 3) == 0);
  // A zero or backwards clock never ends the page early.
  WaitingPageTimer zero;
  assert(zero.observe(0, 1) == 1);
  assert(zero.observe(0, 1) == 1);
  WaitingPageTimer backwards;
  assert(backwards.observe(9000, 2) == 2);
  assert(backwards.observe(8000, 2) == 2);
  // Bits beyond the three display sides are ignored.
  WaitingPageTimer wide;
  assert(wide.observe(100, 0xffu) == 7);
  // A side still requested that briefly lost its display texture (recreated by
  // the aircraft) resumes without the page within WaitingPageResumeMs.
  using taxi_camera::standalone::WaitingPageResumeMs;
  WaitingPageTimer texture;
  assert(texture.observe(1000, 1, 1) == 1);
  assert(texture.observe(1000 + WaitingPageMinimumMs, 1, 1) == 0);
  assert(texture.observe(3000, 0, 1) == 0);
  assert(texture.observe(3000 + WaitingPageResumeMs, 1, 1) == 0);
  // Repeated recreation keeps resuming.
  assert(texture.observe(6100, 0, 1) == 0);
  assert(texture.observe(7000, 1, 1) == 0);
  // A gap longer than WaitingPageResumeMs shows the page again.
  assert(texture.observe(8000, 0, 1) == 0);
  assert(texture.observe(8001 + WaitingPageResumeMs, 1, 1) == 1);
  // A side lost before finishing its page does not skip it on return.
  WaitingPageTimer early;
  assert(early.observe(1000, 1, 1) == 1);
  assert(early.observe(1100, 0, 1) == 0);
  assert(early.observe(1200, 1, 1) == 1);
  // Switched off (no longer requested) in the gap: the next ON shows the page.
  WaitingPageTimer pilot;
  assert(pilot.observe(1000, 1, 1) == 1);
  assert(pilot.observe(1000 + WaitingPageMinimumMs, 1, 1) == 0);
  assert(pilot.observe(2000, 0, 1) == 0);
  assert(pilot.observe(2100, 0, 0) == 0);
  assert(pilot.observe(2200, 1, 1) == 1);
  std::puts("PASS waiting page timer: per-side minimum, OFF reset, texture-recreation resume, clock guards and side bounds");
  return 0;
}
