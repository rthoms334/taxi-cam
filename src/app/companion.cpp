#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include "../camera/aircraft_identity.hpp"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>
#include "launcher.hpp"
#include "launcher_log.hpp"
#include "connection_recoverability.hpp"
#include "../shared/camera_rate_policy.hpp"
#include "../shared/display_snapshot.hpp"
#include "../shared/protocol.hpp"
#include "../shared/sim_messages.hpp"
#include "settings_store.hpp"
#include "startup_state.hpp"
#include "camera_hotkeys.hpp"
#include "camera_buttons.hpp"
#include "bug_report.hpp"
#include "../shared/manual_camera_intent.hpp"
#include "../shared/profile_selection.hpp"
#include "../graphics/target_assignment.hpp"
#include "updater.hpp"
#include "../setup/setup_commands.hpp"
#include "changelog.hpp"
#include "changelog_fetch.hpp"

namespace {
using namespace taxi_camera;
namespace win = standalone;
constexpr UINT TrayMessage = WM_APP + 1, StatusMessage = WM_APP + 2, WhatsNewMessage = WM_APP + 3;
constexpr win::ChangelogVersion InstalledVersion{TAXI_CAM_VERSION_MAJOR, TAXI_CAM_VERSION_MINOR, TAXI_CAM_VERSION_PATCH};
constexpr wchar_t WindowClass[] = L"380TaxiCamera.Settings";
constexpr wchar_t DonationUrl[] = L"https://www.paypal.com/donate/?hosted_button_id=EPVELD44P6NXW";
constexpr wchar_t GithubUrl[] = L"https://github.com/rthoms334/taxi-cam";
constexpr COLORREF Background = RGB(17, 21, 28), Sidebar = RGB(12, 16, 22), Card = RGB(26, 32, 41), Border = RGB(44, 54, 67),
                   Text = RGB(232, 238, 246), Muted = RGB(154, 170, 188), Accent = RGB(66, 219, 184);
HINSTANCE instance{};
HWND window{}, sidebar_tooltip{}, shortcut_window{};
HFONT normal{}, small{}, title_font{}, heading{}, version_font{};
HBRUSH background_brush{}, card_brush{};
HICON icon{};
UINT dpi = 96, taskbar_created{};
int page = 0;
std::vector<HWND> controls;
std::vector<HWND> navigation;
native_camera::AutoProfileSelection profile_selection;
std::wstring installation, expected_simulator, notice = L"Changes are saved for this aircraft.";
std::mutex app_mutex;
win::Settings current;
win::Status status;
bool received_bridge_status{};  // Guarded by app_mutex; retained across simulator sessions.
// Guarded by app_mutex. The connection worker takes unseen bridge events from
// every status sample; the UI thread shows them (Shell_NotifyIcon from the
// window thread only) and clears the queue.
NotificationReader notification_reader;
std::vector<SimEvent> pending_notifications;
std::wstring connection = L"Waiting for Microsoft Flight Simulator 2024";
std::atomic<bool> running{true};
std::atomic<DWORD> simulator_pid{};
HANDLE worker{}, show_event{}, singleton{};
bool dirty = false, refreshing = false, background_start = false, preview_ui = false;
std::atomic<bool> auto_connect{true};
std::atomic<bool> connection_requested{}, connection_disconnected{};
win::ConnectCommandQueue connect_commands;
win::CameraHotkeys hotkey_draft = win::DefaultCameraHotkeys, hotkey_saved = win::DefaultCameraHotkeys;
win::CameraHotkeyRegistration hotkey_registration;
bool hotkey_editor_focused{}, hotkeys_closing{};
win::CameraButtons button_draft{}, button_saved{};
win::CameraButtonInput button_input;
win::CameraButtonRepeatGuard button_repeat;
int button_capture = -1;  // Action waiting for a controller button in the editor.
std::vector<win::CameraButton> button_capture_down;  // Went down while waiting; the first to come up is set.
win::Updater updater;
ULONGLONG next_update_check{};
bool update_prompt{};
win::ChangelogFetcher changelog_fetcher;
bool whats_new{};  // The installed version's notes have not been read yet.
int scale(int v) {
  return MulDiv(v, static_cast<int>(dpi), 96);
}
RECT rectangle(int x, int y, int w, int h) {
  return {scale(x), scale(y), scale(x + w), scale(y + h)};
}
void text(HDC dc,
          const wchar_t* value,
          int x,
          int y,
          int w,
          int h,
          HFONT font,
          COLORREF color = Text,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
  auto r = rectangle(x, y, w, h);
  SelectObject(dc, font);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, value, -1, &r, flags);
}
void panel(HDC dc, int x, int y, int w, int h, COLORREF color = Card) {
  HBRUSH brush = CreateSolidBrush(color);
  HPEN pen = CreatePen(PS_SOLID, 1, Border);
  const auto oldb = SelectObject(dc, brush), oldp = SelectObject(dc, pen);
  RoundRect(dc, scale(x), scale(y), scale(x + w), scale(y + h), scale(14), scale(14));
  SelectObject(dc, oldb);
  SelectObject(dc, oldp);
  DeleteObject(brush);
  DeleteObject(pen);
}
HWND child(const wchar_t* type, const wchar_t* label, int id, int x, int y, int w, int h, DWORD style = 0) {
  HWND value = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, scale(x), scale(y), scale(w), scale(h), window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
  SendMessageW(value, WM_SETFONT, reinterpret_cast<WPARAM>(normal), TRUE);
  SetWindowTheme(value, L"DarkMode_Explorer", nullptr);
  controls.push_back(value);
  return value;
}
HWND button(const wchar_t* label, int id, int x, int y, int w = 130, int h = 36) {
  return child(L"BUTTON", label, id, x, y, w, h, BS_OWNERDRAW);
}
void draw_bug_icon(HDC dc, const RECT& bounds, COLORREF color) {
  const int x = (bounds.left + bounds.right) / 2 - scale(20), y = (bounds.top + bounds.bottom) / 2 - scale(20);
  HPEN pen = CreatePen(PS_SOLID, scale(2), color);
  const auto old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  const auto line = [&](int x1, int y1, int x2, int y2) {
    MoveToEx(dc, x + scale(x1), y + scale(y1), nullptr);
    LineTo(dc, x + scale(x2), y + scale(y2));
  };
  line(17, 10, 14, 6);
  line(23, 10, 26, 6);
  Ellipse(dc, x + scale(16), y + scale(9), x + scale(24), y + scale(17));
  Ellipse(dc, x + scale(13), y + scale(14), x + scale(27), y + scale(31));
  line(20, 15, 20, 30);
  for (const bool right : {false, true}) {
    const auto side = [&](int coordinate) { return right ? 40 - coordinate : coordinate; };
    line(side(13), 18, side(8), 15);
    line(side(13), 23, side(6), 23);
    line(side(14), 28, side(8), 32);
  }
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(pen);
}
void edit(double value, int id, int x, int y, int w = 110) {
  wchar_t buffer[64];
  std::swprintf(buffer, 64, id >= 360 && id <= 367 ? L"%.1f" : id == 201 || id == 202 ? L"%+.2f" : L"%.10g", value);
  auto h = child(L"EDIT", buffer, id, x, y, w, 30, ES_AUTOHSCROLL | ES_LEFT | WS_BORDER);
  SendMessageW(h, EM_SETLIMITTEXT, 32, 0);
}
void toggle(const wchar_t* label, int id, bool enabled, int x, int y, int width = 125) {
  const auto text = std::wstring(label) + (enabled ? L": On" : L": Off");
  button(text.c_str(), id, x, y, width);
}
void make_fonts() {
  for (HFONT f : {normal, small, title_font, heading, version_font})
    if (f)
      DeleteObject(f);
  auto make = [](int size, int weight, bool underline = false) {
    return CreateFontW(-scale(size), 0, 0, 0, weight, FALSE, underline, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable");
  };
  normal = make(15, FW_NORMAL);
  small = make(13, FW_NORMAL);
  title_font = make(30, FW_SEMIBOLD);
  heading = make(18, FW_SEMIBOLD);
  version_font = make(13, FW_NORMAL, true);
}
std::wstring widen(const char* input) {
  if (!input)
    return {};
  const size_t count = strnlen(input, 384);
  if (!count)
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, input, static_cast<int>(count), nullptr, 0);
  std::wstring output(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, input, static_cast<int>(count), output.data(), n);
  return output;
}
win::Settings draft() {
  const std::lock_guard lock(app_mutex);
  return current;
}
void publish(const win::Settings& value) {
  const std::lock_guard lock(app_mutex);
  const auto enabled = current.enabled;
  const auto profile_request = std::max(current.profile_request, value.profile_request);
  // A snapshot request is session-only; an older draft must not withdraw it.
  const auto snapshot = current.snapshot_request >= value.snapshot_request ? current : value;
  current = value;
  current.enabled = enabled;  // Profile/settings edits cannot override connection state.
  current.profile_request = profile_request;
  current.snapshot_request = snapshot.snapshot_request;
  current.snapshot_id = snapshot.snapshot_id;
}
void refresh_connection_button() {
  SetDlgItemTextW(window, 241, win::connection_button_label(connection_requested.load(std::memory_order_acquire)));
}
// PMDG 777: the lower DU is a separate texture, chosen in the second list.
bool separate_lower_profile(const win::Settings& s) noexcept {
  const auto* profile = profiles::find(s.profile);
  return profile && profiles::separate_lower_texture(*profile);
}
bool pmdg_cam_control(const win::Settings& s) noexcept {
  const auto* profile = profiles::find(s.profile);
  return profile && profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam;
}
// Side 2 is the lower ECAM (SD) on Airbus aircraft and the lower DU on the 777.
const wchar_t* side_toggle_name(const win::Settings& s, unsigned side) {
  return side == 0 ? L"Left" : side == 1 ? L"Right" : pmdg_cam_control(s) ? L"Lower" : L"SD";
}
// PFD routing's Preview and Calibrate rows, from the current request.
void refresh_side_toggles(const win::Settings& s) {
  constexpr int previews[]{224, 225, 232}, calibrations[]{226, 227, 233};
  for (unsigned side = 0; side < 3; ++side) {
    const auto name = std::wstring(side_toggle_name(s, side));
    SetDlgItemTextW(window, previews[side], (name + (s.manual_mask & (1u << side) ? L": On" : L": Off")).c_str());
    SetDlgItemTextW(window, calibrations[side], (name + (s.calibration_mask & (1u << side) ? L": On" : L": Off")).c_str());
  }
}
void request_connection(win::ConnectCommand command) {
  {
    const std::lock_guard lock(app_mutex);
    const bool disconnect = command == win::ConnectCommand::disconnect;
    if (!disconnect && !win::begin_connection(current)) {
      notice = L"Restart Taxi Cam to begin a new connection.";
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
    connection_disconnected.store(disconnect, std::memory_order_release);
    connection_requested.store(!disconnect, std::memory_order_release);
    if (disconnect)
      win::apply_connection_command(current, command);
    if (disconnect) {
      status = {};
      connection = L"Disconnected. Choose Connect to enable the cameras again.";
      // Publish the stop immediately, including when the attach worker is still
      // waiting for a remote load. The worker uses the same settings lock.
      win::Mailbox mailbox;
      if (const auto pid = simulator_pid.load(); pid && mailbox.open(pid, false) && mailbox.lock(200)) {
        mailbox.data()->settings = current;
        mailbox.data()->owner_heartbeat = 0;
        mailbox.unlock();
      }
    }
  }
  connect_commands.request(command);
  notice = command == win::ConnectCommand::disconnect ? L"Disconnected. Camera output and temporary requests are off."
                                                      : L"Connect requested. Cameras will enable when the bridge is ready.";
  refresh_connection_button();
  if (command == win::ConnectCommand::disconnect) {
    SetDlgItemTextW(window, 229, L"Scene test: Off");
    refresh_side_toggles(draft());
  }
  InvalidateRect(window, nullptr, FALSE);
}
void toggle_connection() {
  request_connection(connection_requested.load(std::memory_order_acquire) ? win::ConnectCommand::disconnect : win::ConnectCommand::connect);
}
bool exchange_control(win::Mailbox& mailbox, win::Status* sample = nullptr) {
  // Serialize publication with Connect/Disconnect so a previously copied
  // enabled setting cannot be written after the user has disconnected.
  const std::lock_guard lock(app_mutex);
  if (!mailbox.data() || !mailbox.lock(100))
    return false;
  mailbox.data()->settings = current;
  mailbox.data()->owner_pid = GetCurrentProcessId();
  mailbox.data()->owner_heartbeat = connection_requested.load(std::memory_order_acquire) ? GetTickCount64() : 0;
  if (sample)
    *sample = mailbox.data()->status;
  mailbox.unlock();
  return true;
}
void donate() {
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", DonationUrl, nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32)
    MessageBoxW(window, L"Could not open your browser. You can also find the PayPal donation link in the Taxi Cam README.",
                L"Donate to Taxi Cam", MB_OK | MB_ICONWARNING);
}
void report_bug() {
  win::BugReportContext context;
  {
    const std::lock_guard lock(app_mutex);
    context.settings = current;
    context.status = status;
    context.bridge_seen = received_bridge_status;
    context.simulator_running = simulator_pid.load() != 0;
  }
  context.now = GetTickCount64();
  context.ui_preview = preview_ui;
  context.unsaved_edits = dirty;
  const auto url = win::bug_report_url(context);
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    MessageBoxW(window,
                L"Could not open your browser. Open github.com/rthoms334/taxi-cam/issues and choose Bug report. "
                L"Attach logs from Diagnostics > Open log folder and include your Taxi Cam version.",
                L"Taxi Cam bug report", MB_OK | MB_ICONWARNING);
    return;
  }
  notice = L"Bug report opened. Review the details and attach logs before submitting on GitHub.";
  InvalidateRect(window, nullptr, FALSE);
}
void dirty_notice() {
  dirty = true;
  notice = L"Unsaved changes";
  InvalidateRect(window, nullptr, FALSE);
}
bool apply_color_selection(const win::Settings& expected, bool markings, COLORREF color) {
  {
    const std::lock_guard lock(app_mutex);
    // The modal picker pumps status messages, which can select another aircraft
    // or restart this profile. Its result belongs only to the opening session.
    if (current.profile != expected.profile || current.profile_request != expected.profile_request)
      return false;
    auto& target = markings ? current.guide_color : current.speed_color;
    target = {GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f};
  }
  dirty_notice();
  return true;
}
double number(int id, double previous, bool& ok) {
  auto control = GetDlgItem(window, id);
  if (!control)
    return previous;
  wchar_t value[64];
  GetWindowTextW(control, value, 64);
  wchar_t* end{};
  const double n = std::wcstod(value, &end);
  if (end == value || *end || !std::isfinite(n)) {
    ok = false;
    return previous;
  }
  return n;
}
bool single_display_profile(const win::Settings& s) noexcept {
  const auto* profile = profiles::find(s.profile);
  return profile && profile->pfd_detection == profiles::PfdDetectionPolicy::single_display;
}
// Indexed by CameraMode; CameraModeOrder is the left-to-right card order.
constexpr const wchar_t* CameraModeNames[]{L"Performance", L"Balanced", L"Smooth", L"Custom", L"Auto"};
constexpr const wchar_t* CameraModeHints[]{L"Lowest cost", L"Medium cost", L"Highest cost", L"Your target", L"Adapts to fps"};
constexpr CameraMode CameraModeOrder[]{CameraMode::automatic, CameraMode::performance, CameraMode::balanced, CameraMode::smooth,
                                       CameraMode::custom};
static_assert(std::size(CameraModeNames) == kCameraModeCount && std::size(CameraModeHints) == kCameraModeCount &&
              std::size(CameraModeOrder) == kCameraModeCount);
// Display page mode cards (270 + CameraMode) and the custom target slider
// (205). The chosen card is unsaved until Save changes, like the other fields.
constexpr int CameraModeCardId = 270, CameraTargetId = 205;
unsigned pending_camera_mode = static_cast<unsigned>(kDefaultCameraMode);
unsigned selected_camera_mode() {
  if (GetDlgItem(window, CameraModeCardId))
    return pending_camera_mode;
  const auto mode = draft().camera_mode;
  return mode < kCameraModeCount ? mode : static_cast<unsigned>(kDefaultCameraMode);
}
unsigned selected_camera_target() {
  if (const auto target = GetDlgItem(window, CameraTargetId))
    return static_cast<unsigned>(SendMessageW(target, TBM_GETPOS, 0, 0));
  return draft().camera_rate;
}
void invalidate_camera_mode_cards() {
  for (unsigned mode = 0; mode < kCameraModeCount; ++mode)
    if (const auto card = GetDlgItem(window, CameraModeCardId + static_cast<int>(mode)))
      InvalidateRect(card, nullptr, FALSE);
}
// Name, target fps, what each camera gets on this flight right now, and a
// three-step frame-cost meter that follows the target.
void draw_camera_mode_card(const DRAWITEMSTRUCT& item, unsigned mode) {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  const double update_hz = sample.heartbeat && sample.update_hz > 0 ? sample.update_hz : std::numeric_limits<double>::quiet_NaN();
  const bool automatic = mode == static_cast<unsigned>(CameraMode::automatic);
  // Auto reports the cameras per frame it runs; the other modes their target.
  const bool auto_live = automatic && sample.heartbeat && s.camera_mode == mode && std::isfinite(update_hz);
  const unsigned feeds = s.single_camera ? 1u : (profile && profile->composition.split_bottom != 0 ? 3u : 2u);
  const unsigned per_frame = auto_cameras_per_frame(auto_live ? sample.auto_level : 0, feeds);
  const unsigned target =
      automatic ? 0 : effective_camera_rate(camera_mode_target(mode, selected_camera_target()), profile ? profile->pfd_refresh_hz : 0).rate;
  const double reached = automatic ? (auto_live ? update_hz * per_frame / feeds : std::numeric_limits<double>::quiet_NaN())
                                   : reachable_camera_rate(target, update_hz);
  const bool chosen = mode == selected_camera_mode();
  const bool down = (item.itemState & ODS_SELECTED) != 0;
  const auto dc = item.hDC;
  HBRUSH surround = CreateSolidBrush(Card);
  FillRect(dc, &item.rcItem, surround);
  DeleteObject(surround);
  HBRUSH brush = CreateSolidBrush(chosen ? RGB(30, 64, 63) : down ? Border : Background);
  HPEN pen = CreatePen(PS_SOLID, scale(chosen ? 2 : 1), chosen ? Accent : Border);
  const auto oldb = SelectObject(dc, brush), oldp = SelectObject(dc, pen);
  RoundRect(dc, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, scale(12), scale(12));
  SelectObject(dc, oldb);
  SelectObject(dc, oldp);
  DeleteObject(brush);
  DeleteObject(pen);
  SetBkMode(dc, TRANSPARENT);
  const auto line = [&](const wchar_t* value, int top, int height, HFONT font, COLORREF color) {
    RECT r{item.rcItem.left + scale(10), item.rcItem.top + scale(top), item.rcItem.right - scale(4), item.rcItem.top + scale(top + height)};
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
  };
  line(CameraModeNames[mode], 9, 26, heading, chosen ? Accent : Text);
  wchar_t label[48];
  if (auto_live)
    std::swprintf(label, 48, per_frame >= feeds ? L"Every camera, every frame" : L"%u camera%ls per frame", per_frame,
                  per_frame == 1 ? L"" : L"s");
  else if (automatic)
    std::swprintf(label, 48, L"Even frames");
  else
    std::swprintf(label, 48, L"%u fps", target);
  line(label, 36, 20, small, Muted);
  wchar_t now[48];
  if (std::isfinite(reached))
    std::swprintf(now, 48, reached < 9.95 ? L"≈ %.1f fps now" : L"≈ %.0f fps now", reached);
  const bool show_now = std::isfinite(reached) && (!automatic || auto_live);
  line(show_now ? now : CameraModeHints[mode], 57, 22, normal, show_now ? Text : Muted);
  // Frame cost follows the images rendered per second (Auto: per frame).
  const int bars = automatic                          ? static_cast<int>(auto_live ? std::min(per_frame, 3u) : 1u)
                   : target <= kPerformanceCameraRate ? 1
                   : target <= 20                     ? 2
                                                      : 3;
  for (int i = 0; i < 3; ++i) {
    RECT bar{item.rcItem.right - scale(12 + (3 - i) * 16) + scale(4), item.rcItem.bottom - scale(15),
             item.rcItem.right - scale(12 + (2 - i) * 16), item.rcItem.bottom - scale(10)};
    HBRUSH fill = CreateSolidBrush(i < bars ? (chosen ? Accent : Muted) : Border);
    FillRect(dc, &bar, fill);
    DeleteObject(fill);
  }
  RECT cost{item.rcItem.left + scale(10), item.rcItem.bottom - scale(22), item.rcItem.right - scale(64), item.rcItem.bottom - scale(4)};
  SelectObject(dc, small);
  SetTextColor(dc, Muted);
  DrawTextW(dc, L"Cost", -1, &cost, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  if (item.itemState & ODS_FOCUS) {
    RECT focus = item.rcItem;
    InflateRect(&focus, -scale(4), -scale(4));
    DrawFocusRect(dc, &focus);
  }
}
bool read_fields(win::Settings& settings, const wchar_t** error = nullptr) {
  if (error)
    *error = nullptr;
  bool ok = true;
  const double rate = number(200, settings.camera_rate, ok);
  if (rate < kMinimumCameraRate || rate > kMaximumCameraRate || std::floor(rate) != rate)
    ok = false;
  if (ok)
    settings.camera_rate = static_cast<UINT>(rate);
  if (GetDlgItem(window, CameraModeCardId))
    settings.camera_mode = pending_camera_mode;
  if (const auto target = GetDlgItem(window, CameraTargetId))
    settings.camera_rate =
        static_cast<UINT>(std::clamp<LRESULT>(SendMessageW(target, TBM_GETPOS, 0, 0), kMinimumCameraRate, kMaximumCameraRate));
  const double budget = number(203, settings.calibration_budget, ok);
  if (budget < 64 || budget > 16384 || std::floor(budget) != budget)
    ok = false;
  if (ok)
    settings.calibration_budget = static_cast<UINT>(budget);
  float* brightness[]{&settings.day_brightness, &settings.night_brightness};
  for (int i = 0; i < 2; ++i) {
    const auto value = static_cast<float>(number(201 + i, *brightness[i], ok));
    if (!valid_camera_brightness(value))
      ok = false;
    else
      *brightness[i] = value;
  }
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 6; ++j)
      settings.mounts[i][j] = number(300 + static_cast<int>(i * 10 + j), settings.mounts[i][j], ok);
  const auto* guide_profile = profiles::find(settings.profile);
  if (!guide_profile || guide_profile->reference_guides) {
    std::array<float, 2>* guides[]{&settings.nose_dot, &settings.tail_upper, &settings.tail_corner, &settings.tail_inner};
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned axis = 0; axis < 2; ++axis) {
        const auto value = number(360 + static_cast<int>(i * 2 + axis), (*guides[i])[axis] * 100., ok);
        if (value < 0 || value > (axis ? 100 : 50))
          ok = false;
        else
          (*guides[i])[axis] = static_cast<float>(value / 100.);
      }
  }
  return ok && win::valid_settings(settings);
}
void build_controls();
void refresh_shortcut_status() {
  if (!shortcut_window)
    return;
  for (unsigned i = 0; i < win::CameraHotkeyNames.size(); ++i) {
    const auto state = hotkey_draft[i] != hotkey_saved[i] ? std::wstring(L"Unsaved — select Save changes to apply")
                       : hotkey_editor_focused            ? std::wstring(L"Editing — shortcuts paused until you leave the field")
                                                          : hotkey_registration.status(i);
    SetDlgItemTextW(shortcut_window, 650 + i, state.c_str());
    const auto binding = button_draft[i];
    const auto device = binding.button ? win::CameraButtonInput::device_name(binding.vendor, binding.product) : std::wstring();
    const bool capturing = button_capture == static_cast<int>(i);
    SetDlgItemTextW(shortcut_window, 670 + i,
                    capturing ? L"Press and release a controller button…" : win::describe_camera_button(binding, device).c_str());
    const auto button_state = capturing                    ? std::wstring(L"Waiting — press and release the button, or Clear to cancel")
                              : binding != button_saved[i] ? std::wstring(L"Unsaved — select Save changes to apply")
                              : !binding.button            ? std::wstring(L"Disabled")
                              : preview_ui                 ? std::wstring(L"Preview only — button not active")
                              : !button_input.enabled()    ? L"Unavailable — Windows error " + std::to_wstring(button_input.error())
                              : device.empty()             ? std::wstring(L"Controller not connected")
                                                           : std::wstring(L"Ready — works while Taxi Cam is hidden");
    SetDlgItemTextW(shortcut_window, 675 + i, button_state.c_str());
  }
}
void register_camera_hotkeys() {
  if (hotkey_editor_focused || hotkeys_closing)
    hotkey_registration.clear();
  else
    hotkey_registration.configure(window, hotkey_saved, preview_ui);
}
// Raw Input runs only while a saved button can act or the editor is waiting
// for one, so controller reports cost nothing when no button is bound.
void update_camera_button_input() {
  if (!hotkeys_closing && (button_capture >= 0 || (!preview_ui && win::any_camera_button(button_saved))))
    button_input.enable(window);
  else
    button_input.disable();
}
void toggle_camera_from_hotkey(unsigned action);
void start_button_capture(int action) {
  button_capture = action;
  button_capture_down.clear();
}
void camera_button_event(win::CameraButtonEvent event) {
  // Setting a button needs a full press: down, then up. A switch position the
  // controller holds permanently never comes up, so it cannot be picked up.
  if (button_capture >= 0) {
    const auto down = std::find(button_capture_down.begin(), button_capture_down.end(), event.button);
    if (event.pressed) {
      if (down == button_capture_down.end() && button_capture_down.size() < 32)
        button_capture_down.push_back(event.button);
    } else if (down != button_capture_down.end()) {
      button_draft[button_capture] = event.button;
      button_capture = -1;
      button_capture_down.clear();
      update_camera_button_input();
      refresh_shortcut_status();
    }
    return;
  }
  const int action = event.pressed ? win::camera_button_action(button_saved, event.button) : -1;
  if (action >= 0 && !preview_ui && button_repeat.accept(static_cast<size_t>(action), GetTickCount64()))
    toggle_camera_from_hotkey(static_cast<unsigned>(action));
}
LRESULT CALLBACK shortcut_editor(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_SETFOCUS) {
    // RegisterHotKey consumes its chord before the native editor sees it. Release
    // our bindings while editing so existing shortcuts can be captured as well.
    hotkey_editor_focused = true;
    hotkey_registration.clear();
    refresh_shortcut_status();
  } else if (message == WM_KILLFOCUS) {
    const auto next = reinterpret_cast<HWND>(w);
    const int next_id = next && GetParent(next) == shortcut_window ? GetDlgCtrlID(next) : 0;
    hotkey_editor_focused = next_id >= 620 && next_id < 620 + static_cast<int>(win::CameraHotkeyNames.size());
    register_camera_hotkeys();
    refresh_shortcut_status();
  } else if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(control, shortcut_editor, id);
  }
  return DefSubclassProc(control, message, w, l);
}
struct DialogTemplate {
  DLGTEMPLATE dialog{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, WS_EX_DLGMODALFRAME, 0, 0, 0, 450, 260};
  WORD menu{}, window_class{}, title{};
};
// Dark caption and a client area of the given unscaled size, centred on Settings.
void place_dialog(HWND hwnd, const wchar_t* title, int client_width, int client_height) {
  SetWindowTextW(hwnd, title);
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
  RECT bounds{0, 0, scale(client_width), scale(client_height)}, owner{};
  AdjustWindowRectExForDpi(&bounds, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, FALSE, WS_EX_DLGMODALFRAME, dpi);
  GetWindowRect(window, &owner);
  const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  SetWindowPos(hwnd, nullptr, owner.left + (owner.right - owner.left - width) / 2, owner.top + (owner.bottom - owner.top - height) / 2,
               width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
HWND dialog_control(HWND hwnd,
                    const wchar_t* type,
                    const wchar_t* label,
                    int id,
                    int x,
                    int y,
                    int width,
                    int height,
                    DWORD style = 0,
                    HFONT font = nullptr) {
  HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(width), scale(height), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : normal), TRUE);
  SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
  return control;
}
INT_PTR dialog_colors(WPARAM w) {
  const auto dc = reinterpret_cast<HDC>(w);
  SetTextColor(dc, Text);
  SetBkColor(dc, Card);
  return reinterpret_cast<INT_PTR>(card_brush);
}
INT_PTR CALLBACK shortcut_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM) {
  if (message == WM_INITDIALOG) {
    shortcut_window = hwnd;
    hotkey_draft = hotkey_saved;
    button_draft = button_saved;
    button_capture = -1;
    place_dialog(hwnd, L"Taxi Cam — Flight-deck shortcuts and buttons", 940, 568);
    const auto make = [&](const wchar_t* type, const wchar_t* label, int id, int x, int y, int width, int height, DWORD style = 0,
                          HFONT font = nullptr) { return dialog_control(hwnd, type, label, id, x, y, width, height, style, font); };
    make(L"STATIC", L"Flight-deck shortcuts and buttons", -1, 20, 17, 900, 28, 0, heading);
    make(L"STATIC",
         L"Use Ctrl or Alt with a letter, number or function key, or set a joystick, button box or gamepad button. "
         L"Clear disables it.",
         -1, 20, 51, 900, 27, 0, small);
    make(L"STATIC", L"Keyboard", -1, 205, 86, 275, 22, 0, small);
    make(L"STATIC", L"Controller button", -1, 500, 86, 420, 22, 0, small);
    for (unsigned i = 0; i < win::CameraHotkeyNames.size(); ++i) {
      const int y = 112 + static_cast<int>(i) * 84;
      make(L"STATIC", win::CameraHotkeyNames[i], -1, 20, y + 5, 180, 26);
      auto field = make(HOTKEY_CLASSW, L"", 620 + i, 205, y, 195, 32, WS_TABSTOP | WS_BORDER);
      SendMessageW(field, HKM_SETHOTKEY, win::hotkey_control_value(hotkey_draft[i]), 0);
      SetWindowSubclass(field, shortcut_editor, 1, 0);
      make(L"BUTTON", L"Clear", 630 + i, 408, y, 72, 32, WS_TABSTOP | BS_PUSHBUTTON);
      make(L"STATIC", L"", 650 + i, 205, y + 36, 275, 40, 0, small);
      make(L"STATIC", L"", 670 + i, 500, y, 240, 32, WS_BORDER | SS_CENTERIMAGE | SS_ENDELLIPSIS);
      make(L"BUTTON", L"Set button", 680 + i, 748, y, 92, 32, WS_TABSTOP | BS_PUSHBUTTON);
      make(L"BUTTON", L"Clear", 690 + i, 848, y, 72, 32, WS_TABSTOP | BS_PUSHBUTTON);
      make(L"STATIC", L"", 675 + i, 500, y + 36, 420, 40, 0, small);
    }
    make(L"STATIC",
         L"Both turns the captain and first-officer displays on; press again to turn both off. SD is the lower ECAM or, on the PMDG 777, "
         L"the lower DU. Shortcuts and buttons apply to all aircraft and work while Taxi Cam is hidden. MSFS also receives controller "
         L"buttons, so leave a button you set here unassigned in the simulator.",
         660, 20, 450, 900, 56, 0, small);
    make(L"BUTTON", L"Reset shortcuts", 640, 20, 514, 165, 34, WS_TABSTOP | BS_PUSHBUTTON);
    make(L"BUTTON", L"Save changes", IDOK, 660, 514, 145, 34, WS_TABSTOP | BS_DEFPUSHBUTTON);
    make(L"BUTTON", L"Close", IDCANCEL, 822, 514, 98, 34, WS_TABSTOP | BS_PUSHBUTTON);
    refresh_shortcut_status();
    return TRUE;
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
    return dialog_colors(w);
  if (message == WM_COMMAND) {
    const int id = LOWORD(w);
    if (id >= 620 && id < 620 + static_cast<int>(win::CameraHotkeyNames.size()) && HIWORD(w) == EN_CHANGE) {
      hotkey_draft[id - 620] = win::hotkey_from_control(static_cast<WORD>(SendDlgItemMessageW(hwnd, id, HKM_GETHOTKEY, 0, 0)));
      refresh_shortcut_status();
      return TRUE;
    }
    if ((id >= 630 && id < 630 + static_cast<int>(win::CameraHotkeyNames.size())) || id == 640) {
      if (id == 640)
        hotkey_draft = win::DefaultCameraHotkeys;
      else
        hotkey_draft[id - 630] = {};
      for (unsigned i = 0; i < win::CameraHotkeyNames.size(); ++i)
        SendDlgItemMessageW(hwnd, 620 + i, HKM_SETHOTKEY, win::hotkey_control_value(hotkey_draft[i]), 0);
      refresh_shortcut_status();
      return TRUE;
    }
    if (id >= 680 && id < 680 + static_cast<int>(win::CameraHotkeyNames.size())) {
      start_button_capture(id - 680);
      update_camera_button_input();
      if (!button_input.enabled()) {
        button_capture = -1;
        const auto error = L"Could not listen for controller buttons (Windows error " + std::to_wstring(button_input.error()) + L").";
        SetDlgItemTextW(hwnd, 660, error.c_str());
      }
      refresh_shortcut_status();
      return TRUE;
    }
    if (id >= 690 && id < 690 + static_cast<int>(win::CameraHotkeyNames.size())) {
      if (button_capture == id - 690)
        button_capture = -1;
      button_draft[id - 690] = {};
      update_camera_button_input();
      refresh_shortcut_status();
      return TRUE;
    }
    if (id == IDOK) {
      std::wstring error;
      if (!win::valid_camera_hotkeys(hotkey_draft, &error) || !win::valid_camera_buttons(button_draft, &error)) {
        SetDlgItemTextW(hwnd, 660, error.c_str());
        return TRUE;
      }
      if (!win::save_camera_hotkeys(hotkey_draft, win::settings_directory())) {
        SetDlgItemTextW(hwnd, 660, L"Could not save shortcuts. Check access to the local settings folder.");
        return TRUE;
      }
      hotkey_saved = hotkey_draft;
      button_capture = -1;
      // buttons.ini is created only once a button has been set.
      const bool buttons_saved = button_draft == button_saved || win::save_camera_buttons(button_draft, win::settings_directory());
      if (buttons_saved)
        button_saved = button_draft;
      register_camera_hotkeys();
      update_camera_button_input();
      refresh_shortcut_status();
      SetDlgItemTextW(hwnd, 660,
                      !buttons_saved
                          ? L"Keyboard shortcuts saved. Could not save controller buttons. Check access to the local settings folder."
                      : hotkey_registration.conflicts()
                          ? L"Saved. Unavailable shortcuts need a different combination. The other shortcuts remain active."
                      : win::any_camera_button(button_saved) && !preview_ui && !button_input.enabled()
                          ? L"Saved. Controller buttons are unavailable; see the status below each button."
                          : L"Shortcuts and buttons saved for all aircraft. Camera settings and unfinished edits are unchanged.");
      return TRUE;
    }
    if (id == IDCANCEL) {
      EndDialog(hwnd, IDCANCEL);
      return TRUE;
    }
  }
  if (message == WM_CLOSE) {
    EndDialog(hwnd, IDCANCEL);
    return TRUE;
  }
  if (message == WM_DESTROY) {
    shortcut_window = nullptr;
    hotkey_editor_focused = false;
    hotkey_draft = hotkey_saved;
    button_draft = button_saved;
    button_capture = -1;
    register_camera_hotkeys();
    update_camera_button_input();
  }
  return FALSE;
}
void edit_camera_hotkeys() {
  const DialogTemplate layout;
  if (DialogBoxIndirectParamW(instance, &layout.dialog, window, shortcut_dialog, 0) == -1) {
    notice = L"Could not open the shortcut and button editor.";
    InvalidateRect(window, nullptr, FALSE);
  }
}
INT_PTR CALLBACK whats_new_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_INITDIALOG) {
    place_dialog(hwnd, L"Taxi Cam — What's new", 620, 480);
    dialog_control(hwnd, L"STATIC", L"What's new in Taxi Cam", -1, 20, 17, 580, 28, 0, heading);
    const auto notes = dialog_control(hwnd, L"EDIT", reinterpret_cast<const std::wstring*>(l)->c_str(), 700, 20, 56, 580, 356,
                                      WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
    SendMessageW(notes, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(scale(8), scale(8)));
    const auto close = dialog_control(hwnd, L"BUTTON", L"Close", IDCANCEL, 502, 428, 98, 34, WS_TABSTOP | BS_DEFPUSHBUTTON);
    SetFocus(close);  // Keep the notes unselected until the user tabs into them.
    return FALSE;
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
    return dialog_colors(w);
  if ((message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) || message == WM_CLOSE) {
    EndDialog(hwnd, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}
void request_whats_new() {
  if (changelog_fetcher.busy())
    return;
  if (!changelog_fetcher.begin(window, WhatsNewMessage)) {
    notice = L"Could not start loading What's new. Try again.";
  } else {
    SetDlgItemTextW(window, 515, L"Loading…");
    notice = L"Loading What's new from GitHub…";
  }
  InvalidateRect(window, nullptr, FALSE);
}
// PFD routing: a one-shot picture of a tracked display texture, with the
// selected profile's display rectangles drawn over it. The bridge writes the
// image; this dialog only reads it.
struct SnapshotView {
  std::uint64_t id{}, serial{};
  ULONGLONG requested_ms{};
  HBITMAP image{};
  int width{}, height{};
  std::wstring caption, legend;
} snapshot_view;
std::uint64_t request_display_snapshot(std::uint64_t id) {
  const std::lock_guard lock(app_mutex);
  // Past the bridge's last handled serial too, which survives a profile reload.
  current.snapshot_request = std::max(current.snapshot_request, status.snapshot_serial) + 1;
  current.snapshot_id = id;
  return current.snapshot_request;
}
void release_snapshot_image() {
  if (snapshot_view.image)
    DeleteObject(snapshot_view.image);
  snapshot_view.image = nullptr;
  snapshot_view.width = snapshot_view.height = 0;
}
// Texture each display side is routed to, from the bridge's status.
std::array<std::uint64_t, MaxDisplaySides> routed_texture_ids(const win::Status& sample, const profiles::AircraftProfile& profile) {
  const bool single = profile.pfd_detection == profiles::PfdDetectionPolicy::single_display;
  return {sample.left_id, single ? sample.left_id : sample.right_id,
          profiles::separate_lower_texture(profile) ? sample.lower_id : sample.left_id};
}
const wchar_t* snapshot_side_name(const win::Settings& s, unsigned side) {
  return side == 0 ? L"Left" : side == 1 ? L"Right" : pmdg_cam_control(s) ? L"Lower DU" : L"SD";
}
// Profile rectangles apply only when the texture has the profile's display size.
const profiles::AircraftProfile* snapshot_profile(const win::Settings& s, const win::Status& sample) {
  const auto* profile = profiles::find(s.profile);
  return profile && sample.snapshot_width == profile->width && sample.snapshot_height == profile->height ? profile : nullptr;
}
void update_snapshot_dialog(HWND hwnd) {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  const bool answered = sample.snapshot_serial == snapshot_view.serial;
  const auto result = answered ? static_cast<win::DisplaySnapshotResult>(sample.snapshot_result) : win::DisplaySnapshotResult::pending;
  if (answered && result == win::DisplaySnapshotResult::ready && !snapshot_view.image) {
    snapshot_view.image = static_cast<HBITMAP>(
        LoadImageW(nullptr, win::display_snapshot_path().c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    BITMAP bitmap{};
    if (snapshot_view.image && GetObjectW(snapshot_view.image, sizeof(bitmap), &bitmap)) {
      snapshot_view.width = bitmap.bmWidth;
      snapshot_view.height = std::abs(bitmap.bmHeight);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  wchar_t caption[320];
  if (!answered)
    std::swprintf(caption, 320, L"Texture #%llu. %ls", static_cast<unsigned long long>(snapshot_view.id),
                  GetTickCount64() - snapshot_view.requested_ms > 3000
                      ? L"Waiting for the simulator. Connect Taxi Cam with a flight loaded."
                      : L"Requesting a snapshot...");
  else
    std::swprintf(caption, 320, L"Texture #%llu | %u x %u | format %u. %ls", static_cast<unsigned long long>(sample.snapshot_id),
                  sample.snapshot_width, sample.snapshot_height, sample.snapshot_format,
                  result == win::DisplaySnapshotResult::ready && !snapshot_view.image ? L"The saved image could not be opened."
                                                                                      : win::display_snapshot_text(result));
  std::wstring legend;
  if (answered && result == win::DisplaySnapshotResult::ready) {
    const auto* profile = snapshot_profile(s, sample);
    std::wstring bound;
    if (profile) {
      const auto routed = routed_texture_ids(sample, *profile);
      for (unsigned side = 0; side < profile->sides && side < MaxDisplaySides; ++side)
        if (routed[side] == sample.snapshot_id)
          bound += (bound.empty() ? L"" : L", ") + std::wstring(snapshot_side_name(s, side));
    }
    const auto* named = profiles::find(s.profile);
    const std::wstring name = named ? named->name : L"the selected profile";
    legend = !profile ? L"This texture is not the size of a " + name + L" display texture, so no display rectangles are drawn."
             : bound.empty()
                 ? L"Not routed to a display on " + name + L". Dotted outlines show where its displays would be drawn."
                 : L"Routed to " + bound + L" on " + name + L". Solid outlines are routed display rectangles; dotted ones are not.";
  }
  if (legend != snapshot_view.legend) {
    snapshot_view.legend = legend;
    SetDlgItemTextW(hwnd, 702, legend.c_str());
  }
  if (caption != snapshot_view.caption) {
    snapshot_view.caption = caption;
    SetDlgItemTextW(hwnd, 701, caption);
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}
void paint_snapshot(HDC dc) {
  const auto area = rectangle(20, 92, 860, 560);
  HBRUSH fill = CreateSolidBrush(Sidebar);
  FillRect(dc, &area, fill);
  DeleteObject(fill);
  if (!snapshot_view.image || snapshot_view.width <= 0 || snapshot_view.height <= 0)
    return;
  const int area_width = area.right - area.left, area_height = area.bottom - area.top;
  const double fit = std::min(double(area_width) / snapshot_view.width, double(area_height) / snapshot_view.height);
  const int width = std::max(1, int(snapshot_view.width * fit)), height = std::max(1, int(snapshot_view.height * fit));
  const int left = area.left + (area_width - width) / 2, top = area.top + (area_height - height) / 2;
  HDC memory = CreateCompatibleDC(dc);
  const auto old_bitmap = SelectObject(memory, snapshot_view.image);
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  StretchBlt(dc, left, top, width, height, memory, 0, 0, snapshot_view.width, snapshot_view.height, SRCCOPY);
  SelectObject(memory, old_bitmap);
  DeleteDC(memory);
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  const auto* profile = sample.snapshot_serial == snapshot_view.serial ? snapshot_profile(s, sample) : nullptr;
  if (!profile)
    return;
  const auto routed = routed_texture_ids(sample, *profile);
  const auto saved = SaveDC(dc);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  SelectObject(dc, small);
  SetBkMode(dc, OPAQUE);
  SetBkColor(dc, Sidebar);
  for (unsigned side = 0; side < profile->sides && side < MaxDisplaySides; ++side) {
    const auto r = profiles::display_rect(*profile, side);
    const bool bound = routed[side] == sample.snapshot_id;
    const COLORREF color = bound ? Accent : Muted;
    HPEN pen = CreatePen(bound ? PS_SOLID : PS_DOT, bound ? scale(2) : 1, color);
    const auto old_pen = SelectObject(dc, pen);
    RECT box{left + MulDiv(int(r.left), width, int(profile->width)), top + MulDiv(int(r.top), height, int(profile->height)),
             left + MulDiv(int(r.right), width, int(profile->width)), top + MulDiv(int(r.bottom), height, int(profile->height))};
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    SetTextColor(dc, color);
    InflateRect(&box, -scale(4), -scale(3));
    DrawTextW(dc, snapshot_side_name(s, side), -1, &box, DT_LEFT | DT_TOP | DT_SINGLELINE);
  }
  RestoreDC(dc, saved);
}
INT_PTR CALLBACK snapshot_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM) {
  if (message == WM_INITDIALOG) {
    place_dialog(hwnd, L"Taxi Cam — Display texture snapshot", 900, 760);
    dialog_control(hwnd, L"STATIC", L"Display texture snapshot", -1, 20, 17, 860, 28, 0, heading);
    dialog_control(hwnd, L"STATIC", L"", 701, 20, 54, 860, 30);
    dialog_control(hwnd, L"STATIC", L"", 702, 20, 662, 860, 44, 0, small);
    dialog_control(hwnd, L"BUTTON", L"Take again", 703, 652, 714, 120, 34, WS_TABSTOP);
    const auto close = dialog_control(hwnd, L"BUTTON", L"Close", IDCANCEL, 782, 714, 98, 34, WS_TABSTOP | BS_DEFPUSHBUTTON);
    SetTimer(hwnd, 1, 200, nullptr);
    update_snapshot_dialog(hwnd);
    SetFocus(close);
    return FALSE;
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC)
    return dialog_colors(w);
  if (message == WM_TIMER) {
    update_snapshot_dialog(hwnd);
    return TRUE;
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(hwnd, &paint);
    paint_snapshot(dc);
    EndPaint(hwnd, &paint);
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == 703) {
    release_snapshot_image();
    snapshot_view.serial = request_display_snapshot(snapshot_view.id);
    snapshot_view.requested_ms = GetTickCount64();
    update_snapshot_dialog(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    return TRUE;
  }
  if ((message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) || message == WM_CLOSE) {
    EndDialog(hwnd, IDCANCEL);
    return TRUE;
  }
  if (message == WM_DESTROY) {
    KillTimer(hwnd, 1);
    release_snapshot_image();
  }
  return FALSE;
}
bool snapshot_dialog_open{};
void show_display_snapshot(std::uint64_t id) {
  release_snapshot_image();
  snapshot_view = {};
  snapshot_view.id = id;
  snapshot_view.serial = request_display_snapshot(id);
  snapshot_view.requested_ms = GetTickCount64();
  // The dialog owns the snapshot request until it closes; card pictures wait.
  snapshot_dialog_open = true;
  const DialogTemplate layout;
  if (DialogBoxIndirectParamW(instance, &layout.dialog, window, snapshot_dialog, 0) == -1) {
    notice = L"Could not open the display snapshot.";
    InvalidateRect(window, nullptr, FALSE);
  }
  snapshot_dialog_open = false;
}
// PFD routing and Overview cards: small pictures of the display textures,
// taken one at a time through the snapshot request while one of those pages
// is on screen. Never periodic: a picture is retaken when a page opens and it
// is older than ThumbnailFreshMs, or when the user asks for a refresh.
struct Thumbnail {
  std::uint64_t id{};
  HBITMAP image{};
  int width{}, height{};
  std::uint32_t texture_width{}, texture_height{};
  ULONGLONG taken_ms{}, tried_ms{};
};
struct Thumbnails {
  std::vector<Thumbnail> items;
  std::uint64_t epoch{}, serial{}, id{};
  ULONGLONG requested_ms{}, fresh_after{};
  bool showing{};
} thumbnails;
constexpr unsigned ThumbnailEdge = 512;
constexpr ULONGLONG ThumbnailWaitMs = 8000, ThumbnailRetryMs = 15000, ThumbnailFreshMs = 120000;
Thumbnail* find_thumbnail(std::uint64_t id) {
  for (auto& t : thumbnails.items)
    if (t.id == id)
      return &t;
  return nullptr;
}
void clear_thumbnails() {
  for (auto& t : thumbnails.items)
    if (t.image)
      DeleteObject(t.image);
  thumbnails.items.clear();
  thumbnails.serial = 0;
}
// The bridge's snapshot image, kept no larger than ThumbnailEdge.
HBITMAP load_thumbnail(int& width, int& height) {
  auto* source = static_cast<HBITMAP>(
      LoadImageW(nullptr, win::display_snapshot_path().c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
  BITMAP bitmap{};
  if (!source || !GetObjectW(source, sizeof(bitmap), &bitmap) || bitmap.bmWidth <= 0 || !bitmap.bmHeight) {
    if (source)
      DeleteObject(source);
    return nullptr;
  }
  const auto source_width = static_cast<unsigned>(bitmap.bmWidth), source_height = static_cast<unsigned>(std::abs(bitmap.bmHeight));
  unsigned fit_width = 0, fit_height = 0;
  win::fit_snapshot(source_width, source_height, ThumbnailEdge, fit_width, fit_height);
  width = static_cast<int>(fit_width);
  height = static_cast<int>(fit_height);
  if (fit_width == source_width && fit_height == source_height)
    return source;
  HDC screen = GetDC(nullptr);
  HBITMAP result = CreateCompatibleBitmap(screen, width, height);
  HDC from = CreateCompatibleDC(screen), to = CreateCompatibleDC(screen);
  if (result && from && to) {
    const auto old_from = SelectObject(from, source), old_to = SelectObject(to, result);
    SetStretchBltMode(to, HALFTONE);
    SetBrushOrgEx(to, 0, 0, nullptr);
    StretchBlt(to, 0, 0, width, height, from, 0, 0, static_cast<int>(source_width), static_cast<int>(source_height), SRCCOPY);
    SelectObject(from, old_from);
    SelectObject(to, old_to);
  }
  if (from)
    DeleteDC(from);
  if (to)
    DeleteDC(to);
  ReleaseDC(nullptr, screen);
  DeleteObject(source);
  return result;
}
// Routing lists: list 0 is the left PFD, or the shared display texture on a
// single-display aircraft; list 1 is the right PFD, or the PMDG 777 lower DU.
unsigned routing_lists(const win::Settings& s) {
  return single_display_profile(s) && !separate_lower_profile(s) ? 1 : 2;
}
std::array<std::uint64_t, 2> routed_lists(const win::Settings& s, const win::Status& sample) {
  return {sample.left_id, separate_lower_profile(s) ? sample.lower_id : routing_lists(s) == 1 ? 0 : sample.right_id};
}
std::array<std::uint64_t, 2> chosen_lists(const win::Settings& s) {
  return {s.left_id, separate_lower_profile(s) ? s.lower_id : routing_lists(s) == 1 ? 0 : s.right_id};
}
unsigned side_list(const win::Settings& s, unsigned side) {
  return separate_lower_profile(s) ? side == 2 : routing_lists(s) == 2 && side == 1;
}
const wchar_t* list_title(const win::Settings& s, unsigned list) {
  return separate_lower_profile(s) ? (list ? L"Lower DU" : L"Navigation displays")
         : routing_lists(s) == 1   ? L"Display texture"
         : list                    ? L"Right PFD"
                                   : L"Left PFD";
}
const wchar_t* list_action(const win::Settings& s, unsigned list) {
  return separate_lower_profile(s) ? (list ? L"Lower" : L"NDs") : routing_lists(s) == 1 ? L"Use" : list ? L"Right" : L"Left";
}
const wchar_t* display_side_title(const win::Settings& s, unsigned side) {
  if (pmdg_cam_control(s))
    return side == 0 ? L"L INBD" : side == 1 ? L"R INBD" : L"LWR CTR";
  return side == 0 ? L"Left PFD" : side == 1 ? L"Right PFD" : L"SD";
}
enum class RouteSource { none, detected, named, chosen };
RouteSource route_source(const win::Settings& s, const win::Status& sample, unsigned side, std::uint64_t id) {
  if (!id)
    return RouteSource::none;
  if (sample.named_mask & (1u << side))
    return RouteSource::named;
  return chosen_lists(s)[side_list(s, side)] == id ? RouteSource::chosen : RouteSource::detected;
}
const win::Candidate* find_candidate(const win::Status& sample, std::uint64_t id) {
  for (UINT i = 0; id && i < std::min(sample.candidate_count, 16u); ++i)
    if (sample.candidates[i].id == id)
      return &sample.candidates[i];
  return nullptr;
}
std::wstring candidate_name(const win::Candidate* candidate) {
  return candidate && candidate->name[0] ? widen(std::string(candidate->name, strnlen(candidate->name, sizeof(candidate->name))).c_str())
                                         : std::wstring();
}
std::wstring candidate_details(std::uint64_t id, const win::Candidate* candidate) {
  wchar_t details[96];
  if (candidate)
    std::swprintf(details, 96, L"#%llu · %u × %u · %u mips", static_cast<unsigned long long>(id), candidate->width, candidate->height,
                  candidate->mips);
  else
    std::swprintf(details, 96, L"#%llu", static_cast<unsigned long long>(id));
  return details;
}
// Gallery order: routed textures, then named ones, then the rest; by ID
// within each group so cards do not move as draw counts change.
std::vector<win::Candidate> gallery_items(const win::Settings& s, const win::Status& sample) {
  std::vector<win::Candidate> items(sample.candidates, sample.candidates + std::min(sample.candidate_count, 16u));
  const auto routed = routed_lists(s, sample);
  const auto rank = [&](const win::Candidate& c) { return c.id == routed[0] || c.id == routed[1] ? 0 : c.name[0] ? 1 : 2; };
  std::sort(items.begin(), items.end(), [&](const auto& a, const auto& b) { return rank(a) != rank(b) ? rank(a) < rank(b) : a.id < b.id; });
  return items;
}
constexpr unsigned GalleryColumns = 4;
unsigned gallery_page{};
std::array<std::uint64_t, GalleryColumns> gallery_ids{};
// Shows the card buttons of the visible gallery page and repaints their
// routed state. The buttons exist from build_controls on PFD routing only.
void update_gallery_buttons() {
  if (page != 3 || !GetDlgItem(window, 420))
    return;
  const auto s = draft();
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto items = gallery_items(s, sample);
  const unsigned pages = std::max(1u, static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns));
  gallery_page = std::min(gallery_page, pages - 1);
  const unsigned lists = routing_lists(s);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto index = gallery_page * GalleryColumns + slot;
    gallery_ids[slot] = index < items.size() ? items[index].id : 0;
    for (unsigned list = 0; list < 2; ++list) {
      HWND card_button = GetDlgItem(window, static_cast<int>(420 + slot * 2 + list));
      const bool visible = gallery_ids[slot] && list < lists;
      if (card_button && (IsWindowVisible(card_button) != FALSE) != visible)
        ShowWindow(card_button, visible ? SW_SHOWNA : SW_HIDE);
      if (card_button && visible)
        InvalidateRect(card_button, nullptr, FALSE);
    }
  }
  EnableWindow(GetDlgItem(window, 407), gallery_page > 0);
  EnableWindow(GetDlgItem(window, 408), gallery_page + 1 < pages);
}
// Whether a gallery card button's texture is routed to its list now.
bool gallery_button_routed(int id) {
  const auto slot = static_cast<unsigned>(id - 420) / 2, list = static_cast<unsigned>(id - 420) % 2;
  if (slot >= GalleryColumns || !gallery_ids[slot])
    return false;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  return routed_lists(draft(), sample)[list] == gallery_ids[slot];
}
// Sends one explicit choice per list (0 = automatic) to the bridge.
bool choose_routes(std::array<std::uint64_t, 2> choice) {
  auto s = draft();
  const bool separate = separate_lower_profile(s);
  const auto lower = separate ? choice[1] : 0;
  if (single_display_profile(s))
    choice[1] = 0;
  const auto result = win::update_target_assignment(s.left_id, s.right_id, s.route_request, choice[0], choice[1], s.lower_id, lower);
  if (result == win::TargetAssignmentResult::duplicate || result == win::TargetAssignmentResult::sequence_exhausted) {
    notice = result != win::TargetAssignmentResult::duplicate ? L"Display assignment request limit reached. Restart Taxi Cam."
             : separate ? L"Choose a lower DU texture different from the navigation display texture, or Automatic."
                        : L"Choose different textures for left and right, or Automatic.";
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  publish(s);
  dirty_notice();
  return true;
}
// A card button. Taking the texture the other list shows swaps the two, so
// one texture never serves both lists.
void choose_texture(unsigned list, std::uint64_t id) {
  const auto s = draft();
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  auto choice = chosen_lists(s);
  const auto routed = routed_lists(s, sample);
  if (routing_lists(s) == 2 && (choice[1 - list] == id || routed[1 - list] == id))
    choice[1 - list] = routed[list] != id ? routed[list] : 0;
  choice[list] = id;
  if (choose_routes(choice))
    notice = std::wstring(list_title(s, list)) + L" uses #" + std::to_wstring(id) + L" for this flight. Automatic restores detection.";
}
// Takes the next missing or stale picture. Requests go one at a time; the
// snapshot dialog has the request to itself while it is open.
void service_thumbnails() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto now = GetTickCount64();
  if (sample.aircraft_session_epoch != thumbnails.epoch) {
    clear_thumbnails();
    thumbnails.epoch = sample.aircraft_session_epoch;
  }
  if (thumbnails.serial && !snapshot_dialog_open) {
    const auto result = static_cast<win::DisplaySnapshotResult>(sample.snapshot_result);
    const bool answered = sample.snapshot_serial == thumbnails.serial && result != win::DisplaySnapshotResult::pending;
    if (!answered && now - thumbnails.requested_ms < ThumbnailWaitMs)
      return;
    if (auto* t = find_thumbnail(thumbnails.id); t && answered && result == win::DisplaySnapshotResult::ready) {
      int width = 0, height = 0;
      if (const auto image = load_thumbnail(width, height)) {
        if (t->image)
          DeleteObject(t->image);
        t->image = image;
        t->width = width;
        t->height = height;
        t->texture_width = sample.snapshot_width;
        t->texture_height = sample.snapshot_height;
        t->taken_ms = now;
      }
    }
    thumbnails.serial = 0;
    InvalidateRect(window, nullptr, FALSE);
  } else if (snapshot_dialog_open) {
    thumbnails.serial = 0;
  }
  const bool showing = IsWindowVisible(window) && !IsIconic(window) && (page == 0 || page == 3);
  if (showing && !thumbnails.showing)
    thumbnails.fresh_after = std::max(thumbnails.fresh_after, now > ThumbnailFreshMs ? now - ThumbnailFreshMs : 0);
  thumbnails.showing = showing;
  const auto s = draft();
  const auto routed = routed_lists(s, sample);
  std::vector<std::uint64_t> wanted;
  const auto want = [&](std::uint64_t id) {
    if (id && std::find(wanted.begin(), wanted.end(), id) == wanted.end())
      wanted.push_back(id);
  };
  want(routed[0]);
  want(routed[1]);
  if (page == 3) {
    for (const auto id : gallery_ids)
      want(id);
    for (const auto& c : gallery_items(s, sample))
      want(c.id);
  }
  // Destroyed textures leave the candidate list; drop their pictures.
  std::erase_if(thumbnails.items, [&](Thumbnail& t) {
    const bool gone = t.id != routed[0] && t.id != routed[1] && !find_candidate(sample, t.id);
    if (gone && t.image)
      DeleteObject(t.image);
    return gone;
  });
  if (!showing || snapshot_dialog_open || !sample.heartbeat || now < sample.heartbeat || now - sample.heartbeat > 3000 ||
      !sample.graphics_ready)
    return;
  for (const auto id : wanted) {
    auto* t = find_thumbnail(id);
    if (t && now - t->tried_ms < ThumbnailRetryMs)
      continue;
    if (t && t->image && t->taken_ms >= thumbnails.fresh_after)
      continue;
    if (!t) {
      thumbnails.items.push_back({id});
      t = &thumbnails.items.back();
    }
    t->tried_ms = now;
    thumbnails.id = id;
    thumbnails.serial = request_display_snapshot(id);
    thumbnails.requested_ms = now;
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
}
// Clickable pictures drawn by draw_page, in device pixels.
struct HitArea {
  RECT bounds{};
  std::uint64_t id{};  // Picture to enlarge; 0 opens PFD routing.
};
std::vector<HitArea> hit_areas;
const HitArea* hit_area_at(POINT point) {
  for (const auto& area : hit_areas)
    if (PtInRect(&area.bounds, point))
      return &area;
  return nullptr;
}
void chip(HDC dc, int x, int y, const wchar_t* label, COLORREF color) {
  SelectObject(dc, small);
  SIZE size{};
  GetTextExtentPoint32W(dc, label, static_cast<int>(std::wcslen(label)), &size);
  RECT r{scale(x), scale(y), scale(x) + size.cx + scale(16), scale(y + 22)};
  HBRUSH brush = CreateSolidBrush(Background);
  HPEN pen = CreatePen(PS_SOLID, 1, color);
  const auto oldb = SelectObject(dc, brush), oldp = SelectObject(dc, pen);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, scale(10), scale(10));
  SelectObject(dc, oldb);
  SelectObject(dc, oldp);
  DeleteObject(brush);
  DeleteObject(pen);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void route_chip(HDC dc, int x, int y, RouteSource source) {
  switch (source) {
    case RouteSource::named:
      return chip(dc, x, y, L"Named", Accent);
    case RouteSource::chosen:
      return chip(dc, x, y, L"Your choice", RGB(126, 176, 255));
    case RouteSource::detected:
      return chip(dc, x, y, L"Detected", Muted);
    case RouteSource::none:
      return chip(dc, x, y, L"Not found", RGB(236, 182, 92));
  }
}
// Draws a texture's picture, or the part `crop` (texture pixels) of it,
// fitted and centred in box. Returns the drawn rectangle; empty without one.
RECT draw_thumbnail(HDC dc, const RECT& box, std::uint64_t id, const wchar_t* missing, const profiles::DisplayRect* crop = nullptr) {
  HBRUSH fill = CreateSolidBrush(Sidebar);
  FillRect(dc, &box, fill);
  DeleteObject(fill);
  const auto* t = find_thumbnail(id);
  if (!id || !t || !t->image || t->width <= 0 || t->height <= 0) {
    const wchar_t* label = !id                                                   ? missing
                           : thumbnails.serial && thumbnails.id == id            ? L"Taking a picture…"
                           : t && t->tried_ms && !t->image && !thumbnails.serial ? L"No picture"
                                                                                 : L"Waiting…";
    auto r = box;
    SelectObject(dc, small);
    SetTextColor(dc, Muted);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return {};
  }
  int x = 0, y = 0, w = t->width, h = t->height;
  if (crop && t->texture_width && t->texture_height && crop->right > crop->left && crop->bottom > crop->top &&
      crop->right <= t->texture_width && crop->bottom <= t->texture_height) {
    x = MulDiv(static_cast<int>(crop->left), t->width, static_cast<int>(t->texture_width));
    y = MulDiv(static_cast<int>(crop->top), t->height, static_cast<int>(t->texture_height));
    w = std::max(1, MulDiv(static_cast<int>(crop->right), t->width, static_cast<int>(t->texture_width)) - x);
    h = std::max(1, MulDiv(static_cast<int>(crop->bottom), t->height, static_cast<int>(t->texture_height)) - y);
  }
  const int box_width = box.right - box.left, box_height = box.bottom - box.top;
  const double fit = std::min(double(box_width) / w, double(box_height) / h);
  const int width = std::max(1, int(w * fit)), height = std::max(1, int(h * fit));
  const RECT drawn{box.left + (box_width - width) / 2, box.top + (box_height - height) / 2, 0, 0};
  HDC memory = CreateCompatibleDC(dc);
  const auto old_bitmap = SelectObject(memory, t->image);
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  StretchBlt(dc, drawn.left, drawn.top, width, height, memory, x, y, w, h, SRCCOPY);
  SelectObject(memory, old_bitmap);
  DeleteDC(memory);
  return {drawn.left, drawn.top, drawn.left + width, drawn.top + height};
}
// Overview: what each display side's camera draws into, cropped from the
// texture routed to that side.
void draw_display_strip(HDC dc, const win::Status& sample) {
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  if (!profile)
    return;
  const auto routed = routed_texture_ids(sample, *profile);
  const int sides = static_cast<int>(std::min<unsigned>(profile->sides, MaxDisplaySides));
  const int gap = 10, width = (734 - gap * (sides - 1)) / sides;
  for (int side = 0; side < sides; ++side) {
    const int x = 260 + side * (width + gap), y = 553;
    const auto id = routed[side];
    panel(dc, x, y, width, 114, Background);
    const auto* t = find_thumbnail(id);
    const bool fits = t && t->texture_width == profile->width && t->texture_height == profile->height;
    const auto crop = profiles::display_rect(*profile, static_cast<unsigned>(side));
    draw_thumbnail(dc, rectangle(x + 8, y + 8, 98, 98), id, L"No display", fits ? &crop : nullptr);
    hit_areas.push_back({rectangle(x, y, width, 114), 0});
    const int tx = x + 116, tw = width - 124;
    text(dc, display_side_title(s, static_cast<unsigned>(side)), tx, y + 8, tw, 26, heading);
    const auto* candidate = find_candidate(sample, id);
    const auto name = candidate_name(candidate);
    text(dc, id ? (name.empty() ? (L"#" + std::to_wstring(id)).c_str() : name.c_str()) : L"Waiting for detection", tx, y + 38, tw, 22,
         small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    route_chip(dc, tx, y + 76, route_source(s, sample, static_cast<unsigned>(side), id));
  }
}
// PFD routing: the texture behind each routing list, with the display
// rectangles it serves outlined.
void draw_routing_slots(HDC dc, const win::Status& sample) {
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  const auto routed = routed_lists(s, sample);
  const unsigned lists = routing_lists(s);
  for (unsigned list = 0; list < lists; ++list) {
    const int x = 260 + static_cast<int>(list) * 375, y = 169, width = lists == 1 ? 734 : 359;
    const auto id = routed[list];
    panel(dc, x, y, width, 130, Background);
    const auto box = rectangle(x + 7, y + 7, 116, 116);
    const auto drawn = draw_thumbnail(dc, box, id, L"Not found");
    const auto* t = find_thumbnail(id);
    if (id && drawn.right > drawn.left && profile && t && t->texture_width == profile->width && t->texture_height == profile->height) {
      const auto sides = routed_texture_ids(sample, *profile);
      HPEN pen = CreatePen(PS_SOLID, scale(2), Accent);
      const auto old_pen = SelectObject(dc, pen);
      const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      const int dw = drawn.right - drawn.left, dh = drawn.bottom - drawn.top;
      for (unsigned side = 0; side < profile->sides && side < MaxDisplaySides; ++side) {
        if (sides[side] != id)
          continue;
        const auto r = profiles::display_rect(*profile, side);
        Rectangle(dc, drawn.left + MulDiv(int(r.left), dw, int(profile->width)), drawn.top + MulDiv(int(r.top), dh, int(profile->height)),
                  drawn.left + MulDiv(int(r.right), dw, int(profile->width)), drawn.top + MulDiv(int(r.bottom), dh, int(profile->height)));
      }
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
    }
    if (id)
      hit_areas.push_back({box, id});
    const int tx = x + 136, tw = width - 144;
    text(dc, list_title(s, list), tx, y + 8, tw, 28, heading);
    const auto* candidate = find_candidate(sample, id);
    const auto name = candidate_name(candidate);
    text(dc,
         !id            ? L"Choose a texture below, or wait for detection."
         : name.empty() ? L"Unnamed texture"
                        : name.c_str(),
         tx, y + 40, tw, 24, normal, id && !name.empty() ? Text : Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (id)
      text(dc, candidate_details(id, candidate).c_str(), tx, y + 66, tw, 22, small, Muted);
    const unsigned side = list && separate_lower_profile(s) ? 2 : list;
    route_chip(dc, tx, y + 96, route_source(s, sample, side, id));
  }
}
// PFD routing gallery: one card per tracked display texture on this page.
void draw_gallery(HDC dc, const win::Status& sample) {
  const auto s = draft();
  const auto items = gallery_items(s, sample);
  const auto routed = routed_lists(s, sample);
  const bool any_named = std::any_of(items.begin(), items.end(), [](const auto& c) { return c.name[0] != 0; });
  if (items.empty()) {
    text(dc,
         sample.graphics_ready ? L"Waiting for cockpit displays to be drawn. Restart Flight only if this stays empty."
                               : L"Connect Taxi Cam with a flight loaded to see the display textures.",
         260, 440, 734, 60, normal, Muted, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    return;
  }
  const unsigned pages = static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns);
  wchar_t page_label[32];
  std::swprintf(page_label, 32, L"%u / %u", std::min(gallery_page, pages - 1) + 1, pages);
  text(dc, page_label, 806, 327, 60, 32, small, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto id = gallery_ids[slot];
    const auto* candidate = find_candidate(sample, id);
    if (!id || !candidate)
      continue;
    const int x = 260 + static_cast<int>(slot) * 186, y = 367;
    const bool in_use = id == routed[0] || id == routed[1];
    panel(dc, x, y, 178, 236, Background);
    const auto box = rectangle(x + 6, y + 6, 166, 140);
    draw_thumbnail(dc, box, id, L"");
    hit_areas.push_back({box, id});
    if (in_use)
      chip(dc, x + 12, y + 12, routing_lists(s) == 1 ? L"In use" : list_action(s, id == routed[0] ? 0 : 1), Accent);
    const auto name = candidate_name(candidate);
    text(dc, name.empty() ? L"Unnamed" : name.c_str(), x + 10, y + 150, 158, 22, normal, name.empty() && any_named ? Muted : Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    wchar_t details[64];
    std::swprintf(details, 64, L"#%llu · %u × %u · m%u", static_cast<unsigned long long>(id), candidate->width, candidate->height,
                  candidate->mips);
    text(dc, details, x + 10, y + 172, 158, 20, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}
// Preview and Calibrate rows: the group label sits before each set of sides.
int side_toggle_x(unsigned group, unsigned sides) {
  return group ? 312 + static_cast<int>(sides) * 100 + 86 : 312;
}
void show_whats_new() {
  win::ChangelogFetchResult result;
  if (!changelog_fetcher.take(result))
    return;
  SetDlgItemTextW(window, 515, L"What's new");
  std::vector<win::ChangelogRelease> releases;
  if (!result.ok || !win::parse_changelog(result.body, releases)) {
    notice = result.ok ? L"What's new could not be read. Try again later."
                       : L"Could not load What's new from GitHub. Check your connection and try again.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  const auto text = win::format_changelog(win::releases_up_to(releases, InstalledVersion), InstalledVersion);
  const DialogTemplate layout;
  if (DialogBoxIndirectParamW(instance, &layout.dialog, window, whats_new_dialog, reinterpret_cast<LPARAM>(&text)) == -1) {
    notice = L"Could not open What's new.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  // Read once per installed version; the link returns after the next update.
  whats_new = false;
  if (const auto link = GetDlgItem(window, 515)) {
    if (GetFocus() == link)
      SetFocus(GetDlgItem(window, 514));
    ShowWindow(link, SW_HIDE);
  }
  if (!win::record_whats_new_seen(win::settings_directory(), InstalledVersion))
    notice = L"Could not remember that What's new was read. It will show again next launch.";
  InvalidateRect(window, nullptr, FALSE);
}
bool apply(bool save = true) {
  auto settings = draft();
  const wchar_t* field_error{};
  if (!read_fields(settings, &field_error)) {
    notice = field_error ? field_error
             : page == 5 ? L"Guide X must be 0–50%; Y must be 0–100%. Enter finite numbers."
                         : L"Check the values: brightness −4 to +2 EV in 0.25 steps, lens 0.05–1.55.";
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  if (save && !win::save_settings(settings)) {
    notice = L"Could not save settings. Check access to your local settings folder.";
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  publish(settings);
  dirty = false;
  notice = save ? L"Saved. Adjustments apply while the cameras are running." : L"Settings updated.";
  if (save && hotkey_registration.conflicts())
    notice = L"Saved. Some shortcuts are unavailable; check Overview > Flight-deck control.";
  InvalidateRect(window, nullptr, FALSE);
  return true;
}
// Runs on the UI thread, including when hidden to the tray.
void sync_aircraft_session() {
  auto s = draft();
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto now = GetTickCount64();
  if (!sample.heartbeat || now < sample.heartbeat || now - sample.heartbeat > 3000 ||
      sample.aircraft_session_epoch == s.aircraft_session_epoch)
    return;
  win::reset_aircraft_session(s, sample.aircraft_session_epoch);
  publish(s);
  profile_selection = {};
  // Do not rebuild numeric edits when a flight changes in the background.
  SetDlgItemTextW(window, 229, L"Scene test: Off");
  refresh_side_toggles(s);
  update_gallery_buttons();
}
void toggle_camera_from_hotkey(unsigned action) {
  // Keep unfinished numeric edits in their controls. A global shortcut must not
  // run Apply or rebuild the page, even when the companion is hidden.
  sync_aircraft_session();
  auto s = draft();
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto now = GetTickCount64();
  if (const auto* profile = profiles::find(s.profile); profile && !win::camera_action_sides(*profile, action)) {
    notice = L"This aircraft has no lower ECAM (SD) camera.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  const auto result = win::request_camera_hotkey(s, action, sample, now);
  if (result == win::CameraHotkeyResult::unavailable) {
    notice = s.enabled ? L"Waiting for current aircraft TAXI-button state. Try the shortcut again when connected."
                       : L"Choose Connect before using aircraft camera shortcuts.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  publish(s);
  dirty_notice();
  notice = L"Camera request: left " + std::wstring(s.manual_mask & 1 ? L"on" : L"off") + L", right " +
           (s.manual_mask & 2 ? L"on" : L"off") + L".";
  if (const auto* profile = profiles::find(s.profile); profile && profile->sides > 2)
    notice.insert(notice.size() - 1, std::wstring(pmdg_cam_control(s) ? L", lower DU " : L", SD ") + (s.manual_mask & 4 ? L"on" : L"off"));
  if (pmdg_cam_control(s) && s.follow_taxi)
    notice += L" Displays selected with CAM stay on until CAM is pressed again.";
  if (!s.enabled)
    notice = L"Camera request updated. Choose Connect to enable camera output.";
  refresh_side_toggles(s);
  SetDlgItemTextW(window, 221,
                  pmdg_cam_control(s) ? (s.follow_taxi ? L"CAM button: On" : L"CAM button: Off")
                  : s.follow_taxi     ? L"TAXI buttons: On"
                                      : L"TAXI buttons: Off");
  SetDlgItemTextW(window, 229, L"Scene test: Off");
  InvalidateRect(window, nullptr, FALSE);
}
void auto_profile() {
  const auto s = draft();
  if (!s.auto_profile) {
    profile_selection = {};
    return;
  }
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto now = GetTickCount64();
  if (!sample.heartbeat || now < sample.heartbeat || now - sample.heartbeat > 3000)
    return;
  const auto detected = profile_selection.observe(sample.detected_profile, sample.identity_sample_ms);
  if (!detected || detected == s.profile)
    return;
  // Preserve edits on the departing profile; invalid unfinished fields delay
  // switching instead of silently discarding the user's calibration.
  if (!apply())
    return;
  win::Settings next;
  if (!win::load_settings(next, installation, detected))
    return;
  next.enabled = s.enabled;
  if (!win::prepare_profile_selection(next, s, true) || !win::save_settings(next))
    return;
  publish(next);
  notice = L"Aircraft detected. Its saved calibration is active.";
  build_controls();
}
void build_controls() {
  refreshing = true;
  if (sidebar_tooltip) {
    DestroyWindow(sidebar_tooltip);
    sidebar_tooltip = nullptr;
  }
  for (HWND h : controls)
    DestroyWindow(h);
  controls.clear();
  navigation.clear();
  const auto s = draft();
  const wchar_t* names[]{L"Overview", L"Camera views", L"Display", L"PFD routing", L"Diagnostics", L"Reference guides"};
  for (int i = 0; i < 6; ++i)
    navigation.push_back(button(names[i], 100 + i, 20, 156 + i * 49, 166, 40));
  const auto* nav_profile = profiles::find(s.profile);
  HWND guides_nav = GetDlgItem(window, 105);
  const bool guides_enabled = !nav_profile || nav_profile->reference_guides;
  EnableWindow(guides_nav, guides_enabled);
  InvalidateRect(guides_nav, nullptr, FALSE);
  const auto donate_button = button(L"Donate", 513, 24, 590, 110, 40);
  const auto report_button = button(L"Report a bug", 512, 24, 638, 40, 40);
  const auto version_link = button(L"v" TAXI_CAM_VERSION_WIDE, 514, 24, 692, 155, 22);
  SendMessageW(version_link, WM_SETFONT, reinterpret_cast<WPARAM>(version_font), TRUE);
  HWND whats_new_link{};
  if (whats_new) {
    whats_new_link = button(changelog_fetcher.busy() ? L"Loading…" : L"What's new", 515, 76, 647, 110, 22);
    SendMessageW(whats_new_link, WM_SETFONT, reinterpret_cast<WPARAM>(version_font), TRUE);
  }
  sidebar_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
  if (sidebar_tooltip) {
    TOOLINFOW tip{};
    tip.cbSize = sizeof(tip);
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window;
    tip.uId = reinterpret_cast<UINT_PTR>(report_button);
    tip.lpszText = const_cast<wchar_t*>(L"Report a bug");
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    tip.uId = reinterpret_cast<UINT_PTR>(donate_button);
    tip.lpszText = const_cast<wchar_t*>(L"Donate via PayPal");
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    tip.uId = reinterpret_cast<UINT_PTR>(version_link);
    tip.lpszText = const_cast<wchar_t*>(L"Open Taxi Cam on GitHub");
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    if (whats_new_link) {
      tip.uId = reinterpret_cast<UINT_PTR>(whats_new_link);
      tip.lpszText = const_cast<wchar_t*>(L"See what changed in this version");
      SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
  }
  button(L"Menu", 602, 930, 37, 80, 34);
  button(L"Save changes", 500, 835, 686, 175, 42);
  button(L"Hide to tray", 501, 650, 686, 165, 42);
  if (page == 0) {
    HWND combo = child(L"COMBOBOX", L"", 210, 260, 326, 420, 220, CBS_DROPDOWNLIST | WS_VSCROLL);
    for (const auto* profile : profiles::Catalog)
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile->name));
    for (size_t i = 0; i < profiles::Catalog.size(); ++i)
      if (profiles::Catalog[i]->id == s.profile)
        SendMessageW(combo, CB_SETCURSEL, i, 0);
    toggle(L"Auto aircraft", 230, s.auto_profile, 707, 318, 140);
    toggle(L"Auto-connect", 240, auto_connect.load(std::memory_order_acquire), 707, 236, 155);
    button(win::connection_button_label(connection_requested.load(std::memory_order_acquire)), 241, 260, 236, 155, 34);
    const auto* profile = profiles::find(s.profile);
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    toggle(pmdg_cam_control(s) ? L"CAM button" : L"TAXI buttons", 221, s.follow_taxi && !manual, 800, 412, 180);
    EnableWindow(GetDlgItem(window, 221), !manual);
    button(L"Shortcuts and buttons…", 645, 580, 412, 205);
    button(L"Refresh", 409, 880, 512, 110, 32);
  } else if (page == 1) {
    const auto* profile = profiles::find(s.profile);
    const int feed_count = profile && profile->composition.split_bottom != 0 ? 3 : 2;
    for (int i = 0; i < feed_count; ++i) {
      const int x = feed_count == 3 ? (244 + i * 252) : (260 + i * 375);
      for (int j = 0; j < 6; ++j)
        edit(s.mounts[i][j], 300 + i * 10 + j, x + (j % 3) * (feed_count == 3 ? 72 : 106), 245 + (j / 3) * 108,
             feed_count == 3 ? 64 : 88);
      button(L"Lower 0.25 m", 330 + i * 10, x, 439, feed_count == 3 ? 110 : 146);
      button(L"Raise 0.25 m", 331 + i * 10, x + (feed_count == 3 ? 120 : 160), 439, feed_count == 3 ? 110 : 146);
      button(L"Aft 1 m", 332 + i * 10, x, 488, feed_count == 3 ? 110 : 146);
      button(L"Forward 1 m", 333 + i * 10, x + (feed_count == 3 ? 120 : 160), 488, feed_count == 3 ? 110 : 146);
    }
    button(L"Reset camera mounts", 359, 260, 594, 240);
  } else if (page == 2) {
    pending_camera_mode = s.camera_mode < kCameraModeCount ? s.camera_mode : static_cast<unsigned>(kDefaultCameraMode);
    for (unsigned slot = 0; slot < kCameraModeCount; ++slot) {
      const auto mode = static_cast<unsigned>(CameraModeOrder[slot]);
      button(CameraModeNames[mode], CameraModeCardId + static_cast<int>(mode), 264 + static_cast<int>(slot) * 149, 186, 138, 102);
    }
    HWND target = child(TRACKBAR_CLASSW, L"", CameraTargetId, 420, 300, 470, 32, TBS_HORZ | TBS_NOTICKS);
    SendMessageW(target, TBM_SETRANGE, FALSE, MAKELPARAM(kMinimumCameraRate, kMaximumCameraRate));
    SendMessageW(target, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(target, TBM_SETPOS, TRUE, std::clamp(s.camera_rate, kMinimumCameraRate, kMaximumCameraRate));
    EnableWindow(target, pending_camera_mode == static_cast<unsigned>(CameraMode::custom));
    edit(s.day_brightness, 201, 840, 420, 120);
    edit(s.night_brightness, 202, 840, 518, 120);
    button(L"Ground-speed colour", 231, 740, 630, 235);
    const auto* display_profile = profiles::find(s.profile);
    EnableWindow(GetDlgItem(window, 231), !display_profile || display_profile->ground_speed);
  } else if (page == 3) {
    button(L"Automatic", 406, 525, 126, 120);
    button(L"Swap left / right", 403, 655, 126, 165);
    EnableWindow(GetDlgItem(window, 403), !single_display_profile(s));
    toggle(L"Auto detect", 223, s.auto_detect, 830, 126, 165);
    button(L"Refresh pictures", 402, 640, 327, 160, 32);
    button(L"‹", 407, 870, 327, 56, 32);
    button(L"›", 408, 934, 327, 56, 32);
    const unsigned lists = routing_lists(s);
    for (unsigned slot = 0; slot < GalleryColumns; ++slot)
      for (unsigned list = 0; list < lists; ++list)
        ShowWindow(button(list_action(s, list), static_cast<int>(420 + slot * 2 + list),
                          266 + static_cast<int>(slot) * 186 + static_cast<int>(list) * 86, 563, lists == 1 ? 166 : 80, 32),
                   SW_HIDE);
    const auto* side_profile = profiles::find(s.profile);
    const unsigned sides = side_profile && side_profile->sides > 2 ? 3 : 2;
    constexpr int previews[]{224, 225, 232}, calibrations[]{226, 227, 233};
    for (unsigned side = 0; side < sides; ++side) {
      toggle(side_toggle_name(s, side), previews[side], (s.manual_mask & (1u << side)) != 0,
             side_toggle_x(0, sides) + static_cast<int>(side) * 100, 622, 96);
      toggle(side_toggle_name(s, side), calibrations[side], (s.calibration_mask & (1u << side)) != 0,
             side_toggle_x(1, sides) + static_cast<int>(side) * 100, 622, 96);
    }
    update_gallery_buttons();
  } else if (page == 4) {
    toggle(L"Scene test", 229, s.scene_test, 260, 449, 200);
    toggle(L"First camera only", 228, s.single_camera, 505, 449, 200);
    edit(s.calibration_budget, 203, 840, 548, 120);
    button(L"Open log folder", 510, 260, 591, 210);
    button(L"Stop camera tests", 511, 500, 591, 210);
  } else if (page == 5) {
    const auto* guide_profile = profiles::find(s.profile);
    const bool draw_guides = !guide_profile || guide_profile->reference_guides;
    const std::array<float, 2> guides[]{s.nose_dot, s.tail_upper, s.tail_corner, s.tail_inner};
    for (unsigned i = 0; i < 4; ++i) {
      const int y = i ? 338 + static_cast<int>(i - 1) * 64 : 204;
      edit(guides[i][0] * 100., 360 + static_cast<int>(i * 2), 505, y, 113);
      edit(guides[i][1] * 100., 361 + static_cast<int>(i * 2), 655, y, 113);
      EnableWindow(GetDlgItem(window, 360 + static_cast<int>(i * 2)), draw_guides);
      EnableWindow(GetDlgItem(window, 361 + static_cast<int>(i * 2)), draw_guides);
    }
    button(L"Apply live", 370, 260, 608, 185);
    button(L"Reset guides", 371, 467, 608, 250);
    button(L"Marking colour", 372, 740, 608, 235);
    EnableWindow(GetDlgItem(window, 370), draw_guides);
    EnableWindow(GetDlgItem(window, 371), draw_guides);
    EnableWindow(GetDlgItem(window, 372), draw_guides);
  }
  refreshing = false;
  InvalidateRect(window, nullptr, TRUE);
}
HICON make_icon() {
  // LoadIcon returns shared handles, including the fallback: do not destroy them.
  const auto resource = LoadIconW(instance, MAKEINTRESOURCEW(101));
  return resource ? resource : LoadIconW(nullptr, IDI_APPLICATION);
}
void tray(bool add) {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = 1;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = TrayMessage;
  data.hIcon = icon;
  wcscpy_s(data.szTip, L"Taxi Cam — Settings");
  Shell_NotifyIconW(add ? NIM_ADD : NIM_DELETE, &data);
  if (add) {
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
  }
}
// NIM_MODIFY balloons need the icon still registered (NIM_ADD already done) and,
// under NOTIFYICON_VERSION_4 on Windows 10/11, a process AppUserModelID so the
// shell can deliver a toast instead of silently dropping the legacy balloon.
void fill_tray_identity(NOTIFYICONDATAW& data) {
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = 1;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
  data.uCallbackMessage = TrayMessage;
  data.hIcon = icon;
}
void surface_notification(const std::wstring& body, bool alert) {
  if (body.empty())
    return;
  NOTIFYICONDATAW data{};
  fill_tray_identity(data);
  // Never NIIF_RESPECT_QUIET_TIME: Focus Assist "when I'm playing a game" (and
  // Quiet Hours) suppress those banners while MSFS is fullscreen — exactly when
  // bridge events matter. Shell_NotifyIcon still returns success when suppressed.
  data.dwInfoFlags = alert ? NIIF_WARNING : NIIF_INFO;
  wcscpy_s(data.szInfoTitle, L"Taxi Cam");
  wcscpy_s(data.szInfo, body.c_str());
  std::wstring tip = body;
  if (tip.size() >= 128)
    tip.resize(127);
  for (auto& ch : tip)
    if (ch == L'\n')
      ch = L' ';
  wcscpy_s(data.szTip, tip.c_str());
  Shell_NotifyIconW(NIM_MODIFY, &data);
  // Fallback when the OS still hides the banner (Focus Assist priority, per-app
  // toast mute on a generated AUMID, full-screen cover): status line + taskbar flash.
  notice = body;
  if (window) {
    FLASHWINFO flash{sizeof(flash), window, FLASHW_TRAY | FLASHW_TIMERNOFG, 4, 0};
    FlashWindowEx(&flash);
    InvalidateRect(window, nullptr, FALSE);
  }
}
void update_balloon() {
  surface_notification(
      L"Update downloaded. Close Microsoft Flight Simulator, then use Check for updates in the tray menu to install.",
      false);
}
// Bridge events as tray notifications. Events from one sample share a balloon
// while they fit; a later balloon replaces an earlier one on screen, and the
// notification centre keeps them. The bridge applied the repeat limiter.
void show_notifications() {
  std::vector<SimEvent> events;
  {
    const std::lock_guard lock(app_mutex);
    events.swap(pending_notifications);
  }
  if (events.empty() || preview_ui)
    return;
  NOTIFYICONDATAW sizing{};
  constexpr std::size_t capacity = sizeof(sizing.szInfo) / sizeof(sizing.szInfo[0]) - 1;
  std::wstring body;
  bool alert = false;
  const auto flush = [&] {
    if (body.empty())
      return;
    surface_notification(body, alert);
    body.clear();
    alert = false;
  };
  for (const auto event : events) {
    const auto message = sim_message_for(event);
    std::wstring line;
    for (const char* c = message.text; *c; ++c)
      line += static_cast<wchar_t>(static_cast<unsigned char>(*c));
    if (line.empty() || line.size() > capacity)
      continue;
    if (!body.empty() && body.size() + 1 + line.size() > capacity)
      flush();
    if (!body.empty())
      body += L'\n';
    body += line;
    alert = alert || message.alert;
  }
  flush();
}
void show() {
  ShowWindow(window, SW_SHOW);
  ShowWindow(window, SW_RESTORE);
  SetForegroundWindow(window);
}
void draw_page(HDC dc) {
  hit_areas.clear();
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, background_brush);
  auto left = rectangle(0, 0, 210, 780);
  HBRUSH b = CreateSolidBrush(Sidebar);
  FillRect(dc, &left, b);
  DeleteObject(b);
  DrawIconEx(dc, scale(24), scale(35), icon, scale(32), scale(32), 0, nullptr, DI_NORMAL);
  text(dc, L"TAXI CAM", 68, 33, 134, 22, heading);
  text(dc, L"Native taxi cameras", 24, 84, 176, 22, small, Muted);
  text(dc, L"WINDOWS COMPANION", 24, 120, 182, 22, small, Muted);
  const wchar_t* titles[]{L"Taxi camera", L"Camera views", L"Display", L"PFD routing", L"Diagnostics", L"Reference guides"};
  const wchar_t* subtitles[]{L"Your taxi cameras, controlled from the flight deck.",
                             L"Fine-tune each camera independently. Changes stay with this aircraft.",
                             L"Balance visibility, colour and camera update rate.",
                             L"Connect each TAXI button to the correct display.",
                             L"Live status and the controls used during camera testing.",
                             L"Move the guide points, preview them live, then save for this aircraft."};
  text(dc, titles[page], 244, 30, 740, 48, title_font);
  text(dc, subtitles[page], 247, 84, 758, 30, normal, Muted);
  win::Status sample;
  std::wstring live;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
    live = connection;
  }
  const bool has_displays = sample.candidate_count != 0 || sample.left_id != 0 || sample.right_id != 0 || sample.lower_id != 0;
  if (page == 0) {
    panel(dc, 244, 137, 766, 140);
    text(dc,
         connection_disconnected.load(std::memory_order_acquire) ? L"Disconnected"
         : !sample.graphics_ready                                ? L"Waiting for the simulator"
         : has_displays                                          ? L"Native bridge connected"
                                                                 : L"Native bridge connected — waiting for cockpit displays",
         266, 153, 500, 30, heading, sample.graphics_ready && has_displays ? Accent : Text);
    const bool late_empty_pfds = sample.graphics_ready && !has_displays;
    const auto line = late_empty_pfds    ? std::wstring(
                                               L"Waiting for cockpit displays to be drawn. "
                                               L"Restart Flight only if the list stays empty.")
                      : sample.heartbeat ? widen(sample.message)
                                         : live;
    text(dc, line.c_str(), 266, 188, 715, 36, normal, late_empty_pfds ? Accent : Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 295, 766, 93);
    text(dc, L"Aircraft profile", 260, 298, 350, 22, small, Muted);
    panel(dc, 244, 395, 766, 96);
    text(dc, L"Flight-deck control", 264, 406, 300, 30, heading);
    const auto* profile = profiles::find(draft().profile);
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    const bool cam = profile && profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam;
    text(dc,
         manual ? L"Use Shortcuts and buttons or PFD routing previews for this aircraft."
         : cam  ? L"Select L INBD, R INBD or LWR CTR, then press CAM. Press CAM again with that display selected to turn it off."
                : L"Left and right EFIS TAXI buttons activate their own PFD.",
         264, 446, 530, 40, small, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 505, 766, 171);
    text(dc, L"Cockpit displays", 264, 513, 300, 30, heading);
    text(dc, L"Where each camera is drawn. Select one to change it.", 470, 513, 400, 30, small, Muted);
    draw_display_strip(dc, sample);
  } else if (page == 1) {
    constexpr const wchar_t* labels[]{L"Right (m)", L"Up (m)", L"Forward (m)", L"Pitch (deg)", L"Yaw (deg)", L"Lens (rad)"};
    const auto* profile = profiles::find(draft().profile);
    const bool split = profile && profile->composition.split_bottom != 0;
    const int feed_count = split ? 3 : 2;
    const wchar_t* titles[]{L"Nose-wheel camera", split ? L"Left wing camera" : L"Tail camera", L"Right wing camera"};
    const wchar_t* views[]{L"UPPER VIEW", split ? L"LOWER LEFT" : L"LOWER VIEW", L"LOWER RIGHT"};
    for (int i = 0; i < feed_count; ++i) {
      const int x = split ? (244 + i * 252) : (244 + i * 375);
      const int width = split ? 240 : 354;
      panel(dc, x, 138, width, 424);
      text(dc, titles[i], x + 12, 154, width - 24, 30, heading);
      const auto& dimensions = (profile ? *profile : profiles::A380).camera_panes[i];
      wchar_t view_label[96];
      std::swprintf(view_label, 96, L"%ls · %d × %d", views[i], dimensions[0], dimensions[1]);
      text(dc, view_label, x + 12, 190, width - 24, 22, small, Muted);
      for (int j = 0; j < 6; ++j)
        text(dc, labels[j], x + 12 + (j % 3) * (split ? 72 : 106), 215 + (j / 3) * 108, split ? 68 : 98, 24, small, Muted);
      text(dc, L"Position relative to the aircraft datum", x + 12, 397, width - 24, 22, small, Muted);
    }
    text(dc, L"Positive pitch looks up. Positive yaw looks right.", 530, 594, 462, 45, small, Muted, DT_LEFT | DT_WORDBREAK);
  } else if (page == 2) {
    // The camera images take the main view's lighting; brightness is the
    // user's offset on top of it.
    const bool custom = selected_camera_mode() == static_cast<unsigned>(CameraMode::custom);
    panel(dc, 244, 119, 766, 262);
    text(dc, L"Camera frame rate", 264, 127, 400, 29, heading);
    text(dc, L"Frames per second for each taxi camera, parked or moving. Auto keeps the simulator within 10% of its frame rate.", 264, 157,
         734, 24, small, Muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, L"Custom target", 264, 305, 150, 24, normal, custom ? Text : Muted);
    wchar_t target_label[32];
    std::swprintf(target_label, 32, L"%u fps", selected_camera_target());
    text(dc, target_label, 900, 305, 98, 24, normal, custom ? Text : Muted);
    if (sample.heartbeat) {
      wchar_t rate_line[256];
      if (sample.reachable_rate > 0)
        std::swprintf(rate_line, 256, L"Now: each camera about %.1f fps (target %u fps%ls), simulator %.0f fps.", sample.reachable_rate,
                      sample.effective_rate, camera_rate_limit_text(sample.rate_limits), sample.update_hz);
      else
        std::swprintf(rate_line, 256, L"Now: target %u fps per camera%ls. Measuring the simulator's frame rate.", sample.effective_rate,
                      camera_rate_limit_text(sample.rate_limits));
      std::wstring line = rate_line;
      // Saved Auto: the update rate it keeps the simulator above.
      if (draft().camera_mode == static_cast<std::uint32_t>(CameraMode::automatic) && sample.auto_floor > 0) {
        wchar_t auto_line[96];
        std::swprintf(auto_line, 96, L" Auto keeps the simulator above %.0f fps.", sample.auto_floor);
        line += auto_line;
      }
      text(dc, line.c_str(), 264, 341, 734, 36, small, Muted, DT_LEFT | DT_WORDBREAK);
    }
    // The camera images take the main view's lighting; brightness is the
    // user's offset on top of it.
    const int ys[]{391, 489};
    const wchar_t* names[]{L"Daytime brightness", L"Night brightness"};
    const wchar_t* descriptions[]{L"EV added to the main view's exposure in daylight, −4 to +2 in 0.25 steps. 0 matches the main view.",
                                  L"EV added at night, −4 to +2 in 0.25 steps. Dusk blends the two with the ambient light."};
    for (int i = 0; i < 2; ++i) {
      panel(dc, 244, ys[i], 766, 88);
      text(dc, names[i], 264, ys[i] + 8, 515, 29, heading);
      text(dc, descriptions[i], 264, ys[i] + 40, 525, 41, small, Muted, DT_LEFT | DT_WORDBREAK);
    }
  } else if (page == 3) {
    panel(dc, 244, 119, 766, 190);
    text(dc, L"Assigned displays", 262, 127, 255, 34, heading);
    draw_routing_slots(dc, sample);
    panel(dc, 244, 319, 766, 294);
    text(dc, L"Display textures", 262, 327, 180, 32, heading);
    text(dc, L"Select a picture to enlarge it.", 446, 327, 190, 32, small, Muted);
    draw_gallery(dc, sample);
    const auto s = draft();
    const auto* profile = profiles::find(s.profile);
    const unsigned sides = profile && profile->sides > 2 ? 3 : 2;
    text(dc, L"Preview", 250, 622, 60, 36, small, Muted);
    text(dc, L"Calibrate", side_toggle_x(1, sides) - 76, 622, 72, 36, small, Muted);
    text(dc,
         pmdg_cam_control(s) ? L"Previews add displays on top of the CAM button. Calibration bars identify each screen and turn CAM off."
         : profile && profile->taxi_control == profiles::TaxiControl::manual_only
             ? L"Previews are this aircraft's camera control. Calibration bars identify each screen."
             : L"Previews and calibration turn off TAXI-button control. Calibration bars identify each screen.",
         250, 660, 760, 22, small, Muted);
  } else if (page == 4) {
    panel(dc, 244, 138, 766, 277);
    wchar_t data[1024];
    std::swprintf(data, 1024,
                  L"Bridge                 %s\nCamera pair         %s\nLeft / right PFD    %llu / %llu\nCaptured frames  "
                  L"%llu\nCompositions       %llu\nPFD writes            %llu\nHook failures        %llu",
                  sample.graphics_ready ? (has_displays ? L"Connected" : L"Waiting for displays") : L"Waiting",
                  sample.scene_ready ? L"Ready" : L"Waiting", static_cast<unsigned long long>(sample.left_id),
                  static_cast<unsigned long long>(sample.right_id), static_cast<unsigned long long>(sample.captures),
                  static_cast<unsigned long long>(sample.composed), static_cast<unsigned long long>(sample.stamps),
                  static_cast<unsigned long long>(sample.hook_failures));
    text(dc, data, 266, 153, 355, 242, normal, Text, DT_LEFT | DT_WORDBREAK);
    std::swprintf(data, 1024,
                  L"Probe CPU: %.2f ms (max %.2f)\nManager %.3f   Pool %.3f\nLifecycle %.3f   Entries %.3f\nView 1 %.3f   View 2 "
                  L"%.3f\nHandoff %.3f   Pose %.3f\nActivation %.3f   Publish %.3f\n\nExcludes engine rendering and GPU time.",
                  sample.probe_cpu_ms, sample.probe_max_ms, sample.stage_ms[0], sample.stage_ms[1], sample.stage_ms[2], sample.stage_ms[3],
                  sample.stage_ms[4], sample.stage_ms[5], sample.stage_ms[6], sample.stage_ms[7], sample.stage_ms[8], sample.stage_ms[9]);
    text(dc, data, 637, 156, 350, 236, small, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 436, 766, 102);
    text(dc, L"Scene test renders without PFD delivery.", 260, 488, 730, 42, small, Muted, DT_LEFT | DT_WORDBREAK);
    const auto line = sample.heartbeat ? widen(sample.message) : live;
    text(dc, L"Calibration batches per 50 ms (64–16384)", 260, 546, 550, 27, small, Muted);
    text(dc, line.c_str(), 260, 635, 730, 38, small, Muted, DT_LEFT | DT_WORDBREAK);
  } else if (page == 5) {
    panel(dc, 244, 138, 766, 118);
    panel(dc, 244, 278, 766, 259);
    text(dc, L"Nose-wheel view", 260, 150, 250, 30, heading);
    const auto* guide_profile = profiles::find(draft().profile);
    const bool draw_guides = !guide_profile || guide_profile->reference_guides;
    const bool nose_squares = (guide_profile ? guide_profile : &profiles::A380)->composition.square_nose_markers != 0;
    text(dc, nose_squares ? L"Nose squares" : L"Nose dot", 260, 205, 225, 28, normal);
    text(dc, L"Tail view", 260, 289, 250, 30, heading);
    const wchar_t* labels[]{L"Upper endpoint", L"Outside corner", L"Inner endpoint"};
    for (int i = 0; i < 3; ++i)
      text(dc, labels[i], 260, 338 + i * 64, 233, 30, normal);
    for (const int y : {176, 311}) {
      text(dc, L"X from left (%)", 505, y, 139, 23, small, Muted);
      text(dc, L"Y from top (%)", 655, y, 139, 23, small, Muted);
    }
    text(dc, draw_guides ? L"X: 0–50%. Y: 0–100% of each camera view. The right guide mirrors the left."
                         : L"This aircraft does not use alignment markers.",
         260, 551, 732, 26, small, Muted);
    text(dc, draw_guides ? L"Preview is temporary until saved. Reset restores guide positions and colour."
                         : L"No marker calibration is required for this profile.",
         260, 578, 732, 23, small, Muted);
    if (draw_guides) {
      auto preview = draft();
      if (!read_fields(preview))
        preview = draft();
      const auto color = preview.guide_color;
      const auto brush = CreateSolidBrush(RGB(UINT(color[0] * 255), UINT(color[1] * 255), UINT(color[2] * 255)));
      const auto pen = CreatePen(PS_SOLID, scale(2), RGB(UINT(color[0] * 255), UINT(color[1] * 255), UINT(color[2] * 255)));
      const auto old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
      const auto point = [&](const std::array<float, 2>& value, bool right, int y, int height) {
        return POINT{scale(817 + static_cast<int>(std::lround((right ? 1 - value[0] : value[0]) * 172))),
                     scale(y + static_cast<int>(std::lround(value[1] * height)))};
      };
      const auto dot = [&](POINT p, int radius) {
        Ellipse(dc, p.x - scale(radius), p.y - scale(radius), p.x + scale(radius), p.y + scale(radius));
      };
      text(dc, L"Mirrored preview", 811, 155, 183, 23, small, Muted);
      for (const bool right : {false, true}) {
        if (nose_squares) {
          const auto nose = point(preview.nose_dot, right, 191, 45);
          const RECT marker{nose.x - scale(7), nose.y - scale(7), nose.x + scale(7), nose.y + scale(7)};
          FillRect(dc, &marker, brush);
        } else {
          dot(point(preview.nose_dot, right, 191, 45), 6);
        }
        const auto a = point(preview.tail_upper, right, 334, 158), b = point(preview.tail_corner, right, 334, 158),
                   c = point(preview.tail_inner, right, 334, 158);
        const POINT points[]{a, b, c};
        Polyline(dc, points, 3);
        dot(a, 3);
        dot(b, 3);
        dot(c, 3);
      }
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(brush);
      DeleteObject(pen);
    }
  }
  text(dc, notice.c_str(), 248, 687, 382, 43, small, dirty ? Accent : Muted, DT_LEFT | DT_WORDBREAK);
}

DWORD WINAPI connection_worker(void*) {
  win::Mailbox mailbox;
  DWORD attached{};
  bool attempted = false;
  bool load_started_this_session = false;
  bool bridge_ok = false;
  bool manual_armed = false;
  std::uint64_t ignore_heartbeat_through = 0;
  win::LaunchRetry startup_retry;
  HANDLE process{};
  // A full process scan costs 7-12 ms (CreateToolhelp32Snapshot of every
  // process on the system). The attached simulator is not rescanned while it
  // runs: its handle keeps the PID from being reused and shows its exit. With
  // nothing attached, a scan runs once a second, or at once for Connect.
  constexpr std::uint64_t kDetachedRescanMs = 1000;
  win::SimulatorAttach attach;
  std::uint64_t last_scan_ms = 0;
  while (running.load()) {
    if (preview_ui) {
      Sleep(100);
      continue;
    }
    const bool auto_on = auto_connect.load(std::memory_order_acquire);
    const auto command = connect_commands.take();
    if (command == win::ConnectCommand::disconnect) {
      exchange_control(mailbox);
      mailbox.close();
      attempted = bridge_ok = manual_armed = false;
      ignore_heartbeat_through = 0;
      startup_retry.reset();
      PostMessageW(window, StatusMessage, 0, 0);
      Sleep(200);
      continue;
    }
    if (command == win::ConnectCommand::connect || command == win::ConnectCommand::reset)
      manual_armed = true;
    if (command == win::ConnectCommand::connect || command == win::ConnectCommand::reset) {
      attempted = false;
      bridge_ok = false;
      ignore_heartbeat_through = 0;
      startup_retry.reset();
      mailbox.close();
      {
        const std::lock_guard lock(app_mutex);
        if (!connection_disconnected.load(std::memory_order_acquire))
          connection =
              attached ? L"Connect requested. Retrying bridge attach." : L"Connect requested. Waiting for Microsoft Flight Simulator 2024.";
      }
      PostMessageW(window, StatusMessage, 0, 0);
    }
    const auto now_ms = GetTickCount64();
    const bool attached_alive = attached && process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    // An attached process that exited, or one without a handle, is rescanned
    // every loop as before. A skipped scan clears the last result, which could
    // name a process that has since exited.
    if (!attached_alive && (attached || !last_scan_ms || command == win::ConnectCommand::connect || command == win::ConnectCommand::reset ||
                            now_ms - last_scan_ms >= kDetachedRescanMs)) {
      attach = win::find_simulator_attach(expected_simulator);
      last_scan_ms = now_ms;
    } else if (!attached_alive) {
      attach = {};
    }
    const DWORD pid = attach.pid;
    if (attached && (pid != attached || (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0))) {
      mailbox.close();
      if (process)
        CloseHandle(process);
      process = nullptr;
      attached = 0;
      attempted = false;
      load_started_this_session = false;
      bridge_ok = false;
      manual_armed = false;
      ignore_heartbeat_through = 0;
      startup_retry.reset();
      simulator_pid = 0;
      {
        const std::lock_guard lock(app_mutex);
        status = {};
        current.enabled = 0;
        connection_requested.store(false, std::memory_order_release);
        connection = connection_disconnected.load(std::memory_order_acquire) ? L"Disconnected. Choose Connect to enable the cameras again."
                                                                             : L"Simulator closed. Waiting for the next session.";
      }
      PostMessageW(window, StatusMessage, 0, 0);
      // Stay in the tray so a later manual or auto connect can attach without
      // depending on MSFS auto-launch of Taxi Cam.
    }
    if (pid && pid != attached) {
      win::log_attach(win::settings_directory(), attach, expected_simulator);
      attached = pid;
      {
        // A new simulator process restarts the bridge's notification serial.
        const std::lock_guard lock(app_mutex);
        notification_reader.reset();
      }
      simulator_pid = pid;
      attempted = false;
      load_started_this_session = false;
      bridge_ok = false;
      manual_armed = manual_armed || command == win::ConnectCommand::connect || command == win::ConnectCommand::reset;
      ignore_heartbeat_through = 0;
      startup_retry = {};
      process = OpenProcess(SYNCHRONIZE, FALSE, pid);
      if (!auto_on && !manual_armed) {
        {
          const std::lock_guard lock(app_mutex);
          connection = L"MSFS detected. Auto-connect is off — choose Connect when ready.";
        }
        PostMessageW(window, StatusMessage, 0, 0);
      }
    }
    const bool want_connect =
        win::should_attempt_connect(auto_on, attempted, command, manual_armed, connection_disconnected.load(std::memory_order_acquire));
    if (attached && want_connect && startup_retry.ready(GetTickCount64())) {
      {
        const std::lock_guard lock(app_mutex);
        if (connection_disconnected.load(std::memory_order_acquire))
          continue;
        if (!connection_requested.load(std::memory_order_acquire) && !win::begin_connection(current)) {
          attempted = true;
          connection = L"Restart Taxi Cam to begin a new connection.";
          continue;
        }
        connection_requested.store(true, std::memory_order_release);
        win::apply_connection_command(current, win::ConnectCommand::connect);
      }
      PostMessageW(window, StatusMessage, 0, 0);
      attempted = true;
      if (!mailbox.data() && !mailbox.open(attached, true)) {
        const bool retrying = startup_retry.schedule({false, GetLastError(), L"mailbox", true}, GetTickCount64()) ||
                              startup_retry.schedule_recovery(GetTickCount64());
        attempted = !retrying;
        if (!retrying)
          manual_armed = false;
        {
          const std::lock_guard lock(app_mutex);
          if (!connection_disconnected.load(std::memory_order_acquire)) {
            connection = L"Could not open the camera control channel.";
            if (retrying)
              connection += L" Retrying.";
            else
              connection += L" Choose Disconnect, then Connect to try again.";
          }
        }
        PostMessageW(window, StatusMessage, 0, 0);
      } else {
        exchange_control(mailbox);
        win::LaunchDiagnostics launch_diagnostics;
        const bool allow_load = win::fresh_load_allowed(load_started_this_session);
        const auto loaded = win::load_bridge(attached, attach.path.empty() ? expected_simulator : attach.path,
                                             installation + L"\\taxi-camera-bridge.dll", &running, &launch_diagnostics, allow_load);
        if (launch_diagnostics.load_started)
          load_started_this_session = true;
        win::log_launch(win::settings_directory(), attached, loaded, launch_diagnostics);
        bool retrying = startup_retry.schedule(loaded, GetTickCount64());
        if (!retrying && !loaded.ok && loaded.retry_before_load)
          retrying = startup_retry.schedule_recovery(GetTickCount64());
        attempted = !retrying;
        if (loaded.ok) {
          manual_armed = false;
          // Confirm health from a beat newer than any stale-recovery watermark.
          bridge_ok = ignore_heartbeat_through == 0;
        } else {
          bridge_ok = false;
          if (!retrying)
            manual_armed = false;
        }
        {
          const std::lock_guard lock(app_mutex);
          if (!connection_disconnected.load(std::memory_order_acquire)) {
            connection = loaded.message;
            if (!loaded.ok)
              connection += L" (Windows " + std::to_wstring(loaded.error) + L")";
            if (retrying && loaded.retry_before_load && startup_retry.retries() == 0)
              connection += L" Automatic recovery scheduled.";
            else if (retrying)
              connection += L" Retrying startup preflight.";
            else if (loaded.retry_before_load && !loaded.ok)
              connection += L" Automatic retries paused; choose Disconnect, then Connect to retry.";
            else if (!loaded.ok)
              connection += L" Choose Disconnect, then Connect to try again.";
          }
        }
        PostMessageW(window, StatusMessage, 0, 0);
      }
    }
    // If the bridge previously started but heartbeats went stale while MSFS is
    // still running, request a safe rescan/start without a second LoadLibrary.
    if (attached && bridge_ok && attempted && mailbox.data() && !connection_disconnected.load(std::memory_order_acquire)) {
      win::Status sample;
      {
        const std::lock_guard lock(app_mutex);
        sample = status;
      }
      const auto now = GetTickCount64();
      if (sample.heartbeat && now > sample.heartbeat + 15000) {
        {
          const std::lock_guard lock(app_mutex);
          if (!connection_disconnected.load(std::memory_order_acquire)) {
            ignore_heartbeat_through = sample.heartbeat;
            bridge_ok = false;
            attempted = false;
            manual_armed = true;
            startup_retry.reset();
            // Retry through the existing authorization, preserving the stale
            // heartbeat watermark and any newer explicit Disconnect command.
            status.heartbeat = 0;
            connection = L"Bridge status went stale. Retrying attach automatically.";
          }
        }
        PostMessageW(window, StatusMessage, 0, 0);
      }
    }
    win::Status sample;
    if (running.load() && exchange_control(mailbox, &sample)) {
      {
        const std::lock_guard lock(app_mutex);
        std::array<SimEvent, 8> events{};
        const auto count = notification_reader.take(sample.notifications, GetTickCount64(), events.data(), events.size());
        // The bridge stops publishing once it reads the setting; this covers the poll in between.
        // The toast policy is applied here as well so routine events never reach the desktop.
        if (current.notifications && pending_notifications.size() + count <= 32)
          for (std::size_t i = 0; i < count; ++i)
            if (sim_event_toasts(events[i]))
              pending_notifications.push_back(events[i]);
        if (!connection_disconnected.load(std::memory_order_acquire)) {
          status = sample;
          if (!win::heartbeat_confirms_bridge(sample.heartbeat, ignore_heartbeat_through))
            status.heartbeat = 0;
          else {
            bridge_ok = true;
            ignore_heartbeat_through = 0;
          }
          received_bridge_status = received_bridge_status || sample.heartbeat != 0;
        }
      }
      PostMessageW(window, StatusMessage, 0, 0);
    }
    Sleep(200);
  }
  if (mailbox.data() && mailbox.lock(100)) {
    mailbox.data()->settings.enabled = 0;
    mailbox.data()->owner_heartbeat = 0;
    mailbox.unlock();
  }
  if (process)
    CloseHandle(process);
  return 0;
}
void stop_service() {
  running = false;
  if (const auto pid = simulator_pid.load()) {
    win::Mailbox mailbox;
    if (mailbox.open(pid, false) && mailbox.lock(200)) {
      mailbox.data()->settings.enabled = 0;
      mailbox.data()->owner_heartbeat = 0;
      mailbox.unlock();
    }
  }
}
void check_updates(bool manual) {
  if (preview_ui || update_prompt)
    return;
  if (updater.begin(installation, manual)) {
    next_update_check = GetTickCount64() + 24ULL * 60 * 60 * 1000;
    if (manual) {
      notice = L"Checking for updates in the background...";
      InvalidateRect(window, nullptr, FALSE);
    }
  }
}
void poll_updates() {
  if (preview_ui || update_prompt)
    return;
  win::UpdateResult result;
  if (updater.take(result)) {
    if (!result.available) {
      if (result.manual) {
        notice = result.error.empty() ? L"Taxi Cam is up to date." : result.error;
        MessageBoxW(window, notice.c_str(), L"Taxi Cam updates", MB_OK | MB_ICONINFORMATION);
      }
    } else {
      update_prompt = true;
      if (win::simulator_blocks_update()) {
        notice = L"Update downloaded. Close Microsoft Flight Simulator, then choose Check for updates to install.";
        if (result.manual)
          MessageBoxW(window, notice.c_str(), L"Taxi Cam updates", MB_OK | MB_ICONINFORMATION);
        else
          update_balloon();
      } else {
        const auto prompt =
            L"Taxi Cam " + result.tag + L" has been downloaded and verified.\n\nClose Taxi Cam and start the installer now?";
        if (MessageBoxW(window, prompt.c_str(), L"Taxi Cam update ready", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES) {
          bool proceed = true;
          if (dirty) {
            const int choice = MessageBoxW(window,
                                           L"Save your unsaved settings before installing?\n\nYes: save and continue.\nNo: discard "
                                           L"changes.\nCancel: keep the app open.",
                                           L"Unsaved settings", MB_YESNOCANCEL | MB_ICONQUESTION);
            proceed = choice == IDNO || (choice == IDYES && apply());
          }
          if (proceed) {
            std::wstring error;
            if (updater.launch(result, installation, error)) {
              stop_service();
              DestroyWindow(window);
              update_prompt = false;
              return;
            }
            MessageBoxW(window, error.c_str(), L"Taxi Cam updates", MB_OK | MB_ICONWARNING);
          }
        }
      }
      update_prompt = false;
    }
    InvalidateRect(window, nullptr, FALSE);
  }
  if (!updater.busy() && GetTickCount64() >= next_update_check)
    check_updates(false);
}
bool is_on(int id, const win::Settings& s) {
  switch (id) {
    case 230:
      return s.auto_profile;
    case 241:
      return connection_requested.load(std::memory_order_acquire);
    case 240:
      return auto_connect.load(std::memory_order_acquire);
    case 221:
      return s.follow_taxi;
    case 223:
      return s.auto_detect;
    case 224:
      return s.manual_mask & 1;
    case 225:
      return s.manual_mask & 2;
    case 226:
      return s.calibration_mask & 1;
    case 227:
      return s.calibration_mask & 2;
    case 232:
      return s.manual_mask & 4;
    case 233:
      return s.calibration_mask & 4;
    case 228:
      return s.single_camera;
    case 229:
      return s.scene_test;
    default:
      return false;
  }
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  if (taskbar_created && message == taskbar_created) {
    tray(true);
    return 0;
  }
  switch (message) {
    case WM_CREATE: {
      window = hwnd;
      dpi = GetDpiForWindow(hwnd);
      make_fonts();
      background_brush = CreateSolidBrush(Background);
      card_brush = CreateSolidBrush(Card);
      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
      DWORD corner = 2;
      DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
      icon = make_icon();
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
      taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
      register_camera_hotkeys();
      update_camera_button_input();
      if (hotkey_registration.conflicts())
        notice = L"Some shortcuts are unavailable. Check Overview > Flight-deck control.";
      else if (win::any_camera_button(button_saved) && !preview_ui && !button_input.enabled())
        notice = L"Controller buttons are unavailable. Check Overview > Flight-deck control.";
      build_controls();
      tray(true);
      SetTimer(hwnd, 1, 250, nullptr);
      return 0;
    }
    case WM_TIMER:
      if (show_event && WaitForSingleObject(show_event, 0) == WAIT_OBJECT_0)
        show();
      poll_updates();
      service_thumbnails();
      return 0;
    case WM_HOTKEY: {
      const int action = hotkey_registration.action(w, l);
      const auto focus = GetFocus();
      const auto focused_id = focus && GetParent(focus) == hwnd ? GetDlgCtrlID(focus) : 0;
      if (action >= 0 && !preview_ui &&
          !(focused_id >= 620 && focused_id < 620 + static_cast<int>(win::CameraHotkeyNames.size())))
        toggle_camera_from_hotkey(static_cast<unsigned>(action));
      return 0;
    }
    case WM_INPUT:
      for (const auto event : button_input.read(reinterpret_cast<HRAWINPUT>(l))) {
        const bool capturing = button_capture >= 0;
        camera_button_event(event);
        if (capturing && button_capture < 0)
          break;  // Other buttons changing with the captured one do not act.
      }
      break;  // DefWindowProc releases foreground Raw Input.
    case WM_INPUT_DEVICE_CHANGE:
      if (w == GIDC_REMOVAL)
        button_input.remove(reinterpret_cast<HANDLE>(l));
      else if (w == GIDC_ARRIVAL)
        button_input.arrived(reinterpret_cast<HANDLE>(l));
      refresh_shortcut_status();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(l);
      info->ptMinTrackSize = {scale(1055), scale(795)};
      return 0;
    }
    case WM_DPICHANGED: {
      dpi = HIWORD(w);
      make_fonts();
      const auto* r = reinterpret_cast<RECT*>(l);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      build_controls();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_LBUTTONUP: {
      const POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      if (const auto* area = hit_area_at(point)) {
        if (area->id) {
          show_display_snapshot(area->id);
        } else {
          SendMessageW(hwnd, WM_COMMAND, 103, 0);
        }
        return 0;
      }
      break;
    }
    case WM_SETCURSOR:
      if (reinterpret_cast<HWND>(w) == hwnd && LOWORD(l) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (hit_area_at(point)) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      if (const auto link = reinterpret_cast<HWND>(w);
          link && GetParent(link) == hwnd && (GetDlgCtrlID(link) == 514 || GetDlgCtrlID(link) == 515) && LOWORD(l) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      HDC memory = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
      const auto old = SelectObject(memory, bitmap);
      draw_page(memory);
      BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_HSCROLL:
      if (l && reinterpret_cast<HWND>(l) == GetDlgItem(hwnd, CameraTargetId)) {
        InvalidateRect(GetDlgItem(hwnd, CameraModeCardId + static_cast<int>(CameraMode::custom)), nullptr, FALSE);
        dirty_notice();
        return 0;
      }
      break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      auto dc = reinterpret_cast<HDC>(w);
      SetTextColor(dc, Text);
      SetBkColor(dc, Card);
      return reinterpret_cast<LRESULT>(card_brush);
    }
    case WM_DRAWITEM: {
      auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(l);
      if (item->CtlType != ODT_BUTTON)
        break;
      const int id = static_cast<int>(item->CtlID);
      if (id >= CameraModeCardId && id < CameraModeCardId + static_cast<int>(kCameraModeCount)) {
        draw_camera_mode_card(*item, static_cast<unsigned>(id - CameraModeCardId));
        return TRUE;
      }
      const bool disabled = (item->itemState & ODS_DISABLED) != 0;
      const bool selected = !disabled && ((id >= 100 && id < 106 && id - 100 == page) || is_on(id, draft()) ||
                                          (id >= 420 && id < 428 && gallery_button_routed(id)));
      HBRUSH surround = CreateSolidBrush((id >= 100 && id < 106) || (id >= 512 && id <= 515) ? Sidebar : Background);
      FillRect(item->hDC, &item->rcItem, surround);
      DeleteObject(surround);
      if (id == 514 || id == 515) {
        const int saved = SaveDC(item->hDC);
        wchar_t label[64]{};
        GetWindowTextW(item->hwndItem, label, 64);
        SelectObject(item->hDC, version_font);
        SetTextColor(item->hDC, disabled ? Muted : (item->itemState & ODS_SELECTED ? Text : Accent));
        SetBkMode(item->hDC, TRANSPARENT);
        auto bounds = item->rcItem;
        DrawTextW(item->hDC, label, -1, &bounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (!disabled && (item->itemState & ODS_FOCUS)) {
          InflateRect(&bounds, -1, -1);
          DrawFocusRect(item->hDC, &bounds);
        }
        RestoreDC(item->hDC, saved);
        return TRUE;
      }
      const bool primary = id == 500;
      const bool down = !disabled && (item->itemState & ODS_SELECTED) != 0;
      // Disabled owner-draw buttons need an explicit muted fill/label; EnableWindow
      // alone leaves them looking enabled because this path paints every button.
      const COLORREF fill = disabled ? RGB(20, 24, 31) : primary ? Accent : selected ? RGB(30, 64, 63) : down ? Border : Card;
      const COLORREF outline = disabled ? RGB(36, 44, 54) : selected || primary ? Accent : Border;
      HBRUSH brush = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, scale(1), outline);
      const auto oldb = SelectObject(item->hDC, brush), oldp = SelectObject(item->hDC, pen);
      RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, scale(9), scale(9));
      SelectObject(item->hDC, oldb);
      SelectObject(item->hDC, oldp);
      DeleteObject(brush);
      DeleteObject(pen);
      RECT r = item->rcItem;
      if (id == 512) {
        draw_bug_icon(item->hDC, r, disabled ? Muted : Accent);
        InflateRect(&r, -scale(4), -scale(4));
      } else {
        wchar_t label[160];
        GetWindowTextW(item->hwndItem, label, 160);
        SelectObject(item->hDC, normal);
        SetTextColor(item->hDC, disabled ? Muted : primary ? Background : selected ? Accent : Text);
        SetBkMode(item->hDC, TRANSPARENT);
        InflateRect(&r, -scale(10), 0);
        DrawTextW(item->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      if (!disabled && (item->itemState & ODS_FOCUS)) {
        if (id != 512)
          InflateRect(&r, -2, -4);
        DrawFocusRect(item->hDC, &r);
      }
      return TRUE;
    }
    case StatusMessage:
      refresh_connection_button();
      sync_aircraft_session();
      auto_profile();
      show_notifications();
      update_gallery_buttons();
      service_thumbnails();
      if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        InvalidateRect(hwnd, nullptr, FALSE);
        invalidate_camera_mode_cards();
      }
      return 0;
    case WhatsNewMessage:
      show_whats_new();
      return 0;
    case WM_CONTEXTMENU:
      PostMessageW(hwnd, TrayMessage, 0, WM_CONTEXTMENU);
      return 0;
    case TrayMessage:
      if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 600, L"Settings");
        AppendMenuW(menu, MF_STRING | (preview_ui ? MF_GRAYED : 0), 604,
                    win::connection_button_label(connection_requested.load(std::memory_order_acquire)));
        AppendMenuW(menu, MF_STRING | (preview_ui || updater.busy() || update_prompt ? MF_GRAYED : 0), 603,
                    updater.busy() ? L"Checking for updates..." : L"Check for updates");
        AppendMenuW(menu, MF_STRING | (draft().notifications ? MF_CHECKED : MF_UNCHECKED), 605, L"Show notifications");
        AppendMenuW(menu, MF_STRING, 512, L"Report a bug");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 601, L"Exit");
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(hwnd);
        const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (selected == 600)
          show();
        if (selected == 604) {
          toggle_connection();
        }
        if (selected == 603)
          check_updates(true);
        if (selected == 605) {
          // Global preference; persisted with the other selections in settings.ini.
          auto s = draft();
          s.notifications = s.notifications ? 0u : 1u;
          publish(s);
          if (!win::save_settings(draft()))
            notice = L"Could not save settings. Check access to your local settings folder.";
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (selected == 512)
          report_bug();
        if (selected == 601) {
          stop_service();
          DestroyWindow(hwnd);
        }
        PostMessageW(hwnd, WM_NULL, 0, 0);
      } else if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT || LOWORD(l) == NIN_BALLOONUSERCLICK ||
                 LOWORD(l) == WM_LBUTTONDBLCLK)
        show();
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(w);
      if (refreshing)
        return 0;
      if (id == 645) {
        edit_camera_hotkeys();
        return 0;
      }
      if (id >= 420 && id < 428) {
        const auto slot = static_cast<unsigned>(id - 420) / 2;
        if (slot < GalleryColumns && gallery_ids[slot])
          choose_texture(static_cast<unsigned>(id - 420) % 2, gallery_ids[slot]);
        update_gallery_buttons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id == 406) {
        if (choose_routes({0, 0}))
          notice = L"Automatic assignment restored. Named and detected displays apply again.";
        update_gallery_buttons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id == 407 || id == 408) {
        if (id == 407 && gallery_page > 0)
          --gallery_page;
        if (id == 408)
          ++gallery_page;
        update_gallery_buttons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id == 409) {
        thumbnails.fresh_after = GetTickCount64();
        service_thumbnails();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id >= CameraModeCardId && id < CameraModeCardId + static_cast<int>(kCameraModeCount)) {
        pending_camera_mode = static_cast<unsigned>(id - CameraModeCardId);
        EnableWindow(GetDlgItem(hwnd, CameraTargetId), pending_camera_mode == static_cast<unsigned>(CameraMode::custom));
        invalidate_camera_mode_cards();
        dirty_notice();
        return 0;
      }
      if (HIWORD(w) == EN_CHANGE || (HIWORD(w) == CBN_SELCHANGE && id != 210)) {
        dirty_notice();
        return 0;
      }
      if (id >= 100 && id < 106) {
        auto s = draft();
        if (id == 105) {
          const auto* guide_profile = profiles::find(s.profile);
          if (guide_profile && !guide_profile->reference_guides)
            return 0;
        }
        const wchar_t* field_error{};
        if (!read_fields(s, &field_error)) {
          notice = field_error ? field_error : L"Finish the current values before changing pages.";
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        publish(s);
        page = id - 100;
        build_controls();
        return 0;
      }
      if (id == 602) {
        PostMessageW(hwnd, TrayMessage, 0, WM_CONTEXTMENU);
        return 0;
      }
      if (id == 512) {
        report_bug();
        return 0;
      }
      if (id == 513) {
        donate();
        return 0;
      }
      if (id == 515) {
        request_whats_new();
        return 0;
      }
      if (id == 514) {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", GithubUrl, nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
          MessageBoxW(hwnd, L"Could not open your browser. Visit https://github.com/rthoms334/taxi-cam.", L"Taxi Cam on GitHub",
                      MB_OK | MB_ICONWARNING);
        return 0;
      }
      if (id == 210 && HIWORD(w) == CBN_SELENDOK) {
        const auto index = SendDlgItemMessageW(hwnd, 210, CB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<size_t>(index) >= profiles::Catalog.size())
          return 0;
        if (!apply())
          return 0;
        win::Settings next;
        if (!win::load_settings(next, installation, profiles::Catalog[index]->id)) {
          notice = L"Could not load that aircraft profile.";
          return 0;
        }
        next.enabled = draft().enabled;
        if (!win::prepare_profile_selection(next, draft(), false) || !win::save_settings(next)) {
          notice = L"Could not apply that aircraft profile.";
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        publish(next);
        dirty = false;
        notice = L"Aircraft profile selected. Reconnecting its cameras and displays.";
        build_controls();
        return 0;
      }
      if (id == 230) {
        if (!apply(false))
          return 0;
        auto s = draft();
        s.auto_profile = !s.auto_profile;
        publish(s);
        apply();
        build_controls();
        return 0;
      }
      if (id == 231 || id == 372) {
        if (!apply(false))
          return 0;
        const auto s = draft();
        const auto* color_profile = profiles::find(s.profile);
        const bool markings = id == 372;
        if ((!markings && color_profile && !color_profile->ground_speed) || (markings && color_profile && !color_profile->reference_guides))
          return 0;
        const auto& color = markings ? s.guide_color : s.speed_color;
        static COLORREF custom[16]{};
        CHOOSECOLORW choice{};
        choice.lStructSize = sizeof(choice);
        choice.hwndOwner = hwnd;
        choice.lpCustColors = custom;
        choice.Flags = CC_FULLOPEN | CC_RGBINIT;
        choice.rgbResult = RGB(UINT(color[0] * 255), UINT(color[1] * 255), UINT(color[2] * 255));
        if (ChooseColorW(&choice))
          apply_color_selection(s, markings, choice.rgbResult);
        return 0;
      }
      if (id == 500) {
        apply();
        return 0;
      }
      if (id == 501) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      if (id == 240) {
        const bool next = !auto_connect.load(std::memory_order_acquire);
        auto_connect.store(next, std::memory_order_release);
        if (!win::save_auto_connect(win::settings_directory(), next))
          notice = L"Could not save the Auto-connect preference.";
        else
          notice = next ? L"Auto-connect on. Taxi Cam will connect when the simulator is available."
                        : L"Auto-connect off. Use Connect whenever you are ready, including in a loaded flight.";
        if (next)
          request_connection(win::ConnectCommand::connect);
        build_controls();
        return 0;
      }
      if (id == 241) {
        toggle_connection();
        return 0;
      }
      if (id == 221 || (id >= 223 && id <= 229) || id == 232 || id == 233) {
        if (!apply(false))
          return 0;
        auto s = draft();
        if (id == 221) {
          const auto* profile = profiles::find(s.profile);
          if (profile && profile->taxi_control == profiles::TaxiControl::manual_only)
            return 0;
          s.follow_taxi = !s.follow_taxi;
          if (s.follow_taxi) {
            s.manual_mask = 0;
            s.calibration_mask = 0;
          }
        }
        if (id == 223)
          s.auto_detect = !s.auto_detect;
        if (id == 224 || id == 225 || id == 232) {
          const auto action = id == 224 ? win::CameraLeft : id == 225 ? win::CameraRight : win::CameraSd;
          if (pmdg_cam_control(s))
            win::toggle_manual_layer(s, action);
          else
            win::toggle_manual_camera(s, action);
        }
        if (id == 226 || id == 227 || id == 233) {
          s.manual_mask = 0;
          s.calibration_mask ^= id == 226 ? 1u : id == 227 ? 2u : 4u;
          const auto* profile = profiles::find(s.profile);
          s.follow_taxi = s.calibration_mask == 0 && profile && profile->taxi_control != profiles::TaxiControl::manual_only;
        }
        if (id == 228)
          s.single_camera = !s.single_camera;
        if (id == 229)
          s.scene_test = !s.scene_test;
        publish(s);
        dirty_notice();
        build_controls();
        return 0;
      }
      if ((id >= 330 && id <= 333) || (id >= 340 && id <= 343) || (id >= 350 && id <= 353)) {
        if (!apply(false))
          return 0;
        auto s = draft();
        const unsigned side = static_cast<unsigned>((id - 330) / 10);
        const int action = (id - 330) % 10;
        if (action == 0)
          s.mounts[side][1] -= 0.25;
        if (action == 1)
          s.mounts[side][1] += 0.25;
        if (action == 2)
          s.mounts[side][2] -= 1;
        if (action == 3)
          s.mounts[side][2] += 1;
        if (win::valid_settings(s)) {
          publish(s);
          dirty_notice();
          build_controls();
        }
        return 0;
      }
      if (id == 359) {
        auto s = draft();
        s.mounts = profiles::find(s.profile)->mounts;
        publish(s);
        dirty_notice();
        build_controls();
        return 0;
      }
      if (id == 370) {
        const auto* guide_profile = profiles::find(draft().profile);
        if (guide_profile && !guide_profile->reference_guides)
          return 0;
        if (apply(false)) {
          dirty_notice();
          notice = L"Preview applied. Save changes to keep these guides.";
        }
        return 0;
      }
      if (id == 371) {
        auto s = draft();
        if (const auto* profile = profiles::find(s.profile)) {
          if (!profile->reference_guides)
            return 0;
          win::reset_guide_settings(s, *profile);
          publish(s);
          dirty_notice();
          notice = L"Profile guide positions and colour restored. Save changes to keep them.";
          build_controls();
        }
        return 0;
      }
      if (id == 402) {
        thumbnails.fresh_after = GetTickCount64();
        if (apply(false)) {
          win::Status sample;
          {
            const std::lock_guard lock(app_mutex);
            sample = status;
          }
          if (sample.graphics_ready && sample.candidate_count == 0 && sample.left_id == 0 && sample.right_id == 0) {
            notice = L"Waiting for cockpit displays to be drawn. Restart Flight only if the list stays empty.";
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          build_controls();
        }
        return 0;
      }
      if (id == 403) {
        if (!apply(false))
          return 0;
        win::Status sample;
        {
          const std::lock_guard lock(app_mutex);
          sample = status;
        }
        const auto routed = routed_lists(draft(), sample);
        if (routed[0] && routed[1] && choose_routes({routed[1], routed[0]}))
          notice = L"Left and right swapped for this flight. Automatic restores detection.";
        build_controls();
        return 0;
      }
      if (id == 510) {
        ShellExecuteW(hwnd, L"open", win::settings_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
      }
      if (id == 511) {
        auto s = draft();
        s.follow_taxi = 0;
        s.manual_mask = s.calibration_mask = s.scene_test = 0;
        publish(s);
        dirty_notice();
        build_controls();
        return 0;
      }
      return 0;
    }
    case WM_CLOSE:
      if (w == 1) {
        stop_service();
        DestroyWindow(hwnd);
      } else
        ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_DESTROY:
      hotkeys_closing = true;
      hotkey_registration.clear();
      button_input.disable();
      if (sidebar_tooltip) {
        DestroyWindow(sidebar_tooltip);
        sidebar_tooltip = nullptr;
      }
      stop_service();
      tray(false);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, w, l);
}
}  // namespace
int WINAPI wWinMain(HINSTANCE app, HINSTANCE, LPWSTR, int) {
  int argc{};
  auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    return 1;
  // Setup and the uninstaller run installer commands without starting the tray app.
  if (argc >= 2 && !std::wcscmp(argv[1], L"--setup")) {
    const int code = setup::run_setup_command(argc - 2, argv + 2);
    LocalFree(argv);
    return code;
  }
  instance = app;
  // Required for NOTIFYICON_VERSION_4 tray toasts on Windows 10/11. Without an
  // explicit AppUserModelID the shell assigns a generated one and often drops
  // NIF_INFO balloons (or stores them under a muteable NotifyIconGeneratedAumid).
  SetCurrentProcessExplicitAppUserModelID(L"TaxiCam.Companion");
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  for (int i = 1; i < argc; ++i) {
    if (!std::wcscmp(argv[i], L"--background"))
      background_start = true;
    else if (!std::wcscmp(argv[i], L"--preview"))
      preview_ui = true;
    else if (!std::wcscmp(argv[i], L"--simulator") && i + 1 < argc)
      expected_simulator = argv[++i];
    else {
      LocalFree(argv);
      return ERROR_INVALID_PARAMETER;
    }
  }
  LocalFree(argv);
  // Retain coordination names so an older installed app cannot run alongside this one.
  singleton = CreateMutexW(nullptr, FALSE, preview_ui ? L"Local\\380TaxiCamera.Preview" : L"Local\\380TaxiCamera.Companion");
  const DWORD existing = GetLastError();
  show_event = CreateEventW(nullptr, FALSE, FALSE, preview_ui ? L"Local\\380TaxiCamera.Preview.Show" : L"Local\\380TaxiCamera.Show");
  if (!singleton || !show_event)
    return 1;
  if (existing == ERROR_ALREADY_EXISTS) {
    if (!background_start)
      SetEvent(show_event);
    CloseHandle(show_event);
    CloseHandle(singleton);
    return 0;
  }
  wchar_t path[32768]{};
  const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
  if (!n || n >= 32768)
    return 1;
  installation = path;
  installation.resize(installation.find_last_of(L"\\/"));
  if (preview_ui)
    win::settings_override = installation + L"\\preview-settings";
  win::migrate_saved_camera_rate();
  if (!win::load_settings(current, installation))
    notice = L"Saved settings were invalid; profile defaults loaded.";
  whats_new = win::whats_new_pending(win::settings_directory(), InstalledVersion);
  // enabled is runtime connection state, not saved: it starts off until
  // Connect or Auto-connect enables it.
  current.enabled = 0;
  auto_connect.store(win::load_auto_connect(win::settings_directory()), std::memory_order_release);
  if (!win::load_camera_hotkeys(hotkey_saved, win::settings_directory()))
    notice = L"Saved shortcuts were invalid and disabled. Configure them in Overview > Flight-deck control.";
  hotkey_draft = hotkey_saved;
  if (!win::load_camera_buttons(button_saved, win::settings_directory()))
    notice = L"Saved controller buttons were invalid and disabled. Configure them in Overview > Flight-deck control.";
  button_draft = button_saved;
  INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&common);
  WNDCLASSEXW type{};
  type.cbSize = sizeof(type);
  type.hInstance = instance;
  type.lpfnWndProc = procedure;
  type.lpszClassName = WindowClass;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&type))
    return 1;
  dpi = GetDpiForSystem();
  HWND main = CreateWindowExW(WS_EX_APPWINDOW, WindowClass, L"Taxi Cam", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, scale(1070), scale(810), nullptr, nullptr, instance, nullptr);
  if (!main)
    return 1;
  worker = CreateThread(nullptr, 0, connection_worker, nullptr, 0, nullptr);
  if (!worker) {
    DestroyWindow(main);
    return 1;
  }
  const win::StartupSettings startup(win::settings_directory(), background_start, preview_ui);
  if (startup.should_show()) {
    ShowWindow(main, SW_SHOW);
    if (!startup.record_shown(IsWindowVisible(main) != FALSE, worker != nullptr)) {
      notice = L"Could not remember the first launch. Settings may reopen next time.";
      InvalidateRect(main, nullptr, FALSE);
    }
  }
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(main, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  running = false;
  updater.stop();
  changelog_fetcher.stop();
  WaitForSingleObject(worker, 1500);
  CloseHandle(worker);
  CloseHandle(show_event);
  CloseHandle(singleton);
  return 0;
}
