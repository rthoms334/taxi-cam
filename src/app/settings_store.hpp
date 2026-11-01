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
// An aircraft the user set up is saved per variant under profiles\discovered,
// in folders named after its SimObject path: Airplanes\FNX_32X\presets\fnx\
// FNX_320_CFM_SL\variant.ini. layout.ini in the airplane folder keeps the
// latest display layout of any of its presets, which share one cockpit.
// Empty for a key that is not a variant key. Creates nothing.
inline std::wstring discovered_folder(std::string_view key, bool family = false) {
  profiles::VariantKey own{};
  if (!profiles::variant_key(key, own) || std::string_view(own.data()) != key)
    return {};
  if (family)
    key = profiles::variant_family(key);
  auto folder = settings_directory();
  if (folder.empty())
    return {};
  folder += L"\\profiles\\discovered";
  // Skip the leading SimObjects component; the rest are checked folder names.
  for (std::size_t begin = key.find('/') + 1; begin < key.size();) {
    auto end = key.find('/', begin);
    if (end == std::string_view::npos)
      end = key.size();
    folder += L'\\';
    for (const char c : key.substr(begin, end - begin))
      folder += static_cast<wchar_t>(c);
    begin = end + 1;
  }
  return folder;
}
inline std::wstring discovered_layout_path(std::string_view key) {
  const auto folder = discovered_folder(key, true);
  return folder.empty() ? folder : folder + L"\\layout.ini";
}
inline std::wstring settings_path(const Settings& s) {
  if (s.profile == profiles::DiscoveredProfileId) {
    const auto folder = discovered_folder(s.discovered.key);
    return folder.empty() ? folder : folder + L"\\variant.ini";
  }
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
// Creates the folders of a settings file below the settings directory, and
// refuses a path through a junction or symbolic link.
inline bool prepare_settings_file(const std::wstring& file) {
  const auto root = settings_directory();
  if (root.empty() || file.size() <= root.size() + 1 || file.compare(0, root.size(), root) != 0 || file[root.size()] != L'\\')
    return false;
  for (auto end = file.find(L'\\', root.size() + 1); end != std::wstring::npos; end = file.find(L'\\', end + 1))
    CreateDirectoryW(file.substr(0, end).c_str(), nullptr);
  return settings_path_without_links(file);
}
// [layout] of a set-up aircraft, in its variant.ini and its airplane folder's
// layout.ini. The variant key is the file's location, not part of the text.
inline std::wstring discovered_layout_text(const profiles::DiscoveredLayout& l) {
  const auto ascii = [](const char* text) { return std::wstring(text, text + std::strlen(text)); };
  wchar_t line[160];
  std::swprintf(line, 160, L"[layout]\r\nsides=%u\r\npadding=%u,%u,%u,%u\r\n", l.sides, l.padding.left, l.padding.top, l.padding.right,
                l.padding.bottom);
  std::wstring text = line;
  for (unsigned side = 0; side < l.sides && side < MaxDisplaySides; ++side) {
    const auto& shape = l.shapes[side];
    std::swprintf(line, 160, L"shape%u=%u,%u,%u\r\n", side, shape.width, shape.height, shape.mips);
    text += L"texture" + std::to_wstring(side) + L"=" + ascii(l.textures[side]) + L"\r\n" + line;
  }
  for (unsigned i = 0; i < profiles::MaxPanelAnchors && l.anchors[i].name[0]; ++i) {
    std::swprintf(line, 160, L"anchor%u_size=%u,%u\r\n", i, l.anchors[i].width, l.anchors[i].height);
    text += L"anchor" + std::to_wstring(i) + L"=" + ascii(l.anchors[i].name) + L"\r\n" + line;
  }
  return text;
}
inline bool read_discovered_layout(const std::wstring& path, std::string_view key, profiles::DiscoveredLayout& out) {
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES || !settings_path_without_links(path))
    return false;
  profiles::DiscoveredLayout l{};
  if (key.size() >= sizeof(l.key))
    return false;
  key.copy(l.key, key.size());
  const auto value = [&](const std::wstring& name, wchar_t(&text)[64]) {
    GetPrivateProfileStringW(L"layout", name.c_str(), L"", text, 64, path.c_str());
  };
  const auto name = [&](const std::wstring& key_name, char (&out_name)[profiles::MaxPanelName]) {
    wchar_t text[64]{};
    value(key_name, text);
    const auto n = std::wcslen(text);
    if (n >= profiles::MaxPanelName)
      return false;
    for (std::size_t i = 0; i < n; ++i) {
      if (text[i] < 32 || text[i] > 126)
        return false;
      out_name[i] = static_cast<char>(text[i]);
    }
    return true;
  };
  const auto numbers = [&](const std::wstring& key_name, unsigned count, std::uint32_t* out_numbers) {
    wchar_t text[64]{};
    value(key_name, text);
    unsigned v[4]{};
    wchar_t extra{};
    const int read = count == 4   ? std::swscanf(text, L"%u,%u,%u,%u%lc", &v[0], &v[1], &v[2], &v[3], &extra)
                     : count == 3 ? std::swscanf(text, L"%u,%u,%u%lc", &v[0], &v[1], &v[2], &extra)
                     : count == 2 ? std::swscanf(text, L"%u,%u%lc", &v[0], &v[1], &extra)
                                  : std::swscanf(text, L"%u%lc", &v[0], &extra);
    if (read != static_cast<int>(count))
      return false;
    std::copy_n(v, count, out_numbers);
    return true;
  };
  std::uint32_t padding[4]{};
  bool ok = numbers(L"sides", 1, &l.sides) && numbers(L"padding", 4, padding);
  l.padding = {padding[0], padding[1], padding[2], padding[3]};
  for (unsigned side = 0; ok && side < l.sides && side < MaxDisplaySides; ++side) {
    std::uint32_t shape[3]{};
    ok = name(L"texture" + std::to_wstring(side), l.textures[side]) && numbers(L"shape" + std::to_wstring(side), 3, shape);
    l.shapes[side] = {shape[0], shape[1], shape[2]};
  }
  for (unsigned i = 0; ok && i < profiles::MaxPanelAnchors; ++i) {
    if (!name(L"anchor" + std::to_wstring(i), l.anchors[i].name))
      return false;
    if (!l.anchors[i].name[0])
      break;
    std::uint32_t size[2]{};
    ok = numbers(L"anchor" + std::to_wstring(i) + L"_size", 2, size);
    l.anchors[i].width = size[0];
    l.anchors[i].height = size[1];
  }
  if (!ok || !profiles::valid_layout(l))
    return false;
  out = l;
  return true;
}
// The variant's own layout, else the latest one saved for its airplane folder.
inline bool load_discovered_layout(std::string_view key, profiles::DiscoveredLayout& out) {
  const auto folder = discovered_folder(key);
  return !folder.empty() &&
         (read_discovered_layout(folder + L"\\variant.ini", key, out) || read_discovered_layout(discovered_layout_path(key), key, out));
}
// Every set-up aircraft saved in profiles\discovered, by variant key, sorted.
// Junctions and symbolic links are not followed.
inline std::vector<std::string> saved_discovered_variants() {
  std::vector<std::string> keys;
  const auto root = settings_directory();
  if (root.empty())
    return keys;
  std::vector<std::pair<std::wstring, std::string>> folders{{root + L"\\profiles\\discovered", "SimObjects"}};
  for (std::size_t visited = 0; !folders.empty() && visited < 4096; ++visited) {
    const auto [folder, key] = folders.back();
    folders.pop_back();
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
      continue;
    do {
      const std::wstring name = data.cFileName;
      if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        continue;
      if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        std::string component;
        for (const wchar_t c : name)
          component += c < 127 ? static_cast<char>(c) : '?';
        folders.push_back({folder + L"\\" + name, key + "/" + component});
      } else if (_wcsicmp(name.c_str(), L"variant.ini") == 0) {
        profiles::VariantKey own{};
        if (profiles::variant_key(key, own) && key == own.data())
          keys.push_back(key);
      }
    } while (FindNextFileW(find, &data));
    FindClose(find);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}
// Deletes a set-up aircraft's settings: its variant.ini, the folders left
// empty, and its airplane folder's layout.ini with the folder's last variant.
inline bool remove_discovered_variant(std::string_view key) {
  const auto folder = discovered_folder(key), family = discovered_folder(key, true);
  const auto root = settings_directory() + L"\\profiles\\discovered";
  const auto file = folder + L"\\variant.ini";
  if (folder.empty() || family.empty() || !settings_path_without_links(file) || !DeleteFileW(file.c_str()))
    return false;
  const auto family_key = profiles::variant_family(key);
  const auto others = saved_discovered_variants();
  if (std::none_of(others.begin(), others.end(), [&](const std::string& other) { return profiles::variant_family(other) == family_key; }))
    DeleteFileW((family + L"\\layout.ini").c_str());
  for (auto dir = folder; dir.size() > root.size() && RemoveDirectoryW(dir.c_str());)
    dir.resize(dir.find_last_of(L'\\'));
  return true;
}
inline bool load_settings(Settings& s,
                          const std::wstring& installation,
                          std::uint32_t profile_id,
                          std::string_view variant,
                          const profiles::DiscoveredLayout* layout);
inline bool save_settings(const Settings& s);
// Saves a set-up aircraft's displays, cameras and guides as another variant's
// (a preset of the same aircraft starts from one already set up). `out` is
// the copy, saved and ready to select.
inline bool copy_discovered_variant(std::string_view source, std::string_view target, const std::wstring& installation, Settings& out) {
  profiles::VariantKey own{};
  if (!profiles::variant_key(target, own) || std::string_view(own.data()) != target || target.size() >= profiles::MaxVariantKey)
    return false;
  Settings copy;
  if (!load_settings(copy, installation, profiles::DiscoveredProfileId, source, nullptr))
    return false;
  std::fill(std::begin(copy.discovered.key), std::end(copy.discovered.key), '\0');
  target.copy(copy.discovered.key, target.size());
  if (!profiles::valid_layout(copy.discovered) || !save_settings(copy))
    return false;
  out = copy;
  return true;
}
inline constexpr unsigned kCamButtonRevision = 1;
// profile_id 0 loads the saved selection, falling back to the A380 profile.
// An explicit set-up aircraft (DiscoveredProfileId) needs its variant key and
// a saved layout for it, or a new layout (setup) that replaces the saved one
// while the variant keeps its saved calibration.
inline bool load_settings(Settings& s,
                          const std::wstring& installation,
                          std::uint32_t profile_id = 0,
                          std::string_view variant = {},
                          const profiles::DiscoveredLayout* layout = nullptr) {
  const auto selection = settings_directory() + L"\\settings.ini";
  const bool saved_selection = !profile_id;
  char saved_variant[profiles::MaxVariantKey]{};
  if (saved_selection) {
    profile_id = GetPrivateProfileIntW(L"aircraft", L"profile", 1, selection.c_str());
    wchar_t text[profiles::MaxVariantKey]{};
    GetPrivateProfileStringW(L"aircraft", L"variant", L"", text, profiles::MaxVariantKey, selection.c_str());
    for (std::size_t i = 0; text[i] && text[i] < 127; ++i)
      saved_variant[i] = static_cast<char>(text[i]);
    variant = saved_variant;
  }
  profiles::DiscoveredProfile discovered{};
  const profiles::AircraftProfile* profile = nullptr;
  if (profile_id == profiles::DiscoveredProfileId) {
    if (layout && profiles::valid_layout(*layout))
      discovered.layout = *layout;
    if (layout ? discovered.layout.sides != 0 : load_discovered_layout(variant, discovered.layout)) {
      profiles::build_discovered(discovered);
      profile = &discovered.profile;
    } else if (!saved_selection) {
      return false;
    }
  } else {
    profile = profiles::find(profile_id);
  }
  if (!profile)
    profile = &profiles::A380;
  Settings value;
  value.profile = profile->id;
  if (profile->id == profiles::DiscoveredProfileId)
    value.discovered = discovered.layout;
  value.follow_taxi = profile->taxi_control == profiles::TaxiControl::manual_only ? 0u : 1u;
  value.mounts = profile->mounts;
  value.speed_color = profile->composition.speed_color;
  reset_guide_settings(value, *profile);
  value.auto_profile = GetPrivateProfileIntW(L"aircraft", L"automatic", 1, selection.c_str());
  value.notifications = load_notification_preference(selection);
  const auto destination = settings_path(value);
  auto path = destination;
  // Set-up aircraft are newer than the former settings folder.
  if (profile->id != profiles::DiscoveredProfileId && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const auto legacy = legacy_settings_path(path);
    if (!legacy.empty() && GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) {
      // Import without replacing a newer profile or deleting the original.
      // If migration fails, read the original so calibration is not reset.
      if (!CopyFileW(legacy.c_str(), path.c_str(), TRUE) && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        path = legacy;
    }
  }
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
      (profile->id == profiles::DiscoveredProfileId && !settings_path_without_links(path))) {
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
  // Profiles saved before camera modes hold only camera_rate: a lowered rate
  // meant a smaller budget (Performance), a raised one asked for smoother
  // cameras (Custom at that target), and the shipped 10 takes the default
  // mode. Later saved modes are kept. The parked_rate, parked_rate_revision,
  // dynamic_tail and single_camera keys of earlier versions are ignored and
  // dropped.
  const UINT camera_mode = integer(L"display", L"camera_mode", kCameraModeCount);
  if (camera_mode < kCameraModeCount)
    value.camera_mode = camera_mode;
  else if (value.camera_rate < kDefaultCameraRate)
    value.camera_mode = static_cast<UINT>(CameraMode::performance);
  else if (value.camera_rate > kDefaultCameraRate)
    value.camera_mode = static_cast<UINT>(CameraMode::custom);
  else
    value.camera_mode = static_cast<UINT>(kDefaultCameraMode);
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
  // [guides_right]: the right tail bracket; files without it mirror the left.
  const auto right = [&](std::array<float, 2>& position, const std::array<float, 2>& left, const wchar_t* x_key, const wchar_t* y_key) {
    position[0] = static_cast<float>(read(L"guides_right", x_key, left[0]));
    position[1] = static_cast<float>(read(L"guides_right", y_key, left[1]));
  };
  right(value.tail_upper_right, value.tail_upper, L"tail_upper_x", L"tail_upper_y");
  right(value.tail_corner_right, value.tail_corner, L"tail_corner_x", L"tail_corner_y");
  right(value.tail_inner_right, value.tail_inner, L"tail_inner_x", L"tail_inner_y");
  // [displays]: the user's panel and display area per side, if any. An
  // unreadable entry keeps the profile's own; the whole set must be valid.
  {
    profiles::DisplayOverride choices{};
    for (unsigned side = 0; side < MaxDisplaySides; ++side) {
      wchar_t text[64]{};
      GetPrivateProfileStringW(L"displays", (L"texture" + std::to_wstring(side)).c_str(), L"", text, 64, path.c_str());
      const auto n = std::wcslen(text);
      bool printable = n < profiles::MaxPanelName;
      for (std::size_t i = 0; printable && i < n; ++i)
        printable = text[i] >= 32 && text[i] <= 126;
      for (std::size_t i = 0; printable && i < n; ++i)
        choices.textures[side][i] = static_cast<char>(text[i]);
      GetPrivateProfileStringW(L"displays", (L"area" + std::to_wstring(side)).c_str(), L"", text, 64, path.c_str());
      unsigned r[4]{};
      wchar_t extra{};
      if (std::swscanf(text, L"%u,%u,%u,%u%lc", &r[0], &r[1], &r[2], &r[3], &extra) == 4)
        choices.areas[side] = {r[0], r[1], r[2], r[3]};
    }
    value.display_override = choices;
    if (!valid_settings(value))
      value.display_override = {};
  }
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
  const bool discovered = s.profile == profiles::DiscoveredProfileId;
  const auto path = settings_path(s);
  if (path.empty() || (discovered && !prepare_settings_file(path)))
    return false;
  wchar_t text[4096];
  const auto& n = s.mounts[0];
  const auto& t = s.mounts[1];
  const auto& w = s.mounts[2];
  const int count = std::swprintf(
      text, 4096,
      L"[service]\r\nfollow_taxi=%u\r\nauto_detect=%u\r\ncam_button_revision=%u\r\n"
      L"[display]\r\ncamera_mode=%u\r\ncamera_rate=%u\r\n"
      L"calibration_budget=%u\r\nday_brightness=%.9g\r\nnight_brightness=%.9g\r\nspeed_red=%.9g\r\nspeed_green=%.9g\r\n"
      L"speed_blue=%.9g\r\n"
      L"[guides]\r\nguide_red=%.9g\r\nguide_green=%.9g\r\nguide_blue=%.9g\r\n"
      L"nose_dot_x=%.9g\r\nnose_dot_y=%.9g\r\ntail_upper_x=%.9g\r\ntail_upper_y=%.9g\r\n"
      L"tail_corner_x=%.9g\r\ntail_corner_y=%.9g\r\ntail_inner_x=%.9g\r\ntail_inner_y=%.9g\r\n"
      L"[nose]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n"
      L"[tail]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n"
      L"[wing_right]\r\nright=%.12g\r\nup=%.12g\r\nforward=%.12g\r\npitch=%.12g\r\nyaw=%.12g\r\nlens=%.12g\r\n",
      s.follow_taxi, s.auto_detect, kCamButtonRevision, s.camera_mode, s.camera_rate, s.calibration_budget, s.day_brightness,
      s.night_brightness, s.speed_color[0], s.speed_color[1], s.speed_color[2], s.guide_color[0], s.guide_color[1], s.guide_color[2],
      s.nose_dot[0], s.nose_dot[1], s.tail_upper[0], s.tail_upper[1], s.tail_corner[0], s.tail_corner[1], s.tail_inner[0], s.tail_inner[1],
      n[0], n[1], n[2], n[3], n[4], n[5], t[0], t[1], t[2], t[3], t[4], t[5], w[0], w[1], w[2], w[3], w[4], w[5]);
  if (count <= 0)
    return false;
  const auto write = [](const std::wstring& destination, const std::wstring& body) {
    const auto staged = destination + L".tmp";
    HANDLE file = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return false;
    const wchar_t bom = 0xfeff;
    DWORD wrote{};
    bool ok = WriteFile(file, &bom, sizeof(bom), &wrote, nullptr) && wrote == sizeof(bom);
    const DWORD bytes = static_cast<DWORD>(body.size() * sizeof(wchar_t));
    ok = ok && WriteFile(file, body.data(), bytes, &wrote, nullptr) && wrote == bytes && FlushFileBuffers(file);
    CloseHandle(file);
    return ok && MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  };
  const auto layout = discovered ? discovered_layout_text(s.discovered) : std::wstring{};
  wchar_t right[384];
  const int right_count =
      std::swprintf(right, 384,
                    L"[guides_right]\r\ntail_upper_x=%.9g\r\ntail_upper_y=%.9g\r\ntail_corner_x=%.9g\r\ntail_corner_y=%.9g\r\n"
                    L"tail_inner_x=%.9g\r\ntail_inner_y=%.9g\r\n",
                    s.tail_upper_right[0], s.tail_upper_right[1], s.tail_corner_right[0], s.tail_corner_right[1], s.tail_inner_right[0],
                    s.tail_inner_right[1]);
  if (right_count <= 0)
    return false;
  std::wstring displays = L"[displays]\r\n";
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const auto& o = s.display_override;
    if (o.textures[side][0])
      displays += L"texture" + std::to_wstring(side) + L"=" +
                  std::wstring(o.textures[side], o.textures[side] + std::strlen(o.textures[side])) + L"\r\n";
    if (!profiles::empty_area(o.areas[side])) {
      wchar_t line[96];
      std::swprintf(line, 96, L"area%u=%u,%u,%u,%u\r\n", side, o.areas[side].left, o.areas[side].top, o.areas[side].right,
                    o.areas[side].bottom);
      displays += line;
    }
  }
  if (!write(path, std::wstring(text, static_cast<std::size_t>(count)) + std::wstring(right, static_cast<std::size_t>(right_count)) +
                       displays + layout))
    return false;
  // The airplane folder's latest layout lets its other presets start from it.
  const auto family = discovered ? discovered_layout_path(s.discovered.key) : std::wstring{};
  if (discovered && (family.empty() || !prepare_settings_file(family) || !write(family, layout)))
    return false;
  wchar_t profile_text[16];
  std::swprintf(profile_text, 16, L"%u", s.profile);
  const auto selection = settings_directory() + L"\\settings.ini";
  const std::wstring variant(s.discovered.key, s.discovered.key + std::strlen(s.discovered.key));
  const bool saved = WritePrivateProfileStringW(L"aircraft", L"profile", profile_text, selection.c_str()) &&
                     (!discovered || WritePrivateProfileStringW(L"aircraft", L"variant", variant.c_str(), selection.c_str())) &&
                     WritePrivateProfileStringW(L"aircraft", L"automatic", s.auto_profile ? L"1" : L"0", selection.c_str()) &&
                     WritePrivateProfileStringW(L"messages", L"notifications", s.notifications ? L"1" : L"0", selection.c_str());
  if (saved)
    WritePrivateProfileStringW(L"messages", L"in_simulator", nullptr, selection.c_str());  // Legacy protocol 10 key.
  return saved;
}
}  // namespace taxi_camera::standalone
