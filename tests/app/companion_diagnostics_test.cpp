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
      const int y = 191 + static_cast<int>(std::lround(current.nose_dot[1] * 45));
      // The corner is inside a square and outside the former circular marker.
      matching = matching && GetPixel(dc, x, y) == marking_color && GetPixel(dc, x + 6, y + 6) == marking_color;
    }
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    require(matching, "Both A350 nose previews draw squares in the selected marking colour");
    command(500);
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
    require(GetDlgItem(window, 229) && GetDlgItem(window, 228) && GetDlgItem(window, 203) && GetDlgItem(window, 511),
            "Scene, first-camera, calibration budget and stop controls remain available");
    require(!GetDlgItem(window, 232), "Temporary graphics control is absent");
    command(229);
    require(current.scene_test == 1 && is_on(229, current), "Scene control enables scene testing");
    require(current.manual_mask == original.manual_mask && current.mounts == original.mounts && current.nose_dot == original.nose_dot &&
                current.tail_corner == original.tail_corner && current.speed_color == original.speed_color,
            "Scene control preserves previews, calibration and display settings");
    wchar_t label[96]{};
    GetDlgItemTextW(window, 229, label, 96);
    require(std::wstring(label) == L"Scene test: On", "Scene control label reflects its enabled state");
    command(228);
    require(current.single_camera == 1 && is_on(228, current), "First-camera control remains functional");
    command(228);
    require(!current.single_camera, "First-camera control can be disabled");
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
    command(500);
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
    DestroyWindow(window);
    for (const auto* profile : profiles::Catalog) {
      win::Settings saved;
      saved.profile = profile->id;
      DeleteFileW(win::settings_path(saved).c_str());
    }
    DeleteFileW((win::settings_override + L"\\settings.ini").c_str());
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
