#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include "../shared/display_sides.hpp"
namespace taxi_camera::profiles {
struct DisplayRect {
  unsigned left, top, right, bottom;
};
struct DisplayInsets {
  unsigned left, top, right, bottom;
};
// Index 0 nose, 1 tail / bottom-left, 2 bottom-right (split_bottom profiles).
// Non-split profiles leave [2] equal to [1]; only the first two feeds are used.
using CameraPanes = std::array<std::array<std::int32_t, 2>, 3>;
struct Composition {
  float nose_height = 255, tail_top = 259, divider_top = 251, divider_bottom = 263;
  std::array<float, 2> nose_dot{0.14f, 0.48f};
  std::array<float, 2> tail_corner{0.305f, 0.75f}, tail_upper{0.33f, 0.64f}, tail_inner{0.365f, 0.758f};
  std::array<float, 3> guide_color{1, 0, 1};
  std::array<float, 3> speed_color{22.f / 255, 109.f / 255, 19.f / 255};
  // Working-image pixels; padding surrounds the GS label and current value.
  std::array<float, 2> speed_panel_origin{16, 12}, speed_panel_padding{8, 8};
  std::array<float, 2> speed_panel_min_size{0, 0};
  // Float 0/1 is passed directly in the compositor's GPU constants.
  float square_nose_markers = 1;
  // 0 keeps the full-width second feed. 1 places distinct left and right bottom
  // feeds with bottom_gap working-image pixels of black between them.
  float split_bottom = 0;
  float bottom_gap = 0;
  // Split-bottom only: pane height in working-image pixels (0 keeps square
  // panes) and the black frame width around the nose and bottom panes.
  float bottom_pane_height = 0;
  float frame_border = 10;
};
static_assert(sizeof(Composition) == 29 * sizeof(float));
// pmdg_dsp_cam reads the glareshield Display Select Panel. Nothing is written.
enum class TaxiControl { push_event, lvar_off, manual_only, pmdg_dsp_cam };
enum class PfdDetectionPolicy { dominant_activity, ini_a380_allocation_group, single_display };
inline constexpr Composition A350Etacs = [] {
  Composition c;
  // A350-900 image-space guide defaults; live adjustments remain per profile.
  c.tail_upper = {0.29f, 0.76f};
  c.tail_corner = {0.26f, 0.87f};
  c.tail_inner = {0.31f, 0.87f};
  return c;
}();
inline constexpr Composition A350EtacsA35K = [] {
  auto c = A350Etacs;
  c.tail_upper = {0.31f, 0.69f};
  c.tail_corner = {0.29f, 0.85f};
  c.tail_inner = {0.37f, 0.855f};
  return c;
}();
// How bright the aircraft's camera display shines, in the simulator's scene
// units for a full code (sRGB-decoded 1): emissive x the gain its behaviour
// maps from A:AMBIENT LIGHT SENSOR, linear between the two lux values and
// clamped outside them. emissive 0: not decoded for this aircraft.
// floor_low/high: light that falls on the display (cockpit lighting, sun), as
// a fraction of its full-code light, interpolated on the same ambient mapping.
struct DisplayLight {
  float emissive = 0;
  float lux_low = 0, lux_high = 0, gain_low = 0, gain_high = 0;
  float floor_low = 0, floor_high = 0;
};
inline double display_ambient_position(const DisplayLight& d, double ambient) noexcept {
  return ambient <= d.lux_low ? 0 : ambient >= d.lux_high ? 1 : (ambient - d.lux_low) / (d.lux_high - d.lux_low);
}
inline double display_full_light(const DisplayLight& d, double ambient) noexcept {
  if (!(d.emissive > 0) || !(d.lux_high > d.lux_low) || ambient != ambient)
    return 0;
  return d.emissive * (d.gain_low + (d.gain_high - d.gain_low) * display_ambient_position(d, ambient));
}
// How bright a full code looks in the main view, before its tone curve (full
// light x exposure; y = scene light x exposure), below which the camera image
// stops passing the scene light through. The night PMDG 777 (full code 22.3,
// exposure 0.0333: 0.74) and dusk (1125 x 0.00146: 1.6) stay above it;
// daylight (1637 x 0.000111: 0.18) is far below, so everything the main view
// shows brighter than 0.18 would clip. The gap between the two is hysteresis.
constexpr double DisplayPassOff = 0.45, DisplayPassOn = 0.55;
// Whether the camera image can be the scene light for the display to give
// back. Otherwise the main view's tone curve makes the image, as on other
// aircraft: the display then shows it at most this dim, where the main view's
// exposure of the display is close to linear. exposure: the simulator's
// main-view exposure, 0 when unknown; then only the night display (the gain at
// its low clamp) passes through. was: the previous result.
inline bool display_pass_through(const DisplayLight& d, double ambient, double exposure, bool was) noexcept {
  const double full_light = display_full_light(d, ambient);
  if (!(full_light > 0))
    return false;
  if (!(exposure > 0) || !(exposure < 1e6))
    return display_ambient_position(d, ambient) == 0;
  return full_light * exposure >= (was ? DisplayPassOff : DisplayPassOn);
}
inline double display_floor(const DisplayLight& d, double ambient) noexcept {
  if (ambient != ambient || !(d.lux_high > d.lux_low))
    return 0;
  return d.floor_low + (d.floor_high - d.floor_low) * display_ambient_position(d, ambient);
}
struct AircraftProfile {
  std::uint32_t id;
  std::string_view key;
  const wchar_t* name;
  // Per display side (see display_sides.hpp). Entries at or beyond `sides`
  // are unused and may be null.
  std::array<const char*, MaxDisplaySides> taxi_lvars;
  std::array<const char*, MaxDisplaySides> taxi_events;
  std::array<const char*, MaxDisplaySides> pfd_labels;
  // right/up/forward metres, pitch/yaw degrees, lens radians.
  // [0] nose, [1] tail or bottom-left, [2] bottom-right when split_bottom != 0.
  std::array<std::array<double, 6>, 3> mounts;
  unsigned width, height, mips;
  TaxiControl taxi_control = TaxiControl::push_event;
  std::array<std::string_view, 3> aircraft_types{"A388"};
  std::array<DisplayRect, MaxDisplaySides> display_regions{{{0, 0, 768, 763}, {0, 0, 768, 763}}};
  CameraPanes camera_panes{{{736, 251}, {736, 496}, {736, 496}}};
  bool higher_id_left = true;
  Composition composition{};
  std::array<unsigned, 6> formats{28};
  std::array<std::string_view, 3> package_markers{"flybywire-aircraft-a380-842", "flybywire_a380_842"};
  // Target pixels: the outer display region is black around this camera inset.
  DisplayInsets camera_padding{16, 12, 16, 0};
  PfdDetectionPolicy pfd_detection = PfdDetectionPolicy::dominant_activity;
  // Daytime EV of Taxi Cam's fallback exposure (DisplayExposureController),
  // used only while the camera images cannot take the main view's lighting.
  float exposure = -8.f;
  // Measured PFD redraws per second per side (bridge stamps/s ÷ 2). Composing
  // faster than this cannot reach the screen. 0 = not measured: only the
  // camera-manager ceiling caps the useful camera_rate.
  unsigned pfd_refresh_hz = 0;
  // Scanned texture name, when the display is not named by pfd_labels alone.
  const char* display_texture = "";
  // Single-display profiles: a separate texture for side 2 (the PMDG 777
  // lower DU). Empty keeps every side on display_texture.
  const char* lower_display_texture = "";
  // Existing compositor guide switch. False draws no alignment markers.
  bool reference_guides = true;
  // False keeps ground speed out of the composed ND (PMDG 777).
  bool ground_speed = true;
  // Every profile shows a black PLEASE WAIT page while a side starts or its
  // camera image is missing or stale. Airbus text uses the GS colour; true
  // draws it white (PMDG 777, which has no ground-speed readout).
  bool waiting_white_text = false;
  // Display sides this aircraft drives: captain and first officer, plus the
  // lower ECAM on the A340s or the lower DU on the PMDG 777.
  unsigned sides = 2;
  // Decoded camera-display brightness, used to match the main view's lighting;
  // emissive 0: not decoded for this aircraft.
  DisplayLight display_light{};
  // panel.cfg [VCockpitNN] texture= name of each side's display texture, for
  // routing by name. Empty keeps automatic detection for that side.
  std::array<const char*, MaxDisplaySides> panel_textures{"", "", ""};
  // A second accepted mip count (0 = none), for named panel textures whose
  // mip count differs from the detected display targets'. Kept narrow: every
  // profile's shape admits display candidates on every aircraft.
  unsigned panel_mips = 0;
  // Texture shapes of other panels, read from logged cockpit loads. Name
  // pairing must land every listed panel on its shape, so extra textures from
  // an aircraft switch cannot slide the names onto look-alike displays.
  struct PanelShape {
    const char* name;
    unsigned width, height;
  };
  std::array<PanelShape, 6> panel_shapes{};
};
inline constexpr unsigned side_mask(const AircraftProfile& p) noexcept {
  return p.sides >= MaxDisplaySides ? AllDisplaySides : (1u << p.sides) - 1;
}
// Side 2 lives on its own texture rather than the shared display texture.
inline constexpr bool separate_lower_texture(const AircraftProfile& p) noexcept {
  return p.sides > 2 && p.pfd_detection == PfdDetectionPolicy::single_display && p.lower_display_texture[0] != '\0';
}
// Sides drawn from the shared single-display texture.
inline constexpr unsigned shared_texture_sides(const AircraftProfile& p) noexcept {
  return separate_lower_texture(p) ? side_mask(p) & ~4u : side_mask(p);
}
// Taxi Cam may send button commands only to these cockpit controls.
inline constexpr bool commandable_buttons(const AircraftProfile& p) noexcept {
  return p.taxi_control == TaxiControl::push_event || p.taxi_control == TaxiControl::lvar_off;
}
// Minimum PLEASE WAIT time after a side starts drawing, and the camera-image
// age that brings the page back while the side stays on.
inline constexpr std::uint64_t WaitingPageMinimumMs = 750;
inline constexpr std::uint64_t WaitingPageStaleMs = 1000;
// panel.cfg texture names were read live on 2026-10-04 and match the
// cockpit material labels.
inline constexpr AircraftProfile A380 = [] {
  AircraftProfile p{1,
                    "fbw-a380x",
                    L"FlyByWire A380X",
                    {"L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON", "L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON"},
                    {"A32NX.FCU_EFIS_L_TAXI_PUSH", "A32NX.FCU_EFIS_R_TAXI_PUSH"},
                    {"SCREEN_DU_PFDL", "SCREEN_DU_PFDR"},
                    {{{0, -1.75, 26.950668984, -17.5, 0, 1.24}, {0, 18, -25, -32, 0, 1.02}, {0, 18, -25, -32, 0, 1.02}}},
                    768,
                    1024,
                    5};
  p.panel_textures = {"SCREEN_DU_PFDL", "SCREEN_DU_PFDR", ""};
  p.panel_shapes = {{{"SCREEN_DU_MFD", 1646, 1024},
                     {"SCREEN_DU_EWD", 768, 1024},
                     {"SCREEN_DU_SD", 768, 1024},
                     {"SCREEN_DU_NDL", 768, 1024},
                     {"SCREEN_DU_NDR", 768, 1024}}};
  return p;
}();
// Display dimensions and Lvars: iniBuilds A350 1.2.6 panel/behaviour XML.
// The accepted -900 calibration transfers to the -1000 with its physical
// longitudinal offsets, retaining the same height, pitch and lens. Composition
// defaults reflect the user's saved per-aircraft calibrations from 2026-09-15.
// The compositor retains its bounded 768px working image.
// Preserve a 32px central gap around the PFD/MFDDivider artwork on both sides.
// A350 PFD refresh: both-PFD stamps 160–179/s (80–90 per side) on the 0.9.35
// rate sweep, parked A350-900 under Man FG target 30. Above the manager
// ceiling, so only that ceiling caps the useful rate on this aircraft.
inline constexpr unsigned A350PfdRefreshHz = 80;
inline constexpr AircraftProfile A359 = [] {
  AircraftProfile p{2,
                    "ini-a350-900",
                    L"iniBuilds A350-900 / ULR",
                    {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                    {"", ""},
                    {"$EFIS_LEFT", "$EFIS_RIGHT"},
                    {{{0, -2, 16, -15, 0, 0.55}, {0, 10, -33.0, -15, 0, 0.62}, {0, 10, -33.0, -15, 0, 0.62}}},
                    1644,
                    1024,
                    0,
                    TaxiControl::lvar_off,
                    {"A359", "A359 ULR"},
                    {{{0, 0, 806, 763}, {838, 0, 1644, 763}}},
                    {{{774, 251}, {774, 496}, {774, 496}}},
                    true,
                    A350Etacs,
                    {28, 29, 87, 91, 27, 90},
                    {"inibuilds-aircraft-a350", "presets/inibuilds", "attachments/inibuilds"}};
  p.pfd_refresh_hz = A350PfdRefreshHz;
  p.panel_textures = {"$EFIS_LEFT", "$EFIS_RIGHT", ""};
  p.panel_shapes = {{{"$INI_FAP", 2048, 2048}, {"$SD", 1644, 1024}}};
  return p;
}();
inline constexpr AircraftProfile A35K = [] {
  AircraftProfile p{3,
                    "ini-a350-1000",
                    L"iniBuilds A350-1000",
                    {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                    {"", ""},
                    {"$EFIS_LEFT", "$EFIS_RIGHT"},
                    {{{0, -2, 19.81, -15, 0, 0.55}, {0, 10, -36.17, -15, 0, 0.62}, {0, 10, -36.17, -15, 0, 0.62}}},
                    1644,
                    1024,
                    0,
                    TaxiControl::lvar_off,
                    {"A35K"},
                    {{{0, 0, 806, 763}, {838, 0, 1644, 763}}},
                    {{{774, 251}, {774, 496}, {774, 496}}},
                    true,
                    A350EtacsA35K,
                    {28, 29, 87, 91, 27, 90},
                    {"inibuilds-aircraft-a350", "presets/inibuilds", "attachments/inibuilds"}};
  p.pfd_refresh_hz = A350PfdRefreshHz;
  p.panel_textures = {"$EFIS_LEFT", "$EFIS_RIGHT", ""};
  p.panel_shapes = {{{"$INI_FAP", 2048, 2048}, {"$SD", 1644, 1024}}};
  return p;
}();
// iniBuilds A380 display layout started from FBW. Mounts and guide defaults
// reflect the user's accepted live calibration from 2026-09-15.
// The streamed aircraft reports Airbus with this exact AircraftLoaded path
// component. Its INOP TAXI buttons produced no observed latch changes.
inline constexpr AircraftProfile IniA380 = [] {
  auto p = A380;
  p.id = 4;
  p.key = "ini-a380";
  p.name = L"iniBuilds A380";
  p.taxi_lvars = {"", ""};
  p.taxi_events = {"", ""};
  p.pfd_labels = {"", ""};
  p.taxi_control = TaxiControl::manual_only;
  p.aircraft_types = {"Airbus", "A388"};
  p.package_markers = {"simobjects/airplanes/inibuilds-a380", "fs24-inibuilds-aircraft-a380"};
  p.mounts = {{{0, 2.2, 16, -17.5, 0, 1}, {0, 18, -34, -32, 0, 1}, {0, 18, -34, -32, 0, 1}}};
  p.composition.tail_upper = {0.34f, 0.52f};
  p.composition.tail_corner = {0.305f, 0.65f};
  p.composition.tail_inner = {0.355f, 0.65f};
  // The aircraft draws its PFDs in one-mip typeless targets and then into the
  // five-mip panel textures named in panel.cfg; routing by name stamps the
  // named $PFD_CPT/$PFD_FO panel textures (2026-10-04 panel table: eight
  // 768 x 1024 five-mip displays). The allocation-group fallback still pairs
  // only the one-mip typeless group.
  p.mips = 1;
  p.panel_mips = 5;
  p.panel_textures = {"$PFD_CPT", "$PFD_FO", ""};
  p.panel_shapes = {{{"$INI_FAP", 2048, 1536}, {"$ND_CPT", 768, 1024}, {"$ND_FO", 768, 1024}, {"$EWD", 768, 1024}, {"$SD", 768, 1024}}};
  p.formats = {28, 29, 87, 91, 27, 90};
  p.pfd_detection = PfdDetectionPolicy::ini_a380_allocation_group;
  // 31.3 both-PFD stamps/s at camera_rate 15 on installed 0.9.11 (no FG):
  // ~15.7 redraws per side. FBW A380 (profile 1) is not measured yet.
  p.pfd_refresh_hz = 16;
  return p;
}();
// Scanned on the open 777-200ER RR. Both inboard gauges draw one shared texture.
// There is no separate taxi-camera texture.
inline constexpr const char* Pmdg777Texture = "DUS";
inline constexpr const char* Pmdg777LeftGauge = "DU_LeftInboard";
inline constexpr const char* Pmdg777RightGauge = "DU_RightInboard";
inline constexpr unsigned Pmdg777DisplayWidth = 2048;
inline constexpr unsigned Pmdg777DisplayHeight = 2048;
// panel.cfg htmlgauge x, y, width, height on DUS. The same values are in the
// 777-200ER, 777-300ER, and 777F files. The navigation display is the whole
// inboard gauge, so these rectangles are not cropped further.
inline constexpr unsigned Pmdg777LeftNdX = 1058;
inline constexpr unsigned Pmdg777LeftNdY = 33;
inline constexpr unsigned Pmdg777RightNdX = 30;
inline constexpr unsigned Pmdg777RightNdY = 1058;
inline constexpr unsigned Pmdg777NdWidth = 958;
inline constexpr unsigned Pmdg777NdHeight = 971;
// Lower DU: panel.cfg [VCockpit02] draws DU_Lower on the separate 2048 x 2048
// EICASCDU texture at 1058, 21, 958, 971 (same on all three variants). The
// upper EICAS and the three CDU screens share that texture.
inline constexpr const char* Pmdg777LowerTexture = "EICASCDU";
inline constexpr unsigned Pmdg777LowerX = 1058;
inline constexpr unsigned Pmdg777LowerY = 21;
// Mip count and DXGI format were not in the scan.
inline constexpr unsigned Pmdg777DisplayMips = 0;
inline constexpr std::array<unsigned, 6> Pmdg777Formats{};
// Separate settings files per airplane folder (200ER / 300ER / 777F). They share
// this cockpit layout on DUS. AircraftLoaded selects the airplane folder. ATC
// TYPE is the Boeing brand string and is not required.
// Both navigation displays are rectangles on one destination texture. Taxi Cam
// owns three viewpoints: nose on top, and independent left/right wing cameras
// on the split bottom. Pixels outside the two inboard gauges stay untouched.
// Working-image layout matched to a real 777 camera page photo (issue 96) on a
// nearly square 958x971 gauge: a short nose picture (~28% of the width), a thin
// T (~3% bar, ~4% stem), thin black frames and tall bottom panes (~0.85 wide to
// tall). The nose stays taller than the default GS panel so the GS font
// fixture still sees camera pixels around it. Bottom panes fill the rows down
// to the old pane bottom (678); the 85 rows below stay black. Extra black above
// the stamped block on the ND comes from camera_padding top. L/R padding stay 0.
inline constexpr unsigned Pmdg777NosePictureHeight = 220;
inline constexpr unsigned Pmdg777BottomGap = 32;
inline constexpr unsigned Pmdg777FrameBorder = 8;
inline constexpr unsigned Pmdg777BottomPaneWidth = (768 - Pmdg777BottomGap) / 2;
inline constexpr unsigned Pmdg777DividerThickness = 24;
inline constexpr unsigned Pmdg777DividerTop = Pmdg777NosePictureHeight;
inline constexpr unsigned Pmdg777DividerBottom = Pmdg777DividerTop + Pmdg777DividerThickness;
inline constexpr unsigned Pmdg777TailTop = Pmdg777DividerBottom;
inline constexpr unsigned Pmdg777TopPadding = 85;
inline constexpr unsigned Pmdg777BottomPaneHeight = 763 - Pmdg777TopPadding - Pmdg777TailTop;
static_assert(Pmdg777BottomPaneWidth == 368);
static_assert(Pmdg777TailTop == 244);
static_assert(Pmdg777BottomPaneHeight == 434);
static_assert(Pmdg777NosePictureHeight > 12 + 56 + Pmdg777FrameBorder, "Nose stays taller than the default GS panel");
inline constexpr Composition Pmdg777Composition = [] {
  Composition c;
  c.nose_height = static_cast<float>(Pmdg777NosePictureHeight);
  c.divider_top = static_cast<float>(Pmdg777DividerTop);
  c.divider_bottom = static_cast<float>(Pmdg777DividerBottom);
  c.tail_top = static_cast<float>(Pmdg777TailTop);
  c.split_bottom = 1;
  c.bottom_gap = static_cast<float>(Pmdg777BottomGap);
  c.bottom_pane_height = static_cast<float>(Pmdg777BottomPaneHeight);
  c.frame_border = static_cast<float>(Pmdg777FrameBorder);
  return c;
}();
// Nose: forward/down over the nose gear. Bottom-left/right: outside each side of
// the fuselage looking at that wing (own Right offset and yaw; not a mirrored
// single tail capture). 200ER/777F published nose from Robert's live 777-200ER
// calibration. 300ER nose from his live longer-fuselage capture (forward 22 m);
// wing mounts stay the published shared defaults until calibrated separately.
inline constexpr std::array<std::array<double, 6>, 3> Pmdg777Mounts{
    {{0, -2, 16, -18, 0, 1}, {-6, 1.5, -28, -5, -12, 0.6}, {6, 1.5, -28, -5, 12, 0.6}}};
inline constexpr std::array<std::array<double, 6>, 3> Pmdg777300Mounts{
    {{0, -2, 22, -18, 0, 1}, Pmdg777Mounts[1], Pmdg777Mounts[2]}};
inline constexpr std::array<std::array<double, 6>, 3> Pmdg777FMounts{
    {Pmdg777Mounts[0], Pmdg777Mounts[1], Pmdg777Mounts[2]}};
// Nose matches the 220 px picture; each bottom feed fills its tall half-pane.
inline constexpr CameraPanes Pmdg777Panes{
    {{736, static_cast<std::int32_t>((Pmdg777NosePictureHeight * 736 + 384) / 768)},
     {static_cast<std::int32_t>(Pmdg777BottomPaneWidth), static_cast<std::int32_t>(Pmdg777BottomPaneHeight)},
     {static_cast<std::int32_t>(Pmdg777BottomPaneWidth), static_cast<std::int32_t>(Pmdg777BottomPaneHeight)}}};
inline constexpr auto make_pmdg_777 =
    [](std::uint32_t id, std::string_view key, const wchar_t* name, std::string_view marker, std::array<std::array<double, 6>, 3> mounts) {
      AircraftProfile p{id,
                        key,
                        name,
                        {"", "", ""},
                        {"", "", ""},
                        {Pmdg777LeftGauge, Pmdg777RightGauge, "DU_Lower"},
                        mounts,
                        Pmdg777DisplayWidth,
                        Pmdg777DisplayHeight,
                        Pmdg777DisplayMips,
                        TaxiControl::pmdg_dsp_cam,
                        {"", "", ""},
                        {{{Pmdg777LeftNdX, Pmdg777LeftNdY, Pmdg777LeftNdX + Pmdg777NdWidth, Pmdg777LeftNdY + Pmdg777NdHeight},
                          {Pmdg777RightNdX, Pmdg777RightNdY, Pmdg777RightNdX + Pmdg777NdWidth, Pmdg777RightNdY + Pmdg777NdHeight},
                          {Pmdg777LowerX, Pmdg777LowerY, Pmdg777LowerX + Pmdg777NdWidth, Pmdg777LowerY + Pmdg777NdHeight}}},
                        Pmdg777Panes,
                        true,
                        Pmdg777Composition,
                        Pmdg777Formats,
                        {marker, "", ""}};
      p.pfd_detection = PfdDetectionPolicy::single_display;
      p.display_texture = Pmdg777Texture;
      p.lower_display_texture = Pmdg777LowerTexture;
      p.sides = 3;
      p.reference_guides = false;
      p.ground_speed = false;
      p.waiting_white_text = true;
      // Larger top ND inset moves the stamped page down without shortening the nose
      // picture. Bottom inset stays 0; leftover working-image rows under the squares
      // are black. L/R stay 0.
      p.camera_padding = {0, Pmdg777TopPadding, 0, 0};
      // PMDG773ER_VC.gltf material DUS emissiveFactor 148.8235 (150 x 253/255);
      // 77W_Cockpit_Behavior.xml: (A:AMBIENT LIGHT SENSOR) 200 2000 0.15 11.0
      // (F:MapRange). The shared ph_base_gbuffer emissive path (PIX 2026-10-02)
      // gives 148.8235 x 11 = 1637.06 by day, matched bit for bit, with no screen
      // filter or glass; the lower EICASCDU uses 150 (0.8% brighter).
      // Live calibration (2026-10-02, night, ambient 0.64): a code ramp on the ND
      // read back from the main view fits this light with a floor of 0.0013 of
      // full-code light (30 points, 1.2 codes RMS); the day capture's offset is
      // about 0.013. With these the ND's ground and sky matched the window.
      p.display_light = {148.8235f, 200, 2000, 0.15f, 11.0f, 0.0013f, 0.0134f};
      p.panel_textures = {Pmdg777Texture, Pmdg777Texture, Pmdg777LowerTexture};
      return p;
    };
inline constexpr AircraftProfile Pmdg777 =
    make_pmdg_777(5, "pmdg-777", L"PMDG 777-200ER", "PMDG 777-200ER", Pmdg777Mounts);
inline constexpr AircraftProfile Pmdg777300ER =
    make_pmdg_777(6, "pmdg-777-300er", L"PMDG 777-300ER", "PMDG 777-300ER", Pmdg777300Mounts);
inline constexpr AircraftProfile Pmdg777F =
    make_pmdg_777(7, "pmdg-777f", L"PMDG 777F", "PMDG 777F", Pmdg777FMounts);
static_assert(Pmdg777300ER.mounts[0][2] == 22);
static_assert(Pmdg777300ER.mounts[0] != Pmdg777Mounts[0]);
static_assert(Pmdg777F.mounts[0] == Pmdg777Mounts[0]);
// Aerosoft A346 Pro 1.0.1 panel.cfg: every display is an htmlgauge on one
// 4096x4096 $GAUGES_UNIFIED texture. The TACS selectors show the camera on the
// captain's PFD (CAM CAPT), the first officer's PFD (CAM FO) and the lower ECAM
// (CAM SD), which are sides 0, 1 and 2: the CaptPFD, CoPFD and ECAM_LOWER
// regions, as in the other Airbus profiles. Live testing on 2026-09-23 showed
// the ND regions one display out on both pilot sides. Mip count and DXGI format
// are not scanned yet.
inline constexpr const char* A346Texture = "$GAUGES_UNIFIED";
inline constexpr unsigned A346DisplaySize = 4096;
// panel.cfg CaptPFD 9, 470; CoPFD 9, 1230; ECAM_LOWER 1529, 1230; each 750 x 750.
inline constexpr unsigned A346PfdX = 9;
inline constexpr unsigned A346CaptY = 470;
inline constexpr unsigned A346FoY = 1230;
inline constexpr unsigned A346DisplayUnit = 750;
inline constexpr unsigned A346SdX = 1529;
inline constexpr unsigned A346SdY = 1230;
// flight_model.cfg contact points (feet): NLG forward/up 103.51/-21.4; left
// MLG right/up/forward -17.5/-22.4/-4.46. Mounts started from the A350-900's
// gear-relative offsets (nose 8.22 m aft of the NLG contact; tail 27.63 m aft
// and 15.01 m above the MLG contact). Defaults reflect the user's saved live
// calibration from 2026-09-23, which raised the nose camera to -2.5 m.
inline constexpr std::array<std::array<double, 6>, 3> A346Mounts{
    {{0, -2.5, 23.33, -15, 0, 0.55}, {0, 8.18, -28.99, -15, 0, 0.62}, {0, 8.18, -28.99, -15, 0, 0.62}}};
// Guide defaults from the same live calibration; the nose markers keep the
// shared position.
inline constexpr Composition A346Composition = [] {
  auto c = A350Etacs;
  c.tail_upper = {0.28f, 0.72f};
  c.tail_corner = {0.24f, 0.86f};
  c.tail_inner = {0.31f, 0.86f};
  return c;
}();
// All three TAXI selectors are two-state XML switches whose setter writes 1/0
// to the latch, like the A350.
inline constexpr AircraftProfile AerosoftA346 = [] {
  AircraftProfile p{8,
                    "aerosoft-a346",
                    L"Aerosoft A340-600",
                    {"L:AB_VC_CAM_CAPT_SEL", "L:AB_VC_CAM_FO_SEL"},
                    {"", ""},
                    {"CaptPFD", "CoPFD"},
                    A346Mounts,
                    A346DisplaySize,
                    A346DisplaySize,
                    0,
                    TaxiControl::lvar_off,
                    {"", "", ""},
                    {{{A346PfdX, A346CaptY, A346PfdX + A346DisplayUnit, A346CaptY + A346DisplayUnit},
                      {A346PfdX, A346FoY, A346PfdX + A346DisplayUnit, A346FoY + A346DisplayUnit}}}};
  p.sides = 3;
  p.taxi_lvars[2] = "L:AB_VC_CAM_SD_SEL";
  p.taxi_events[2] = "";
  p.pfd_labels[2] = "ECAM_LOWER";
  p.display_regions[2] = {A346SdX, A346SdY, A346SdX + A346DisplayUnit, A346SdY + A346DisplayUnit};
  p.composition = A346Composition;
  p.formats = {};
  p.package_markers = {"simobjects/airplanes/airbus-a346-pro", "aerosoft-aircraft-a346-pro", ""};
  p.pfd_detection = PfdDetectionPolicy::single_display;
  p.display_texture = A346Texture;
  p.panel_textures = {A346Texture, A346Texture, A346Texture};
  return p;
}();
// iniBuilds A340-300 (streamed fs24-inibuilds-aircraft-a340; encrypted, so no
// panel.cfg). The bridge's render-target log on 2026-09-23 (a340-300_eis2)
// showed the cockpit's one-mip typeless group, including one 1560 x 2340
// texture: a 2 x 3 grid of 780 x 780 cells, one per display unit. Live
// testing on 2026-09-23 showed cell (0,0) on the CAPT ND, (1,1) on the F/O PFD
// and (1,2) on the lower ECAM. Rows are therefore ND / PFD pairs:
// CAPT ND, CAPT PFD / F/O ND, F/O PFD / E/WD, SD. CAPT PFD at (1,0) follows
// that pattern and is not yet re-checked.
inline constexpr unsigned IniA343DisplayWidth = 1560;
inline constexpr unsigned IniA343DisplayHeight = 2340;
inline constexpr unsigned IniA343Cell = 780;
static_assert(IniA343DisplayWidth == 2 * IniA343Cell && IniA343DisplayHeight == 3 * IniA343Cell);
inline constexpr DisplayRect ini_a343_cell(unsigned column, unsigned row) noexcept {
  return {column * IniA343Cell, row * IniA343Cell, (column + 1) * IniA343Cell, (row + 1) * IniA343Cell};
}
// No TACS selector or other camera control was found, so the displays follow
// the camera shortcuts and previews only, as on the iniBuilds A380. Mounts
// start from the A350-900 defaults until calibrated live.
inline constexpr AircraftProfile IniA343 = [] {
  AircraftProfile p{9,
                    "ini-a340-300",
                    L"iniBuilds A340-300",
                    {"", "", ""},
                    {"", "", ""},
                    {"", "", ""},
                    A359.mounts,
                    IniA343DisplayWidth,
                    IniA343DisplayHeight,
                    1,
                    TaxiControl::manual_only,
                    {"", "", ""},
                    {{ini_a343_cell(1, 0), ini_a343_cell(1, 1), ini_a343_cell(1, 2)}}};
  p.sides = 3;
  p.composition = A346Composition;
  p.formats = {28, 29, 87, 91, 27, 90};
  p.package_markers = {"simobjects/airplanes/inibuilds-a340", "fs24-inibuilds-aircraft-a340", ""};
  p.pfd_detection = PfdDetectionPolicy::single_display;
  return p;
}();
inline constexpr std::array<const AircraftProfile*, 9> Catalog{&A380, &A359, &A35K, &IniA380, &Pmdg777, &Pmdg777300ER,
                                                              &Pmdg777F, &AerosoftA346, &IniA343};
inline constexpr DisplayRect display_rect(const AircraftProfile& p, unsigned side) noexcept {
  return p.display_regions[side < p.sides && side < MaxDisplaySides ? side : 0];
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
  if (width != p.width || height != p.height || !mips || mips > 12 || (p.mips && p.mips != mips && p.panel_mips != mips) || !format)
    return false;
  bool listed = false;
  for (auto supported : p.formats) {
    if (!supported)
      continue;
    listed = true;
    if (supported == format)
      return true;
  }
  // No scanned format list: admit the known size and leave a shared size ambiguous.
  return !listed;
}
// Automatic detection: the profile's own mip count only. panel_mips admits
// named panel textures for routing by name, not for detection.
inline constexpr bool matches_detection(const AircraftProfile& p,
                                        unsigned width,
                                        unsigned height,
                                        unsigned mips,
                                        unsigned format) noexcept {
  return matches_display(p, width, height, mips, format) && (!p.mips || mips == p.mips);
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
  for (const auto marker : IniA380.package_markers)
    if (!marker.empty() && path_contains(path, marker))
      return matches_aircraft(IniA380, type) ? IniA380.id : 0;
  // PMDG ATC TYPE is the Boeing brand string, not a product id. The public
  // AircraftLoaded path names the airplane folder. The open 777-200ER RR
  // reported SimObjects\Airplanes\PMDG 777-200ER\... and did not contain
  // pmdg-aircraft-77er. The 300ER and 777F use the same folder-component
  // pattern; those two paths were not in this log.
  // The Aerosoft A346 and the iniBuilds A340-300 also report the Airbus brand
  // string; their SimObject folders identify the product. The live A340-300
  // path was SimObjects\Airplanes\inibuilds-a340\presets\inibuilds\a340-300_eis2\...
  for (const auto* profile : {&Pmdg777, &Pmdg777300ER, &Pmdg777F, &AerosoftA346, &IniA343}) {
    for (const auto marker : profile->package_markers)
      if (!marker.empty() && path_contains(path, marker))
        return profile->id;
  }
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
