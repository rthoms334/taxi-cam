#include <cassert>
#include <cstdio>
#include <filesystem>
#include "settings_store.hpp"
#include "../native-camera/view_resize.hpp"
#include "../src/pfd_target_detector.hpp"
using namespace taxi_camera;
int main() {
  using namespace standalone;
  wchar_t temporary[32768];
  assert(GetTempPathW(32768, temporary));
  settings_override = std::wstring(temporary) + L"TaxiCam-profile-test-" + std::to_wstring(GetCurrentProcessId());
  struct Cleanup {
    ~Cleanup() { std::filesystem::remove_all(settings_override); }
  } cleanup;
  Settings a380;
  a380.mounts[0][2] = 27.123;
  a380.exposure = -9.1f;
  assert(save_settings(a380));
  Settings a350;
  assert(load_settings(a350, L"missing", 2));
  assert(a350.profile == 2 && a350.mounts == profiles::A359.mounts);
  a350.mounts[1][3] = -19.125;
  a350.exposure = -7;
  assert(save_settings(a350));
  Settings loaded;
  assert(load_settings(loaded, L"missing") && loaded.profile == 2 && loaded.mounts == a350.mounts && loaded.exposure == -7);
  assert(load_settings(loaded, L"missing", 1) && loaded.mounts == a380.mounts && loaded.exposure == a380.exposure);
  assert(settings_path(a380) != settings_path(a350));
  assert(profiles::matches_aircraft(profiles::A359, "A359 ULR"));
  assert(!profiles::matches_aircraft(profiles::A35K, "A359 ULR"));
  assert(!profiles::matches_aircraft(profiles::A359, ""));
  for (const auto* profile : profiles::Catalog) {
    Settings s;
    s.profile = profile->id;
    s.mounts = profile->mounts;
    assert(valid_settings(s));
    for (unsigned side = 0; side < 2; ++side) {
      const auto rect = profiles::display_rect(*profile, side);
      assert(rect.left < rect.right && rect.right <= profile->width && rect.bottom <= profile->height);
      native_camera::ViewDimensions desired, original{{{3413, 913}, {3413, 913}, {3413, 913}}};
      assert(native_camera::plan_view_resize(original, side, desired, profile->camera_panes));
      assert(desired[0] == profile->camera_panes[side] && desired[1] == desired[0] && desired[2] == desired[0]);
      assert(profiles::camera_candidate(desired[0][0], desired[0][1]));
    }
  }
  assert(profiles::display_rect(profiles::A359, 0).right == 822);
  assert(profiles::display_rect(profiles::A359, 1).left == 822);
  static PfdTargetDetector detector;
  detector.configure(profiles::A359);
  std::array<PfdTargetObservation, 4> observations{
      {{10, 0, 1644, 1024, 1, 28}, {20, 0, 1644, 1024, 1, 28}, {30, 0, 1644, 1024, 1, 28}, {40, 0, 768, 1024, 5, 28}}};
  detector.observe(observations.data(), observations.size(), 0);
  for (unsigned i = 1; i <= 3; ++i) {
    observations[0].draws += 100;
    observations[1].draws += 100;
    observations[2].draws += 20;
    observations[3].draws += 1000;
    detector.observe(observations.data(), observations.size(), i * 1000);
  }
  assert(detector.snapshot().valid && detector.snapshot().targets[0] == 20 && detector.snapshot().targets[1] == 10);
  detector.configure(profiles::A380);
  assert(!detector.snapshot().valid);
  assert(!detector.observe(observations.data(), observations.size(), 4000).valid);
  assert(!profiles::matches_display(profiles::A359, 1644, 1024, 0, 28));
  assert(!profiles::matches_display(profiles::A359, 1644, 1024, 1, 0));
  std::puts("PASS aircraft profiles: isolated settings, variant identity, pane dimensions, mirrored display regions and detector reset");
}
