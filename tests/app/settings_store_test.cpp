#include "../../src/app/settings_store.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace taxi_camera;
using namespace taxi_camera::standalone;

namespace {
unsigned checks{};
std::wstring fixture_root;

void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}

// [display] keys of the retired lighting settings, with values that differ
// from what the camera views now always do.
constexpr std::pair<const wchar_t*, const wchar_t*> RetiredKeys[]{{L"camera_tone", L"0"},
                                                                  {L"automatic_exposure", L"0"},
                                                                  {L"exposure", L"-3"},
                                                                  {L"night_boost", L"7.5"},
                                                                  {L"night_boost_revision", L"1"}};

std::vector<char> contents(const std::wstring& path) {
  std::ifstream stream(std::filesystem::path(path), std::ios::binary);
  require(stream.is_open(), "Read isolated fixture file");
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool contains_utf16(const std::vector<char>& bytes, const std::wstring& text) {
  const auto* first = reinterpret_cast<const char*>(text.data());
  return std::search(bytes.begin(), bytes.end(), first, first + text.size() * sizeof(wchar_t)) != bytes.end();
}

void patch(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
  require(WritePrivateProfileStringW(section, key, value, path.c_str()), "Patch isolated fixture INI");
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

std::wstring ini(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
  wchar_t value[256]{};
  GetPrivateProfileStringW(section, key, L"<missing>", value, 256, path.c_str());
  return value;
}

void select_fixture(const std::wstring& name) {
  settings_override = fixture_root + L"\\" + name;
  require(std::filesystem::create_directories(settings_override), "Create unique ignored settings fixture");
}

bool same_preferences(const Settings& a, const Settings& b) {
  return a.camera_rate == b.camera_rate && a.camera_mode == b.camera_mode && a.auto_profile == b.auto_profile &&
         a.speed_color == b.speed_color && a.nose_dot == b.nose_dot && a.tail_upper == b.tail_upper && a.tail_corner == b.tail_corner &&
         a.tail_inner == b.tail_inner && a.profile == b.profile && a.follow_taxi == b.follow_taxi && a.auto_detect == b.auto_detect &&
         a.single_camera == b.single_camera && a.calibration_budget == b.calibration_budget && a.mounts == b.mounts &&
         a.day_brightness == b.day_brightness && a.night_brightness == b.night_brightness;
}

Settings customized(const profiles::AircraftProfile& profile) {
  Settings value;
  require(load_settings(value, L"missing-installation", profile.id), "Load missing profile with defaults");
  value.enabled = 0;
  value.auto_profile = 0;
  value.follow_taxi = 0;
  value.auto_detect = 0;
  value.camera_rate = 10;
  value.camera_mode = static_cast<UINT>(taxi_camera::CameraMode::smooth);
  value.single_camera = 1;
  value.calibration_budget = 2048;
  value.day_brightness = -1.25f;
  value.night_brightness = 0.5f;
  value.speed_color = {0.125f, 0.5f, 0.875f};
  value.nose_dot = {0.125f, 0.75f};
  value.tail_upper = {0.25f, 0.625f};
  value.tail_corner = {0.375f, 0.5f};
  value.tail_inner = {0.4375f, 0.25f};
  value.mounts[0] = {1.25, 2.5, 3.75, -12.5, 4.25, 0.75};
  value.mounts[1] = {-2.25, 5.5, -7.75, -25.5, 1.75, 0.875};
  value.mounts[2] = value.mounts[1];
  require(valid_settings(value), "Custom fixture preferences are valid");
  return value;
}

// A profile as builds with the lighting settings saved it, plus keys this
// build does not know.
std::wstring older_profile(const Settings& value) {
  require(save_settings(value), "Save isolated profile fixture");
  const auto path = settings_path(value);
  for (const auto& [key, saved] : RetiredKeys)
    patch(path, L"display", key, saved);
  patch(path, L"display", L"future_display_preference", L"keep display value");
  patch(path, L"future_section", L"unrelated", L"keep section value");
  return path;
}

void missing_rate_uses_shipped_default_without_rewriting_saved_fifteen() {
  select_fixture(L"camera-rate-default");
  auto saved = customized(profiles::A380);
  saved.camera_rate = 15;
  require(save_settings(saved), "Save an existing 15 fps profile");
  Settings loaded;
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.camera_rate == 15 && loaded.mounts == saved.mounts,
          "An already-saved 15 fps preference is left unchanged");
  const auto path = settings_path(saved);
  patch(path, L"display", L"camera_rate", nullptr);
  require(ini(path, L"display", L"camera_rate") == L"<missing>", "camera_rate key removed from isolated profile");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.camera_rate == taxi_camera::kDefaultCameraRate &&
              loaded.mounts == saved.mounts,
          "A profile with no camera_rate key uses the shipped default of 10");
  require(!contains_utf16(contents(path), L"camera_rate="), "Loading a missing rate must not write a rate key");
}

void camera_mode_persists_and_migrates() {
  select_fixture(L"camera-mode");
  auto saved = customized(profiles::A380);
  require(save_settings(saved), "Save a profile for camera modes");
  Settings loaded;
  const auto path = settings_path(saved);
  // Keys of the removed parked floor and dynamic tail rate are ignored and
  // dropped by the next save.
  patch(path, L"display", L"parked_rate", L"2");
  patch(path, L"display", L"parked_rate_revision", L"1");
  patch(path, L"display", L"dynamic_tail", L"1");
  require(load_settings(loaded, L"missing-installation", saved.profile) && same_preferences(loaded, saved) && save_settings(saved) &&
              ini(path, L"display", L"parked_rate") == L"<missing>" && ini(path, L"display", L"parked_rate_revision") == L"<missing>" &&
              ini(path, L"display", L"dynamic_tail") == L"<missing>",
          "Removed parked floor and dynamic tail keys were not ignored and dropped");
  // Camera mode: saved per profile; profiles from before modes map their rate.
  using taxi_camera::CameraMode;
  const auto mode = [](CameraMode value) { return static_cast<UINT>(value); };
  require(ini(path, L"display", L"camera_mode") == std::to_wstring(saved.camera_mode), "Camera mode saved");
  saved.camera_mode = mode(CameraMode::smooth);
  require(save_settings(saved) && load_settings(loaded, L"missing-installation", saved.profile) &&
              loaded.camera_mode == mode(CameraMode::smooth),
          "A saved camera mode is kept");
  patch(path, L"display", L"camera_mode", nullptr);
  const std::pair<const wchar_t*, CameraMode> legacy[]{
      {L"5", CameraMode::performance}, {L"10", CameraMode::automatic}, {L"15", CameraMode::custom}, {L"30", CameraMode::custom}};
  for (const auto& [rate, expected] : legacy) {
    patch(path, L"display", L"camera_rate", rate);
    require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.camera_mode == mode(expected) &&
                loaded.camera_rate == static_cast<UINT>(std::wcstoul(rate, nullptr, 10)),
            "A profile from before camera modes did not map its saved rate");
  }
  patch(path, L"display", L"camera_mode", L"7");
  patch(path, L"display", L"camera_rate", L"10");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.camera_mode == mode(CameraMode::automatic),
          "An unknown camera mode did not fall back like a missing key");
  // Camera weather is always on: a camera_weather key saved by a build that
  // had the setting is ignored and dropped by the next save.
  patch(path, L"display", L"camera_weather", L"0");
  patch(path, L"display", L"camera_weather_revision", L"1");
  require(load_settings(loaded, L"missing-installation", saved.profile) && save_settings(saved) &&
              ini(path, L"display", L"camera_weather") == L"<missing>" && ini(path, L"display", L"camera_weather_revision") == L"<missing>",
          "A saved camera_weather key was not ignored and dropped");
}

// Camera brightness: 0 when missing, saved per profile, and a hand edit out of
// range or off a step is snapped instead of rejecting the calibration file.
void camera_brightness_persists_and_snaps() {
  using namespace taxi_camera;
  require(valid_camera_brightness(0) && valid_camera_brightness(-4) && valid_camera_brightness(2) && valid_camera_brightness(-1.25f) &&
              !valid_camera_brightness(-4.25f) && !valid_camera_brightness(2.25f) && !valid_camera_brightness(0.1f) &&
              !valid_camera_brightness(NAN),
          "Brightness is -4 to +2 EV in 0.25 steps");
  require(snap_camera_brightness(-9) == -4 && snap_camera_brightness(7) == 2 && snap_camera_brightness(0.3) == 0.25f &&
              snap_camera_brightness(NAN) == 0,
          "Brightness snaps into range and onto a step");
  require(camera_brightness_ev(-1, 1, 0) == -1 && camera_brightness_ev(-1, 1, 1) == 1 && camera_brightness_ev(-1, 1, 0.5) == 0 &&
              camera_brightness_ev(-1, 1, 7) == 1 && camera_brightness_ev(-1, 1, NAN) == -1,
          "Day and night brightness blend by darkness");
  require(ambient_darkness(4000) == 0 && ambient_darkness(1e6) == 0 && ambient_darkness(1) == 1 && ambient_darkness(0) == 1,
          "Ambient 4000 is day and 1 is night");
  select_fixture(L"camera-brightness");
  auto saved = customized(profiles::Pmdg777300ER);
  require(save_settings(saved), "Save a profile with camera brightness");
  const auto path = settings_path(saved);
  require(ini(path, L"display", L"day_brightness") == L"-1.25" && ini(path, L"display", L"night_brightness") == L"0.5",
          "Save writes both brightness keys");
  Settings loaded;
  require(load_settings(loaded, L"missing-installation", saved.profile) && same_preferences(loaded, saved), "Brightness survives a save");
  patch(path, L"display", L"day_brightness", nullptr);
  patch(path, L"display", L"night_brightness", L"9");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.day_brightness == 0 && loaded.night_brightness == 2 &&
              loaded.mounts == saved.mounts,
          "A missing brightness is 0 and an out-of-range one is clamped");
  // The absolute exposure of 0.9.50 and earlier is not read as a brightness.
  patch(path, L"display", L"night_brightness", nullptr);
  patch(path, L"display", L"exposure", L"-8.8");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.day_brightness == 0 && loaded.night_brightness == 0,
          "A retired exposure key does not become a brightness");
}

// The camera views always take the main view's lighting: the keys of the
// retired lighting and exposure settings are ignored on every aircraft and
// dropped by the next save; everything else in the profile is kept.
void retired_lighting_keys_are_ignored() {
  select_fixture(L"retired-lighting");
  for (const auto* profile : profiles::Catalog) {
    const auto expected = customized(*profile);
    const auto path = older_profile(expected);
    const auto selection = settings_override + L"\\settings.ini";
    patch(selection, L"display", L"exposure_revision", L"1");
    const auto global_before = contents(selection);
    const auto before = contents(path);
    Settings loaded;
    require(load_settings(loaded, L"missing-installation", profile->id) && same_preferences(loaded, expected),
            "Retired lighting keys do not change the loaded preferences");
    require(contents(path) == before && contents(selection) == global_before, "Loading does not rewrite the profile or settings.ini");
    require(save_settings(loaded), "Save the profile again");
    for (const auto& [key, saved] : RetiredKeys)
      require(ini(path, L"display", key) == L"<missing>", "A save drops the retired lighting keys");
    require(ini(path, L"display", L"future_display_preference") == L"<missing>",
            "A save writes the profile from the known preferences only");
    require(load_settings(loaded, L"missing-installation", profile->id) && same_preferences(loaded, expected),
            "Preferences survive the save");
  }
}

// enabled is runtime connection state: a save does not write it, and a
// [service] enabled key saved by an earlier version, even an invalid one, is
// ignored and dropped by the next save.
void saved_enabled_key_is_ignored() {
  select_fixture(L"retired-enabled");
  const auto saved = customized(profiles::A380);
  require(save_settings(saved), "Save a profile with the connection off");
  const auto path = settings_path(saved);
  require(ini(path, L"service", L"enabled") == L"<missing>", "A save does not write the connection state");
  for (const wchar_t* value : {L"0", L"7"}) {
    patch(path, L"service", L"enabled", value);
    Settings loaded;
    require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.enabled == Settings{}.enabled &&
                same_preferences(loaded, saved),
            "A saved enabled key does not change the loaded settings");
    require(save_settings(loaded) && ini(path, L"service", L"enabled") == L"<missing>", "A save drops the saved enabled key");
  }
}

void rejected_preferences_are_left_unchanged() {
  select_fixture(L"invalid-preferences");
  const auto expected = customized(profiles::A359);
  const auto path = older_profile(expected);
  patch(path, L"nose", L"lens", L"0.01");
  const auto before = contents(path);
  const auto selection_before = contents(settings_override + L"\\settings.ini");
  Settings caller = expected;
  caller.camera_rate = 30;
  caller.route_request = 123;
  caller.calibration_mask = 2;
  const auto caller_before = caller;
  require(!load_settings(caller, L"missing-installation", expected.profile), "Invalid calibration rejects profile load");
  require(same_preferences(caller, caller_before) && caller.route_request == caller_before.route_request &&
              caller.calibration_mask == caller_before.calibration_mask,
          "Invalid profile leaves caller preferences and session state unchanged");
  require(contents(path) == before, "Invalid profile is not rewritten");
  require(contents(settings_override + L"\\settings.ini") == selection_before, "Rejected load preserves global settings");
}

struct LocalAppDataOverride {
  std::wstring original;
  bool existed{};
  explicit LocalAppDataOverride(const std::wstring& directory) {
    wchar_t value[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", value, 32768);
    require(length < 32768, "Read existing process-local application data path");
    existed = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    original.assign(value, length);
    require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory.c_str()), "Redirect only this test process for legacy import");
  }
  ~LocalAppDataOverride() { SetEnvironmentVariableW(L"LOCALAPPDATA", existed ? original.c_str() : nullptr); }
};

void legacy_import_preserves_original() {
  const auto local = fixture_root + L"\\legacy-import";
  require(std::filesystem::create_directories(local + L"\\380 Taxi Cam"), "Create isolated legacy application directory");
  LocalAppDataOverride environment(local);
  settings_override = local + L"\\380 Taxi Cam";
  auto expected = customized(profiles::A359);
  const auto legacy_path = older_profile(expected);
  const auto original = contents(legacy_path);
  const auto original_selection = contents(settings_override + L"\\settings.ini");
  settings_override.clear();
  // Legacy profile import does not import the separate global automatic-selection preference.
  expected.auto_profile = 1;
  Settings loaded;
  require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
          "Legacy renamed-application profile imports calibration and preferences");
  const auto imported_path = settings_path(loaded);
  require(imported_path != legacy_path && contents(imported_path) == original,
          "The imported profile is a copy in the current application directory");
  require(contents(legacy_path) == original, "Legacy import never modifies the original profile");
  require(contents(local + L"\\380 Taxi Cam\\settings.ini") == original_selection, "Legacy global settings remain untouched");
  require(ini(imported_path, L"future_section", L"unrelated") == L"keep section value", "Legacy unknown preferences survive import");
}
}  // namespace

int main() {
  try {
    wchar_t repository[32768]{};
    const auto length = GetCurrentDirectoryW(32768, repository);
    require(length && length < 32768, "Locate working directory for ignored fixture files");
    fixture_root = std::wstring(repository) + L"\\build\\settings-store-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64());
    require(std::filesystem::create_directories(fixture_root), "Create ignored test root");
    require(Settings{}.camera_rate == taxi_camera::kDefaultCameraRate &&
                Settings{}.camera_mode == static_cast<UINT>(taxi_camera::kDefaultCameraMode) && valid_settings(Settings{}),
            "The shipped camera rate is ten and the default mode is Auto");
    missing_rate_uses_shipped_default_without_rewriting_saved_fifteen();
    camera_mode_persists_and_migrates();
    camera_brightness_persists_and_snaps();
    retired_lighting_keys_are_ignored();
    saved_enabled_key_is_ignored();
    rejected_preferences_are_left_unchanged();
    legacy_import_preserves_original();
    settings_override.clear();
    std::printf("PASS: %u settings store checks (CPU/file fixtures only).\n", checks);
    return 0;
  } catch (const std::exception& error) {
    settings_override.clear();
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
