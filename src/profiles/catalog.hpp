#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>
#include "../shared/display_sides.hpp"
#include "../shared/lvar_binding.hpp"
#include "aircraft_variant.hpp"
namespace taxi_camera::profiles {
struct DisplayRect {
  unsigned left, top, right, bottom;
};
struct DisplayInsets {
  unsigned left, top, right, bottom;
};
// Every aircraft's camera set has the same four views, so a change of
// aircraft keeps it: 0 nose, 1 tail or bottom-left, 2 bottom-right
// (split_bottom profiles; otherwise idle at the tail's size and mount), 3 the
// door camera.
inline constexpr unsigned CameraViews = 4;
using CameraPanes = std::array<std::array<std::int32_t, 2>, CameraViews>;
using CameraMounts = std::array<std::array<double, 6>, CameraViews>;
// The door camera: one camera that fills a display of its own, separate from
// the taxi cameras. It renders at the shape of that display's camera area
// (shape_door), never reshaped with the taxi panes. The mount is an approximate cabin view behind
// the flight deck looking aft (the Fenix A320's cabin from its cameras.cfg);
// each aircraft adjusts it.
inline constexpr unsigned DoorFeed = 3;
inline constexpr std::array<std::int32_t, 2> DoorPane{768, 763};
inline constexpr std::array<double, 6> DoorMount{0, 2, 9, -10, 180, 1.2};
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
  // Right tail bracket, measured from the right edge as the left one is from
  // the left. Negative mirrors the left bracket. In the same order as the left
  // points above: corner, upper, inner.
  std::array<float, 2> tail_corner_right{-1, -1}, tail_upper_right{-1, -1}, tail_inner_right{-1, -1};
  // Width of an overlay pixel relative to its height once the working image
  // is stretched over the display's camera area, against the area the profile
  // was made for. The GS text, nose markers and guide strokes are drawn that
  // much narrower so a reshaped area (shape_cameras) keeps their proportions.
  float ui_aspect = 1;
};
static_assert(sizeof(Composition) == 36 * sizeof(float));
// pmdg_dsp_cam reads the glareshield Display Select Panel. Nothing is written.
enum class TaxiControl { push_event, lvar_off, manual_only, pmdg_dsp_cam };
// named routes only by panel name (aircraft set up by the user): a side whose
// name does not resolve waits instead of guessing among same-sized displays.
// How an aircraft's display sides sit on its textures. pair: each pilot side
// has a texture of its own (A380s, A350s). shared_texture: every side is a
// region of one texture (PMDG 777, A340s; the 777's lower DU may be separate).
// fitted: a set-up aircraft, each side the whole texture the user picked.
enum class DisplayLayout { pair, shared_texture, fitted };
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
  // [0] nose, [1] tail or bottom-left, [2] bottom-right when split_bottom != 0
  // (otherwise the tail's again), [3] the door camera.
  CameraMounts mounts;
  unsigned width, height, mips;
  TaxiControl taxi_control = TaxiControl::push_event;
  std::array<std::string_view, 3> aircraft_types{"A388"};
  std::array<DisplayRect, MaxDisplaySides> display_regions{{{0, 0, 768, 763}, {0, 0, 768, 763}}};
  CameraPanes camera_panes{{{736, 251}, {736, 496}, {736, 496}, DoorPane}};
  bool higher_id_left = true;
  Composition composition{};
  std::array<unsigned, 6> formats{28};
  std::array<std::string_view, 3> package_markers{"flybywire-aircraft-a380-842", "flybywire_a380_842"};
  // Target pixels: the outer display region is black around this camera inset.
  DisplayInsets camera_padding{16, 12, 16, 0};
  DisplayLayout display_layout = DisplayLayout::pair;
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
  // Each side's display by its panel-mark key (src/shared/mark_identity.hpp):
  // "@gauge" or "@gauge#rank/count" (twins). Empty leaves the side to
  // the user's choice. A panel.cfg texture= name (an older catalog entry) is
  // converted by the companion from the aircraft's panel.cfg on first use.
  std::array<const char*, MaxDisplaySides> panel_textures{"", "", ""};
  // A pilot pair's lower display (a panel of the pair's shape), placed for the
  // door camera while the aircraft's settings have no door camera yet; the
  // user's choice in Displays is kept from then on. Empty: none known. The
  // area is its display area on that panel, or on an aircraft's own lower
  // display (no texture) its display area there; empty keeps the region.
  const char* door_lower_texture = "";
  DisplayRect door_lower_area{};
  // The aircraft's own door camera switch, bound to the door camera while the
  // aircraft's settings have no door camera yet and that action is unbound:
  // an L:var without "L:" and its standalone::LvarTrigger (lvar_binding.hpp).
  const char* door_lvar = "";
  std::uint32_t door_lvar_trigger = 0;
  // Cameras that start in black and white (a bit per camera view) until the
  // aircraft's settings save their look.
  std::uint32_t black_and_white = 0;
  // Displays whose instrument redraws only what changed (the FBW A380X), per
  // side. Their camera keeps a shadow of the display's own pixels and writes
  // by queue copies only, so the instrument's whole picture comes back when the
  // camera stops. Every other display repaints whole and is stamped inside the
  // instrument's own list, which no submission can drop.
  std::array<bool, MaxDisplaySides> partial_repaint{};
  // A second accepted mip count (0 = none), for panel textures whose mip
  // count differs from the profile's display shape. Kept narrow: every
  // profile's shape admits display candidates on every aircraft.
  unsigned panel_mips = 0;
  // Aircraft set up by the user (discovered.hpp): any panel texture is a
  // display, and each side's area is the whole texture routed to it, whatever
  // its size. side_shapes (width, height, mips) are the panels the user picked;
  // name routing expects those shapes. False for catalog profiles.
  bool fit_texture = false;
  std::array<std::array<unsigned, 3>, MaxDisplaySides> side_shapes{};
  // fit_texture only: a display area the user chose on each side's texture
  // (texture pixels); empty uses the whole texture.
  std::array<DisplayRect, MaxDisplaySides> areas{};
  // Width / height of the camera picture each side shows, when the cameras
  // were shaped for another side's area (shape_cameras): a side whose camera
  // area has another shape shows the picture at this one inside it, with
  // black either side. 0 fills the area.
  std::array<float, MaxDisplaySides> fit_aspect{};
};
inline constexpr unsigned side_mask(const AircraftProfile& p) noexcept {
  return p.sides >= MaxDisplaySides ? AllDisplaySides : (1u << p.sides) - 1;
}
// Taxi cameras: nose and tail, or nose and both wings on a split bottom page.
inline constexpr unsigned taxi_feeds(const AircraftProfile& p) noexcept {
  return p.composition.split_bottom != 0 ? 3u : 2u;
}

// Side 2 lives on its own texture rather than the shared display texture.
inline constexpr bool separate_lower_texture(const AircraftProfile& p) noexcept {
  return p.sides > 2 && p.display_layout == DisplayLayout::shared_texture && p.lower_display_texture[0] != '\0';
}
// A catalog profile with a pilot pair of displays can take a third: the lower
// display the user placed in Displays (apply_choices), any cockpit panel on
// its own texture (lower_any_panel), found by its name only. Used for the door
// camera.
inline constexpr bool pilot_pair(const AircraftProfile& p) noexcept {
  return !p.fit_texture && p.display_layout == DisplayLayout::pair;
}
inline constexpr bool named_lower_side(const AircraftProfile& p) noexcept {
  return p.sides > 2 && pilot_pair(p);
}
// Side 2 has a texture of its own: the 777's lower DU, a pilot pair's placed
// lower display, or a set-up aircraft's (each of its sides is a panel).
inline constexpr bool own_lower_texture(const AircraftProfile& p) noexcept {
  return separate_lower_texture(p) || named_lower_side(p) || (p.fit_texture && p.sides > 2);
}
// Sides drawn from the shared single-display texture.
inline constexpr unsigned shared_texture_sides(const AircraftProfile& p) noexcept {
  return separate_lower_texture(p) ? side_mask(p) & ~4u : side_mask(p);
}
// Taxi Cam may send button commands only to these cockpit controls.
inline constexpr bool commandable_buttons(const AircraftProfile& p) noexcept {
  return p.taxi_control == TaxiControl::push_event || p.taxi_control == TaxiControl::lvar_off;
}
// TAXI latches read for a profile: its leading sides that name a TAXI L:var.
// A lower display placed on a two-display aircraft (apply_choices) adds a side
// with no TAXI button of its own, so it adds no latch.
inline constexpr unsigned taxi_latches(const AircraftProfile& p) noexcept {
  unsigned count = 0;
  while (count < p.sides && count < MaxDisplaySides && p.taxi_lvars[count] && p.taxi_lvars[count][0])
    ++count;
  return count;
}
// The sides with a TAXI button of their own. On aircraft Taxi Cam commands,
// any other side is a manual layer on top of the cockpit buttons.
inline constexpr unsigned taxi_button_sides(const AircraftProfile& p) noexcept {
  return (1u << taxi_latches(p)) - 1u;
}
// Minimum PLEASE WAIT time after a side starts drawing, and the camera-image
// age that brings the page back while the side stays on.
inline constexpr std::uint64_t WaitingPageMinimumMs = 750;
inline constexpr std::uint64_t WaitingPageStaleMs = 1000;
inline constexpr std::array<double, 6> A380XDoorMount{-0.7, 4.1, 30.5, -62.83, 161.61, 1.2};
// panel.cfg texture names were read live on 2026-10-04 and match the
// cockpit material labels.
inline constexpr AircraftProfile A380 = [] {
  AircraftProfile p{1,
                    "fbw-a380x",
                    L"FlyByWire A380X",
                    {"L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON", "L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON"},
                    {"A32NX.FCU_EFIS_L_TAXI_PUSH", "A32NX.FCU_EFIS_R_TAXI_PUSH"},
                    {"SCREEN_DU_PFDL", "SCREEN_DU_PFDR"},
                    {{{0, -1.75, 26.950668984, -17.5, 0, 1.24}, {0, 18, -25, -32, 0, 1.02}, {0, 18, -25, -32, 0, 1.02}, DoorMount}},
                    768,
                    1024,
                    5};
  // Part_Interior_Cockpit panel.cfg: SCREEN_DU_PFDL, SCREEN_DU_PFDR, SCREEN_DU_SD (whose
  // sd gauge block lies under sdv2's, so its marks name it sdv2).
  p.panel_textures = {"@pfd#0/2", "@pfd#1/2", ""};
  // The system display below the EWD (panel.cfg VCockpit04, 768 x 1024 like
  // the PFDs), read from the installed package on 2026-11-02.
  p.door_lower_texture = "@sdv2";
  // Door: Robert's live A380X door camera (2026-11-02) on the system display
  // above its bottom strip. The ECAM control panel's VIDEO key selects page 15
  // of A32NX_ECAM_SD_CURRENT_PAGE_INDEX (read live; -1 when it is deselected).
  p.door_lower_area = {0, 0, 768, 664};
  p.mounts[DoorFeed] = A380XDoorMount;
  p.door_lvar = "A32NX_ECAM_SD_CURRENT_PAGE_INDEX";
  p.door_lvar_trigger = standalone::lvar_equals_trigger(15);
  // The system display repaints only what changed (seen live 2026-11-02 with
  // the door camera). The PFDs keep the shadow they were checked with.
  p.partial_repaint = {true, true, true};
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
// The door camera's display: $SD, the centre unit (1644 x 1024 like the EFIS
// textures). Its left half holds the engine display, the system page and the
// TAT/SAT/GW strip; its right half the ECAM messages. The area is Robert's
// live one inside the system page (2026-11-05; the page between the dividers
// at y 266 and 927 is x 0–806, y 268–925).
inline constexpr DisplayRect A350DoorArea{11, 402, 671, 875};
// Door: Robert's live A350-900 door camera (2026-11-02, unchanged 2026-11-05)
// on that area, behind the cockpit door looking aft and down. The -1000's
// moves forward with its nose camera (3.81 m); not checked live. The ECAM
// control panel's VIDEO key selects page 16 of INI_ECAM_ACTIVE_PAGE, as on the
// iniBuilds A380 (A350_Interior.behavior.xml; INI_ECAM_VIDEO is 1 only while
// held; bound live 2026-11-05).
inline constexpr std::array<double, 6> A350DoorMount{-0.4, 2.8, 24, -57, 179.88, 1.2};
inline constexpr std::array<double, 6> A35KDoorMount{-0.4, 2.8, 27.81, -57, 179.88, 1.2};
inline constexpr AircraftProfile A359 = [] {
  AircraftProfile p{2,
                    "ini-a350-900",
                    L"iniBuilds A350-900 / ULR",
                    {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                    {"", ""},
                    {"$EFIS_LEFT", "$EFIS_RIGHT"},
                    {{{0, -2, 16, -15, 0, 0.55}, {0, 10, -33.0, -15, 0, 0.62}, {0, 10, -33.0, -15, 0, 0.62}, A350DoorMount}},
                    1644,
                    1024,
                    0,
                    TaxiControl::lvar_off,
                    {"A359", "A359 ULR"},
                    {{{0, 0, 806, 763}, {838, 0, 1644, 763}}},
                    {{{774, 251}, {774, 496}, {774, 496}, DoorPane}},
                    true,
                    A350Etacs,
                    {28, 29, 87, 91, 27, 90},
                    {"inibuilds-aircraft-a350", "presets/inibuilds", "attachments/inibuilds"}};
  p.pfd_refresh_hz = A350PfdRefreshHz;
  // Function_Interior_A350 panel.cfg: $EFIS_LEFT, $EFIS_RIGHT, $SD (live marks 2026-11-04).
  p.panel_textures = {"@EFIS#0/2", "@EFIS#1/2", ""};
  p.door_lower_texture = "@CTR";
  p.door_lower_area = A350DoorArea;
  p.door_lvar = "INI_ECAM_ACTIVE_PAGE";
  p.door_lvar_trigger = standalone::lvar_equals_trigger(16);
  return p;
}();
inline constexpr AircraftProfile A35K = [] {
  AircraftProfile p{3,
                    "ini-a350-1000",
                    L"iniBuilds A350-1000",
                    {"L:INI_TAXI_LEFT", "L:INI_TAXI_RIGHT"},
                    {"", ""},
                    {"$EFIS_LEFT", "$EFIS_RIGHT"},
                    {{{0, -2, 19.81, -15, 0, 0.55}, {0, 10, -36.17, -15, 0, 0.62}, {0, 10, -36.17, -15, 0, 0.62}, A35KDoorMount}},
                    1644,
                    1024,
                    0,
                    TaxiControl::lvar_off,
                    {"A35K"},
                    {{{0, 0, 806, 763}, {838, 0, 1644, 763}}},
                    {{{774, 251}, {774, 496}, {774, 496}, DoorPane}},
                    true,
                    A350EtacsA35K,
                    {28, 29, 87, 91, 27, 90},
                    {"inibuilds-aircraft-a350", "presets/inibuilds", "attachments/inibuilds"}};
  p.pfd_refresh_hz = A350PfdRefreshHz;
  // Function_Interior_A350 panel.cfg: $EFIS_LEFT, $EFIS_RIGHT, $SD (live marks 2026-11-04).
  p.panel_textures = {"@EFIS#0/2", "@EFIS#1/2", ""};
  p.door_lower_texture = "@CTR";
  p.door_lower_area = A350DoorArea;
  p.door_lvar = "INI_ECAM_ACTIVE_PAGE";
  p.door_lvar_trigger = standalone::lvar_equals_trigger(16);
  return p;
}();
inline constexpr std::array<double, 6> IniA380DoorMount{-0.6, 8, 20, -64.54, 158.19, 1.2};
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
  p.mounts = {{{0, 2.2, 16, -17.5, 0, 1}, {0, 18, -34, -32, 0, 1}, {0, 18, -34, -32, 0, 1}, DoorMount}};
  p.composition.tail_upper = {0.34f, 0.52f};
  p.composition.tail_corner = {0.305f, 0.65f};
  p.composition.tail_inner = {0.355f, 0.65f};
  // The aircraft draws its PFDs in one-mip typeless targets and then into
  // five-mip 768 x 1024 panel textures, which the cameras are drawn on. Its
  // package is encrypted, so the keys come from Robert's choice in Displays
  // (2026-11-04, routed and Ready live): the two PFD panels' first gauge is
  // ini-crash-report, ranked by panel number, and the system display is the
  // fourth of the six DU panels.
  p.mips = 1;
  p.panel_mips = 5;
  p.panel_textures = {"@ini-crash-report#0/2", "@ini-crash-report#1/2", ""};
  p.door_lower_texture = "@DU#3/6";
  // Door: Robert's live iniBuilds A380 door camera (2026-11-02), in black and
  // white in the picture of the system display's VIDEO page, between its tabs
  // and its TAT/SAT strip. The ECAM control panel's VIDEO key selects page 16
  // of INI_ECAM_ACTIVE_PAGE (read live; INI_ECAM_VIDEO stays 0).
  p.door_lower_area = {0, 84, 768, 655};
  p.mounts[DoorFeed] = IniA380DoorMount;
  p.door_lvar = "INI_ECAM_ACTIVE_PAGE";
  p.door_lvar_trigger = standalone::lvar_equals_trigger(16);
  p.black_and_white = 1u << DoorFeed;
  p.formats = {28, 29, 87, 91, 27, 90};
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
// 777-200ER, 777-300ER, and 777F files; the 777-200LR shares the 200ER cockpit.
// The navigation display is the whole inboard gauge, so these rectangles are
// not cropped further.
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
// Separate settings files per airplane folder (200ER / 200LR / 300ER / 777F).
// They share this cockpit layout on DUS. AircraftLoaded selects the airplane folder. ATC
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
// Door: Robert's live 777-200ER door camera (2026-11-02), behind the cockpit
// door looking aft and down the aisle, on the lower DU. The 300ER's moves
// forward with its nose camera (6 m); not checked live.
inline constexpr std::array<double, 6> Pmdg777DoorMount{-0.6, 3.2, 23.5, -54.86, -178.77, 1.2};
inline constexpr std::array<double, 6> Pmdg777300DoorMount{-0.6, 3.2, 29.5, -54.86, -178.77, 1.2};
inline constexpr CameraMounts Pmdg777Mounts{
    {{0, -2, 16, -18, 0, 1}, {-6, 1.5, -28, -5, -12, 0.6}, {6, 1.5, -28, -5, 12, 0.6}, Pmdg777DoorMount}};
inline constexpr CameraMounts Pmdg777300Mounts{{{0, -2, 22, -18, 0, 1}, Pmdg777Mounts[1], Pmdg777Mounts[2], Pmdg777300DoorMount}};
inline constexpr CameraMounts Pmdg777FMounts{{Pmdg777Mounts[0], Pmdg777Mounts[1], Pmdg777Mounts[2], Pmdg777DoorMount}};
// The 200LR has the 200ER's fuselage length, so it starts from the 200ER mounts
// until calibrated live.
inline constexpr CameraMounts Pmdg777200LRMounts = Pmdg777Mounts;
// Nose matches the 220 px picture; each bottom feed fills its tall half-pane.
inline constexpr CameraPanes Pmdg777Panes{
    {{736, static_cast<std::int32_t>((Pmdg777NosePictureHeight * 736 + 384) / 768)},
     {static_cast<std::int32_t>(Pmdg777BottomPaneWidth), static_cast<std::int32_t>(Pmdg777BottomPaneHeight)},
     {static_cast<std::int32_t>(Pmdg777BottomPaneWidth), static_cast<std::int32_t>(Pmdg777BottomPaneHeight)},
     DoorPane}};
inline constexpr auto make_pmdg_777 =
    [](std::uint32_t id, std::string_view key, const wchar_t* name, std::string_view marker, CameraMounts mounts) {
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
      p.display_layout = DisplayLayout::shared_texture;
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
      // Function_Interior panel.cfg: DUS and EICASCDU (live marks 2026-11-04).
      p.panel_textures = {"@DU_LeftInboard", "@DU_RightInboard", "@DU_Lower"};
      return p;
    };
inline constexpr AircraftProfile Pmdg777 = make_pmdg_777(5, "pmdg-777", L"PMDG 777-200ER", "PMDG 777-200ER", Pmdg777Mounts);
inline constexpr AircraftProfile Pmdg777300ER = make_pmdg_777(6, "pmdg-777-300er", L"PMDG 777-300ER", "PMDG 777-300ER", Pmdg777300Mounts);
inline constexpr AircraftProfile Pmdg777F = make_pmdg_777(7, "pmdg-777f", L"PMDG 777F", "PMDG 777F", Pmdg777FMounts);
// The 200LR's airplane folder follows the 200ER's naming (PMDG 777-200LR); its
// AircraftLoaded path has not been logged.
inline constexpr AircraftProfile Pmdg777200LR =
    make_pmdg_777(10, "pmdg-777-200lr", L"PMDG 777-200LR", "PMDG 777-200LR", Pmdg777200LRMounts);
static_assert(Pmdg777300ER.mounts[0][2] == 22);
static_assert(Pmdg777300ER.mounts[0] != Pmdg777Mounts[0]);
static_assert(Pmdg777300DoorMount[2] - Pmdg777DoorMount[2] == Pmdg777300Mounts[0][2] - Pmdg777Mounts[0][2]);
inline constexpr double A35KDoorShift = A35KDoorMount[2] - A350DoorMount[2] - (A35K.mounts[0][2] - A359.mounts[0][2]);
static_assert(A35KDoorShift < 1e-9 && A35KDoorShift > -1e-9);
static_assert(Pmdg777F.mounts[0] == Pmdg777Mounts[0]);
static_assert(Pmdg777200LR.mounts == Pmdg777Mounts);
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
// Door: Robert's live A340-600 door camera (2026-11-02) on the lower ECAM above
// its bottom strip. Its pedestal ECAM video selector has no function (its
// L:var stays 0), so the door camera has no cockpit switch.
inline constexpr std::array<double, 6> A346DoorMount{-0.4, 1.8, 33.5, -57.1, 167.58, 1.55};
inline constexpr CameraMounts A346Mounts{
    {{0, -2.5, 23.33, -15, 0, 0.55}, {0, 8.18, -28.99, -15, 0, 0.62}, {0, 8.18, -28.99, -15, 0, 0.62}, A346DoorMount}};
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
  p.door_lower_area = {A346SdX, A346SdY, A346SdX + A346DisplayUnit, 1873};
  p.composition = A346Composition;
  p.formats = {};
  p.package_markers = {"simobjects/airplanes/airbus-a346-pro", "aerosoft-aircraft-a346-pro", ""};
  p.display_layout = DisplayLayout::shared_texture;
  p.display_texture = A346Texture;
  // panel.cfg [VCockpit01] texture=$GAUGES_UNIFIED, its gauges NanoVGInstruments.
  p.panel_textures = {"@NanoVGInstruments", "@NanoVGInstruments", "@NanoVGInstruments"};
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
// Since displays are found only by panel marks (2026-11-04) the cameras draw
// on the cockpit panel texture, 2340 x 2340 with five mips: its
// DisplayManager gauge holds the same grid at 0,0, and the FMGS pages sit to
// its right. The one-mip 1560 x 2340 texture the grid is drawn on first
// carries no marks and is not copied into the panel (the bridge saw no copy),
// so name routing cannot reach it; Displays offered only the panel and
// refused it for its shape.
inline constexpr unsigned IniA343PanelSize = 2340, IniA343PanelMips = 5;
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
// Door: Robert's live A340-300 door camera (2026-11-02) on the lower ECAM
// cell above its bottom strip; its ECAM control panel's VIDEO key selects
// page 15 of INI_ECAM_ACTIVE_PAGE (read live).
inline constexpr std::array<double, 6> IniA343DoorMount{-0.5, 3.2, 23, -52.28, 163.55, 1.53};
inline constexpr AircraftProfile IniA343 = [] {
  AircraftProfile p{9,
                    "ini-a340-300",
                    L"iniBuilds A340-300",
                    {"", "", ""},
                    {"", "", ""},
                    {"", "", ""},
                    {{A359.mounts[0], A359.mounts[1], A359.mounts[2], IniA343DoorMount}},
                    IniA343PanelSize,
                    IniA343PanelSize,
                    IniA343PanelMips,
                    TaxiControl::manual_only,
                    {"", "", ""},
                    {{ini_a343_cell(1, 0), ini_a343_cell(1, 1), ini_a343_cell(1, 2)}}};
  p.sides = 3;
  p.composition = A346Composition;
  p.formats = {28, 29, 87, 91, 27, 90};
  p.package_markers = {"simobjects/airplanes/inibuilds-a340", "fs24-inibuilds-aircraft-a340", ""};
  p.display_layout = DisplayLayout::shared_texture;
  p.door_lower_area = {IniA343Cell, 2 * IniA343Cell, IniA343DisplayWidth, 2210};
  p.door_lvar = "INI_ECAM_ACTIVE_PAGE";
  p.door_lvar_trigger = standalone::lvar_equals_trigger(15);
  // Its panel's marks (2026-11-04): VCockpit02 gauge 0 DisplayManager, the
  // only gauge of that name.
  p.panel_textures = {"@DisplayManager", "@DisplayManager", "@DisplayManager"};
  return p;
}();
inline constexpr std::array<const AircraftProfile*, 10> Catalog{&A380,         &A359,         &A35K,     &IniA380,      &Pmdg777,
                                                                &Pmdg777200LR, &Pmdg777300ER, &Pmdg777F, &AerosoftA346, &IniA343};
inline constexpr DisplayRect display_rect(const AircraftProfile& p, unsigned side) noexcept {
  return p.display_regions[side < p.sides && side < MaxDisplaySides ? side : 0];
}
// The camera inset of a display area; empty when the padding leaves none.
inline constexpr DisplayRect inset_rect(DisplayRect outer, DisplayInsets padding) noexcept {
  if (outer.right <= outer.left || outer.bottom <= outer.top)
    return {};
  const auto width = outer.right - outer.left, height = outer.bottom - outer.top;
  if (padding.left >= width || padding.right >= width - padding.left || padding.top >= height || padding.bottom >= height - padding.top)
    return {};
  return {outer.left + padding.left, outer.top + padding.top, outer.right - padding.right, outer.bottom - padding.bottom};
}
inline constexpr DisplayRect display_content_rect(const AircraftProfile& p, unsigned side) noexcept {
  return inset_rect(display_rect(p, side), p.camera_padding);
}
// A side's display area on the width x height texture routed to it: the
// profile's region, or the whole texture on a set-up aircraft.
inline constexpr DisplayRect side_rect(const AircraftProfile& p, unsigned side, unsigned width, unsigned height) noexcept {
  if (!p.fit_texture)
    return display_rect(p, side);
  // A chosen area, kept on the texture; else (or if nothing is left) all of it.
  const auto a = side < MaxDisplaySides ? p.areas[side] : DisplayRect{};
  const DisplayRect kept{a.left < width ? a.left : width, a.top < height ? a.top : height, a.right < width ? a.right : width,
                         a.bottom < height ? a.bottom : height};
  return kept.right > kept.left && kept.bottom > kept.top ? kept : DisplayRect{0, 0, width, height};
}
// The largest rectangle of width / height `aspect` centred in r. r itself for
// no aspect, or one within half a percent of r's.
inline constexpr DisplayRect fit_rect(DisplayRect r, float aspect) noexcept {
  if (!(aspect > 0) || r.right <= r.left || r.bottom <= r.top)
    return r;
  const double width = r.right - r.left, height = r.bottom - r.top, own = width / height;
  if (own > aspect * 0.995 && own < aspect * 1.005)
    return r;
  if (own > aspect) {
    const auto fitted = static_cast<unsigned>(height * aspect + 0.5), left = r.left + (r.right - r.left - fitted) / 2;
    return {left, r.top, left + fitted, r.bottom};
  }
  const auto fitted = static_cast<unsigned>(width / aspect + 0.5), top = r.top + (r.bottom - r.top - fitted) / 2;
  return {r.left, top, r.right, top + fitted};
}
// The camera area of a side: the taxi picture at the cameras' shape, or for
// the door camera (door) the whole area, which its pane takes (shape_door).
inline constexpr DisplayRect side_content_rect(const AircraftProfile& p,
                                               unsigned side,
                                               unsigned width,
                                               unsigned height,
                                               bool door = false) noexcept {
  return fit_rect(inset_rect(side_rect(p, side, width, height), p.camera_padding),
                  side < MaxDisplaySides && !door ? p.fit_aspect[side] : 0);
}
// Camera views are 32 to 2048 pixels on each edge (view_resize.cpp).
inline constexpr std::int32_t MinCameraPane = 32, MaxCameraPane = 2048;
// The cameras rendered at the shape of side 0's camera area instead of
// stretched to it. `panes` were made for a camera area of `reference`'s size
// with overlays at `ui_aspect`; `ratios` are each side's own area shape over
// side 0's when the profile was made (1 where every side shares side 0's). The
// panes scale with the area (all by one factor when an edge would pass the
// engine's limit), the overlays keep their proportions, and other sides show
// the picture at its shape inside their areas. An area of the reference size
// keeps everything as it is.
inline void shape_cameras(AircraftProfile& p,
                          const CameraPanes& panes,
                          DisplayRect reference,
                          float ui_aspect,
                          const std::array<float, MaxDisplaySides>& ratios) noexcept {
  p.camera_panes = panes;
  p.composition.ui_aspect = ui_aspect;
  p.fit_aspect = {};
  const auto texture = p.fit_texture ? p.side_shapes[0] : std::array<unsigned, 3>{p.width, p.height, p.mips};
  const auto content = inset_rect(side_rect(p, 0, texture[0], texture[1]), p.camera_padding);
  const double width = content.right - content.left, height = content.bottom - content.top;
  const double reference_width = reference.right - reference.left, reference_height = reference.bottom - reference.top;
  if (!(width > 0 && height > 0 && reference_width > 0 && reference_height > 0))
    return;
  const double aspect = width / height;
  for (unsigned side = 1; side < p.sides && side < MaxDisplaySides; ++side)
    p.fit_aspect[side] = static_cast<float>(aspect * ratios[side]);
  if (width == reference_width && height == reference_height)
    return;
  // The door camera takes its own display's shape (shape_door).
  const std::size_t shaped = DoorFeed;
  double scale_x = width / reference_width, scale_y = height / reference_height, largest = 0;
  for (std::size_t i = 0; i < shaped; ++i)
    largest = std::max({largest, panes[i][0] * scale_x, panes[i][1] * scale_y});
  if (largest > MaxCameraPane) {
    scale_x *= MaxCameraPane / largest;
    scale_y *= MaxCameraPane / largest;
  }
  for (std::size_t i = 0; i < shaped; ++i)
    p.camera_panes[i] = {std::clamp(static_cast<std::int32_t>(panes[i][0] * scale_x + 0.5), MinCameraPane, MaxCameraPane),
                         std::clamp(static_cast<std::int32_t>(panes[i][1] * scale_y + 0.5), MinCameraPane, MaxCameraPane)};
  p.composition.ui_aspect = static_cast<float>(ui_aspect * aspect / (reference_width / reference_height));
}
// The door camera rendered at the shape of the camera area of the side it
// shows on (MaxDisplaySides or beyond: none, keeping DoorPane), with
// DoorPane's pixel count so a new shape costs no more to render.
inline void shape_door(AircraftProfile& p, unsigned side) noexcept {
  p.camera_panes[DoorFeed] = DoorPane;
  if (side >= p.sides || side >= MaxDisplaySides)
    return;
  const auto texture = p.fit_texture ? p.side_shapes[side] : std::array<unsigned, 3>{p.width, p.height, p.mips};
  const auto content = side_content_rect(p, side, texture[0], texture[1], true);
  if (content.right <= content.left || content.bottom <= content.top)
    return;
  const double aspect = double(content.right - content.left) / double(content.bottom - content.top);
  const double pixels = double(DoorPane[0]) * DoorPane[1], own = double(DoorPane[0]) / DoorPane[1];
  if (aspect > own * 0.995 && aspect < own * 1.005)
    return;
  double width = std::sqrt(pixels * aspect), height = std::sqrt(pixels / aspect);
  if (const double largest = std::max(width, height); largest > MaxCameraPane) {
    width *= MaxCameraPane / largest;
    height *= MaxCameraPane / largest;
  }
  p.camera_panes[DoorFeed] = {std::clamp(static_cast<std::int32_t>(width + 0.5), MinCameraPane, MaxCameraPane),
                              std::clamp(static_cast<std::int32_t>(height + 0.5), MinCameraPane, MaxCameraPane)};
}
// Cockpit panel textures: multi-mip R8G8B8A8_UNORM render targets, drawn
// through UNORM or sRGB views.
inline constexpr unsigned MaxPanelEdge = 16384;
inline constexpr bool panel_texture_shape(unsigned width, unsigned height, unsigned mips, unsigned format) noexcept {
  return width && height && width <= MaxPanelEdge && height <= MaxPanelEdge && mips >= 2 && mips <= 12 && (format == 28 || format == 29);
}
inline constexpr bool matches_display(const AircraftProfile& p, unsigned width, unsigned height, unsigned mips, unsigned format) noexcept {
  if (p.fit_texture)
    return panel_texture_shape(width, height, mips, format);
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
// Name routing: a texture a named side may resolve to. On a set-up aircraft
// that is one of the panel shapes the user picked, not any panel texture.
inline constexpr bool named_display(const AircraftProfile& p, unsigned width, unsigned height, unsigned mips, unsigned format) noexcept {
  if (!p.fit_texture)
    return matches_display(p, width, height, mips, format);
  if (!panel_texture_shape(width, height, mips, format))
    return false;
  for (unsigned side = 0; side < p.sides && side < MaxDisplaySides; ++side)
    if (p.side_shapes[side] == std::array<unsigned, 3>{width, height, mips})
      return true;
  return false;
}
// A pilot pair's placed lower display (named_lower_side) is any cockpit panel
// its name picks, such as the A380X's 1646 x 1024 MFD for the door camera. Its
// pilot displays keep their own shape.
inline constexpr bool lower_any_panel(const AircraftProfile& p) noexcept {
  return named_lower_side(p);
}
// A texture some side may be drawn on, once routed there: the display shape,
// or any panel texture where the placed lower display can be one.
inline constexpr bool drawable(const AircraftProfile& p, unsigned width, unsigned height, unsigned mips, unsigned format) noexcept {
  return matches_display(p, width, height, mips, format) || (lower_any_panel(p) && panel_texture_shape(width, height, mips, format));
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
// An aircraft the user set up (discovered.hpp). Its profile is built at run
// time from the saved layout; each layout gets a new immutable profile that
// is never freed, so a reader holding an older one stays valid. find()
// returns the newest, or nullptr before any is published in this process.
inline constexpr std::uint32_t DiscoveredProfileId = 100;
inline std::atomic<const AircraftProfile*> discovered_profile{};
// The selected profile with the user's display choices (discovered.hpp
// DisplayOverride): a copy of a catalog profile or of the set-up aircraft's,
// never freed. One profile at a time has one.
inline std::atomic<const AircraftProfile*> override_profile{};
// The profile without the user's display choices.
inline const AircraftProfile* base_profile(std::uint32_t id) noexcept {
  if (id == DiscoveredProfileId)
    return discovered_profile.load(std::memory_order_acquire);
  for (auto* p : Catalog)
    if (p->id == id)
      return p;
  return nullptr;
}
inline const AircraftProfile* find(std::uint32_t id) noexcept {
  if (const auto* chosen = override_profile.load(std::memory_order_acquire); chosen && chosen->id == id)
    return chosen;
  return base_profile(id);
}
// A camera view's output: the panes of a catalog profile, or the ones shaped
// for a set-up aircraft's panels or the user's display areas (shape_cameras).
inline bool camera_candidate(unsigned width, unsigned height) noexcept {
  const auto pane_of = [&](const AircraftProfile* profile) {
    if (profile)
      for (const auto& pane : profile->camera_panes)
        if (width == static_cast<unsigned>(pane[0]) && height == static_cast<unsigned>(pane[1]))
          return true;
    return false;
  };
  for (const auto* profile : Catalog)
    if (pane_of(profile))
      return true;
  return pane_of(discovered_profile.load(std::memory_order_acquire)) || pane_of(override_profile.load(std::memory_order_acquire));
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
  // pmdg-aircraft-77er. The 200LR, 300ER and 777F use the same folder-component
  // pattern; those paths were not in this log.
  // The Aerosoft A346 and the iniBuilds A340-300 also report the Airbus brand
  // string; their SimObject folders identify the product. The live A340-300
  // path was SimObjects\Airplanes\inibuilds-a340\presets\inibuilds\a340-300_eis2\...
  for (const auto* profile : {&Pmdg777, &Pmdg777200LR, &Pmdg777300ER, &Pmdg777F, &AerosoftA346, &IniA343}) {
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
  // A set-up aircraft is its exact variant only (aircraft_variant.hpp).
  if (const auto* discovered = discovered_profile.load(std::memory_order_acquire); !match && discovered)
    return same_variant(path, discovered->key) ? DiscoveredProfileId : 0;
  return match;
}
// Legacy adapter default. Native sessions choose their explicit profile through
// the companion and pass it into control and display adapters.
inline const AircraftProfile& active() noexcept {
  return A380;
}
}  // namespace taxi_camera::profiles
