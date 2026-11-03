#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define wWinMain companion_entry_unused
#include "../../src/app/companion.cpp"
#undef wWinMain
#include <cstdio>
#include <stdexcept>

namespace {
unsigned checks{};
void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}
LRESULT CALLBACK host(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_COMMAND || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX || m == WM_DRAWITEM)
    return procedure(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}
void command(int id) {
  procedure(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}
// Every change saves by itself once edits pause (there is no Save button):
// the pending autosave, made due now.
void save_now() {
  dirty = true;
  autosave_due = 1;
  autosave();
}
void marking_controls_tests() {
  for (const auto* profile : {&profiles::A359, &profiles::A35K}) {
    current = {};
    current.profile = profile->id;
    current.auto_profile = 0;
    current.mounts = profile->mounts;
    current.mounts[0][1] += 0.125;
    current.speed_color = {0.25f, 0.5f, 0.75f};
    win::reset_guide_settings(current, *profile);
    page = 5;
    build_controls();
    wchar_t label[64]{};
    GetDlgItemTextW(window, 372, label, 64);
    require(GetDlgItem(window, 370) && GetDlgItem(window, 371) && GetDlgItem(window, 372) && !GetDlgItem(window, 231) &&
                std::wstring(label) == L"Marking colour",
            "Reference guides offers a separate Marking colour picker beside Apply and Reset");
    RECT marking{}, reset{}, client{};
    GetWindowRect(GetDlgItem(window, 372), &marking);
    GetWindowRect(GetDlgItem(window, 371), &reset);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&marking), 2);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&reset), 2);
    GetClientRect(window, &client);
    require(marking.left >= reset.right && marking.right <= client.right && marking.bottom <= client.bottom,
            "Marking colour control fits without overlapping Reset");
    SetDlgItemTextW(window, 360, L"25");
    SetDlgItemTextW(window, 361, L"50");
    require(apply(false), "Colour selection applies pending guide coordinates first");
    const auto before = current;
    const COLORREF marking_color = RGB(51, 102, 204);
    // Exercise the production picker completion without opening a modal dialog.
    require(apply_color_selection(before, true, marking_color), "Marking picker result applies to its opening profile session");
    require(current.guide_color == std::array<float, 3>{51 / 255.f, 102 / 255.f, 204 / 255.f} && dirty &&
                current.speed_color == before.speed_color && current.nose_dot == before.nose_dot &&
                current.tail_corner == before.tail_corner && current.mounts == before.mounts,
            "Marking selection updates the live draft and preserves ground-speed colour and calibration");
    const auto dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = 1055;
    bitmap_info.bmiHeader.biHeight = -750;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    void* pixels{};
    const auto bitmap = CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(dc && bitmap && pixels, "Create an offscreen surface for the own-window marking preview");
    const auto old_bitmap = SelectObject(dc, bitmap);
    draw_page(dc);
    bool matching = true;
    for (const bool right : {false, true}) {
      const int x = 817 + static_cast<int>(std::lround((right ? 1 - current.nose_dot[0] : current.nose_dot[0]) * 172));
      const int y = 225 + static_cast<int>(std::lround(current.nose_dot[1] * 45));  // Below the step pills.
      // The corner is inside a square and outside the former circular marker.
      matching = matching && GetPixel(dc, x, y) == marking_color && GetPixel(dc, x + 6, y + 6) == marking_color;
    }
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    require(matching, "Both A350 nose previews draw squares in the selected marking colour");
    save_now();
    win::Settings loaded;
    require(win::load_settings(loaded, L"", profile->id) && loaded.guide_color == current.guide_color &&
                loaded.nose_dot == current.nose_dot && loaded.speed_color == before.speed_color && loaded.mounts == before.mounts,
            "Save retains per-aircraft marking colour and pending coordinates independently of ground-speed colour");
    const auto chosen_markings = current.guide_color;
    command(102);
    GetDlgItemTextW(window, 231, label, 64);
    require(GetDlgItem(window, 231) && !GetDlgItem(window, 372) && std::wstring(label) == L"Ground-speed colour",
            "Display keeps its own ground-speed colour picker");
    require(apply_color_selection(draft(), false, RGB(204, 153, 51)), "Ground-speed picker result applies to its opening profile session");
    require(current.speed_color == std::array<float, 3>{204 / 255.f, 153 / 255.f, 51 / 255.f} && current.guide_color == chosen_markings,
            "Ground-speed selection leaves the selected marking colour unchanged");
    const auto chosen_speed = current.speed_color;
    command(105);
    command(371);
    GetDlgItemTextW(window, 371, label, 64);
    require(std::wstring(label) == L"Reset guides" && current.guide_color == profile->composition.guide_color &&
                current.guide_color == profiles::A380.composition.guide_color && current.nose_dot == profile->composition.nose_dot &&
                current.speed_color == chosen_speed && current.mounts == before.mounts && dirty,
            "Reset guides restores profile positions and A380 magenta while preserving ground-speed colour and camera calibration");
  }
}
void stale_color_selection_tests() {
  for (const bool markings : {false, true}) {
    for (const bool reconnect : {false, true}) {
      current = {};
      current.profile = profiles::A359.id;
      current.profile_request = 12;
      const auto opened = draft();
      if (reconnect)
        ++current.profile_request;
      else
        current.profile = profiles::A35K.id;
      current.guide_color = {0.1f, 0.2f, 0.3f};
      current.speed_color = {0.4f, 0.5f, 0.6f};
      current.enabled = 0;
      current.aircraft_session_epoch = 7;
      current.mounts = profiles::A35K.mounts;
      const auto changed = draft();
      dirty = false;
      notice = L"New profile session";
      require(!apply_color_selection(opened, markings, RGB(255, 255, 255)),
              reconnect ? "Picker result is refused after same-profile reconnect" : "Picker result is refused after aircraft switch");
      require(current.profile == changed.profile && current.profile_request == changed.profile_request &&
                  current.guide_color == changed.guide_color && current.speed_color == changed.speed_color &&
                  current.mounts == changed.mounts && current.enabled == changed.enabled &&
                  current.aircraft_session_epoch == changed.aircraft_session_epoch && !dirty && notice == L"New profile session",
              "Stale marking or ground-speed selection leaves the new profile and its unsaved state unchanged");
    }
    const auto opened = draft();
    current.enabled = 1;
    ++current.aircraft_session_epoch;
    require(apply_color_selection(opened, markings, RGB(51, 102, 153)) && current.enabled == 1 &&
                current.aircraft_session_epoch == opened.aircraft_session_epoch + 1 && current.profile_request == opened.profile_request,
            "Current-session colour selection preserves concurrent enabled and aircraft-session fields");
  }
}
}  // namespace
// Setup's three steps: the pills on every page, Advanced and its pages,
// Next saving a step (its L:vars too) and going on, and the door camera's
// display and picture.
void setup_step_tests() {
  current = {};
  current.profile = profiles::A359.id;
  current.auto_profile = 0;
  current.mounts = profiles::A359.mounts;
  dirty = false;
  advanced = false;
  page = OverviewPage;
  build_controls();
  require(GetDlgItem(window, StepPillId) && GetDlgItem(window, StepPillId + 2) && !GetDlgItem(window, StepPillId + 3) &&
              !GetDlgItem(window, NextId),
          "Overview opens each of the three steps");
  command(StepPillId + 1);
  require(page == CamerasPage && GetDlgItem(window, LiveFeedId) && !GetDlgItem(window, 300) && GetDlgItem(window, 620) &&
              GetDlgItem(window, LvarNameId) && !GetDlgItem(window, 624),
          "Taxi cameras has its cameras and their controls, and its values only with Advanced");
  command(AdvancedId);
  require(advanced && GetDlgItem(window, 300) && GetDlgItem(window, GuidesOpenId), "Advanced adds the values and the guides");
  command(GuidesOpenId);
  require(page == GuidesPage && setup_step(page) == 1 && GetDlgItem(window, 370), "Guide values is Taxi cameras' Advanced page");
  command(AdvancedId);
  require(!advanced && page == CamerasPage, "Turning Advanced off returns to the step's own page");
  current.mounts[0][1] += 1;
  command(DefaultsId);
  require(current.mounts[0] == profiles::A359.mounts[0], "Defaults restores that camera's mount");
  SetDlgItemTextW(window, LvarNameId, L"NOT AN LVAR");
  command(NextId);
  require(page == CamerasPage && notice.find(L"L:var") != std::wstring::npos, "A name that is not an L:var keeps the step");
  SetDlgItemTextW(window, LvarNameId, L"A32NX_TAXI");
  command(NextId);
  win::Settings loaded;
  require(page == DoorPage && std::strcmp(current.lvar_bindings[0].name, "A32NX_TAXI") == 0 &&
              win::load_settings(loaded, L"", profiles::A359.id) && std::strcmp(loaded.lvar_bindings[0].name, "A32NX_TAXI") == 0,
          "Next saves the step with its L:vars and goes on to Door camera");
  require(GetDlgItem(window, 624) && GetDlgItem(window, LvarNameId + 4) && !GetDlgItem(window, 620),
          "Door camera has the door camera's own controls");
  command(DoorWhereId + 1);
  require(current.door_side == 0, "Where it shows picks a display");
  command(DoorLiveId);
  require(current.door_live == 1, "Live keeps the door camera rendering");
  command(DoorWhereId);
  require(current.door_side == win::DoorSideNone && !current.door_on, "None leaves the door camera without a display");
  command(BackId);
  require(page == CamerasPage, "Back goes to the step before");
  command(StepPillId + 2);
  command(NextId);
  require(page == OverviewPage && !dirty && !shortcut_window && win::load_settings(loaded, L"", profiles::A359.id) && loaded.door_live == 1,
          "The last step saves and finishes on Overview");
}
// The live view holds the door camera on, rendering live, while Door is
// selected. Saves keep the aircraft's own Live or Snapshot (the user's latest
// pick), never the forced Live, and closing gives back what it still holds.
void door_hold_save_tests() {
  win::Settings s;
  require(win::load_settings(s, L"", profiles::AerosoftA346.id) && s.door_side == 2, "The A340-600 has a door camera on its lower ECAM");
  s.auto_profile = 0;
  s.door_live = s.door_on = 0;
  require(win::save_settings(s), "Save the A340-600 as Snapshot");
  current = s;
  dirty = false;
  page = DoorPage;
  build_controls();
  live_view = {};
  live_keep_door(draft());
  require(live_view.door_taken && current.door_on && current.door_live, "Selecting Door turns it on, live");
  win::Settings loaded;
  require(apply() && win::load_settings(loaded, L"", profiles::AerosoftA346.id) && loaded.door_live == 0,
          "A save while Door is held keeps the aircraft's Snapshot");
  command(DoorLiveId);
  require(apply() && win::load_settings(loaded, L"", profiles::AerosoftA346.id) && loaded.door_live == 1,
          "Live picked during the hold is saved");
  command(DoorSnapshotId);
  live_keep_door(draft());
  require(current.door_live == 1 && apply() && win::load_settings(loaded, L"", profiles::AerosoftA346.id) && loaded.door_live == 0,
          "Snapshot picked during the hold is saved, though the view still renders live");
  // Another aircraft: the hold belongs to the A340-600, so nothing is given
  // back to the A350's settings, and closing ends the hold.
  auto other = current;
  other.profile = profiles::A359.id;
  other.door_live = 1;
  const auto released = release_door_hold(other);
  require(released.door_live == 1 && !live_view.door_taken, "Another aircraft's Live is never replaced by the hold's");
  live_view = {};
}
namespace {
// Displays in a flight whose panel names do not pair (the Fenix display boot
// of 2026-11-02 left each PFD's name on an ND): a display placed by its
// picture routes that picture's texture at once, keeps it when the step is
// left, reopened or passed with Next, and no name from the flight is saved.
void session_display_tests() {
  current = {};
  current.profile = profiles::A380.id;
  current.auto_profile = 0;
  current.mounts = profiles::A380.mounts;
  dirty = false;
  advanced = false;
  page = OverviewPage;
  build_controls();
  {
    const std::lock_guard lock(app_mutex);
    status = {};
    status.heartbeat = GetTickCount64();
    status.graphics_ready = 1;
    status.panel_discovery = 1;
    status.panel_state = static_cast<std::uint32_t>(win::PanelMapState::paired);
    status.panel_clean = 1;
    status.panel_shift = -1;
    status.panel_count = 2;
    std::strcpy(status.aircraft_path, "SimObjects/Airplanes/FlyByWire_A380_842/aircraft.cfg");
    // Each name on the other display's texture.
    const char* names[]{"SCREEN_DU_PFDL", "SCREEN_DU_PFDR"};
    for (int i = 0; i < 2; ++i) {
      std::strcpy(status.panels[i].name, names[i]);
      status.panels[i].id = 702 - i;
      status.panels[i].width = 768;
      status.panels[i].height = 1024;
      status.panels[i].mips = 5;
    }
  }
  command(StepPillId);
  require(page == SetupPage && setup.built_in == profiles::A380.id && setup.names[0] == "SCREEN_DU_PFDL" && setup.sides == 3,
          "Displays opens on the profile's own displays, with a lower display");
  require(setup_blocker(status).find(L"don't match their pictures") != std::wstring::npos,
          "Names that did not pair in the flight are not offered for saving");
  // The left PFD's picture is the texture named SCREEN_DU_PFDR here.
  setup.picked = "SCREEN_DU_PFDR";
  setup.picked_texture = 701;
  place_setup_panel(0);
  require(current.left_id == 701 && setup.textures[0] == 701, "The display placed by picture routes that picture at once");
  command(100);
  require(page == OverviewPage && current.left_id == 701, "Leaving Displays keeps this flight's route");
  command(StepPillId);
  require(page == SetupPage && current.left_id == 701, "Opening Displays again keeps it");
  command(NextId);
  require(
      page == CamerasPage && current.left_id == 701 && !current.display_override.textures[0][0] && !current.display_override.textures[1][0],
      "Next goes on without saving the flight's names, and the route stays");
  // A cockpit test ends without taking the route back.
  command(StepPillId);
  setup.picked = "SCREEN_DU_PFDL";
  setup.picked_texture = 702;
  place_setup_panel(1);
  setup.testing = 2;
  apply_setup_trial();
  command(100);
  require(current.left_id == 701 && current.right_id == 702 && !current.calibration_mask, "Ending a cockpit test keeps both routes");
  {
    const std::lock_guard lock(app_mutex);
    status = {};
  }
}
// Autosave, the setup's target and a drop on an aircraft without a profile.
void autosave_setup_tests() {
  current = {};
  current.profile = profiles::A359.id;
  current.auto_profile = 0;
  current.enabled = 1;
  current.mounts = profiles::A359.mounts;
  dirty = false;
  autosave_due = 0;
  page = 4;
  build_controls();
  SetDlgItemTextW(window, 203, L"2048");
  dirty_notice();  // As the field's EN_CHANGE does.
  require(dirty && autosave_due > GetTickCount64(), "An edit schedules its save for when typing pauses");
  autosave();
  win::Settings loaded;
  require(dirty && (!win::load_settings(loaded, L"", profiles::A359.id) || loaded.calibration_budget != 2048),
          "Nothing saves while the edit may still be typed");
  autosave_due = 1;
  autosave();
  require(!dirty && win::load_settings(loaded, L"", profiles::A359.id) && loaded.calibration_budget == 2048,
          "The edit saves by itself once due");
  // A setup's trial is not the user's profile.
  SetDlgItemTextW(window, 203, L"4096");
  dirty_notice();
  setup.trial = true;
  autosave_due = 1;
  autosave();
  require(dirty && win::load_settings(loaded, L"", profiles::A359.id) && loaded.calibration_budget == 2048,
          "The autosave leaves a setup trial unsaved");
  require(apply() && win::load_settings(loaded, L"", profiles::A359.id) && loaded.calibration_budget == 2048,
          "Applying during a setup trial does not save it either");
  setup.trial = false;
  dirty = false;
  autosave_due = 0;

  win::Settings selected;
  selected.profile = profiles::A380.id;
  win::Status sample{};
  sample.heartbeat = GetTickCount64();
  sample.identity_sample_ms = 1;
  require(!setup_built_in(selected, sample), "An aircraft without a profile is set up as itself, not the selected built-in");
  sample.detected_profile = profiles::DiscoveredProfileId;
  require(!setup_built_in(selected, sample), "So is one the bridge still knows as set up earlier in the session");
  sample.detected_profile = profiles::A380.id;
  require(setup_built_in(selected, sample) == profiles::A380.id, "The built-in profile's own aircraft changes its displays");
  sample = {};
  require(setup_built_in(selected, sample) == profiles::A380.id, "Without a fresh report the selected profile is the one changed");
  selected.profile = profiles::DiscoveredProfileId;
  require(!setup_built_in(selected, sample), "A set-up aircraft is never a built-in one");

  // A drop that completes an aircraft's displays saves its variant at once.
  current = {};
  current.profile = profiles::A380.id;
  current.auto_profile = 0;
  current.enabled = 1;
  current.mounts = profiles::A380.mounts;
  page = OverviewPage;
  build_controls();
  {
    const std::lock_guard lock(app_mutex);
    status = {};
    status.heartbeat = GetTickCount64();
    status.graphics_ready = 1;
    status.panel_discovery = 1;
    status.panel_state = static_cast<std::uint32_t>(win::PanelMapState::paired);
    status.panel_clean = 1;
    status.identity_sample_ms = status.heartbeat;
    status.panel_count = 3;
    std::strcpy(status.aircraft_path, "SimObjects/Airplanes/Test Twin/aircraft.cfg");
    const char* names[]{"$DU1", "$DU6", "$DU4"};
    for (int i = 0; i < 3; ++i) {
      std::strcpy(status.panels[i].name, names[i]);
      status.panels[i].id = 801 + static_cast<std::uint64_t>(i);
      status.panels[i].width = status.panels[i].height = 768;
      status.panels[i].mips = 5;
    }
  }
  command(StepPillId);
  require(page == SetupPage && !setup.built_in, "Displays sets up the aircraft loaded, not the selected built-in");
  setup.picked = "$DU1";
  setup.picked_texture = 801;
  place_setup_panel(0);
  const std::string_view key = "SimObjects/Airplanes/Test Twin";
  require(setup.unsaved && setup.trial && current.profile == profiles::DiscoveredProfileId && (current.manual_mask & 1),
          "One pilot display runs as the trial, its camera on, until the other is placed");
  setup.picked = "$DU6";
  setup.picked_texture = 802;
  place_setup_panel(1);
  require(!setup.unsaved && setup.stored && !setup.trial && current.profile == profiles::DiscoveredProfileId &&
              win::load_settings(loaded, L"", profiles::DiscoveredProfileId, key) && loaded.discovered.sides == 2 &&
              std::string_view(loaded.discovered.textures[0]) == "$DU1" && std::string_view(loaded.discovered.textures[1]) == "$DU6",
          "The drop that completes the pilot displays saves the variant");
  require((current.manual_mask & 3) == 3, "Both placed displays show their cameras at once");
  {
    // The bridge pairs the saved names in this flight.
    const std::lock_guard lock(app_mutex);
    status.panel_shift = 0;
  }
  setup.picked = "$DU4";
  setup.picked_texture = 803;
  place_setup_panel(2);
  require(win::load_settings(loaded, L"", profiles::DiscoveredProfileId, key) && loaded.discovered.sides == 3 && loaded.door_side == 2,
          "A newly placed lower display saves, and the door camera goes there");
  swap_pilot_displays();
  require(win::load_settings(loaded, L"", profiles::DiscoveredProfileId, key) &&
              std::string_view(loaded.discovered.textures[0]) == "$DU6" && std::string_view(loaded.discovered.textures[1]) == "$DU1",
          "The swap button exchanges the pilot displays and saves them");
  // The adjustment window takes cameras already all on as they are: one
  // turned off afterwards stays off.
  {
    win::Status sample;
    {
      const std::lock_guard lock(app_mutex);
      sample = status;
    }
    live_view = {};
    const auto* set_up = profiles::find(current.profile);
    require(set_up != nullptr, "The set-up aircraft's profile is published");
    live_keep_camera(current, sample, *set_up, 0);
    require(live_view.camera_taken && (current.manual_mask & 3) == 3, "Cameras already on are taken as they are");
    current.manual_mask &= ~2u;
    live_keep_camera(current, sample, *set_up, 0);
    require(!(current.manual_mask & 2), "A camera turned off while the window is open stays off");
    live_view = {};
  }
  // Positioning after the setup's saves: closing the window gives back the
  // cameras from before the setup (none here), though the window's first
  // pass found the previews already on.
  current.manual_mask = 3;
  current.door_on = 1;
  {
    // The displays routed, so the window's first pass runs at once.
    const std::lock_guard lock(app_mutex);
    status.heartbeat = GetTickCount64();
    status.left_id = 802;
    status.right_id = 801;
    status.lower_id = 803;
  }
  start_positioning();
  require(page == CamerasPage && live_view.window && live_view.camera_taken && live_view.manual_before == setup.manual_before &&
              live_view.door_preview,
          "Positioning hands the window the cameras from before the setup");
  DestroyWindow(live_view.window);
  require(!live_view.window && !current.manual_mask && current.door_on == setup.door_on_before,
          "Closing the window after positioning turns the previews off");
  live_view = {};

  // A built-in profile: picks save at once, and the door camera's None stays.
  current = {};
  current.profile = profiles::A380.id;
  current.auto_profile = 0;
  current.enabled = 1;
  current.mounts = profiles::A380.mounts;
  std::strcpy(current.display_override.textures[2], "SCREEN_DU_SD");
  current.door_side = win::DoorSideNone;
  require(win::valid_settings(current) && win::save_settings(current), "Save the A380X with its lower display and no door display");
  publish(current);
  page = OverviewPage;
  build_controls();
  {
    const std::lock_guard lock(app_mutex);
    status = {};
    status.heartbeat = GetTickCount64();
    status.graphics_ready = 1;
    status.panel_discovery = 1;
    status.panel_state = static_cast<std::uint32_t>(win::PanelMapState::paired);
    status.panel_clean = 1;
    status.panel_shift = 0;
    status.identity_sample_ms = status.heartbeat;
    status.detected_profile = profiles::A380.id;
    status.panel_count = 3;
    std::strcpy(status.aircraft_path, "SimObjects/Airplanes/FlyByWire_A380_842/aircraft.cfg");
    const char* names[]{"SCREEN_DU_PFDL", "SCREEN_DU_PFDR", "SCREEN_DU_SD"};
    for (int i = 0; i < 3; ++i) {
      std::strcpy(status.panels[i].name, names[i]);
      status.panels[i].id = 901 + static_cast<std::uint64_t>(i);
      status.panels[i].width = 768;
      status.panels[i].height = 1024;
      status.panels[i].mips = 5;
    }
  }
  command(StepPillId);
  require(page == SetupPage && setup.built_in == profiles::A380.id && setup.names[2] == "SCREEN_DU_SD",
          "Displays opens on the A380X with its lower display");
  swap_pilot_displays();
  require(win::load_settings(loaded, L"", profiles::A380.id) && std::string_view(loaded.display_override.textures[0]) == "SCREEN_DU_PFDR" &&
              loaded.door_side == win::DoorSideNone,
          "A built-in swap saves at once and keeps the door camera's None");
  // A pick made during a cockpit test saves on leaving Displays.
  setup.testing = 1;
  swap_pilot_displays();
  require(setup.unsaved, "A pick during a cockpit test waits to be saved");
  command(100);
  require(page == OverviewPage && win::load_settings(loaded, L"", profiles::A380.id) && !loaded.display_override.textures[0][0] &&
              !setup.testing && !current.calibration_mask,
          "Leaving Displays saves it and ends the test");
  {
    const std::lock_guard lock(app_mutex);
    status = {};
  }
  setup.trial = false;
  setup.testing = 0;
  page = OverviewPage;
  const auto variant = win::settings_override + L"\\profiles\\discovered\\Airplanes\\Test Twin";
  DeleteFileW((variant + L"\\variant.ini").c_str());
  DeleteFileW((variant + L"\\layout.ini").c_str());
  RemoveDirectoryW(variant.c_str());
  RemoveDirectoryW((win::settings_override + L"\\profiles\\discovered\\Airplanes").c_str());
  RemoveDirectoryW((win::settings_override + L"\\profiles\\discovered").c_str());
}
}  // namespace
int main() {
  try {
    instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = host;
    type.lpszClassName = L"TaxiDiagnosticsFixture";
    require(RegisterClassW(&type) != 0, "Register own hidden fixture");
    window =
        CreateWindowW(type.lpszClassName, L"Hidden diagnostic controls", WS_POPUP, 0, 0, 1055, 750, nullptr, nullptr, instance, nullptr);
    require(window != nullptr, "Create own hidden window");
    dpi = 96;
    make_fonts();
    background_brush = CreateSolidBrush(Background);
    card_brush = CreateSolidBrush(Card);
    wchar_t temporary[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temporary) != 0, "Get isolated settings parent");
    win::settings_override = std::wstring(temporary) + L"TaxiDiagnosticsFixture-" + std::to_wstring(GetCurrentProcessId());
    current.enabled = 0;
    current.auto_profile = 0;
    page = 0;
    build_controls();
    wchar_t connection_label[96]{};
    GetDlgItemTextW(window, 241, connection_label, 96);
    require(GetDlgItem(window, 241) && !GetDlgItem(window, 220) && !GetDlgItem(window, 242) && std::wstring(connection_label) == L"Connect",
            "Overview has one Connect control without Service or Reconnect controls");
    const auto saved_mounts = current.mounts;
    const auto saved_auto_connect = auto_connect.load();
    command(241);
    GetDlgItemTextW(window, 241, connection_label, 96);
    require(current.enabled && current.profile_request == 1 && connection_requested.load() && !connection_disconnected.load() &&
                std::wstring(connection_label) == L"Disconnect" && connect_commands.take() == win::ConnectCommand::connect,
            "Connect enables a legacy disabled profile and becomes Disconnect");
    win::Mailbox fixture_mailbox;
    require(fixture_mailbox.open(GetCurrentProcessId(), true) && exchange_control(fixture_mailbox), "Publish enabled state to own IPC");
    require(fixture_mailbox.data()->settings.enabled && fixture_mailbox.data()->owner_heartbeat,
            "Connected command publishes enabled settings and a live heartbeat");
    current.manual_mask = current.calibration_mask = 3;
    current.scene_test = 1;
    current.taxi_request = 12;
    current.taxi_selected_mask = 3;
    current.taxi_desired_mask = 2;
    simulator_pid = GetCurrentProcessId();  // The fixture owns this mailbox; no simulator is touched.
    command(241);
    simulator_pid = 0;
    GetDlgItemTextW(window, 241, connection_label, 96);
    require(!current.enabled && current.profile_request == 1 && !connection_requested.load() && connection_disconnected.load() &&
                std::wstring(connection_label) == L"Connect" && connect_commands.take() == win::ConnectCommand::disconnect,
            "Disconnect stops operation, suppresses retries, and restores the Connect label");
    require(!current.manual_mask && !current.calibration_mask && !current.scene_test && !current.taxi_selected_mask &&
                !current.taxi_desired_mask && current.mounts == saved_mounts && auto_connect.load() == saved_auto_connect,
            "Disconnect clears temporary requests without changing calibration or Auto-connect preference");
    require(!fixture_mailbox.data()->settings.enabled && !fixture_mailbox.data()->owner_heartbeat &&
                !fixture_mailbox.data()->settings.manual_mask,
            "Disconnect immediately publishes stopped output to the owned channel");
    require(exchange_control(fixture_mailbox) && !fixture_mailbox.data()->settings.enabled && !fixture_mailbox.data()->owner_heartbeat,
            "A subsequent worker exchange cannot revive disconnected output");
    command(241);
    require(current.enabled && current.profile_request == 2 && !connection_disconnected.load() &&
                connect_commands.take() == win::ConnectCommand::connect && exchange_control(fixture_mailbox) &&
                fixture_mailbox.data()->settings.enabled && fixture_mailbox.data()->owner_heartbeat,
            "Reconnect restores enabled output after Disconnect");
    auto old_disabled_profile = current;
    old_disabled_profile.enabled = 0;
    publish(old_disabled_profile);
    require(current.enabled, "Loading an old disabled profile cannot override an active connection");
    command(241);
    connect_commands.take();
    fixture_mailbox.close();
    current = {};
    current.profile = 2;
    current.auto_profile = 0;
    current.mounts = profiles::A359.mounts;
    current.follow_taxi = 0;
    current.manual_mask = 1;
    page = 4;
    build_controls();
    const auto original = current;
    require(GetDlgItem(window, 229) && GetDlgItem(window, 203) && GetDlgItem(window, 511),
            "Scene, calibration budget and stop controls remain available");
    // First camera only never produced camera output; its control is gone.
    require(!GetDlgItem(window, 228), "First-camera control is absent");
    require(!GetDlgItem(window, 232), "Temporary graphics control is absent");
    command(229);
    require(current.scene_test == 1 && is_on(229, current), "Scene control enables scene testing");
    require(current.manual_mask == original.manual_mask && current.mounts == original.mounts && current.nose_dot == original.nose_dot &&
                current.tail_corner == original.tail_corner && current.speed_color == original.speed_color,
            "Scene control preserves previews, calibration and display settings");
    wchar_t label[96]{};
    GetDlgItemTextW(window, 229, label, 96);
    require(std::wstring(label) == L"Scene test: On", "Scene control label reflects its enabled state");
    require(!GetDlgItem(window, 235), "Dynamic tail rate has no control");
    // Camera weather is always on: no control on the Camera views page.
    page = 1;
    build_controls();
    require(!GetDlgItem(window, 236), "Camera weather has no control");
    // The camera views always take the main view's lighting: the Display page
    // has no lighting toggle or automatic exposure, only the camera mode cards
    // and custom target, the day and night camera brightness and the
    // ground-speed colour.
    page = 2;
    build_controls();
    require(!GetDlgItem(window, 237) && !GetDlgItem(window, 222) && !GetDlgItem(window, 200) && GetDlgItem(window, 201) &&
                GetDlgItem(window, 202) && GetDlgItem(window, 231) && GetDlgItem(window, 205) && GetDlgItem(window, 270) &&
                GetDlgItem(window, 274) && !GetDlgItem(window, 210),
            "Display has the mode, target and brightness controls but no lighting or automatic exposure controls");
    {
      using taxi_camera::CameraMode;
      const auto target = GetDlgItem(window, 205);
      require(current.camera_mode == static_cast<std::uint32_t>(taxi_camera::kDefaultCameraMode) && !IsWindowEnabled(target) &&
                  selected_camera_mode() == static_cast<unsigned>(CameraMode::automatic),
              "Auto is chosen by default and the custom target is off");
      for (unsigned mode = 0; mode < taxi_camera::kCameraModeCount; ++mode)
        require(GetDlgItem(window, 270 + static_cast<int>(mode)) != nullptr, "Every camera mode has a card");
      RECT first{}, second{};
      GetWindowRect(GetDlgItem(window, 270 + static_cast<int>(CameraMode::automatic)), &first);
      GetWindowRect(GetDlgItem(window, 270 + static_cast<int>(CameraMode::performance)), &second);
      require(first.left < second.left, "The Auto card comes first");
      command(270 + static_cast<int>(CameraMode::custom));
      require(IsWindowEnabled(target) && dirty, "The Custom card enables the target slider and marks the page unsaved");
      SendMessageW(target, TBM_SETPOS, TRUE, 30);
      auto edited = current;
      require(read_fields(edited) && edited.camera_mode == static_cast<std::uint32_t>(CameraMode::custom) && edited.camera_rate == 30,
              "The Custom card and its target are read with the page");
      command(270 + static_cast<int>(CameraMode::smooth));
      edited = current;
      require(!IsWindowEnabled(target) && read_fields(edited) && edited.camera_mode == static_cast<std::uint32_t>(CameraMode::smooth) &&
                  edited.camera_rate == 30,
              "A preset card disables the target but keeps it for Custom");
      dirty = false;
      build_controls();
      require(selected_camera_mode() == current.camera_mode, "Rebuilding the page restores the saved mode");
    }
    GetDlgItemTextW(window, 201, label, 96);
    require(std::wstring(label) == L"+0.00", "Daytime brightness starts at 0 EV");
    {
      auto edited = current;
      SetDlgItemTextW(window, 201, L"-1.25");
      SetDlgItemTextW(window, 202, L"+2");
      require(read_fields(edited) && edited.day_brightness == -1.25f && edited.night_brightness == 2,
              "Brightness fields read in 0.25 EV steps");
      for (const wchar_t* rejected : {L"0.1", L"-4.25", L"3", L"x"}) {
        auto refused = current;
        SetDlgItemTextW(window, 202, rejected);
        require(!read_fields(refused) && refused.night_brightness == current.night_brightness,
                "Brightness outside -4 to +2 EV or off a step is refused");
      }
      SetDlgItemTextW(window, 201, L"+0.00");
      SetDlgItemTextW(window, 202, L"+0.00");
    }
    page = 4;
    build_controls();
    SetDlgItemTextW(window, 203, L"1024");
    save_now();
    require(current.scene_test == 1 && current.calibration_budget == 1024, "Save applies the calibration budget without stopping scenes");
    win::Settings loaded;
    require(win::load_settings(loaded, L"", 2) && loaded.calibration_budget == 1024 && !loaded.scene_test &&
                loaded.mounts == original.mounts && loaded.nose_dot == original.nose_dot,
            "Saved calibration settings reload with temporary scene testing off");
    command(103);
    command(226);
    require(current.calibration_mask == 1 && !current.manual_mask && !current.follow_taxi,
            "Left calibration replaces manual preview routing");
    command(227);
    require(current.calibration_mask == 3, "Both calibration targets remain independently selectable");
    command(226);
    require(current.calibration_mask == 2 && !current.follow_taxi, "Remaining right calibration keeps automatic control paused");
    command(227);
    require(!current.calibration_mask && !current.manual_mask && current.follow_taxi,
            "Switching off the last right calibration target restores TAXI buttons");
    command(226);
    command(227);
    command(227);
    require(current.calibration_mask == 1 && !current.follow_taxi, "Remaining left calibration keeps automatic control paused");
    command(226);
    require(!current.calibration_mask && current.follow_taxi, "Switching off the last left calibration target restores TAXI buttons");
    command(226);
    command(224);
    require(current.manual_mask == 1 && !current.calibration_mask, "Preview control exits calibration");
    command(104);
    current.calibration_mask = 2;
    command(511);
    require(!current.manual_mask && !current.scene_test && !current.calibration_mask && !current.follow_taxi,
            "Stop camera tests clears preview, scene and calibration requests");
    require(current.calibration_budget == 1024 && current.mounts == original.mounts && current.nose_dot == original.nose_dot &&
                current.tail_corner == original.tail_corner,
            "Stopping tests preserves calibration values and guides");
    command(229);
    current.manual_mask = 1;
    current.calibration_mask = 2;
    status.heartbeat = GetTickCount64();
    status.aircraft_session_epoch = current.aircraft_session_epoch + 1;
    sync_aircraft_session();
    require(!current.manual_mask && !current.calibration_mask && !current.scene_test &&
                current.aircraft_session_epoch == status.aircraft_session_epoch,
            "Flight-session change clears temporary camera requests");
    GetDlgItemTextW(window, 229, label, 96);
    require(std::wstring(label) == L"Scene test: Off", "Session reset refreshes the scene control without rebuilding fields");
    command(229);
    command(100);
    SendDlgItemMessageW(window, 210, CB_SETCURSEL, 0, 0);
    procedure(window, WM_COMMAND, MAKEWPARAM(210, CBN_SELENDOK), 0);
    require(current.profile == 1 && !current.manual_mask && !current.calibration_mask && !current.scene_test,
            "Profile selection clears temporary camera requests");
    marking_controls_tests();
    stale_color_selection_tests();
    setup_step_tests();
    door_hold_save_tests();
    session_display_tests();
    autosave_setup_tests();
    DestroyWindow(window);
    for (const auto* profile : profiles::Catalog) {
      win::Settings saved;
      saved.profile = profile->id;
      DeleteFileW(win::settings_path(saved).c_str());
    }
    DeleteFileW((win::settings_override + L"\\settings.ini").c_str());
    DeleteFileW((win::settings_override + L"\\hotkeys.ini").c_str());
    RemoveDirectoryW((win::settings_override + L"\\profiles").c_str());
    RemoveDirectoryW(win::settings_override.c_str());
    std::printf("PASS production diagnostic controls: %u checks; hidden own-window only.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    if (window)
      DestroyWindow(window);
    std::fprintf(stderr, "FAIL diagnostics: %s\n", error.what());
    return 1;
  }
}
