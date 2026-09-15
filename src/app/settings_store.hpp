#pragma once
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
inline bool load_settings(Settings& s, const std::wstring& installation, std::uint32_t profile_id = 0) {
  if (!profile_id)
    profile_id = GetPrivateProfileIntW(L"aircraft", L"profile", 1, (settings_directory() + L"\\settings.ini").c_str());
  const auto* profile = profiles::find(profile_id);
  if (!profile)
    profile = &profiles::A380;
  Settings value;
  value.profile = profile->id;
  value.mounts = profile->mounts;
  value.speed_color = profile->composition.speed_color;
  reset_guide_settings(value, *profile);
  value.auto_profile = GetPrivateProfileIntW(L"aircraft", L"automatic", 1, (settings_directory() + L"\\settings.ini").c_str());
  auto path = settings_path(value);
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
          for (unsigned i = 0; i < 2; ++i) {
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
  value.enabled = integer(L"service", L"enabled", 1);
  value.follow_taxi = integer(L"service", L"follow_taxi", 1);
  value.auto_detect = integer(L"service", L"auto_detect", 1);
  value.camera_rate = integer(L"display", L"camera_rate", 15);
  value.single_camera = integer(L"display", L"single_camera", 0);
  value.calibration_budget = integer(L"display", L"calibration_budget", 4096);
  value.automatic_exposure = integer(L"display", L"automatic_exposure", 1);
  constexpr const wchar_t* color_keys[]{L"speed_red", L"speed_green", L"speed_blue"};
  for (unsigned c = 0; c < 3; ++c)
    value.speed_color[c] = static_cast<float>(read(L"display", color_keys[c], value.speed_color[c]));
  auto guide = [&](std::array<float, 2>& position, const wchar_t* x_key, const wchar_t* y_key) {
    position[0] = static_cast<float>(read(L"guides", x_key, position[0]));
    position[1] = static_cast<float>(read(L"guides", y_key, position[1]));
  };
  guide(value.nose_dot, L"nose_dot_x", L"nose_dot_y");
  guide(value.tail_upper, L"tail_upper_x", L"tail_upper_y");
  guide(value.tail_corner, L"tail_corner_x", L"tail_corner_y");
  guide(value.tail_inner, L"tail_inner_x", L"tail_inner_y");
  value.exposure = static_cast<float>(read(L"display", L"exposure", -8.8));
  value.night_boost = static_cast<float>(read(L"display", L"night_boost", 4));
  constexpr std::array<const wchar_t*, 6> names{L"right", L"up", L"forward", L"pitch", L"yaw", L"lens"};
  for (unsigned i = 0; i < 2; ++i)
    for (unsigned j = 0; j < 6; ++j)
      value.mounts[i][j] = read(i ? L"tail" : L"nose", names[j], value.mounts[i][j]);
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
  const int count =
      std::swprintf(text, 4096,
                    L"[service]\r\nenabled=%u\r\nfollow_taxi=%u\r\nauto_detect=%u\r\n"
                    L"[display]\r\ncamera_rate=%u\r\nsingle_camera=%u\r\nautomatic_exposure=%u\r\nexposure=%.9g\r\nnight_boost=%."
                    L"9g\r\ncalibration_budget=%u\r\nspeed_red=%.9g\r\nspeed_green=%.9g\r\nspeed_blue=%.9g\r\n"
                    L"[guides]\r\nnose_dot_x=%.9g\r\nnose_dot_y=%.9g\r\ntail_upper_x=%.9g\r\ntail_upper_y=%.9g\r\n"
                    L"tail_corner_x=%.9g\r\ntail_corner_y=%.9g\r\ntail_inner_x=%.9g\r\ntail_inner_y=%.9g\r\n"
                    L"[nose]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n"
                    L"[tail]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n",
                    s.enabled, s.follow_taxi, s.auto_detect, s.camera_rate, s.single_camera, s.automatic_exposure, s.exposure,
                    s.night_boost, s.calibration_budget, s.speed_color[0], s.speed_color[1], s.speed_color[2], s.nose_dot[0], s.nose_dot[1],
                    s.tail_upper[0], s.tail_upper[1], s.tail_corner[0], s.tail_corner[1], s.tail_inner[0], s.tail_inner[1], n[0], n[1],
                    n[2], n[3], n[4], n[5], t[0], t[1], t[2], t[3], t[4], t[5]);
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
  return WritePrivateProfileStringW(L"aircraft", L"profile", profile_text, selection.c_str()) &&
         WritePrivateProfileStringW(L"aircraft", L"automatic", s.auto_profile ? L"1" : L"0", selection.c_str());
}
}  // namespace taxi_camera::standalone
