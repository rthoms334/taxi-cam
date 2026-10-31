#pragma once
#include <array>
#include <cstddef>
#include <string_view>

namespace taxi_camera::profiles {
// An aircraft no profile matches is identified by its AircraftLoaded path, the
// SimObject the simulator loaded, up to its aircraft.cfg. MSFS 2024 presets
// keep that file in presets/<vendor>/<preset>/config, so each preset is its
// own variant: the Fenix A320 CFM and A321 IAE share SimObjects/Airplanes/
// FNX_32X but report .../presets/fnx/FNX_320_CFM_SL/config/aircraft.CFG and
// .../presets/fnx/FNX_321_IAE_WF_SC/config/aircraft.CFG.
inline constexpr std::size_t MaxVariantKey = 200, MaxVariantComponent = 64;
using VariantKey = std::array<char, MaxVariantKey>;
namespace variant_detail {
inline char fold(char c) noexcept {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
inline bool same_text(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (fold(a[i]) != fold(b[i]))
      return false;
  return true;
}
// A component that names a folder in the settings tree as reported: printable
// ASCII without Windows path characters, never "." or "..", no trailing dot or
// space.
inline bool file_component(std::string_view c) noexcept {
  if (c.empty() || c.size() > MaxVariantComponent || c == "." || c == ".." || c.back() == '.' || c.back() == ' ')
    return false;
  for (const char ch : c)
    if (ch < 32 || ch > 126 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos)
      return false;
  return true;
}
}  // namespace variant_detail
// SimObjects/<category>/<folder>[/...] with the trailing aircraft.cfg and a
// trailing config folder removed, in the path's own case and '/' separators.
// False when the path has no such part or a component cannot name a folder.
inline bool variant_key(std::string_view path, VariantKey& out) noexcept {
  out = {};
  std::array<std::string_view, 16> parts{};
  std::size_t count = 0;
  for (std::size_t begin = 0; begin <= path.size();) {
    auto end = path.find_first_of("/\\", begin);
    if (end == std::string_view::npos)
      end = path.size();
    if (end > begin) {
      if (count == parts.size())
        return false;
      parts[count++] = path.substr(begin, end - begin);
    }
    begin = end + 1;
  }
  std::size_t first = 0;
  while (first < count && !variant_detail::same_text(parts[first], "SimObjects"))
    ++first;
  if (count && variant_detail::same_text(parts[count - 1], "aircraft.cfg"))
    --count;
  if (count && variant_detail::same_text(parts[count - 1], "config"))
    --count;
  if (first + 3 > count)
    return false;
  std::size_t used = 0;
  for (std::size_t i = first; i < count; ++i) {
    if (!variant_detail::file_component(parts[i]) || used + parts[i].size() + 1 >= MaxVariantKey)
      return false;
    if (i != first)
      out[used++] = '/';
    parts[i].copy(out.data() + used, parts[i].size());
    used += parts[i].size();
  }
  return true;
}
// Paths and keys compare without case, as Windows names the same folder.
inline bool same_variant(std::string_view path, std::string_view key) noexcept {
  VariantKey own{};
  return !key.empty() && variant_key(path, own) && variant_detail::same_text(own.data(), key);
}
// SimObjects/<category>/<folder>: the presets of one airplane folder share a
// cockpit, so a new preset can start from a sibling's display layout.
inline std::string_view variant_family(std::string_view key) noexcept {
  std::size_t slashes = 0;
  for (std::size_t i = 0; i < key.size(); ++i)
    if (key[i] == '/' && ++slashes == 3)
      return key.substr(0, i);
  return key;
}
// The last component: the preset (or airplane folder) name shown to the user.
inline std::string_view variant_name(std::string_view key) noexcept {
  const auto slash = key.find_last_of('/');
  return slash == std::string_view::npos ? key : key.substr(slash + 1);
}
}  // namespace taxi_camera::profiles
