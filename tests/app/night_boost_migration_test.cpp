#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/app/settings_store.hpp"

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
  return a.enabled == b.enabled && a.camera_rate == b.camera_rate && a.parked_rate == b.parked_rate &&
         a.automatic_exposure == b.automatic_exposure && a.exposure == b.exposure && a.night_boost == b.night_boost &&
         a.auto_profile == b.auto_profile && a.speed_color == b.speed_color && a.nose_dot == b.nose_dot && a.tail_upper == b.tail_upper &&
         a.tail_corner == b.tail_corner && a.tail_inner == b.tail_inner && a.profile == b.profile && a.follow_taxi == b.follow_taxi &&
         a.auto_detect == b.auto_detect && a.single_camera == b.single_camera && a.dynamic_tail == b.dynamic_tail &&
         a.calibration_budget == b.calibration_budget && a.mounts == b.mounts;
}

Settings customized(const profiles::AircraftProfile& profile) {
  Settings value;
  require(load_settings(value, L"missing-installation", profile.id), "Load missing profile with defaults");
  require(value.night_boost == 8.f, "Every missing aircraft profile defaults to night boost eight");
  value.enabled = 0;
  value.auto_profile = 0;
  value.follow_taxi = 0;
  value.auto_detect = 0;
  value.camera_rate = 10;
  value.parked_rate = 8;
  value.single_camera = 1;
  value.calibration_budget = 2048;
  value.automatic_exposure = 0;
  value.exposure = -6.25f;
  value.night_boost = 2.5f;
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

std::wstring unmarked_profile(const Settings& value) {
  require(save_settings(value), "Save isolated profile fixture");
  const auto path = settings_path(value);
  patch(path, L"display", L"night_boost_revision", nullptr);
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

void parked_rate_persists_and_defaults() {
  select_fixture(L"parked-rate");
  auto saved = customized(profiles::A380);
  saved.parked_rate = 0;
  require(save_settings(saved), "Save a profile with the parked floor disabled");
  Settings loaded;
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == 0 && loaded.camera_rate == 10,
          "A disabled parked floor (0) persists without touching camera_rate");
  const auto path = settings_path(saved);
  patch(path, L"display", L"parked_rate", nullptr);
  require(ini(path, L"display", L"parked_rate") == L"<missing>", "parked_rate key removed from isolated profile");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == taxi_camera::kDefaultParkedCameraRate,
          "A profile with no parked_rate key uses the shipped floor of 2");
  require(!contains_utf16(contents(path), L"parked_rate="), "Loading a missing floor must not write a floor key");
  patch(path, L"display", L"parked_rate", L"3");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == 3,
          "A floor below the moving minimum loads");
  patch(path, L"display", L"parked_rate", L"90");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == taxi_camera::kMaximumCameraRate &&
              loaded.mounts == saved.mounts,
          "An out-of-range floor is clamped instead of rejecting the calibration file");
  patch(path, L"display", L"parked_rate", L"8");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == 8, "An adjusted floor loads");
  saved.parked_rate = 8;
  require(save_settings(saved) && ini(path, L"display", L"parked_rate") == L"8" && ini(path, L"display", L"parked_rate_revision") == L"1",
          "Save writes the parked floor and its revision next to camera_rate");
  // A profile saved before the revision holds the previous shipped floor of 5:
  // it loads as the new default until saved again, then a saved 5 is kept.
  patch(path, L"display", L"parked_rate", L"5");
  patch(path, L"display", L"parked_rate_revision", nullptr);
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == taxi_camera::kDefaultParkedCameraRate &&
              loaded.mounts == saved.mounts,
          "The previous shipped floor of 5 migrates to the new default");
  require(ini(path, L"display", L"parked_rate") == L"5", "Loading must not rewrite the saved floor");
  patch(path, L"display", L"parked_rate", L"8");
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == 8,
          "A pre-revision floor other than the old default is the user's choice");
  saved.parked_rate = 5;
  require(save_settings(saved) && load_settings(loaded, L"missing-installation", saved.profile) && loaded.parked_rate == 5,
          "A floor of 5 saved with the revision is kept");
  // Dynamic tail rate: on when missing, and a saved off is kept.
  require(loaded.dynamic_tail == 1 && ini(path, L"display", L"dynamic_tail") == L"1", "Dynamic tail rate saves on by default");
  saved.dynamic_tail = 0;
  require(save_settings(saved) && load_settings(loaded, L"missing-installation", saved.profile) && loaded.dynamic_tail == 0,
          "A saved dynamic tail off is kept");
  patch(path, L"display", L"dynamic_tail", nullptr);
  require(load_settings(loaded, L"missing-installation", saved.profile) && loaded.dynamic_tail == 1,
          "A profile without dynamic_tail loads with it on");
}

void all_profiles_migrate_once() {
  select_fixture(L"all-profiles");
  for (const auto* profile : profiles::Catalog) {
    auto expected = customized(*profile);
    expected.night_boost = profile->id % 2 ? 4.f : 1.5f;
    const auto path = unmarked_profile(expected);
    const auto selection = settings_override + L"\\settings.ini";
    patch(selection, L"future_global", L"unrelated", L"keep global value");
    const auto global_before = contents(selection);
    expected.night_boost = 8.f;
    Settings loaded;
    require(load_settings(loaded, L"missing-installation", profile->id), "Load an unmarked existing profile");
    require(same_preferences(loaded, expected), "Migrate boost while preserving all other known preferences");
    require(ini(path, L"display", L"night_boost") == L"8", "Persist forced night boost");
    require(ini(path, L"display", L"night_boost_revision") == L"1", "Persist completed migration revision");
    const auto persisted = contents(path);
    require(contains_utf16(persisted, L"night_boost=8\r\n") && contains_utf16(persisted, L"night_boost_revision=1\r\n"),
            "Corrected boost and revision are present in disk bytes, not just the profile API cache");
    require(ini(path, L"display", L"future_display_preference") == L"keep display value" &&
                ini(path, L"future_section", L"unrelated") == L"keep section value",
            "Migration preserves unknown keys and sections");
    require(contents(selection) == global_before, "Profile migration does not rewrite global settings");
    require(GetFileAttributesW((path + L".night-boost.tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
            "Successful migration leaves no temporary file");
    const auto after_first_load = contents(path);
    require(load_settings(loaded, L"missing-installation", profile->id) && same_preferences(loaded, expected),
            "Already migrated profile reloads unchanged");
    require(contents(path) == after_first_load, "Already migrated load does not rewrite profile");
    loaded.night_boost = 0.5f;
    require(save_settings(loaded), "User can save a different night boost after migration");
    Settings edited;
    require(load_settings(edited, L"missing-installation", profile->id) && same_preferences(edited, loaded),
            "Subsequent user preferences survive reload");
    require(ini(path, L"display", L"night_boost_revision") == L"1", "User save retains current migration marker");
  }
}

void completed_revisions_preserve_custom_values() {
  unsigned fixture{};
  for (const auto* revision : {L"1", L"99"})
    for (const float boost : {0.f, 2.5f, 8.f}) {
      select_fixture(L"completed-" + std::to_wstring(++fixture));
      auto expected = customized(profiles::A380);
      expected.night_boost = boost;
      require(save_settings(expected), "Save already migrated user preference");
      const auto path = settings_path(expected);
      patch(path, L"display", L"night_boost_revision", revision);
      const auto before = contents(path);
      Settings loaded;
      require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
              "Current and future revisions preserve custom boost across its full range");
      require(contents(path) == before, "Current and future migration markers prevent rewrites");
      require(save_settings(loaded), "Save marked custom boost");
      require(ini(path, L"display", L"night_boost_revision") == L"1", "Save writes supported migration revision");
      require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
              "Save and reload preserve marked custom boost");
    }
}

void rejected_preferences_do_not_migrate() {
  select_fixture(L"invalid-preferences");
  const auto expected = customized(profiles::A359);
  const auto path = unmarked_profile(expected);
  patch(path, L"nose", L"lens", L"0.01");
  const auto before = contents(path);
  const auto selection_before = contents(settings_override + L"\\settings.ini");
  Settings caller = expected;
  caller.exposure = -3.f;
  caller.night_boost = 7.f;
  caller.route_request = 123;
  caller.calibration_mask = 2;
  const auto caller_before = caller;
  require(!load_settings(caller, L"missing-installation", expected.profile), "Invalid calibration rejects profile load");
  require(same_preferences(caller, caller_before) && caller.route_request == caller_before.route_request &&
              caller.calibration_mask == caller_before.calibration_mask,
          "Invalid profile leaves caller preferences and session state unchanged");
  require(contents(path) == before, "Invalid profile cannot be marked or rewritten by migration");
  require(contents(settings_override + L"\\settings.ini") == selection_before, "Rejected load preserves global settings");
}

void incomplete_revisions_migrate() {
  unsigned fixture{};
  for (const auto* revision : {L"0", L"-1", L"0.5", L"1.5", L"1e100", L"4294967296", L"invalid"}) {
    select_fixture(L"incomplete-" + std::to_wstring(++fixture));
    auto expected = customized(profiles::A380);
    const auto path = unmarked_profile(expected);
    patch(path, L"display", L"night_boost_revision", revision);
    expected.night_boost = 8.f;
    Settings loaded;
    require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
            "Old or malformed revision cannot suppress the required migration");
    require(ini(path, L"display", L"night_boost_revision") == L"1", "Malformed revision is replaced after successful migration");
  }
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};

void failed_replacement_retries_without_losing_preferences() {
  select_fixture(L"blocked-replacement");
  auto expected = customized(profiles::A35K);
  const auto path = unmarked_profile(expected);
  const auto before = contents(path);
  const auto selection_before = contents(settings_override + L"\\settings.ini");
  expected.night_boost = 8.f;
  {
    // Allow reading/copying the original but deny its deletion or atomic replacement.
    Handle lock{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr)};
    require(lock.value != INVALID_HANDLE_VALUE, "Hold original profile against replacement");
    Settings loaded;
    require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
            "Write failure still supplies corrected runtime boost and preserves user preferences");
    require(contents(path) == before, "Failed atomic replacement leaves original profile byte-identical");
    require(ini(path, L"display", L"night_boost_revision") == L"<missing>", "Failed migration remains eligible for retry");
  }
  Settings retried;
  require(load_settings(retried, L"missing-installation", expected.profile) && same_preferences(retried, expected),
          "Migration retries successfully once profile is writable");
  require(ini(path, L"display", L"night_boost") == L"8" && ini(path, L"display", L"night_boost_revision") == L"1",
          "Retry persists both corrected value and completed revision");
  require(ini(path, L"future_section", L"unrelated") == L"keep section value", "Retry preserves unknown preferences");
  require(contents(settings_override + L"\\settings.ini") == selection_before, "Write failure and retry preserve global settings");
}

struct FileAttributes {
  std::wstring path;
  DWORD original{};
  ~FileAttributes() { SetFileAttributesW(path.c_str(), original); }
};

void readonly_profile_retries_without_losing_preferences() {
  select_fixture(L"readonly-profile");
  auto expected = customized(profiles::IniA380);
  const auto path = unmarked_profile(expected);
  const auto before = contents(path);
  expected.night_boost = 8.f;
  {
    FileAttributes attributes{path, GetFileAttributesW(path.c_str())};
    require(
        attributes.original != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(path.c_str(), attributes.original | FILE_ATTRIBUTE_READONLY),
        "Make only the isolated profile read-only");
    Settings loaded;
    require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
            "Read-only profile still receives corrected boost in memory");
    require(contents(path) == before && (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY),
            "Read-only migration failure preserves original bytes and attributes");
    require(GetFileAttributesW((path + L".night-boost.tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
            "Failed read-only temporary copy does not block future retries");
  }
  Settings retried;
  require(load_settings(retried, L"missing-installation", expected.profile) && same_preferences(retried, expected),
          "Migration succeeds after read-only attribute is removed");
  require(ini(path, L"display", L"night_boost") == L"8" && ini(path, L"display", L"night_boost_revision") == L"1",
          "Read-only retry persists the corrected boost and revision");
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
  const auto legacy_path = unmarked_profile(expected);
  const auto original = contents(legacy_path);
  const auto original_selection = contents(settings_override + L"\\settings.ini");
  settings_override.clear();
  expected.night_boost = 8.f;
  // Legacy profile import does not import the separate global automatic-selection preference.
  expected.auto_profile = 1;
  Settings loaded;
  require(load_settings(loaded, L"missing-installation", expected.profile) && same_preferences(loaded, expected),
          "Legacy renamed-application profile imports calibration and migrates its boost");
  const auto imported_path = settings_path(loaded);
  require(imported_path != legacy_path && ini(imported_path, L"display", L"night_boost") == L"8" &&
              ini(imported_path, L"display", L"night_boost_revision") == L"1",
          "Imported profile receives durable migration in the current application directory");
  require(contents(legacy_path) == original, "Legacy import and boost migration never modify the original profile");
  require(contents(local + L"\\380 Taxi Cam\\settings.ini") == original_selection, "Legacy global settings remain untouched");
  require(ini(imported_path, L"future_section", L"unrelated") == L"keep section value", "Legacy unknown preferences survive import");
}
}  // namespace

int main() {
  try {
    wchar_t repository[32768]{};
    const auto length = GetCurrentDirectoryW(32768, repository);
    require(length && length < 32768, "Locate working directory for ignored fixture files");
    fixture_root = std::wstring(repository) + L"\\build\\night-boost-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64());
    require(std::filesystem::create_directories(fixture_root), "Create ignored test root");
    require(Settings{}.night_boost == 8.f && Settings{}.camera_rate == taxi_camera::kDefaultCameraRate &&
                Settings{}.parked_rate == taxi_camera::kDefaultParkedCameraRate && valid_settings(Settings{}),
            "Default night boost is eight, the shipped camera rate is ten and the parked floor is two");
    missing_rate_uses_shipped_default_without_rewriting_saved_fifteen();
    parked_rate_persists_and_defaults();
    all_profiles_migrate_once();
    completed_revisions_preserve_custom_values();
    incomplete_revisions_migrate();
    rejected_preferences_do_not_migrate();
    failed_replacement_retries_without_losing_preferences();
    readonly_profile_retries_without_losing_preferences();
    legacy_import_preserves_original();
    settings_override.clear();
    std::printf("PASS: %u night-boost migration checks (CPU/file fixtures only).\n", checks);
    return 0;
  } catch (const std::exception& error) {
    settings_override.clear();
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
