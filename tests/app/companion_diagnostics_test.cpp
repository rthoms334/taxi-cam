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
