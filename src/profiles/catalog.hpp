#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace taxi_camera::profiles {
struct DisplayRect {
  unsigned left, top, right, bottom;
};
struct DisplayInsets {
  unsigned left, top, right, bottom;
};
using CameraPanes = std::array<std::array<std::int32_t, 2>, 2>;
struct Composition {
  float nose_height = 255, tail_top = 259, divider_top = 251, divider_bottom = 263;
  std::array<float, 2> nose_dot{0.14f, 0.48f};
  std::array<float, 2> tail_corner{0.305f, 0.75f}, tail_upper{0.33f, 0.625f}, tail_inner{0.365f, 0.758f};
  std::array<float, 3> guide_color{1, 0, 1};
  std::array<float, 3> speed_color{0, 1, 0};
  // Working-image pixels; padding surrounds the GS label and current value.
  std::array<float, 2> speed_panel_origin{16, 12}, speed_panel_padding{8, 8};
  std::array<float, 2> speed_panel_min_size{0, 0};
  // Float 0/1 is passed directly in the compositor's GPU constants.
  float square_nose_markers = 1;
};
enum class TaxiControl { push_event, lvar_off };
inline constexpr Composition AmberEtacs = [] {
  Composition c;
  c.square_nose_markers = 0;
  c.guide_color = {1, 0.55f, 0};
  c.speed_color = {0, 0.94f, 0.28f};
  // Shared A350 image-space guide defaults; live adjustments remain per profile.
  c.tail_upper = {0.29f, 0.76f};
  c.tail_corner = {0.26f, 0.87f};
  c.tail_inner = {0.31f, 0.87f};
  return c;
}();
inline constexpr Composition AmberEtacsA35K = AmberEtacs;
struct AircraftProfile {
  std::uint32_t id;
  std::string_view key;
  const wchar_t* name;
  std::array<const char*, 2> taxi_lvars;
  std::array<const char*, 2> taxi_events;
  std::array<const char*, 2> pfd_labels;
  // right/up/forward metres, pitch/yaw degrees, lens radians.
  std::array<std::array<double, 6>, 2> mounts;
  unsigned width, height, mips;
  TaxiControl taxi_control = TaxiControl::push_event;
  std::array<std::string_view, 3> aircraft_types{"A388"};
  std::array<DisplayRect, 2> display_regions{{{0, 0, 768, 763}, {0, 0, 768, 763}}};
  CameraPanes camera_panes{{{736, 251}, {736, 496}}};
  bool higher_id_left = true;
  double speed_cutoff_knots = 60;
  Composition composition{};
  std::array<unsigned, 6> formats{28};
  std::array<std::string_view, 3> package_markers{"flybywire-aircraft-a380-842", "flybywire_a380_842"};
  // Target pixels: the outer display region is black around this camera inset.
  DisplayInsets camera_padding{16, 12, 16, 0};
};
inline constexpr AircraftProfile A380{1,
                                      "fbw-a380x",
                                      L"FlyByWire A380X",
                                      {"L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON", "L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON"},
                                      {"A32NX.FCU_EFIS_L_TAXI_PUSH", "A32NX.FCU_EFIS_R_TAXI_PUSH"},
                                      {"SCREEN_DU_PFDL", "SCREEN_DU_PFDR"},
                                      {{{0, -1.75, 26.950668984, -17.5, 0, 1.24}, {0, 18, -25, -32, 0, 1.02}}},
                                      768,
                                      1024,
                                      5};
// Display dimensions and Lvars: iniBuilds A350 1.2.6 panel/behaviour XML.
// The accepted -900 calibration transfers to the -1000 with its physical
// longitudinal offsets, retaining the same height, pitch and lens. The -1000
// and its guide alignment still need a live check.
// The compositor retains its bounded 768px working image.
// Preserve a 32px central gap around the PFD/MFDDivider artwork on both sides.
inline constexpr AircraftProfile A359{2,
                                      "ini-a350-900",
                                      L"iniBuilds A350-900 / ULR",
                                      {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                                      {"", ""},
                                      {"$EFIS_LEFT", "$EFIS_RIGHT"},
                                      {{{0, -2, 16, -15, 0, 0.55}, {0, 10, -33.0, -15, 0, 0.62}}},
                                      1644,
                                      1024,
                                      0,
                                      TaxiControl::lvar_off,
                                      {"A359", "A359 ULR"},
                                      {{{0, 0, 806, 763}, {838, 0, 1644, 763}}},
                                      {{{774, 251}, {774, 496}}},
                                      true,
                                      60,
                                      AmberEtacs,
                                      {28, 29, 87, 91, 27, 90},
                                      {"inibuilds-aircraft-a350", "presets/inibuilds", "attachments/inibuilds"}};
inline constexpr AircraftProfile A35K{3,
                                      "ini-a350-1000",
                                      L"iniBuilds A350-1000",
                                      {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                                      {"", ""},
                                      {"$EFIS_LEFT", "$EFIS_RIGHT"},
                                      {{{0, -2, 19.81, -15, 0, 0.55}, {0, 10, -36.17, -15, 0, 0.62}}},
                                      1644,
                                      1024,
                                      0,
                                      TaxiControl::lvar_off,
                                      {"A35K"},
                                      {{{0, 0, 806, 763}, {838, 0, 1644, 763}}},
                                      {{{774, 251}, {774, 496}}},
                                      true,
                                      60,
                                      AmberEtacsA35K,
                                      {28, 29, 87, 91, 27, 90},
                                      {"inibuilds-aircraft-a350", "presets/inibuilds", "attachments/inibuilds"}};
inline constexpr std::array<const AircraftProfile*, 3> Catalog{&A380, &A359, &A35K};
inline constexpr DisplayRect display_rect(const AircraftProfile& p, unsigned side) noexcept {
  return p.display_regions[side < 2 ? side : 0];
}
inline constexpr DisplayRect display_content_rect(const AircraftProfile& p, unsigned side) noexcept {
  const auto outer = display_rect(p, side);
  if (outer.right <= outer.left || outer.bottom <= outer.top)
    return {};
  const auto width = outer.right - outer.left, height = outer.bottom - outer.top;
  const auto padding = p.camera_padding;
  if (padding.left >= width || padding.right >= width - padding.left || padding.top >= height || padding.bottom >= height - padding.top)
    return {};
  return {outer.left + padding.left, outer.top + padding.top, outer.right - padding.right, outer.bottom - padding.bottom};
}
inline bool camera_candidate(unsigned width, unsigned height) noexcept {
  for (const auto* profile : Catalog)
    for (const auto& pane : profile->camera_panes)
      if (width == static_cast<unsigned>(pane[0]) && height == static_cast<unsigned>(pane[1]))
        return true;
  return false;
}
inline constexpr bool matches_display(const AircraftProfile& p, unsigned width, unsigned height, unsigned mips, unsigned format) noexcept {
  if (width != p.width || height != p.height || !mips || mips > 12 || (p.mips && p.mips != mips) || !format)
    return false;
  for (auto supported : p.formats)
    if (supported == format)
      return true;
  return false;
}
inline bool matches_aircraft(const AircraftProfile& p, std::string_view type) noexcept {
  const auto equal = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i) {
      const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? char(c - ('a' - 'A')) : c; };
      if (upper(a[i]) != upper(b[i]))
        return false;
    }
    return true;
  };
  for (auto supported : p.aircraft_types)
    if (!supported.empty() && equal(type, supported))
      return true;
  return false;
}
inline const AircraftProfile* find(std::uint32_t id) noexcept {
  for (auto* p : Catalog)
    if (p->id == id)
      return p;
  return nullptr;
}
// Match a complete path component (or component sequence), independent of drive,
// slash direction and case. Aircraft type alone does not identify its vendor.
inline bool path_contains(std::string_view path, std::string_view marker) noexcept {
  const auto fold = [](char c) { return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
  for (size_t i = 0; i + marker.size() <= path.size(); ++i) {
    if (i && fold(path[i - 1]) != '/')
      continue;
    size_t n = 0;
    while (n < marker.size() && fold(path[i + n]) == fold(marker[n]))
      ++n;
    if (n == marker.size() && (i + n == path.size() || fold(path[i + n]) == '/'))
      return true;
  }
  return false;
}
inline std::uint32_t detect_aircraft(std::string_view type, std::string_view path) noexcept {
  // ATC TYPE is an ATC brand, not an ICAO designator. The live FBW aircraft
  // returns "ATCCOM.ATC_NAME AIRBUS.0.text". Its public AircraftLoaded path
  // identifies the actual product, including the MSFS2024 modular/livery tree.
  // Exact components keep generic Airbus/A380 and similarly named packages out.
  if (path_contains(path, "flybywire-aircraft-a380-842") || path_contains(path, "simobjects/airplanes/flybywire_a380x") ||
      path_contains(path, "simobjects/airplanes/flybywire_a380_842"))
    return A380.id;
  std::uint32_t match = 0;
  for (const auto* p : Catalog) {
    if (!matches_aircraft(*p, type))
      continue;
    bool vendor = false;
    for (const auto marker : p->package_markers)
      vendor = vendor || (!marker.empty() && path_contains(path, marker));
    if (vendor) {
      if (match)
        return 0;
      match = p->id;
    }
  }
  return match;
}
// Legacy adapter default. Native sessions choose their explicit profile through
// the companion and pass it into control and display adapters.
inline const AircraftProfile& active() noexcept {
  return A380;
}
}  // namespace taxi_camera::profiles
