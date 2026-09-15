#include <cassert>
#include <cstdio>
#include "../../src/shared/profile_selection.hpp"

int main() {
  using namespace taxi_camera::standalone;
  Settings previous;
  previous.profile = 2;
  previous.profile_request = 8;
  previous.aircraft_session_epoch = 12;
  Settings selected;
  selected.mounts[0][2] = 27.123;
  selected.exposure = -7.5f;
  selected.left_id = 123;
  selected.right_id = 124;
  selected.manual_mask = selected.calibration_mask = 3;
  selected.scene_test = 1;
  assert(prepare_profile_selection(selected, previous, false));
  assert(selected.profile == 1 && selected.profile_request == 9 && !selected.auto_profile);
  assert(selected.aircraft_session_epoch == 12 && !selected.left_id && !selected.right_id && !selected.route_request);
  assert(!selected.manual_mask && !selected.calibration_mask && !selected.scene_test);
  assert(selected.mounts[0][2] == 27.123 && selected.exposure == -7.5f);
  // Reselecting the displayed profile must be a new action without resetting calibration.
  auto retry = selected;
  assert(prepare_profile_selection(retry, selected, false));
  assert(retry.profile == selected.profile && retry.profile_request == 10 && retry.mounts == selected.mounts);
  auto automatic = previous;
  assert(prepare_profile_selection(automatic, retry, true));
  assert(automatic.profile == 2 && automatic.profile_request == 11 && automatic.auto_profile);
  retry.manual_mask = 3;
  retry.left_id = 999;
  reset_aircraft_session(retry, 13);
  assert(retry.aircraft_session_epoch == 13 && !retry.manual_mask && !retry.left_id);
  assert(retry.mounts == selected.mounts && retry.profile_request == 10);
  previous.profile_request = UINT64_MAX;
  const auto unchanged = retry;
  assert(!prepare_profile_selection(retry, previous, true));
  assert(retry.profile_request == unchanged.profile_request && retry.auto_profile == unchanged.auto_profile);
  std::puts("PASS profile selection: switch, same-profile retry, return to auto, flight-scoped requests and calibration preservation");
}
