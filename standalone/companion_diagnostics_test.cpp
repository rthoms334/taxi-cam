#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define wWinMain companion_entry_unused
#include "companion.cpp"
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
    require(!current.graphics_state_test && GetDlgItem(window, 232), "Default-off Diagnostics control exists");
    wchar_t label[96]{};
    GetDlgItemTextW(window, 232, label, 96);
    require(std::wstring(label) == L"Graphics state test: Off", "Control explains default state");
    command(232);
    require(current.graphics_state_test == 1 && is_on(232, current), "Actual toggle publishes diagnostic All");
    GetDlgItemTextW(window, 232, label, 96);
    require(std::wstring(label) == L"Graphics state test: All", "All state label is explicit");
    require(current.manual_mask == 1 && current.follow_taxi == 0 && current.mounts == original.mounts,
            "Diagnostic preserves preview routing and camera mounts");
    require(!dirty && GetFileAttributesW(win::settings_path(current).c_str()) == INVALID_FILE_ATTRIBUTES,
            "Ephemeral toggle is neither dirty nor persisted");
    command(500);
    require(current.graphics_state_test == 1, "Saving ordinary settings does not cancel active diagnostic");
    win::Settings loaded;
    require(win::load_settings(loaded, L"", 2) && loaded.graphics_state_test == 0, "Saved profile reload defaults diagnostic off");
    wchar_t missing[32]{};
    GetPrivateProfileStringW(L"display", L"graphics_state_test", L"absent", missing, 32, win::settings_path(current).c_str());
    require(std::wstring(missing) == L"absent", "Diagnostic has no persisted INI key");
    dirty = true;
    constexpr const wchar_t* expected_labels[]{L"Graphics state test: Off",
                                               L"Graphics state test: All",
                                               L"Graphics state test: Targets only",
                                               L"Graphics state test: Shader state only",
                                               L"Graphics state test: Pipeline only",
                                               L"Graphics state test: Root bindings only",
                                               L"Graphics state test: Raster state only"};
    for (const unsigned mode : {2u, 3u, 4u, 5u, 6u, 0u}) {
      command(232);
      require(current.graphics_state_test == mode && dirty, "Diagnostic cycle preserves unrelated unsaved edits");
      GetDlgItemTextW(window, 232, label, 96);
      require(std::wstring(label) == expected_labels[mode], "Diagnostic mode label matches its stable numeric meaning");
      require(current.manual_mask == original.manual_mask && current.calibration_mask == original.calibration_mask &&
                  current.mounts == original.mounts && current.nose_dot == original.nose_dot &&
                  current.tail_corner == original.tail_corner && current.speed_color == original.speed_color,
              "Every diagnostic mode preserves routing, calibration, guides and display colour");
      win::save_settings(current);
      require(win::load_settings(loaded, L"", 2) && loaded.graphics_state_test == 0,
              "Every diagnostic mode remains absent from persisted profile settings");
    }
    require(!current.graphics_state_test, "Diagnostic cycle returns to normal rendering");
    command(232);
    require(current.graphics_state_test == 1, "Diagnostic can be reenabled");
    current.scene_test = 1;
    current.calibration_mask = 2;
    command(511);
    require(!current.graphics_state_test && !current.manual_mask && !current.scene_test && !current.calibration_mask,
            "Stop camera tests clears every temporary test");
    command(232);
    status.heartbeat = GetTickCount64();
    status.aircraft_session_epoch = current.aircraft_session_epoch + 1;
    sync_aircraft_session();
    require(!current.graphics_state_test, "Flight-session change clears diagnostic");
    GetDlgItemTextW(window, 232, label, 96);
    require(std::wstring(label) == L"Graphics state test: Off", "Session reset refreshes control state without rebuilding fields");
    command(232);
    command(100);
    SendDlgItemMessageW(window, 210, CB_SETCURSEL, 0, 0);
    procedure(window, WM_COMMAND, MAKEWPARAM(210, CBN_SELENDOK), 0);
    require(current.profile == 1 && !current.graphics_state_test, "Profile selection clears temporary diagnostic");
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
