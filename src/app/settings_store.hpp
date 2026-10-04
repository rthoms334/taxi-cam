#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include "../camera/mount_config.hpp"
#include "../shared/protocol.hpp"

namespace taxi_camera::standalone {
inline std::wstring settings_override;
inline std::wstring legacy_settings_path(const std::wstring& path) {
  if (!settings_override.empty())
    return {};
  wchar_t local[32768]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
  if (!n || n > 32000)
    return {};
  return std::wstring(local) + L"\\380 Taxi Cam\\profiles\\" + path.substr(path.find_last_of(L"\\") + 1);
}
inline std::wstring settings_directory() {
  if (!settings_override.empty()) {
    CreateDirectoryW(settings_override.c_str(), nullptr);
    return settings_override;
  }
  wchar_t local[32768]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
  if (!n || n > 32000)
    return {};
  std::wstring folder = std::wstring(local) + L"\\Taxi Cam";
  CreateDirectoryW(folder.c_str(), nullptr);
  return folder;
}
inline std::wstring settings_path(const Settings& s) {
  auto folder = settings_directory() + L"\\profiles";
  CreateDirectoryW(folder.c_str(), nullptr);
  const auto* profile = profiles::find(s.profile);
  std::wstring name;
  if (profile)
    for (char c : profile->key)
      name += wchar_t(c);
  return folder + L"\\" + name + L".ini";
}
// One-shot release default for saved profiles: camera_rate=10. Setup applied
// it before the companion did; both use the same stamp on settings.ini, so a
// user already migrated keeps every later choice.
inline constexpr unsigned kCameraRateMigrationRevision = 2;
// True when the path and every existing ancestor are ordinary files or
// directories. A junction or symbolic link could lead outside the settings tree.
inline bool settings_path_without_links(std::wstring path) {
  for (;;) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return false;
    const auto end = path.find_last_of(L"\\/");
    if (end == std::wstring::npos || end < 3)
      return true;
    path.resize(end);
  }
}
inline bool patch_profile_camera_rate(const std::wstring& path) {
  // Patch a copy so calibration, unknown keys and the file's encoding remain intact.
  const auto temporary = path + L".migrate.tmp";
  if (!CopyFileW(path.c_str(), temporary.c_str(), FALSE))
    return false;
  bool ok = WritePrivateProfileStringW(L"display", L"camera_rate", L"10", temporary.c_str()) != FALSE;
  // The profile API reports zero for a cache flush as well as for failures.
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
  if (ok)
    ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!ok) {
    // CopyFile carries read-only attributes too. Remove them only from our
    // failed temporary copy so a later retry cannot be blocked by that file.
    SetFileAttributesW(temporary.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(temporary.c_str());
  }
  return ok;
}
inline void migrate_saved_camera_rate() {
  if (!settings_override.empty())
    return;
  const auto directory = settings_directory();
  if (directory.empty())
    return;
  const auto selection = directory + L"\\settings.ini";
  if (GetPrivateProfileIntW(L"display", L"camera_rate_revision", 0, selection.c_str()) == kCameraRateMigrationRevision)
    return;
  const auto legacy = directory.substr(0, directory.find_last_of(L"\\")) + L"\\380 Taxi Cam";
  bool ok = settings_path_without_links(selection);
  for (const auto* profile : profiles::Catalog)
    for (const auto& folder : {directory, legacy}) {
      const auto path = folder + L"\\profiles\\" + std::wstring(profile->key.begin(), profile->key.end()) + L".ini";
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        continue;
      // Never follow a junction or symbolic link out of the settings tree.
      ok = settings_path_without_links(path) && patch_profile_camera_rate(path) && ok;
    }
  // Stamp last: a failed or refused profile write is retried at the next start.
  if (ok)
    WritePrivateProfileStringW(L"display", L"camera_rate_revision", L"2", selection.c_str());
}
// Global [messages] notifications; protocol 10 saved the same choice as
// [messages] in_simulator. A file with only the legacy key keeps the user's
// choice; save_settings writes the new key and removes the legacy one.
inline std::uint32_t load_notification_preference(const std::wstring& selection) {
  const auto present = [&](const wchar_t* key) {
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"messages", key, L"", value, 16, selection.c_str());
    return value[0] != 0;
  };
  const wchar_t* key = present(L"notifications") ? L"notifications" : present(L"in_simulator") ? L"in_simulator" : nullptr;
  return !key || GetPrivateProfileIntW(L"messages", key, 1, selection.c_str()) ? 1u : 0u;
}
inline constexpr unsigned kCamButtonRevision = 1;
inline bool load_settings(Settings& s, const std::wstring& installation, std::uint32_t profile_id = 0) {
  if (!profile_id)
    profile_id = GetPrivateProfileIntW(L"aircraft", L"profile", 1, (settings_directory() + L"\\settings.ini").c_str());
  const auto* profile = profiles::find(profile_id);
  if (!profile)
    profile = &profiles::A380;
  Settings value;
  value.profile = profile->id;
  value.follow_taxi = profile->taxi_control == profiles::TaxiControl::manual_only ? 0u : 1u;
  value.mounts = profile->mounts;
  value.speed_color = profile->composition.speed_color;
  reset_guide_settings(value, *profile);
  value.auto_profile = GetPrivateProfileIntW(L"aircraft", L"automatic", 1, (settings_directory() + L"\\settings.ini").c_str());
  value.notifications = load_notification_preference(settings_directory() + L"\\settings.ini");
  const auto destination = settings_path(value);
  auto path = destination;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const auto legacy = legacy_settings_path(path);
    if (!legacy.empty() && GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) {
      // Import without replacing a newer profile or deleting the original.
      // If migration fails, read the original so calibration is not reset.
      if (!CopyFileW(legacy.c_str(), path.c_str(), TRUE) && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        path = legacy;
    }
  }
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // Import the user's existing camera calibration once. The companion becomes
    // the settings owner; stale installer defaults never override later UI edits.
    if (profile->id != profiles::A380.id) {
      s = value;
      return true;
    }
    const auto mount_path = installation + L"\\taxi-camera-mounts.cfg";
    HANDLE file = CreateFileW(mount_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      std::array<char, 4097> buffer{};
      DWORD n{};
      if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) && n <= 4096) {
        native_camera::MountPair mounts;
        const char* error{};
        if (native_camera::parse_mount_config(std::string_view(buffer.data(), n), mounts, error))
          for (unsigned i = 0; i < mounts.size(); ++i) {
            const auto& m = mounts[i];
            value.mounts[i] = {m.position_m[0], m.position_m[1], m.position_m[2], m.pitch_degrees, m.yaw_degrees, m.fov_radians};
          }
      }
      CloseHandle(file);
    }
    s = value;
    return true;
  }
  auto read = [&](const wchar_t* section, const wchar_t* key, double fallback) {
    wchar_t def[64], text[128];
    std::swprintf(def, 64, L"%.12g", fallback);
    GetPrivateProfileStringW(section, key, def, text, 128, path.c_str());
    wchar_t* end{};
    const auto v = std::wcstod(text, &end);
    return end != text && *end == 0 && std::isfinite(v) ? v : fallback;
  };
  auto integer = [&](const wchar_t* section, const wchar_t* key, UINT fallback) {
    const double value = read(section, key, fallback);
    return value >= 0 && value <= 16384 && std::floor(value) == value ? static_cast<UINT>(value) : fallback;
  };
  // enabled is runtime connection state (Connect/Disconnect). Profiles saved
  // by earlier versions hold a [service] enabled key; it is ignored.
  value.follow_taxi = profile->taxi_control == profiles::TaxiControl::manual_only ? 0u : integer(L"service", L"follow_taxi", 1);
  // PMDG 777 profiles were manual-only and saved follow_taxi=0. The CAM button
  // now drives them: turn cockpit control on once; later choices are kept.
  if (profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam && integer(L"service", L"cam_button_revision", 0) < kCamButtonRevision)
    value.follow_taxi = 1;
  value.auto_detect = integer(L"service", L"auto_detect", 1);
  value.camera_rate = integer(L"display", L"camera_rate", kDefaultCameraRate);
  // 0 disables the parked floor; any other value is kept inside the rate range
  // so an odd hand edit cannot reject the whole calibration file.
  const UINT parked_rate = integer(L"display", L"parked_rate", kDefaultParkedCameraRate);
  value.parked_rate = parked_rate ? std::clamp(parked_rate, kMinimumParkedCameraRate, kMaximumCameraRate) : 0;
  // Profiles saved before kParkedRateRevision hold the previous shipped floor,
  // which could not lower the parked render share below about 20 fps. Load it
  // as the new default until the profile is saved with the revision; any other
  // saved value, and any value saved afterwards, is the user's choice.
  if (integer(L"display", L"parked_rate_revision", 0) < kParkedRateRevision && value.parked_rate == kPreviousDefaultParkedCameraRate)
    value.parked_rate = kDefaultParkedCameraRate;
  value.single_camera = integer(L"display", L"single_camera", 0);
  value.dynamic_tail = integer(L"display", L"dynamic_tail", 0) ? 1u : 0u;
  value.calibration_budget = integer(L"display", L"calibration_budget", 4096);
  // New keys: the exposure and night_boost keys of 0.9.50 and earlier held
  // absolute values and are not read.
  value.day_brightness = snap_camera_brightness(read(L"display", L"day_brightness", 0));
  value.night_brightness = snap_camera_brightness(read(L"display", L"night_brightness", 0));
  constexpr const wchar_t* color_keys[]{L"speed_red", L"speed_green", L"speed_blue"};
  for (unsigned c = 0; c < 3; ++c)
    value.speed_color[c] = static_cast<float>(read(L"display", color_keys[c], value.speed_color[c]));
  constexpr const wchar_t* guide_color_keys[]{L"guide_red", L"guide_green", L"guide_blue"};
  for (unsigned c = 0; c < 3; ++c)
    value.guide_color[c] = static_cast<float>(read(L"guides", guide_color_keys[c], value.guide_color[c]));
  auto guide = [&](std::array<float, 2>& position, const wchar_t* x_key, const wchar_t* y_key) {
    position[0] = static_cast<float>(read(L"guides", x_key, position[0]));
    position[1] = static_cast<float>(read(L"guides", y_key, position[1]));
  };
  guide(value.nose_dot, L"nose_dot_x", L"nose_dot_y");
  guide(value.tail_upper, L"tail_upper_x", L"tail_upper_y");
  guide(value.tail_corner, L"tail_corner_x", L"tail_corner_y");
  guide(value.tail_inner, L"tail_inner_x", L"tail_inner_y");
  constexpr std::array<const wchar_t*, 6> names{L"right", L"up", L"forward", L"pitch", L"yaw", L"lens"};
  constexpr std::array<const wchar_t*, 3> sections{L"nose", L"tail", L"wing_right"};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 6; ++j)
      value.mounts[i][j] = read(sections[i], names[j], value.mounts[i][j]);
  // Pre-protocol-13 files only stored nose/tail. Keep the profile's third mount
  // when wing_right keys are absent so split-bottom defaults remain valid.
  {
    wchar_t probe[8];
    GetPrivateProfileStringW(L"wing_right", L"right", L"", probe, 8, path.c_str());
    if (probe[0] == 0)
      value.mounts[2] = profile->mounts[2];
  }
  if (!valid_settings(value))
    return false;
  s = value;
  return true;
}
inline bool save_settings(const Settings& s) {
  if (!valid_settings(s))
    return false;
  const auto path = settings_path(s), temporary = path + L".tmp";
  wchar_t text[4096];
  const auto& n = s.mounts[0];
  const auto& t = s.mounts[1];
  const auto& w = s.mounts[2];
  const int count =
      std::swprintf(text, 4096,
                    L"[service]\r\nfollow_taxi=%u\r\nauto_detect=%u\r\ncam_button_revision=%u\r\n"
                    L"[display]\r\ncamera_rate=%u\r\nparked_rate=%u\r\nparked_rate_revision=%u\r\nsingle_camera=%u\r\ndynamic_tail=%u\r\n"
                    L"calibration_budget=%u\r\nday_brightness=%.9g\r\nnight_brightness=%.9g\r\nspeed_red=%.9g\r\nspeed_green=%.9g\r\n"
                    L"speed_blue=%.9g\r\n"
                    L"[guides]\r\nguide_red=%.9g\r\nguide_green=%.9g\r\nguide_blue=%.9g\r\n"
                    L"nose_dot_x=%.9g\r\nnose_dot_y=%.9g\r\ntail_upper_x=%.9g\r\ntail_upper_y=%.9g\r\n"
                    L"tail_corner_x=%.9g\r\ntail_corner_y=%.9g\r\ntail_inner_x=%.9g\r\ntail_inner_y=%.9g\r\n"
                    L"[nose]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n"
                    L"[tail]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n"
                    L"[wing_right]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n",
                    s.follow_taxi, s.auto_detect, kCamButtonRevision, s.camera_rate, s.parked_rate, kParkedRateRevision, s.single_camera,
                    s.dynamic_tail, s.calibration_budget, s.day_brightness, s.night_brightness, s.speed_color[0], s.speed_color[1],
                    s.speed_color[2], s.guide_color[0], s.guide_color[1], s.guide_color[2], s.nose_dot[0], s.nose_dot[1], s.tail_upper[0],
                    s.tail_upper[1], s.tail_corner[0], s.tail_corner[1], s.tail_inner[0], s.tail_inner[1], n[0], n[1], n[2], n[3], n[4],
                    n[5], t[0], t[1], t[2], t[3], t[4], t[5], w[0], w[1], w[2], w[3], w[4], w[5]);
  if (count <= 0)
    return false;
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  const wchar_t bom = 0xfeff;
  DWORD wrote{};
  bool ok = WriteFile(file, &bom, sizeof(bom), &wrote, nullptr) && wrote == sizeof(bom);
  const DWORD bytes = static_cast<DWORD>(count * sizeof(wchar_t));
  ok = ok && WriteFile(file, text, bytes, &wrote, nullptr) && wrote == bytes && FlushFileBuffers(file);
  CloseHandle(file);
  if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return false;
  wchar_t profile_text[16];
  std::swprintf(profile_text, 16, L"%u", s.profile);
  const auto selection = settings_directory() + L"\\settings.ini";
  const bool saved = WritePrivateProfileStringW(L"aircraft", L"profile", profile_text, selection.c_str()) &&
                     WritePrivateProfileStringW(L"aircraft", L"automatic", s.auto_profile ? L"1" : L"0", selection.c_str()) &&
                     WritePrivateProfileStringW(L"messages", L"notifications", s.notifications ? L"1" : L"0", selection.c_str());
  if (saved)
    WritePrivateProfileStringW(L"messages", L"in_simulator", nullptr, selection.c_str());  // Legacy protocol 10 key.
  return saved;
}
}  // namespace taxi_camera::standalone
