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
  if (m == WM_COMMAND || m == WM_HOTKEY || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_DRAWITEM)
    return procedure(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}
void command(int id) {
  procedure(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}
HWND shortcut_fixture() {
  const ShortcutDialogTemplate layout;
  const auto editor = CreateDialogIndirectParamW(instance, &layout.dialog, window, shortcut_dialog, 0);
  require(editor != nullptr, "Create own hidden native shortcut editor");
  return editor;
}
void shortcut_command(HWND editor, int id) {
  shortcut_dialog(editor, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}
struct FakeRegistration {
  inline static std::array<bool, 3> active{};
  inline static unsigned adds{}, removes{};
  inline static int blocked = -1;
  static BOOL WINAPI add(HWND, int id, UINT modifiers, UINT) {
    ++adds;
    require((modifiers & MOD_NOREPEAT) != 0, "Every actual registration suppresses held-key repeats");
    const int i = id - win::CameraHotkeyFirstId;
    require(i >= 0 && i < 3 && !active[i], "Registration owns unique action IDs and releases before reconfiguration");
    if (i == blocked) {
      SetLastError(ERROR_HOTKEY_ALREADY_REGISTERED);
      return FALSE;
    }
    active[i] = true;
    return TRUE;
  }
  static BOOL WINAPI remove(HWND, int id) {
    ++removes;
    const int i = id - win::CameraHotkeyFirstId;
    require(i >= 0 && i < 3 && active[i], "Only owned registrations are removed, once");
    active[i] = false;
    return TRUE;
  }
};
void registration_checks() {
  using namespace win;
  const auto fixture = reinterpret_cast<HWND>(static_cast<INT_PTR>(1));
  FakeRegistration::adds = FakeRegistration::removes = 0;
  FakeRegistration::active = {};
  FakeRegistration::blocked = -1;
  {
    CameraHotkeyRegistration registration(FakeRegistration::add, FakeRegistration::remove);
    require(registration.configure(fixture, DefaultCameraHotkeys, true) && !FakeRegistration::adds,
            "Preview startup never calls RegisterHotKey");
    require(registration.action(CameraHotkeyFirstId, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F9)) == -1,
            "A fabricated hotkey message cannot activate preview bindings");
    require(registration.configure(fixture, DefaultCameraHotkeys, false) && FakeRegistration::adds == 3,
            "Normal startup registers all three independent actions");
    require(registration.action(CameraHotkeyFirstId + 2, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F11)) == 2,
            "Both action dispatches from the matching chord");
    auto duplicate = DefaultCameraHotkeys;
    duplicate[1] = duplicate[0];
    require(!registration.configure(fixture, duplicate, false) && FakeRegistration::removes == 0,
            "Invalid reconfiguration leaves existing working bindings active");
    auto replacement = DefaultCameraHotkeys;
    replacement[0] = {'L', MOD_ALT | MOD_CONTROL};
    FakeRegistration::blocked = 1;
    require(registration.configure(fixture, replacement, false) && FakeRegistration::removes == 3,
            "Reconfiguration unregisters all previous bindings before replacement");
    require(registration.conflicts() && registration.status(1).find(L"another app") != std::wstring::npos,
            "Registration conflicts identify the affected action");
    require(registration.action(CameraHotkeyFirstId + 1, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F10)) == -1,
            "An unavailable action cannot run from WM_HOTKEY");
    require(registration.action(CameraHotkeyFirstId, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F9)) == -1 &&
                registration.action(CameraHotkeyFirstId, MAKELPARAM(MOD_ALT | MOD_CONTROL, 'L')) == 0,
            "Old queued hotkeys are rejected after changing a shortcut");
    require(registration.configure(fixture, replacement, true) && FakeRegistration::removes == 5 && FakeRegistration::adds == 6,
            "Switching to preview releases live registrations without acquiring any");
    FakeRegistration::blocked = -1;
    replacement[1] = {};
    require(registration.configure(fixture, replacement, false) && FakeRegistration::adds == 8,
            "Disabled actions do not reserve a key combination");
  }
  require(FakeRegistration::removes == 7 && FakeRegistration::active == std::array<bool, 3>{},
          "Shutdown releases every remaining owned shortcut");
}
void persistence_checks() {
  using namespace win;
  CameraHotkeys loaded;
  require(load_camera_hotkeys(loaded, settings_override) && loaded == DefaultCameraHotkeys,
          "First run offers the documented default shortcuts");
  const CameraHotkeys custom{{{'L', MOD_CONTROL | MOD_ALT}, {}, {VK_F16, MOD_ALT | MOD_SHIFT}}};
  for (auto chord : custom)
    require(hotkey_from_control(hotkey_control_value(chord)) == chord, "Editor modifiers and function keys roundtrip");
  require(save_camera_hotkeys(custom, settings_override) && load_camera_hotkeys(loaded, settings_override) && loaded == custom,
          "Custom shortcuts and disabled actions persist across restart");
  auto duplicate = custom;
  duplicate[1] = duplicate[0];
  require(!save_camera_hotkeys(duplicate, settings_override) && load_camera_hotkeys(loaded, settings_override) && loaded == custom,
          "Reject duplicate settings without damaging saved shortcuts");
  for (const CameraHotkey invalid : {CameraHotkey{'L', 0},
                                     {'L', MOD_SHIFT},
                                     {'L', MOD_WIN},
                                     {'L', MOD_CONTROL | MOD_NOREPEAT},
                                     {VK_F12, MOD_CONTROL},
                                     {VK_F4, MOD_ALT},
                                     {VK_TAB, MOD_ALT},
                                     {VK_CONTROL, MOD_CONTROL},
                                     {0, MOD_CONTROL},
                                     {256, MOD_CONTROL}})
    require(!valid_camera_hotkey(invalid), "Reserved, unmodified and invalid key combinations are rejected");
  Settings aircraft;
  require(load_settings(aircraft, L"", 4) && !aircraft.follow_taxi, "New manual-only aircraft defaults to manual control");
  aircraft.follow_taxi = 1;
  require(save_settings(aircraft) && load_settings(aircraft, L"", 4) && !aircraft.follow_taxi,
          "Stale saved TAXI-button settings cannot turn on INOP button control");
  require(load_camera_hotkeys(loaded, settings_override) && loaded == custom,
          "Saving or loading aircraft calibration does not overwrite app shortcuts");
  const auto path = settings_override + L"\\hotkeys.ini";
  require(WritePrivateProfileStringW(L"shortcuts", L"left", L"-1", path.c_str()), "Write malformed fixture");
  require(!load_camera_hotkeys(loaded, settings_override) && loaded == CameraHotkeys{},
          "Malformed saved shortcuts disable registration instead of claiming unexpected keys");
  require(save_camera_hotkeys(custom, settings_override), "Restore saved shortcut fixture");
  require(WritePrivateProfileStringW(L"shortcuts", L"both", nullptr, path.c_str()), "Write incomplete fixture");
  require(!load_camera_hotkeys(loaded, settings_override) && loaded == CameraHotkeys{}, "Incomplete saved shortcuts fail closed");
  require(save_camera_hotkeys(custom, settings_override), "Valid preferences recover after malformed input");
}
void manual_intent_checks() {
  using namespace win;
  for (unsigned mask = 0; mask < 4; ++mask) {
    Settings s;
    s.enabled = 0;
    s.follow_taxi = 0;
    s.manual_mask = mask;
    s.calibration_mask = 3;
    s.scene_test = 1;
    s.aircraft_session_epoch = 18;
    s.profile_request = 11;
    s.left_id = 41;
    s.right_id = 42;
    const auto saved = s;
    toggle_manual_camera(s, 2);
    require(s.manual_mask == (mask == 3 ? 0u : 3u), "Both action always converges to both-on or both-off");
    require(!s.enabled && !s.follow_taxi && !s.calibration_mask && !s.scene_test && s.aircraft_session_epoch == 18 &&
                s.profile_request == 11 && s.left_id == 41 && s.right_id == 42 && s.mounts == saved.mounts &&
                s.speed_color == saved.speed_color && s.nose_dot == saved.nose_dot,
            "Manual intent exits test patterns while preserving service, identity scope, routing and calibration");
    s = saved;
    s.follow_taxi = 1;
    toggle_manual_camera(s, 0, mask);
    require(s.manual_mask == (mask ^ 1u) && !s.follow_taxi, "Left takes over the fresh automatic display state independently");
    toggle_manual_camera(s, 1);
    require(s.manual_mask == (mask ^ 3u), "Right toggles independently in manual mode");
    const auto stable = s;
    toggle_manual_camera(s, 3);
    require(s.manual_mask == stable.manual_mask && s.follow_taxi == stable.follow_taxi, "Invalid actions leave intent unchanged");
    reset_aircraft_session(s, 19);
    require(!s.manual_mask && !s.calibration_mask && s.aircraft_session_epoch == 19, "Flight change clears shortcut requests");
  }
}
void ui_checks() {
  preview_ui = true;
  current = {};
  current.profile = 4;
  current.auto_profile = 0;
  current.follow_taxi = 0;
  page = 0;
  hotkey_draft = hotkey_saved = win::DefaultCameraHotkeys;
  hotkey_registration.configure(window, hotkey_saved, preview_ui);
  build_controls();
  require(GetDlgItem(window, 645) && !GetDlgItem(window, 106),
          "Shortcut editor opens from Flight-deck control without adding another navigation page");
  const auto rate = GetDlgItem(window, 200);
  SetWindowTextW(rate, L"-");
  const auto original_rate = current.camera_rate;
  const auto editor = shortcut_fixture();
  require(GetDlgItem(editor, 620) && GetDlgItem(editor, 621) && GetDlgItem(editor, 622),
          "Flight-deck shortcut editor exposes all three configurable actions");
  shortcut_command(editor, 631);
  require(!hotkey_draft[1].key && hotkey_draft != hotkey_saved, "Clear marks a shortcut disabled without applying it early");
  shortcut_command(editor, IDOK);
  require(hotkey_draft == hotkey_saved && !hotkey_saved[1].key,
          "Save applies disabled shortcuts in UI preview without global registration");
  win::CameraHotkeys loaded;
  require(win::load_camera_hotkeys(loaded, win::settings_override) && loaded == hotkey_saved,
          "Shortcut UI uses the application preferences file");
  wchar_t rate_text[64]{};
  GetWindowTextW(rate, rate_text, 64);
  require(dirty && GetDlgItem(window, 200) == rate && std::wstring(rate_text) == L"-" && current.camera_rate == original_rate,
          "Saving shortcuts preserves unrelated unfinished camera edits");
  SendDlgItemMessageW(editor, 620, HKM_SETHOTKEY, win::hotkey_control_value({VK_F11, MOD_SHIFT | MOD_CONTROL}), 0);
  shortcut_dialog(editor, WM_COMMAND, MAKEWPARAM(620, EN_CHANGE), 0);
  require(hotkey_draft != hotkey_saved && hotkey_draft[0] == hotkey_draft[2], "Native editor changes are captured as a draft");
  shortcut_command(editor, IDOK);
  wchar_t error[256]{};
  GetDlgItemTextW(editor, 660, error, 256);
  require(
      hotkey_draft != hotkey_saved && std::wstring(error).find(L"different shortcut") != std::wstring::npos && hotkey_saved[0].key == VK_F9,
      "Duplicate UI bindings show an error and preserve working preferences");
  shortcut_command(editor, 640);
  shortcut_command(editor, IDOK);
  require(hotkey_saved == win::DefaultCameraHotkeys && hotkey_draft == hotkey_saved, "Reset becomes effective only after Save");
  shortcut_command(editor, 630);
  DestroyWindow(editor);
  require(hotkey_draft == hotkey_saved && hotkey_saved[0].key == VK_F9 && !shortcut_window,
          "Closing the editor discards unsaved shortcut changes and retains saved bindings");
  SetWindowTextW(rate, L"15");
  command(101);
  auto field = GetDlgItem(window, 302);
  SetWindowTextW(field, L"-");
  const auto mounts = current.mounts;
  current.calibration_mask = 3;
  current.scene_test = 1;
  current.enabled = 0;
  toggle_camera_from_hotkey(2);
  wchar_t edit_text[64]{};
  GetWindowTextW(field, edit_text, 64);
  require(GetDlgItem(window, 302) == field && std::wstring(edit_text) == L"-" && dirty && current.mounts == mounts,
          "Hotkeys preserve unfinished numeric edits and their original controls");
  require(!current.enabled && current.manual_mask == 3 && !current.calibration_mask && !current.scene_test,
          "Hidden shortcut request does not turn on a disabled service or leave calibration active");
  procedure(window, WM_HOTKEY, win::CameraHotkeyFirstId + 2, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F11));
  require(current.manual_mask == 3, "UI preview cannot activate global shortcuts through WM_HOTKEY");
  SetWindowTextW(field, L"20");
  command(100);
  require(!IsWindowEnabled(GetDlgItem(window, 221)), "INOP aircraft does not offer automatic TAXI-button control");
  command(221);
  require(!current.follow_taxi, "Manual-only aircraft ignores automatic-control commands");
  command(103);
  command(226);
  command(226);
  require(!current.calibration_mask && !current.follow_taxi, "Ending INOP-aircraft calibration retains manual control");
  current.profile = 2;
  command(226);
  command(226);
  require(current.follow_taxi, "Ending existing-aircraft calibration still restores TAXI buttons");
  current.manual_mask = 0;
  current.aircraft_session_epoch = 10;
  status = {};
  status.heartbeat = GetTickCount64();
  status.aircraft_session_epoch = 10;
  status.active_profile = 2;
  status.taxi_mask = 3;
  toggle_camera_from_hotkey(0);
  require(current.manual_mask == 2, "Hotkey takeover toggles the displayed side while preserving its automatic peer");
  status.aircraft_session_epoch = 11;
  status.taxi_mask = 0;
  toggle_camera_from_hotkey(1);
  require(current.manual_mask == 2 && current.aircraft_session_epoch == 11,
          "A shortcut synchronizes a new flight before setting its session-scoped request");
}
void native_registration_checks() {
  using namespace win;
  // Reserve an unusual combination briefly; never synthesize user keystrokes or
  // register the user's configured shortcuts during validation.
  const CameraHotkeys fixture{{{VK_F24, MOD_CONTROL | MOD_ALT | MOD_SHIFT}, {}, {}}};
  constexpr int blocker = 890;
  require(RegisterHotKey(window, blocker, fixture[0].modifiers | MOD_NOREPEAT, fixture[0].key),
          "Reserve an isolated native fixture shortcut");
  CameraHotkeyRegistration other;
  require(other.configure(window, fixture, false) && other.conflicts(), "Windows registration conflict is surfaced");
  UnregisterHotKey(window, blocker);
  require(other.configure(window, fixture, false) && !other.conflicts(), "Reconfiguration recovers after a Windows conflict clears");
  other.clear();
  preview_ui = false;
  require(hotkey_registration.configure(window, fixture, false) && !hotkey_registration.conflicts(),
          "Hidden companion window owns its global shortcut");
  current.follow_taxi = 0;
  current.manual_mask = 0;
  PostMessageW(window, WM_HOTKEY, CameraHotkeyFirstId, MAKELPARAM(fixture[0].modifiers, fixture[0].key));
  MSG message{};
  require(PeekMessageW(&message, window, WM_HOTKEY, WM_HOTKEY, PM_REMOVE), "Hidden window receives the hotkey action message");
  DispatchMessageW(&message);
  require(!IsWindowVisible(window) && current.manual_mask == 1, "Hidden companion dispatches the registered left action");
  hotkey_saved = hotkey_draft = fixture;
  const auto dialog = shortcut_fixture();
  const auto editor = GetDlgItem(dialog, 620);
  SendMessageW(editor, WM_SETFOCUS, 0, 0);
  require(hotkey_editor_focused && hotkey_registration.action(CameraHotkeyFirstId, MAKELPARAM(fixture[0].modifiers, fixture[0].key)) < 0,
          "Focusing the native editor pauses global dispatch so it can capture an existing chord");
  require(RegisterHotKey(window, blocker, fixture[0].modifiers | MOD_NOREPEAT, fixture[0].key),
          "Editor focus actually releases the binding in Windows");
  UnregisterHotKey(window, blocker);
  SendMessageW(editor, WM_KILLFOCUS, reinterpret_cast<WPARAM>(GetDlgItem(dialog, IDOK)), 0);
  require(!hotkey_editor_focused && hotkey_registration.action(CameraHotkeyFirstId, MAKELPARAM(fixture[0].modifiers, fixture[0].key)) == 0,
          "Leaving the shortcut editor restores the saved binding");
  DestroyWindow(dialog);
  hotkey_registration.clear();
  require(RegisterHotKey(window, blocker, fixture[0].modifiers | MOD_NOREPEAT, fixture[0].key),
          "Shutdown releases the native combination for other apps");
  UnregisterHotKey(window, blocker);
  preview_ui = true;
}
}  // namespace
int main() {
  try {
    instance = GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_HOTKEY_CLASS};
    require(InitCommonControlsEx(&common), "Initialize native shortcut controls");
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = host;
    type.lpszClassName = L"TaxiHotkeysFixture";
    require(RegisterClassW(&type), "Register own hidden fixture class");
    window = CreateWindowW(type.lpszClassName, L"Hidden shortcut controls", WS_POPUP, 0, 0, 1055, 750, nullptr, nullptr, instance, nullptr);
    require(window != nullptr, "Create own hidden shortcut window");
    dpi = 96;
    make_fonts();
    background_brush = CreateSolidBrush(Background);
    card_brush = CreateSolidBrush(Card);
    wchar_t repository[32768]{};
    require(GetCurrentDirectoryW(32768, repository), "Get test artifact directory");
    win::settings_override = std::wstring(repository) + L"\\build\\hotkeys-test-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(win::settings_override.c_str(), nullptr), "Create isolated ignored settings fixture");
    registration_checks();
    persistence_checks();
    manual_intent_checks();
    ui_checks();
    native_registration_checks();
    hotkey_registration.clear();
    DestroyWindow(window);
    window = nullptr;
    for (const auto* profile : profiles::Catalog) {
      win::Settings saved;
      saved.profile = profile->id;
      DeleteFileW(win::settings_path(saved).c_str());
    }
    DeleteFileW((win::settings_override + L"\\settings.ini").c_str());
    DeleteFileW((win::settings_override + L"\\hotkeys.ini").c_str());
    RemoveDirectoryW((win::settings_override + L"\\profiles").c_str());
    RemoveDirectoryW(win::settings_override.c_str());
    std::printf("PASS camera shortcuts: %u registration, persistence, hidden-window and manual-control checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    hotkey_registration.clear();
    if (window)
      DestroyWindow(window);
    std::fprintf(stderr, "FAIL camera shortcuts: %s\n", error.what());
    return 1;
  }
}
