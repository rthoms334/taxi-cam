#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "../../src/app/settings_store.hpp"

// settings.ini [messages] notifications replaced protocol 10's [messages]
// in_simulator. Existing files keep the user's choice; a save rewrites the
// key under its new name. CPU/file fixtures only, under the ignored build/.
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

std::wstring select_fixture(const std::wstring& name) {
  settings_override = fixture_root + L"\\" + name;
  require(std::filesystem::create_directories(settings_override), "Create unique ignored settings fixture");
  return settings_override + L"\\settings.ini";
}

void patch(const std::wstring& path, const wchar_t* key, const wchar_t* value) {
  require(WritePrivateProfileStringW(L"messages", key, value, path.c_str()), "Patch isolated fixture INI");
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

std::wstring ini(const std::wstring& path, const wchar_t* key) {
  wchar_t value[64]{};
  GetPrivateProfileStringW(L"messages", key, L"<missing>", value, 64, path.c_str());
  return value;
}

Settings loaded() {
  Settings value;
  require(load_settings(value, L"missing-installation", profiles::A380.id), "Load settings from the fixture");
  return value;
}

void missing_keys_default_on() {
  const auto path = select_fixture(L"defaults");
  require(loaded().notifications == 1, "No [messages] section defaults to notifications on");
  require(ini(path, L"notifications") == L"<missing>" && ini(path, L"in_simulator") == L"<missing>",
          "Loading a default must not write either key");
}

void legacy_off_is_kept() {
  const auto path = select_fixture(L"legacy-off");
  patch(path, L"in_simulator", L"0");
  const auto value = loaded();
  require(value.notifications == 0, "A protocol 10 file with in_simulator=0 keeps notifications off");
  require(ini(path, L"in_simulator") == L"0" && ini(path, L"notifications") == L"<missing>", "Loading never rewrites the file");
  require(save_settings(value), "Save migrates the preference");
  require(ini(path, L"notifications") == L"0", "Save writes the user's choice under the new key");
  require(ini(path, L"in_simulator") == L"<missing>", "Save removes the legacy key");
  require(loaded().notifications == 0, "Migrated file still reads off");
}

void legacy_on_is_kept() {
  const auto path = select_fixture(L"legacy-on");
  patch(path, L"in_simulator", L"1");
  require(loaded().notifications == 1, "A protocol 10 file with in_simulator=1 keeps notifications on");
}

void new_key_wins_over_legacy() {
  const auto path = select_fixture(L"both-keys");
  patch(path, L"in_simulator", L"1");
  patch(path, L"notifications", L"0");
  require(loaded().notifications == 0, "notifications=0 wins over a stale in_simulator=1");
  patch(path, L"in_simulator", L"0");
  patch(path, L"notifications", L"1");
  require(loaded().notifications == 1, "notifications=1 wins over a stale in_simulator=0");
}

void save_and_reload_roundtrip() {
  const auto path = select_fixture(L"roundtrip");
  auto value = loaded();
  value.notifications = 0;
  require(save_settings(value), "Save notifications off");
  require(ini(path, L"notifications") == L"0" && loaded().notifications == 0, "Off persists");
  value.notifications = 1;
  require(save_settings(value), "Save notifications on");
  require(ini(path, L"notifications") == L"1" && loaded().notifications == 1, "On persists");
  require(ini(path, L"in_simulator") == L"<missing>", "A current file never gains the legacy key");
}

void odd_values_read_as_flags() {
  const auto path = select_fixture(L"odd-values");
  patch(path, L"notifications", L"7");
  require(loaded().notifications == 1 && valid_settings(loaded()), "Nonzero text reads as on and stays a valid flag");
  patch(path, L"notifications", L"off");
  require(loaded().notifications == 0, "Non-numeric text reads as off, matching the protocol 10 reader");
}

// settings.ini [service] speed_cutoff is the global 60-knot cutoff switch.
std::wstring cutoff_key(const std::wstring& path) {
  wchar_t value[64]{};
  GetPrivateProfileStringW(L"service", L"speed_cutoff", L"<missing>", value, 64, path.c_str());
  return value;
}

void speed_cutoff_default_and_roundtrip() {
  const auto path = select_fixture(L"speed-cutoff");
  require(Settings{}.speed_cutoff == 1 && valid_settings(Settings{}), "Speed cutoff defaults on");
  auto value = loaded();
  require(value.speed_cutoff == 1 && cutoff_key(path) == L"<missing>", "Missing key keeps the cutoff on and is not written by a load");
  value.speed_cutoff = 0;
  require(save_settings(value), "Save speed cutoff off");
  require(cutoff_key(path) == L"0" && loaded().speed_cutoff == 0, "Off persists");
  value.speed_cutoff = 1;
  require(save_settings(value), "Save speed cutoff on");
  require(cutoff_key(path) == L"1" && loaded().speed_cutoff == 1, "On persists");
  value.speed_cutoff = 2;
  require(!valid_settings(value), "Speed cutoff outside 0/1 is rejected");
}
}  // namespace

int main() {
  try {
    wchar_t repository[32768]{};
    const auto length = GetCurrentDirectoryW(32768, repository);
    require(length && length < 32768, "Locate working directory for ignored fixture files");
    fixture_root = std::wstring(repository) + L"\\build\\notification-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64());
    require(std::filesystem::create_directories(fixture_root), "Create ignored test root");
    require(Settings{}.notifications == 1 && valid_settings(Settings{}), "Notifications default on");
    missing_keys_default_on();
    legacy_off_is_kept();
    legacy_on_is_kept();
    new_key_wins_over_legacy();
    save_and_reload_roundtrip();
    odd_values_read_as_flags();
    speed_cutoff_default_and_roundtrip();
    settings_override.clear();
    std::printf("PASS: %u notification settings migration checks (CPU/file fixtures only).\n", checks);
    return 0;
  } catch (const std::exception& error) {
    settings_override.clear();
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
