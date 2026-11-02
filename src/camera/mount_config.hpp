#pragma once

#include "aircraft_mounts.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

namespace taxi_camera::native_camera {
inline constexpr std::size_t kMountConfigMaximumBytes = 4096;

// ASCII (optional UTF-8 BOM), two required lines, in either order:
// nose = right,up,forward,pitch,yaw,fov
// tail = right,up,forward,pitch,yaw,fov
// Units: metres, degrees, radians. Blank lines and whole-line # comments allowed.
// Both complete mounts must pass existing limits. Failure leaves output intact.
inline bool parse_mount_config(std::string_view text, MountPair& output, const char*& error) noexcept {
  error = "invalid_config";
  if (text.size() > kMountConfigMaximumBytes || text.find('\0') != std::string_view::npos)
    return false;
  if (text.starts_with("\xef\xbb\xbf"))
    text.remove_prefix(3);
  const auto trim = [](std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r");
    return begin == std::string_view::npos ? std::string_view{} : value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
  };
  MountPair candidate{};
  unsigned seen = 0;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (line.empty() || line.front() == '#')
      continue;
    const auto equals = line.find('=');
    if (equals == std::string_view::npos)
      return false;
    const auto key = trim(line.substr(0, equals));
    const unsigned index = key == "nose" ? 0 : key == "tail" ? 1 : 2;
    if (index > 1 || (seen & (1u << index)))
      return false;
    line = trim(line.substr(equals + 1));
    std::array<double, 6> values{};
    for (unsigned i = 0; i < values.size(); ++i) {
      const auto comma = line.find(',');
      const auto token = trim(line.substr(0, comma));
      if (token.empty() || (i < 5 && comma == std::string_view::npos) || (i == 5 && comma != std::string_view::npos))
        return false;
      const auto parsed = std::from_chars(token.data(), token.data() + token.size(), values[i], std::chars_format::general);
      if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(values[i]))
        return false;
      line = comma == std::string_view::npos ? std::string_view{} : trim(line.substr(comma + 1));
    }
    // Bound before narrowing, then let valid_mounts apply the public float
    // limits exactly as it does for UI input (including decimal 0.05/1.55).
    if (values[5] < 0 || values[5] > 2)
      return false;
    candidate[index] = {{values[0], values[1], values[2]}, values[3], values[4], static_cast<float>(values[5])};
    seen |= 1u << index;
  }
  if (seen != 3 || !valid_mounts(candidate, 2))
    return false;
  // Legacy nose/tail files only carry two mounts. The third view is the door
  // camera on these aircraft, which keeps its default mount.
  const auto& door = profiles::DoorMount;
  candidate[2] = {{door[0], door[1], door[2]}, door[3], door[4], static_cast<float>(door[5])};
  output = candidate;
  error = "";
  return true;
}

struct MountConfigStatus {
  bool running = false;
  std::uint64_t checks = 0;
  std::uint64_t applications = 0;
  const char* state = "not_initialized";
};

// Reads taxi-camera-mounts.cfg next to the supplied absolute local DLL path.
// Call outside DllMain/private engine callbacks. One worker performs at most
// one bounded file read per second; only changed contents reach the mailbox.
// Missing/invalid files retain current settings. Unchanged files preserve UI edits.
bool initialize_mount_config(const wchar_t* addon_path) noexcept;
// Signals the worker immediately and waits at most 2 seconds. False retains the
// pinned worker/context for later retirement; never unload this module in flight.
bool shutdown_mount_config() noexcept;
// No filesystem access; safe for Present/overlay status display.
MountConfigStatus mount_config_status() noexcept;
}  // namespace taxi_camera::native_camera
