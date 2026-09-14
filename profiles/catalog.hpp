#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace taxi_camera::profiles {
struct DisplayRect {
  unsigned left, top, right, bottom;
};
using CameraPanes = std::array<std::array<std::int32_t, 2>, 2>;
struct Composition {
  float nose_height = 255, tail_top = 259, divider_top = 245, divider_bottom = 269;
  std::array<float, 2> nose_dot{0.14f, 0.48f};
  std::array<float, 2> tail_corner{0.305f, 0.75f}, tail_upper{0.33f, 0.625f}, tail_inner{0.365f, 0.758f};
  std::array<float, 3> guide_color{1, 0, 1};
};
enum class TaxiControl { push_event, lvar_off };
inline constexpr Composition AmberEtacs = [] {
  Composition c;
  c.guide_color = {1, 0.55f, 0};
  return c;
}();
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
  CameraPanes camera_panes{{{768, 255}, {768, 504}}};
  bool higher_id_left = true;
  double speed_cutoff_knots = 60;
  Composition composition{};
  std::array<unsigned, 6> formats{28};
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
// Mount seeds follow the exterior camera mesh positions; lens/pitch remain
// adjustable per variant. The compositor retains its bounded 768px working image.
inline constexpr AircraftProfile A359{2,
                                      "ini-a350-900",
                                      L"iniBuilds A350-900 / ULR",
                                      {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                                      {"", ""},
                                      {"$EFIS_LEFT", "$EFIS_RIGHT"},
                                      {{{0, -1.98, 13.55, -12, 0, 1.24}, {0, 10.85, -33.0, -22, 0, 1.02}}},
                                      1644,
                                      1024,
                                      0,
                                      TaxiControl::lvar_off,
                                      {"A359", "A359 ULR"},
                                      {{{0, 0, 822, 763}, {822, 0, 1644, 763}}},
                                      {{{822, 255}, {822, 504}}},
                                      true,
                                      60,
                                      AmberEtacs,
                                      {28, 29, 87, 91, 27, 90}};
inline constexpr AircraftProfile A35K{3,
                                      "ini-a350-1000",
                                      L"iniBuilds A350-1000",
                                      {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                                      {"", ""},
                                      {"$EFIS_LEFT", "$EFIS_RIGHT"},
                                      {{{0, -1.98, 17.36, -12, 0, 1.24}, {0, 10.85, -36.17, -22, 0, 1.02}}},
                                      1644,
                                      1024,
                                      0,
                                      TaxiControl::lvar_off,
                                      {"A35K"},
                                      {{{0, 0, 822, 763}, {822, 0, 1644, 763}}},
                                      {{{822, 255}, {822, 504}}},
                                      true,
                                      60,
                                      AmberEtacs,
                                      {28, 29, 87, 91, 27, 90}};
inline constexpr std::array<const AircraftProfile*, 3> Catalog{&A380, &A359, &A35K};
inline constexpr DisplayRect display_rect(const AircraftProfile& p, unsigned side) noexcept {
  return p.display_regions[side < 2 ? side : 0];
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
  for (auto supported : p.aircraft_types)
    if (!supported.empty() && type == supported)
      return true;
  return false;
}
inline const AircraftProfile* find(std::uint32_t id) noexcept {
  for (auto* p : Catalog)
    if (p->id == id)
      return p;
  return nullptr;
}
// Legacy adapter default. Native sessions choose their explicit profile through
// the companion and pass it into control and display adapters.
inline const AircraftProfile& active() noexcept {
  return A380;
}
}  // namespace taxi_camera::profiles
