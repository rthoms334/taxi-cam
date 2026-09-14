#include <cassert>
#include <cstdio>
#include <filesystem>
#include "../native-camera/aircraft_identity.hpp"
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
  a350.auto_profile = 0;
  a350.speed_color = {0.125f, 0.875f, 0.25f};
  assert(save_settings(a350));
  Settings loaded;
  assert(load_settings(loaded, L"missing") && loaded.profile == 2 && loaded.mounts == a350.mounts && loaded.exposure == -7);
  assert(loaded.speed_color == a350.speed_color && loaded.auto_profile == 0);
  auto invalid = loaded;
  invalid.speed_color[0] = 1.1f;
  assert(!valid_settings(invalid));
  assert(load_settings(loaded, L"missing", 1) && loaded.mounts == a380.mounts && loaded.exposure == a380.exposure);
  assert(settings_path(a380) != settings_path(a350));
  assert(profiles::matches_aircraft(profiles::A359, "A359 ULR"));
  assert(!profiles::matches_aircraft(profiles::A35K, "A359 ULR"));
  assert(!profiles::matches_aircraft(profiles::A359, ""));
  const char* ini_path = "SimObjects/airplanes/A350/presets/iniBuilds/A350-900_Default_Cabin_ULR/config/aircraft.cfg";
  assert(profiles::detect_aircraft("A359 ULR", ini_path) == 2);
  assert(profiles::detect_aircraft("A35K", ini_path) == 3);
  assert(profiles::detect_aircraft("A388", "D:\\Community\\flybywire-aircraft-a380-842\\aircraft.cfg") == 1);
  assert(!profiles::detect_aircraft("A359", "D:/Community/other-a350/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A359", "D:/Community/not-inibuilds-aircraft-a350/aircraft.cfg"));
  assert(!profiles::detect_aircraft("A359", "D:/Community/inibuilds-aircraft-a350-copy/aircraft.cfg"));
  native_camera::AircraftIdentityCache identity;
  std::array<unsigned char, 296> packet{};
  std::array<std::uint32_t, 10> h{296, 0, 8, 5, 0, 5, 0, 0, 1, 1};
  std::memcpy(packet.data(), h.data(), 40);
  std::strcpy(reinterpret_cast<char*>(packet.data() + 40), "A359 ULR");
  assert(identity.accept(packet.data(), packet.size(), 1000));
  assert(!identity.sample(1000).detected_profile);
  for (unsigned n = 0; n < packet.size(); ++n)
    assert(!identity.accept(packet.data(), n, 1000));
  auto malformed = packet;
  malformed[296 - 1] = 1;
  std::memset(malformed.data() + 40, 'X', 256);
  assert(!identity.accept(malformed.data(), malformed.size(), 1000));
  std::array<unsigned char, 284> path_packet{};
  std::array<std::uint32_t, 6> ph{284, 0, 15, 6, 0, 0};
  std::memcpy(path_packet.data(), ph.data(), 24);
  std::strcpy(reinterpret_cast<char*>(path_packet.data() + 24), ini_path);
  assert(identity.accept(path_packet.data(), path_packet.size(), 1100));
  assert(identity.sample(1100).detected_profile == 2 && identity.sample(1100).fresh);
  assert(identity.sample(4001).detected_profile == 2 && !identity.sample(4001).fresh);
  assert(!identity.sample(999).fresh);
  native_camera::AutoProfileSelection selection;
  assert(!selection.observe(2, 1000));
  assert(!selection.observe(2, 1000));
  assert(selection.observe(2, 2000) == 2);
  assert(!selection.observe(3, 3000));
  assert(!selection.observe(0, 0));
  assert(!selection.observe(3, 4000));
  assert(selection.observe(3, 5000) == 3);
  for (const auto* profile : profiles::Catalog) {
    Settings s;
    s.profile = profile->id;
    s.mounts = profile->mounts;
    assert(valid_settings(s));
    for (unsigned side = 0; side < 2; ++side) {
      const auto rect = profiles::display_rect(*profile, side);
      assert(rect.left < rect.right && rect.right <= profile->width && rect.bottom <= profile->height);
      const auto content = profiles::display_content_rect(*profile, side);
      assert(content.left == rect.left + 16 && content.right + 16 == rect.right && content.top == rect.top + 12 &&
             content.bottom == rect.bottom);
      assert(rect.top == 0 && rect.bottom == 763);
      assert(content.left < content.right && content.top < content.bottom && content.left >= rect.left && content.top >= rect.top &&
             content.right <= rect.right && content.bottom <= rect.bottom);
      assert(content.bottom - content.top == 751);
      const int expected_width = profile->id == 1 ? 736 : 774;
      assert(content.right - content.left == static_cast<unsigned>(expected_width));
      assert(profile->camera_panes[side][0] == expected_width && profile->camera_panes[side][1] == (side == 0 ? 251 : 496));
      native_camera::ViewDimensions desired, original{{{3413, 913}, {3413, 913}, {3413, 913}}};
      assert(native_camera::plan_view_resize(original, side, desired, profile->camera_panes));
      assert(desired[0] == profile->camera_panes[side] && desired[1] == desired[0] && desired[2] == desired[0]);
      assert(profiles::camera_candidate(desired[0][0], desired[0][1]));
    }
  }
  for (unsigned side = 0; side < 2; ++side) {
    const auto outer = profiles::display_rect(profiles::A380, side);
    assert(outer.left == 0 && outer.right == 768 && outer.top == 0 && outer.bottom == 763);
    const auto content = profiles::display_content_rect(profiles::A380, side);
    assert(content.left == 16 && content.right == 752 && content.top == 12 && content.bottom == 763);
  }
  for (const auto* profile : {&profiles::A359, &profiles::A35K}) {
    const auto left = profiles::display_rect(*profile, 0), right = profiles::display_rect(*profile, 1);
    assert(left.left == 0 && left.right == 806 && right.left == 838 && right.right == 1644);
    assert(right.left - left.right == 32);
    const auto left_content = profiles::display_content_rect(*profile, 0), right_content = profiles::display_content_rect(*profile, 1);
    assert(left_content.left == 16 && left_content.right == 790 && right_content.left == 854 && right_content.right == 1628);
    assert(left_content.top == 12 && right_content.top == 12 && left_content.bottom == 763 && right_content.bottom == 763);
    for (const auto& pane : profile->camera_panes)
      assert(pane[0] == 774);
  }
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
