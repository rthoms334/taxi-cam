#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include "../camera/aircraft_identity.hpp"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cwchar>
#include <memory>
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
#include "../shared/discovered_setup.hpp"
#include "../shared/profile_selection.hpp"
#include "../graphics/target_assignment.hpp"
#include "updater.hpp"
#include "../setup/setup_commands.hpp"
#include "changelog.hpp"
#include "changelog_fetch.hpp"
#include "whats_new_image.hpp"

namespace {
using namespace taxi_camera;
namespace win = standalone;
constexpr UINT TrayMessage = WM_APP + 1, StatusMessage = WM_APP + 2, WhatsNewMessage = WM_APP + 3, WhatsNewImageMessage = WM_APP + 4;
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
  std::swprintf(buffer, 64,
                (id >= 360 && id <= 367) || (id >= 380 && id <= 385) ? L"%.1f"
                : id == 201 || id == 202                             ? L"%+.2f"
                                                                     : L"%.10g",
                value);
  auto h = child(L"EDIT", buffer, id, x, y, w, 30, ES_AUTOHSCROLL | ES_LEFT | WS_BORDER);
  SendMessageW(h, EM_SETLIMITTEXT, 32, 0);
}
void toggle(const wchar_t* label, int id, bool enabled, int x, int y, int width = 125, int height = 36) {
  const auto text = std::wstring(label) + (enabled ? L": On" : L": Off");
  button(text.c_str(), id, x, y, width, height);
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
  // find() then answers for the set-up aircraft in use, with the user's
  // display choices for the selected profile.
  if (value.profile == profiles::DiscoveredProfileId)
    profiles::publish_discovered(value.discovered);
  profiles::publish_override(profiles::base_profile(value.profile), value.display_override);
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
    std::array<float, 2>* right[]{&settings.tail_upper_right, &settings.tail_corner_right, &settings.tail_inner_right};
    const auto field = [&](int id, std::array<float, 2>& guide, unsigned axis) {
      const auto value = number(id, guide[axis] * 100., ok);
      if (value < 0 || value > (axis ? 100 : 50))
        ok = false;
      else
        guide[axis] = static_cast<float>(value / 100.);
    };
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned axis = 0; axis < 2; ++axis)
        field(360 + static_cast<int>(i * 2 + axis), *guides[i], axis);
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned axis = 0; axis < 2; ++axis)
        field(380 + static_cast<int>(i * 2 + axis), *right[i], axis);
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
// The --preview UI reads What's new and its pictures from the repository
// checkout it runs in, so notes not yet on main can be checked.
std::wstring preview_notes_root() {
  wchar_t directory[MAX_PATH]{};
  const DWORD n = GetCurrentDirectoryW(MAX_PATH, directory);
  return n && n < MAX_PATH ? std::wstring(directory) : std::wstring(L".");
}
// What's new (open_whats_new): the installed version's notes and earlier
// ones, newest first. The newest release's features are cards with their
// pictures, fetched one at a time from the repository while the window is
// open; a picture that cannot be loaded is left out.
constexpr wchar_t WhatsNewClass[] = L"TaxiCamWhatsNew";
constexpr int WhatsNewWidth = 760, WhatsNewHeight = 720, WhatsNewBar = 68, WhatsNewPad = 28, WhatsNewCardPad = 18;
struct WhatsNewPicture {
  std::string path;
  HBITMAP bitmap{};
  int width{}, height{};
  bool failed{};
};
struct WhatsNewView {
  HWND window{};
  std::vector<win::WhatsNewBlock> blocks;
  std::vector<WhatsNewPicture> pictures;  // One per picture path, fetched in order.
  std::unique_ptr<win::ChangelogFetcher> fetcher;
  std::size_t next{};
  int scroll{};
  RECT close{};
} whats_new_view;
// Lays out the notes at the client width and draws them (draw) scrolled by
// whats_new_view.scroll; returns the height of the whole content.
int layout_whats_new(HWND hwnd, HDC dc, bool draw) {
  auto& v = whats_new_view;
  RECT client{};
  GetClientRect(hwnd, &client);
  const int left = scale(WhatsNewPad), width = client.right - 2 * left, inner = width - 2 * scale(WhatsNewCardPad);
  const int shift = draw ? -v.scroll : 0;
  int y = scale(26);
  SetBkMode(dc, TRANSPARENT);
  const auto measure = [&](const std::wstring& value, HFONT font, int w) {
    RECT r{0, 0, std::max(w, 1), 0};
    SelectObject(dc, font);
    DrawTextW(dc, value.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return static_cast<int>(r.bottom);
  };
  const auto put = [&](const std::wstring& value, HFONT font, COLORREF color, int x, int top, int w, int h, UINT flags = DT_WORDBREAK) {
    if (!draw)
      return;
    SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT r{x, top + shift, x + w, top + shift + h};
    DrawTextW(dc, value.c_str(), -1, &r, flags | DT_NOPREFIX);
  };
  const auto box = [&](int x, int top, int w, int h, COLORREF fill, COLORREF line, int radius) {
    if (!draw)
      return;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, scale(1), line);
    const auto old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, x, top + shift, x + w, top + shift + h, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
  };
  for (const auto& b : v.blocks) {
    using Kind = win::WhatsNewBlock::Kind;
    if (b.kind == Kind::heading) {
      put(L"WHAT'S NEW", small, Accent, left, y, width, scale(18), DT_SINGLELINE);
      y += scale(22);
      const int h = measure(b.text, title_font, width);
      put(b.text, title_font, Text, left, y, width, h);
      y += h;
      if (!b.detail.empty()) {
        put(b.detail, normal, Muted, left, y, width, scale(22), DT_SINGLELINE);
        y += scale(22);
      }
      y += scale(6);
    } else if (b.kind == Kind::section) {
      y += scale(20);
      const int h = measure(b.text, heading, width);
      put(b.text, heading, Accent, left, y, width, h);
      y += h + scale(10);
    } else if (b.kind == Kind::feature) {
      // A card: title, text, then the picture at the card's width.
      const int title_h = measure(b.text, heading, inner), text_h = measure(b.detail, normal, inner);
      const WhatsNewPicture* picture{};
      for (const auto& candidate : v.pictures)
        if (!b.image.empty() && candidate.path == b.image)
          picture = &candidate;
      const int picture_h = !picture || picture->failed ? 0 : picture->bitmap ? picture->height : inner * 9 / 16;
      const int card_h = 2 * scale(WhatsNewCardPad) + title_h + scale(6) + text_h + (picture_h ? scale(16) + picture_h : 0);
      box(left, y, width, card_h, Card, Border, scale(14));
      int top = y + scale(WhatsNewCardPad);
      const int x = left + scale(WhatsNewCardPad);
      put(b.text, heading, Text, x, top, inner, title_h);
      top += title_h + scale(6);
      put(b.detail, normal, Muted, x, top, inner, text_h);
      top += text_h + scale(16);
      if (picture_h && draw) {
        if (picture->bitmap) {
          // Centred, with a hairline frame so dark screenshots keep an edge.
          const int px = x + (inner - picture->width) / 2;
          HDC memory = CreateCompatibleDC(dc);
          const auto old = SelectObject(memory, picture->bitmap);
          BitBlt(dc, px, top + shift, picture->width, picture->height, memory, 0, 0, SRCCOPY);
          SelectObject(memory, old);
          DeleteDC(memory);
          HPEN pen = CreatePen(PS_SOLID, scale(1), Border);
          const auto old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
          Rectangle(dc, px, top + shift, px + picture->width, top + shift + picture->height);
          SelectObject(dc, old_pen);
          SelectObject(dc, old_brush);
          DeleteObject(pen);
        } else {
          box(x, top, inner, picture_h, Sidebar, Border, scale(8));
          put(L"Loading picture…", small, Muted, x, top, inner, picture_h, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
      }
      y += card_h + scale(14);
    } else if (b.kind == Kind::bullet) {
      const int indent = scale(20), h = measure(b.text, normal, width - indent);
      if (draw) {
        HBRUSH dot = CreateSolidBrush(Accent);
        const auto old_brush = SelectObject(dc, dot), old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int cx = left + scale(6), cy = y + shift + scale(10), r = scale(3);
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(dot);
      }
      put(b.text, normal, Text, left + indent, y, width - indent, h);
      y += h + scale(8);
    } else if (b.kind == Kind::release) {
      y += scale(12);
      put(b.text, heading, Text, left, y, width, scale(26), DT_SINGLELINE);
      if (!b.detail.empty()) {
        RECT title{0, 0, width, 0};
        SelectObject(dc, heading);
        DrawTextW(dc, b.text.c_str(), -1, &title, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        put(b.detail, normal, Muted, left + title.right + scale(12), y + scale(3), width, scale(24), DT_SINGLELINE);
      }
      y += scale(32);
    } else {
      const int h = measure(b.text, normal, width);
      put(b.text, normal, Muted, left, y, width, h);
      y += h;
    }
  }
  return y + scale(24);
}
// The scroll range for the notes' height, keeping the position in it.
void update_whats_new_scroll(HWND hwnd) {
  auto& v = whats_new_view;
  RECT client{};
  GetClientRect(hwnd, &client);
  HDC dc = GetDC(hwnd);
  const int content = layout_whats_new(hwnd, dc, false);
  ReleaseDC(hwnd, dc);
  const int view = std::max<int>(client.bottom - scale(WhatsNewBar), 1);
  v.scroll = std::clamp(v.scroll, 0, std::max(content - view, 0));
  SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(content - 1, 0), static_cast<UINT>(view), v.scroll, 0};
  SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
  InvalidateRect(hwnd, nullptr, FALSE);
}
void scroll_whats_new(HWND hwnd, int to) {
  whats_new_view.scroll = to;
  update_whats_new_scroll(hwnd);
}
void fetch_next_whats_new_picture() {
  auto& v = whats_new_view;
  while (v.fetcher && v.next < v.pictures.size()) {
    if (v.fetcher->begin(v.window, WhatsNewImageMessage, v.pictures[v.next].path, win::kWhatsNewImageMaxBytes))
      return;
    v.pictures[v.next++].failed = true;
  }
}
LRESULT CALLBACK whats_new_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  auto& v = whats_new_view;
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);
      HDC memory = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
      const auto old = SelectObject(memory, bitmap);
      FillRect(memory, &client, background_brush);
      // The notes scroll above a fixed bar with Close.
      const int bar = client.bottom - scale(WhatsNewBar);
      SaveDC(memory);
      IntersectClipRect(memory, 0, 0, client.right, bar);
      layout_whats_new(hwnd, memory, true);
      RestoreDC(memory, -1);
      RECT strip{0, bar, client.right, client.bottom};
      HBRUSH fill = CreateSolidBrush(Sidebar);
      FillRect(memory, &strip, fill);
      DeleteObject(fill);
      RECT line{0, bar, client.right, bar + scale(1)};
      HBRUSH edge = CreateSolidBrush(Border);
      FillRect(memory, &line, edge);
      DeleteObject(edge);
      v.close = {client.right - scale(WhatsNewPad) - scale(120), bar + scale(16), client.right - scale(WhatsNewPad), bar + scale(52)};
      HBRUSH accent = CreateSolidBrush(Accent);
      HPEN pen = CreatePen(PS_SOLID, scale(1), Accent);
      const auto old_brush = SelectObject(memory, accent), old_pen = SelectObject(memory, pen);
      RoundRect(memory, v.close.left, v.close.top, v.close.right, v.close.bottom, scale(9), scale(9));
      SelectObject(memory, old_brush);
      SelectObject(memory, old_pen);
      DeleteObject(accent);
      DeleteObject(pen);
      SelectObject(memory, normal);
      SetTextColor(memory, Background);
      SetBkMode(memory, TRANSPARENT);
      auto label = v.close;
      DrawTextW(memory, L"Close", -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SIZE:
      update_whats_new_scroll(hwnd);
      return 0;
    case WM_MOUSEWHEEL:
      scroll_whats_new(hwnd, v.scroll - static_cast<short>(HIWORD(w)) * scale(64) / WHEEL_DELTA);
      return 0;
    case WM_VSCROLL: {
      SCROLLINFO info{};
      info.cbSize = sizeof(info);
      info.fMask = SIF_ALL;
      GetScrollInfo(hwnd, SB_VERT, &info);
      const int page = static_cast<int>(info.nPage);
      switch (LOWORD(w)) {
        case SB_LINEUP:
          scroll_whats_new(hwnd, v.scroll - scale(40));
          break;
        case SB_LINEDOWN:
          scroll_whats_new(hwnd, v.scroll + scale(40));
          break;
        case SB_PAGEUP:
          scroll_whats_new(hwnd, v.scroll - page);
          break;
        case SB_PAGEDOWN:
          scroll_whats_new(hwnd, v.scroll + page);
          break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
          scroll_whats_new(hwnd, info.nTrackPos);
          break;
        case SB_TOP:
          scroll_whats_new(hwnd, 0);
          break;
        case SB_BOTTOM:
          scroll_whats_new(hwnd, info.nMax);
          break;
        default:
          break;
      }
      return 0;
    }
    case WM_KEYDOWN: {
      RECT client{};
      GetClientRect(hwnd, &client);
      const int page = client.bottom - scale(WhatsNewBar) - scale(40);
      if (w == VK_ESCAPE || w == VK_RETURN)
        DestroyWindow(hwnd);
      else if (w == VK_UP || w == VK_DOWN)
        scroll_whats_new(hwnd, v.scroll + (w == VK_UP ? -scale(40) : scale(40)));
      else if (w == VK_PRIOR || w == VK_NEXT || w == VK_SPACE)
        scroll_whats_new(hwnd, v.scroll + (w == VK_PRIOR ? -page : page));
      else if (w == VK_HOME || w == VK_END)
        scroll_whats_new(hwnd, w == VK_HOME ? 0 : INT_MAX / 2);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      if (PtInRect(&v.close, p))
        DestroyWindow(hwnd);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        SetCursor(LoadCursorW(nullptr, PtInRect(&v.close, p) ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WhatsNewImageMessage: {
      // A picture arrived: decoded at the card's width, or left out.
      win::ChangelogFetchResult result;
      if (v.fetcher && v.fetcher->take(result) && v.next < v.pictures.size()) {
        auto& picture = v.pictures[v.next++];
        RECT client{};
        GetClientRect(hwnd, &client);
        const int inner = client.right - 2 * scale(WhatsNewPad) - 2 * scale(WhatsNewCardPad);
        picture.bitmap = result.ok ? win::decode_whats_new_image(result.body, inner, scale(460), picture.width, picture.height) : nullptr;
        picture.failed = !picture.bitmap;
        update_whats_new_scroll(hwnd);
      }
      fetch_next_whats_new_picture();
      return 0;
    }
    case WM_DESTROY:
      if (v.fetcher)
        v.fetcher->stop();
      v.fetcher.reset();
      for (auto& picture : v.pictures)
        if (picture.bitmap)
          DeleteObject(picture.bitmap);
      v.pictures.clear();
      v.blocks.clear();
      v.window = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd, message, w, l);
}
// Opens (or replaces) the What's new window with these notes and starts
// fetching their pictures.
bool open_whats_new(std::vector<win::WhatsNewBlock> blocks) {
  auto& v = whats_new_view;
  if (v.window)
    DestroyWindow(v.window);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpfnWndProc = whats_new_procedure;
    type.lpszClassName = WhatsNewClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassExW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  if (!registered)
    return false;
  v.blocks = std::move(blocks);
  v.pictures.clear();
  for (const auto& b : v.blocks)
    if (!b.image.empty() && std::none_of(v.pictures.begin(), v.pictures.end(), [&](const auto& p) { return p.path == b.image; }))
      v.pictures.push_back({b.image});
  v.next = 0;
  v.scroll = 0;
  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL;
  RECT frame{0, 0, scale(WhatsNewWidth), scale(WhatsNewHeight)};
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  v.window = CreateWindowExW(0, WhatsNewClass, L"Taxi Cam — What's new", style, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                             frame.bottom - frame.top, window, nullptr, instance, nullptr);
  if (!v.window)
    return false;
  BOOL dark = TRUE;
  DwmSetWindowAttribute(v.window, 20, &dark, sizeof(dark));
  SetWindowTheme(v.window, L"DarkMode_Explorer", nullptr);  // A dark scroll bar.
  SendMessageW(v.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
  SendMessageW(v.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
  v.fetcher = std::make_unique<win::ChangelogFetcher>();
  if (preview_ui)
    v.fetcher->set_local_root(preview_notes_root());
  update_whats_new_scroll(v.window);
  ShowWindow(v.window, SW_SHOWNORMAL);
  SetForegroundWindow(v.window);
  fetch_next_whats_new_picture();
  return true;
}
void request_whats_new() {
  if (changelog_fetcher.busy())
    return;
  if (preview_ui)
    changelog_fetcher.set_local_root(preview_notes_root());
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
// Profile rectangles apply only when the texture has the profile's display
// size; a set-up aircraft's sides fill whatever texture they use.
const profiles::AircraftProfile* snapshot_profile(const win::Settings& s, const win::Status& sample) {
  const auto* profile = profiles::find(s.profile);
  return profile && (profile->fit_texture || (sample.snapshot_width == profile->width && sample.snapshot_height == profile->height))
             ? profile
             : nullptr;
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
    const auto texture_width = static_cast<int>(sample.snapshot_width), texture_height = static_cast<int>(sample.snapshot_height);
    const auto r = profiles::side_rect(*profile, side, sample.snapshot_width, sample.snapshot_height);
    const bool bound = routed[side] == sample.snapshot_id;
    const COLORREF color = bound ? Accent : Muted;
    HPEN pen = CreatePen(bound ? PS_SOLID : PS_DOT, bound ? scale(2) : 1, color);
    const auto old_pen = SelectObject(dc, pen);
    RECT box{left + MulDiv(int(r.left), width, texture_width), top + MulDiv(int(r.top), height, texture_height),
             left + MulDiv(int(r.right), width, texture_width), top + MulDiv(int(r.bottom), height, texture_height)};
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
  // The last request gave no picture: the bridge copies a texture only where
  // the simulator draws it, and panels that are off are not drawn.
  bool failed{};
};
struct Thumbnails {
  std::vector<Thumbnail> items;
  std::uint64_t epoch{}, serial{}, id{};
  ULONGLONG requested_ms{}, fresh_after{};
  bool showing{};
} thumbnails;
constexpr unsigned ThumbnailEdge = 512;
// A drawn texture answers in well under a second (0.3-0.4 s on the Fenix,
// 2026-10-31); one that is not drawn would hold every other picture up to
// the bridge's 5 s timeout, so the next request replaces it sooner.
constexpr ULONGLONG ThumbnailWaitMs = 2500, ThumbnailRetryMs = 15000, ThumbnailFreshMs = 120000;
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
  for (UINT i = 0; id && i < std::min<UINT>(sample.candidate_count, win::MaxCandidates); ++i)
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
  std::vector<win::Candidate> items(sample.candidates, sample.candidates + std::min<UINT>(sample.candidate_count, win::MaxCandidates));
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
    notice = std::wstring(list_title(s, list)) + L" uses #" + std::to_wstring(id) +
             L" for this flight. Clear choices goes back to automatic selection.";
}
// Aircraft setup (SetupPage): an aircraft no profile matches is set up by
// picking, by picture, the cockpit panel each camera side draws on. The layout
// is learned from the bridge's panel map (discovered_setup.hpp); the cameras
// are then positioned from their starting mounts on Camera views.
constexpr int SetupPage = 6;
constexpr int SetupCardId = 440, SetupPreviousId = 452, SetupNextId = 453, SetupSidesId = 454, SetupSaveId = 455, SetupRefreshId = 456,
              SetupOpenId = 457, SetupTestId = 458, SetupBuiltInId = 461;
struct AircraftSetup {
  profiles::VariantKey key{};
  std::array<std::string, MaxDisplaySides> names;
  unsigned sides = 2, gallery_page = 0;
  std::array<std::uint64_t, GalleryColumns> ids{};
  std::array<std::string, GalleryColumns> card_names;
  bool positioning{};  // Camera views shows the positioning step after a save.
  // Cockpit test: sides showing the calibration pattern, and the settings in
  // use before the picks ran as an unsaved trial profile.
  unsigned testing{};
  bool trial{};
  win::Settings before{};
  // A built-in profile whose displays are being changed (0: setting up an
  // aircraft of its own). Its choices save as the profile's DisplayOverride.
  std::uint32_t built_in{};
} setup;
bool fresh_status(const win::Status& sample) {
  const auto now = GetTickCount64();
  return sample.heartbeat && now >= sample.heartbeat && now - sample.heartbeat <= 3000;
}
// A loaded aircraft no profile matches; the bridge tracks its panels.
bool unsupported_aircraft(const win::Status& sample) {
  return fresh_status(sample) && sample.panel_discovery && !sample.detected_profile && sample.identity_sample_ms;
}
profiles::VariantKey loaded_variant(const win::Status& sample) {
  profiles::VariantKey key{};
  if (!profiles::variant_key(std::string_view(sample.aircraft_path, strnlen(sample.aircraft_path, sizeof(sample.aircraft_path))), key))
    key = {};
  return key;
}
std::string panel_name(const win::PanelTexture& panel) {
  return std::string(panel.name, strnlen(panel.name, sizeof(panel.name)));
}
// The panel map's tracked textures. A repeated name is listed once, as its
// lowest index (the texture created last), which learn_layout pairs.
std::vector<win::PanelTexture> setup_items(const win::Status& sample) {
  std::vector<win::PanelTexture> items;
  // A built-in profile keeps its own matching: only panels of its display
  // shape can be its displays.
  const auto* built_in = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  for (std::size_t i = 0; i < std::min<std::size_t>(sample.panel_count, win::MaxPanelTextures); ++i) {
    const auto& panel = sample.panels[i];
    if (!panel.name[0])
      continue;
    const auto same = std::find_if(items.begin(), items.end(), [&](const auto& item) { return panel_name(item) == panel_name(panel); });
    if (same != items.end())
      *same = panel;
    else
      items.push_back(panel);
  }
  std::erase_if(items, [&](const auto& item) {
    return !item.id || (built_in && !profiles::named_display(*built_in, item.width, item.height, item.mips, profiles::DiscoveredFormat));
  });
  return items;
}
// Picks belong to one variant: another aircraft starts again, and an aircraft
// already set up starts from its saved displays.
void sync_setup(const win::Status& sample) {
  const auto key = loaded_variant(sample);
  if (key == setup.key)
    return;
  setup.key = key;
  setup.names = {};
  setup.sides = 2;
  setup.gallery_page = 0;
  const auto s = draft();
  // A built-in profile starts from the displays it uses now.
  if (const auto* built_in = setup.built_in ? profiles::find(setup.built_in) : nullptr) {
    setup.sides = std::min<unsigned>(built_in->sides, MaxDisplaySides);
    for (unsigned side = 0; side < setup.sides; ++side)
      setup.names[side] = built_in->panel_textures[side] ? built_in->panel_textures[side] : "";
    return;
  }
  if (s.profile == profiles::DiscoveredProfileId && key[0] && profiles::same_variant(key.data(), s.discovered.key)) {
    setup.sides = s.discovered.sides;
    for (unsigned side = 0; side < s.discovered.sides && side < MaxDisplaySides; ++side)
      setup.names[side] = s.discovered.textures[side];
  }
}
const wchar_t* setup_side_title(unsigned side) {
  return side == 0 ? L"Left display" : side == 1 ? L"Right display" : L"Lower display";
}
const wchar_t* setup_side_action(unsigned side) {
  return side == 0 ? L"Left" : side == 1 ? L"Right" : L"Lower";
}
// What stops the setup now; empty when the picks can be saved.
std::wstring setup_blocker(const win::Status& sample) {
  // Pictures and the panel list need the bridge connection.
  if (!fresh_status(sample) || !sample.graphics_ready || !draft().enabled)
    return L"Choose Connect on Overview with this aircraft loaded in a flight.";
  if (!setup.key[0])
    return L"Waiting for the simulator to report which aircraft is loaded.";
  if (!sample.panel_discovery)
    return L"Waiting for the bridge to track this cockpit's panels.";
  switch (static_cast<win::PanelMapState>(sample.panel_state)) {
    case win::PanelMapState::waiting:
      return L"Reading this cockpit's panels. This takes a few seconds after the flight has loaded.";
    case win::PanelMapState::unavailable:
    case win::PanelMapState::unresolved:
      return L"Taxi Cam cannot read this cockpit's panel list in this simulator version, so this aircraft cannot be set up yet.";
    case win::PanelMapState::incomplete:
      return L"This cockpit's panel list is not complete. Wait a few seconds after the cockpit has loaded.";
    case win::PanelMapState::paired:
      break;
  }
  // A built-in profile's names are its own, so no clean load is needed.
  if (!sample.panel_clean && !setup.built_in)
    return L"Another aircraft was loaded before this one in this simulator session, so panel names can be one texture off. "
           L"Restart the simulator, load this aircraft first, then set it up.";
  return {};
}
// A built-in profile's choices from the picks: a pick that is the profile's
// own panel is no choice, so the profile stays as it comes.
profiles::DisplayOverride built_in_choices(const profiles::AircraftProfile& base, profiles::DisplayOverride choices) {
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    std::fill(std::begin(choices.textures[side]), std::end(choices.textures[side]), '\0');
    const auto& pick = setup.names[side];
    const std::string own = side < base.sides && base.panel_textures[side] ? base.panel_textures[side] : "";
    if (side < base.sides && !pick.empty() && pick != own && pick.size() < profiles::MaxPanelName)
      pick.copy(choices.textures[side], pick.size());
  }
  return choices;
}
std::uint64_t picked_id(const std::vector<win::PanelTexture>& items, unsigned side) {
  for (const auto& item : items)
    if (side < MaxDisplaySides && !setup.names[side].empty() && panel_name(item) == setup.names[side])
      return item.id;
  return 0;
}
// The panels picked by picture as this flight's explicit routes (as PFD
// routing's card buttons set them), so the cameras draw on what was picked
// even where the bridge pairs panel names wrongly. The names carry the setup
// to later flights.
void route_picks(win::Settings& s, const std::vector<win::PanelTexture>& items) {
  const std::uint64_t left = picked_id(items, 0), right = setup.sides > 1 ? picked_id(items, 1) : 0;
  win::update_target_assignment(s.left_id, s.right_id, s.route_request, left, right == left ? 0 : right);
}
// Cockpit test: the picks run as an unsaved trial profile with the
// calibration pattern on each tested side, so the cockpit shows which screen
// a panel is even where panel names do not say. A side not picked yet uses
// a picked side's panel. Leaving the setup unsaved restores the settings.
void apply_setup_trial() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  auto s = draft();
  if (!setup.testing) {
    if (setup.trial && s.calibration_mask) {
      s.calibration_mask = 0;
      publish(s);
    }
    return;
  }
  // A built-in profile tests its choices in place: the bridge re-binds the
  // displays without restarting the cameras.
  if (const auto* base = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr) {
    if (!setup.trial)
      setup.before = s;
    s.display_override = built_in_choices(*base, s.display_override);
    s.calibration_mask = setup.testing & profiles::side_mask(*base);
    s.manual_mask = s.taxi_selected_mask = s.taxi_desired_mask = 0;
    s.panel_setup = 1;
    route_picks(s, setup_items(sample));
    if (!win::valid_settings(s)) {
      setup.testing = 0;
      notice = L"The cockpit test could not start.";
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
    publish(s);
    setup.trial = true;
    notice = L"Test bars show on the tested panels in the cockpit after a few seconds.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  std::array<const char*, MaxDisplaySides> names{};
  const char* fallback = nullptr;
  for (unsigned side = 0; side < setup.sides; ++side)
    if (!setup.names[side].empty())
      fallback = fallback ? fallback : setup.names[side].c_str();
  for (unsigned side = 0; side < setup.sides; ++side)
    names[side] = setup.names[side].empty() ? fallback : setup.names[side].c_str();
  profiles::DiscoveredLayout layout;
  if (!fallback || win::learn_layout(sample, names, setup.sides, layout, false) != win::LayoutLearning::learned) {
    setup.testing = 0;
    notice = fallback ? L"The cockpit test needs the panel list. Wait for the pictures, then try again." : L"Pick a panel first.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (!setup.trial)
    setup.before = s;
  s.profile = profiles::DiscoveredProfileId;
  s.discovered = layout;
  s.calibration_mask = setup.testing & ((1u << setup.sides) - 1);
  s.manual_mask = s.taxi_selected_mask = s.taxi_desired_mask = 0;
  s.lower_id = 0;
  route_picks(s, setup_items(sample));
  s.panel_setup = 1;
  if (!win::valid_settings(s)) {
    setup.testing = 0;
    notice = L"The cockpit test could not start.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  publish(s);
  setup.trial = true;
  notice = L"Test bars show on the tested panels in the cockpit after a few seconds.";
  InvalidateRect(window, nullptr, FALSE);
}
// Leaving the setup without saving: the settings from before the test.
void end_setup_trial(win::Settings& s) {
  if (setup.trial)
    s = setup.before;
  setup.trial = false;
  setup.testing = 0;
  s.panel_setup = 0;
}
// Whether a card button's panel is picked for its side.
bool setup_button_picked(int id) {
  const auto slot = static_cast<unsigned>(id - SetupCardId) / 3, side = static_cast<unsigned>(id - SetupCardId) % 3;
  return slot < GalleryColumns && !setup.card_names[slot].empty() && setup.names[side] == setup.card_names[slot];
}
// One panel per side: picking another side's panel swaps the two.
void pick_setup_panel(unsigned slot, unsigned side) {
  if (slot >= GalleryColumns || side >= setup.sides || setup.card_names[slot].empty())
    return;
  const auto name = setup.card_names[slot];
  // Built-in profiles may share a texture between sides (the PMDG 777's DUS).
  for (unsigned other = 0; other < MaxDisplaySides && !setup.built_in; ++other)
    if (other != side && setup.names[other] == name)
      setup.names[other] = setup.names[side];
  setup.names[side] = name;
  // A running cockpit test follows the new picks.
  if (setup.testing)
    apply_setup_trial();
}
// Overview's setup button, and the cards and Save button of the setup page.
void update_setup_buttons() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  if (HWND open = GetDlgItem(window, SetupOpenId)) {
    // The aircraft loaded is the selected profile's (set up or built in).
    const bool set_up = s.profile && sample.detected_profile == s.profile && fresh_status(sample);
    // The UI preview shows it too, so the setup page can be reviewed.
    const bool visible = unsupported_aircraft(sample) || set_up || preview_ui;
    const wchar_t* label = set_up ? L"Change displays" : L"Set up this aircraft";
    wchar_t current_label[64]{};
    GetWindowTextW(open, current_label, 64);
    if (std::wcscmp(current_label, label) != 0)
      SetWindowTextW(open, label);
    if ((IsWindowVisible(open) != FALSE) != visible)
      ShowWindow(open, visible ? SW_SHOWNA : SW_HIDE);
  }
  if (page != SetupPage || !GetDlgItem(window, SetupCardId))
    return;
  sync_setup(sample);
  const auto items = setup_items(sample);
  const unsigned pages = std::max(1u, static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns));
  setup.gallery_page = std::min(setup.gallery_page, pages - 1);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto index = setup.gallery_page * GalleryColumns + slot;
    setup.ids[slot] = index < items.size() ? items[index].id : 0;
    setup.card_names[slot] = index < items.size() ? panel_name(items[index]) : std::string();
    for (unsigned side = 0; side < MaxDisplaySides; ++side) {
      HWND card_button = GetDlgItem(window, static_cast<int>(SetupCardId + slot * 3 + side));
      const bool visible = setup.ids[slot] && side < setup.sides;
      if (card_button && (IsWindowVisible(card_button) != FALSE) != visible)
        ShowWindow(card_button, visible ? SW_SHOWNA : SW_HIDE);
      if (card_button && visible)
        InvalidateRect(card_button, nullptr, FALSE);
    }
  }
  EnableWindow(GetDlgItem(window, SetupPreviousId), setup.gallery_page > 0);
  EnableWindow(GetDlgItem(window, SetupNextId), setup.gallery_page + 1 < pages);
  bool complete = setup_blocker(sample).empty();
  for (unsigned side = 0; side < setup.sides; ++side)
    complete = complete && picked_id(items, side);
  EnableWindow(GetDlgItem(window, SetupSaveId), complete);
}
// Camera adjustment window (open_live_view): a live picture of a display's
// camera area, retaken through the snapshot request as fast as it answers
// while the window is open, with controls for the cameras and the guides.
// Other pictures wait while it runs.
// A drawn display answers in about 0.4 s; while the cameras start (about 10 s
// after an aircraft is set up) requests go unanswered and are retried.
constexpr ULONGLONG LiveViewWaitMs = 1200, LiveViewIntervalMs = 150;
struct LiveView {
  HWND window{};
  std::uint64_t id{}, serial{};
  ULONGLONG requested_ms{}, taken_ms{};
  HBITMAP image{};
  int width{}, height{};
  unsigned texture_width{}, texture_height{}, side{};
  // The camera control from before this window turned a camera on, restored
  // when it closes unless the cameras were changed meanwhile.
  bool camera_taken{};
  decltype(win::Settings::follow_taxi) follow_before{};
  decltype(win::Settings::manual_mask) manual_before{}, manual_set{};
  // The aircraft's saved settings when the window opened: Undo returns there.
  // Changes are saved when the window closes.
  win::Settings saved{};
  bool changed{};
  unsigned feed{};  // Selected camera: 0 nose, 1 tail or left wing, 2 right wing.
  int step = 1;     // 0 fine, 1 normal, 2 coarse.
  enum class Drag { none, aim, guide, area } drag{};
  // Display area mode: the whole texture of area_side, with its area drawn;
  // a drag moves (handle 8) or resizes (0-3 corners, 4-7 edges) it and
  // applies when let go.
  bool area_mode{};
  unsigned area_side{};
  int area_handle = -1;
  profiles::DisplayRect area{}, area_start{};
  POINT area_from{};
  // Dragged guide point: 0 nose, 1-3 left tail upper, corner, inner, 4-6 the
  // right tail bracket's.
  int guide = -1;
  // The dragged tail point snapped to its partner's mirror, per axis.
  std::array<bool, 2> snapped{};
  POINT last{};
  // Why a change was not applied, shown in the caption for a few seconds.
  std::wstring note;
  ULONGLONG note_ms{};
} live_view;
void release_live_image() {
  if (live_view.image)
    DeleteObject(live_view.image);
  live_view.image = nullptr;
  live_view.width = live_view.height = 0;
}
// Displays with a camera on, or asked for: automatic TAXI control shows what
// the aircraft's buttons select and ignores manual requests; the PMDG 777's
// manual previews add to its CAM page (bridge_main.cpp).
unsigned live_cameras_on(const win::Settings& s, const win::Status& sample, const profiles::AircraftProfile& profile) {
  const bool automatic =
      profile.taxi_control != profiles::TaxiControl::manual_only && profile.taxi_control != profiles::TaxiControl::pmdg_dsp_cam;
  if (s.follow_taxi && automatic)
    return sample.taxi_mask;
  return s.manual_mask | (profile.taxi_control == profiles::TaxiControl::pmdg_dsp_cam ? sample.taxi_mask : 0u);
}
// The picture shows the cameras only where one is drawn, so the display shown
// has its camera on while the window is open. Automatic TAXI control hands
// over to manual control for it, keeping the displays it shows.
void live_keep_camera(const win::Settings& s, const win::Status& sample, const profiles::AircraftProfile& profile, unsigned side) {
  // Not while a cockpit test or calibration owns the displays.
  if (side >= MaxDisplaySides || !fresh_status(sample) || !sample.graphics_ready || s.calibration_mask || s.scene_test ||
      (live_cameras_on(s, sample, profile) & (1u << side)))
    return;
  auto next = s;
  if (next.follow_taxi && profile.taxi_control != profiles::TaxiControl::manual_only &&
      profile.taxi_control != profiles::TaxiControl::pmdg_dsp_cam) {
    next.manual_mask = sample.taxi_mask & profiles::side_mask(profile);
    next.follow_taxi = 0;
  }
  next.manual_mask |= 1u << side;
  if (!win::valid_settings(next))
    return;
  if (!live_view.camera_taken) {
    live_view.camera_taken = true;
    live_view.follow_before = s.follow_taxi;
    live_view.manual_before = s.manual_mask;
  }
  live_view.manual_set = next.manual_mask;
  publish(next);
  refresh_side_toggles(next);
  InvalidateRect(window, nullptr, FALSE);
}
// The display side shown: the first with a camera on, else the first routed.
std::pair<unsigned, std::uint64_t> live_view_target(const win::Settings& s,
                                                    const win::Status& sample,
                                                    const profiles::AircraftProfile& profile) {
  const auto routed = routed_texture_ids(sample, profile);
  if (live_view.area_mode && live_view.area_side < profile.sides && live_view.area_side < MaxDisplaySides)
    return {live_view.area_side, routed[live_view.area_side]};
  const unsigned on = live_cameras_on(s, sample, profile);
  for (unsigned side = 0; side < profile.sides && side < MaxDisplaySides; ++side)
    if (routed[side] && (on & (1u << side)))
      return {side, routed[side]};
  for (unsigned side = 0; side < profile.sides && side < MaxDisplaySides; ++side)
    if (routed[side])
      return {side, routed[side]};
  return {0, 0};
}
void service_live_view() {
  // The display picture holds still while its area is dragged over it.
  if (!live_view.window || snapshot_dialog_open || live_view.drag == LiveView::Drag::area)
    return;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto now = GetTickCount64();
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  const auto [side, id] = profile ? live_view_target(s, sample, *profile) : std::pair<unsigned, std::uint64_t>{0, 0};
  if (live_view.serial) {
    const auto result = static_cast<win::DisplaySnapshotResult>(sample.snapshot_result);
    const bool answered = sample.snapshot_serial == live_view.serial && result != win::DisplaySnapshotResult::pending;
    if (!answered && now - live_view.requested_ms < LiveViewWaitMs)
      return;
    if (answered && result == win::DisplaySnapshotResult::ready && sample.snapshot_id == live_view.id) {
      auto* image = static_cast<HBITMAP>(
          LoadImageW(nullptr, win::display_snapshot_path().c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
      BITMAP bitmap{};
      if (image && GetObjectW(image, sizeof(bitmap), &bitmap) && bitmap.bmWidth > 0 && bitmap.bmHeight) {
        release_live_image();
        live_view.image = image;
        live_view.width = bitmap.bmWidth;
        live_view.height = std::abs(bitmap.bmHeight);
        live_view.texture_width = sample.snapshot_width;
        live_view.texture_height = sample.snapshot_height;
        live_view.taken_ms = now;
      } else if (image) {
        DeleteObject(image);
      }
    }
    live_view.serial = 0;
    InvalidateRect(live_view.window, nullptr, FALSE);
  }
  if (id != live_view.id || side != live_view.side) {
    live_view.id = id;
    live_view.side = side;
    InvalidateRect(live_view.window, nullptr, FALSE);
  }
  if (profile && id)
    live_keep_camera(s, sample, *profile, side);
  if (!id || !fresh_status(sample) || !sample.graphics_ready || now - live_view.requested_ms < LiveViewIntervalMs)
    return;
  live_view.serial = request_display_snapshot(id);
  live_view.requested_ms = now;
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
  // The snapshot dialog and the camera adjustment window own the request.
  const bool request_taken = snapshot_dialog_open || live_view.window;
  if (thumbnails.serial && !request_taken) {
    const auto result = static_cast<win::DisplaySnapshotResult>(sample.snapshot_result);
    const bool answered = sample.snapshot_serial == thumbnails.serial && result != win::DisplaySnapshotResult::pending;
    if (!answered && now - thumbnails.requested_ms < ThumbnailWaitMs)
      return;
    if (auto* t = find_thumbnail(thumbnails.id)) {
      t->failed = true;
      int width = 0, height = 0;
      if (answered && result == win::DisplaySnapshotResult::ready)
        if (const auto image = load_thumbnail(width, height)) {
          if (t->image)
            DeleteObject(t->image);
          t->image = image;
          t->width = width;
          t->height = height;
          t->texture_width = sample.snapshot_width;
          t->texture_height = sample.snapshot_height;
          t->taken_ms = now;
          t->failed = false;
        }
    }
    thumbnails.serial = 0;
    InvalidateRect(window, nullptr, FALSE);
  } else if (request_taken) {
    thumbnails.serial = 0;
  }
  const bool showing = IsWindowVisible(window) && !IsIconic(window) && (page == 0 || page == 3 || page == SetupPage);
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
  const auto panels = setup_items(sample);
  if (page == SetupPage) {
    for (unsigned side = 0; side < setup.sides; ++side)
      want(picked_id(panels, side));
    for (const auto id : setup.ids)
      want(id);
    for (const auto& panel : panels)
      want(panel.id);
  }
  // Destroyed textures leave the candidate list and the panel map; drop
  // their pictures.
  std::erase_if(thumbnails.items, [&](Thumbnail& t) {
    const bool listed = std::any_of(panels.begin(), panels.end(), [&](const auto& panel) { return panel.id == t.id; });
    const bool gone = t.id != routed[0] && t.id != routed[1] && !find_candidate(sample, t.id) && !listed;
    if (gone && t.image)
      DeleteObject(t.image);
    return gone;
  });
  if (!showing || request_taken || !sample.heartbeat || now < sample.heartbeat || now - sample.heartbeat > 3000 || !sample.graphics_ready)
    return;
  for (const auto id : wanted) {
    auto* t = find_thumbnail(id);
    if (t && now - t->tried_ms < ThumbnailRetryMs)
      continue;
    if (t && t->image && t->taken_ms >= thumbnails.fresh_after)
      continue;
    // One that gave no picture waits for Refresh pictures or ThumbnailFreshMs.
    if (t && t->failed && t->tried_ms >= thumbnails.fresh_after && now - t->tried_ms < ThumbnailFreshMs)
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
                           : t && t->failed                                      ? L"Not being drawn"
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
  const int gap = 10, width = (730 - gap * (sides - 1)) / sides;
  for (int side = 0; side < sides; ++side) {
    const int x = 262 + side * (width + gap), y = 542;
    const auto id = routed[side];
    panel(dc, x, y, width, 112, Background);
    const auto* t = find_thumbnail(id);
    // A set-up aircraft's side is the whole texture: nothing to crop.
    const bool fits = t && !profile->fit_texture && t->texture_width == profile->width && t->texture_height == profile->height;
    const auto crop = profiles::display_rect(*profile, static_cast<unsigned>(side));
    draw_thumbnail(dc, rectangle(x + 8, y + 8, 96, 96), id, L"No display", fits ? &crop : nullptr);
    hit_areas.push_back({rectangle(x, y, width, 112), 0});
    const int tx = x + 116, tw = width - 124;
    text(dc, display_side_title(s, static_cast<unsigned>(side)), tx, y + 8, tw, 26, heading);
    const auto* candidate = find_candidate(sample, id);
    const auto name = candidate_name(candidate);
    text(dc, id ? (name.empty() ? (L"#" + std::to_wstring(id)).c_str() : name.c_str()) : L"Waiting for detection", tx, y + 38, tw, 22,
         small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    route_chip(dc, tx, y + 74, route_source(s, sample, static_cast<unsigned>(side), id));
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
    const int x = 262 + static_cast<int>(list) * 370, y = 176, width = lists == 1 ? 730 : 360;
    const auto id = routed[list];
    panel(dc, x, y, width, 120, Background);
    const auto box = rectangle(x + 7, y + 7, 106, 106);
    const auto drawn = draw_thumbnail(dc, box, id, L"Not found");
    const auto* t = find_thumbnail(id);
    if (id && drawn.right > drawn.left && profile && t && t->texture_width && t->texture_height &&
        (profile->fit_texture || (t->texture_width == profile->width && t->texture_height == profile->height))) {
      const auto sides = routed_texture_ids(sample, *profile);
      HPEN pen = CreatePen(PS_SOLID, scale(2), Accent);
      const auto old_pen = SelectObject(dc, pen);
      const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      const int dw = drawn.right - drawn.left, dh = drawn.bottom - drawn.top;
      for (unsigned side = 0; side < profile->sides && side < MaxDisplaySides; ++side) {
        if (sides[side] != id)
          continue;
        const auto r = profiles::side_rect(*profile, side, t->texture_width, t->texture_height);
        const int tw = static_cast<int>(t->texture_width), th = static_cast<int>(t->texture_height);
        Rectangle(dc, drawn.left + MulDiv(int(r.left), dw, tw), drawn.top + MulDiv(int(r.top), dh, th),
                  drawn.left + MulDiv(int(r.right), dw, tw), drawn.top + MulDiv(int(r.bottom), dh, th));
      }
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
    }
    if (id)
      hit_areas.push_back({box, id});
    const int tx = x + 126, tw = width - 134;
    text(dc, list_title(s, list), tx, y + 8, tw, 28, heading);
    const auto* candidate = find_candidate(sample, id);
    const auto name = candidate_name(candidate);
    text(dc,
         !id            ? L"Choose a texture below, or wait for detection."
         : name.empty() ? L"Unnamed texture"
                        : name.c_str(),
         tx, y + 38, tw, 24, normal, id && !name.empty() ? Text : Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (id)
      text(dc, candidate_details(id, candidate).c_str(), tx, y + 62, tw, 22, small, Muted);
    const unsigned side = list && separate_lower_profile(s) ? 2 : list;
    route_chip(dc, tx, y + 88, route_source(s, sample, side, id));
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
         262, 440, 730, 60, normal, Muted, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    return;
  }
  const unsigned pages = static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns);
  wchar_t page_label[32];
  std::swprintf(page_label, 32, L"%u / %u", std::min(gallery_page, pages - 1) + 1, pages);
  text(dc, page_label, 806, 338, 60, 32, small, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto id = gallery_ids[slot];
    const auto* candidate = find_candidate(sample, id);
    if (!id || !candidate)
      continue;
    const int x = 262 + static_cast<int>(slot) * 185, y = 382;
    const bool in_use = id == routed[0] || id == routed[1];
    panel(dc, x, y, 175, 208, Background);
    const auto box = rectangle(x + 6, y + 6, 163, 120);
    draw_thumbnail(dc, box, id, L"");
    hit_areas.push_back({box, id});
    if (in_use)
      chip(dc, x + 12, y + 12, routing_lists(s) == 1 ? L"In use" : list_action(s, id == routed[0] ? 0 : 1), Accent);
    const auto name = candidate_name(candidate);
    text(dc, name.empty() ? L"Unnamed" : name.c_str(), x + 10, y + 130, 155, 22, normal, name.empty() && any_named ? Muted : Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    wchar_t details[64];
    std::swprintf(details, 64, L"#%llu · %u × %u · m%u", static_cast<unsigned long long>(id), candidate->width, candidate->height,
                  candidate->mips);
    text(dc, details, x + 10, y + 152, 155, 20, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}
void draw_setup(HDC dc, const win::Status& sample) {
  sync_setup(sample);
  const auto items = setup_items(sample);
  panel(dc, 244, 124, 766, 196);
  const auto* built_in = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  const auto name = built_in       ? std::wstring(built_in->name)
                    : setup.key[0] ? widen(std::string(profiles::variant_name(setup.key.data())).c_str())
                                   : std::wstring(L"No aircraft reported yet");
  text(dc, name.c_str(), 262, 134, 730, 30, heading, Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  const auto blocker = setup_blocker(sample);
  text(dc,
       !blocker.empty() ? blocker.c_str()
       : built_in       ? L"Pick the panel each camera draws on, from this profile's displays. Use built-in displays returns to its own."
                        : L"Pick the panel each camera draws on. It is saved for this aircraft variant only.",
       262, 166, 730, 36, small, blocker.empty() ? Muted : RGB(236, 182, 92), DT_LEFT | DT_WORDBREAK);
  const int sides = static_cast<int>(setup.sides), gap = 10, width = (730 - gap * (sides - 1)) / sides;
  for (int side = 0; side < sides; ++side) {
    const int x = 262 + side * (width + gap), y = 206;
    panel(dc, x, y, width, 96, Background);
    const auto id = picked_id(items, static_cast<unsigned>(side));
    const auto box = rectangle(x + 6, y + 6, 84, 84);
    draw_thumbnail(dc, box, id, L"");
    if (id)
      hit_areas.push_back({box, id});
    text(dc, setup_side_title(static_cast<unsigned>(side)), x + 100, y + 8, width - 108, 28, heading);
    const auto& picked = setup.names[static_cast<unsigned>(side)];
    text(dc, picked.empty() ? L"Pick a panel below" : widen(picked.c_str()).c_str(), x + 100, y + 36, width - 108, 22, normal,
         picked.empty() ? Muted : Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  panel(dc, 244, 334, 766, 280);
  text(dc, L"Cockpit panels", 262, 344, 180, 32, heading);
  text(dc, L"Select a picture to enlarge it.", 446, 344, 190, 32, small, Muted);
  if (items.empty()) {
    text(dc, L"No panel pictures yet. Panels appear once the cockpit has been drawn.", 262, 445, 730, 60, normal, Muted,
         DT_CENTER | DT_VCENTER | DT_WORDBREAK);
  } else {
    const unsigned pages = static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns);
    wchar_t page_label[32];
    std::swprintf(page_label, 32, L"%u / %u", std::min(setup.gallery_page, pages - 1) + 1, pages);
    text(dc, page_label, 806, 344, 60, 32, small, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto id = setup.ids[slot];
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& panel) { return panel.id == id; });
    if (!id || item == items.end())
      continue;
    const int x = 262 + static_cast<int>(slot) * 185, y = 388;
    panel(dc, x, y, 175, 208, Background);
    const auto box = rectangle(x + 6, y + 6, 163, 120);
    draw_thumbnail(dc, box, id, L"");
    hit_areas.push_back({box, id});
    for (unsigned side = 0; side < setup.sides; ++side)
      if (setup.names[side] == panel_name(*item))
        chip(dc, x + 12, y + 12, setup_side_action(side), Accent);
    text(dc, widen(panel_name(*item).c_str()).c_str(), x + 10, y + 130, 155, 22, normal, Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    wchar_t details[64];
    std::swprintf(details, 64, L"%u × %u · m%u", item->width, item->height, item->mips);
    text(dc, details, x + 10, y + 152, 155, 20, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
  if (!result.ok || !win::parse_changelog(result.body, releases, nullptr, win::ChangelogParse::lenient)) {
    notice = result.ok ? L"What's new could not be read. Try again later."
                       : L"Could not load What's new from GitHub. Check your connection and try again.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (!open_whats_new(win::whats_new_blocks(win::releases_up_to(releases, InstalledVersion), InstalledVersion))) {
    notice = L"Could not open What's new.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  // Opened by itself once per installed version; the link stays.
  whats_new = false;
  if (!preview_ui && !win::record_whats_new_seen(win::settings_directory(), InstalledVersion))
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
// Whether a layout is saved for this variant, checked again every 5 s so one
// saved while the flight runs is found.
struct SavedVariant {
  profiles::VariantKey key{};
  bool saved{};
  std::uint64_t checked_ms{};
} saved_variant;
bool saved_layout(const profiles::VariantKey& key, std::uint64_t now) {
  if (key != saved_variant.key || now - saved_variant.checked_ms >= 5000) {
    profiles::DiscoveredLayout layout;
    saved_variant = {key, win::load_discovered_layout(key.data(), layout), now};
  }
  return saved_variant.saved;
}
std::uint64_t variant_hash(const profiles::VariantKey& key) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : key) {
    if (!c)
      break;
    hash = (hash ^ static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c)) * 1099511628211ull;
  }
  return hash;
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
  // An aircraft no catalog profile matches is selected by its variant when a
  // layout is saved for it. The bridge reports it as the set-up aircraft only
  // once that layout is in use.
  profiles::VariantKey variant{};
  auto candidate = sample.detected_profile;
  if (!candidate || candidate == profiles::DiscoveredProfileId) {
    const std::string_view path(sample.aircraft_path, strnlen(sample.aircraft_path, sizeof(sample.aircraft_path)));
    candidate =
        sample.identity_sample_ms && profiles::variant_key(path, variant) && saved_layout(variant, now) ? profiles::DiscoveredProfileId : 0;
  }
  const bool set_up = candidate == profiles::DiscoveredProfileId;
  const auto detected = profile_selection.observe(candidate, sample.identity_sample_ms, set_up ? variant_hash(variant) : 0);
  if (!detected || (detected == s.profile && (!set_up || profiles::same_variant(variant.data(), s.discovered.key))))
    return;
  // Preserve edits on the departing profile; invalid unfinished fields delay
  // switching instead of silently discarding the user's calibration.
  if (!apply())
    return;
  win::Settings next;
  if (!win::load_settings(next, installation, detected, set_up ? std::string_view(variant.data()) : std::string_view{}))
    return;
  next.enabled = s.enabled;
  next.panel_setup = s.panel_setup;
  if (!win::prepare_profile_selection(next, s, true) || !win::save_settings(next))
    return;
  publish(next);
  notice = set_up ? L"Set-up aircraft detected. Its saved displays and calibration are active."
                  : L"Aircraft detected. Its saved calibration is active.";
  build_controls();
}
// Camera adjustment window. Everything is painted here and hit-tested, so
// the keyboard always reaches the window. Changes apply live like the Camera
// views buttons; Save writes them, and the main window's fields follow them so
// a later Save changes cannot write back older values.
constexpr wchar_t LiveViewClass[] = L"TaxiCamLiveView";
// Mount steps (right, up, forward m; pitch, yaw deg; lens rad), scaled by
// live_view.step: fine x0.2, normal x1, coarse x5.
constexpr std::array<double, 6> LiveSteps{0.1, 0.1, 0.5, 0.5, 0.5, 0.02};
constexpr std::array<double, 3> LiveStepScale{0.2, 1, 5};
constexpr std::array<const wchar_t*, 6> LiveFieldNames{L"Right", L"Up", L"Forward", L"Pitch", L"Yaw", L"Lens"};
constexpr std::array<const wchar_t*, 6> LiveFieldUnits{L"m", L"m", L"m", L"°", L"°", L"rad"};
enum LiveAction : int {
  live_none = 0,
  live_feed = 100,
  live_step = 200,
  live_undo = 201,
  live_defaults = 202,
  live_area = 204,
  live_area_reset = 205,
  live_area_side = 210,
  live_adjust = 300
};
struct LiveButton {
  RECT rect{};
  int action{};
};
std::vector<LiveButton> live_buttons;
// Composition geometry in the 768 x 763 working image, drawn stretched over
// the display's camera area (pfd_stamp_d3d12.cpp), and that area on screen.
// The picture is the camera area too, or in display area mode the whole
// texture with the camera area inside it.
struct LiveLayout {
  RECT picture{}, drawn{};
  profiles::Composition c{};
  unsigned feeds = 2;
  bool guides{};
};
POINT live_point(const LiveLayout& l, float x, float y) {
  return {l.drawn.left + static_cast<LONG>(x * (l.drawn.right - l.drawn.left) / 768.f),
          l.drawn.top + static_cast<LONG>(y * (l.drawn.bottom - l.drawn.top) / 763.f)};
}
std::array<float, 2> live_working(const LiveLayout& l, POINT p) {
  const auto w = std::max<LONG>(l.drawn.right - l.drawn.left, 1), h = std::max<LONG>(l.drawn.bottom - l.drawn.top, 1);
  return {static_cast<float>(p.x - l.drawn.left) * 768.f / w, static_cast<float>(p.y - l.drawn.top) * 763.f / h};
}
float live_split_pane(const LiveLayout& l) {
  return (768.f - l.c.bottom_gap) * 0.5f;
}
// The camera feed under a working-image point, or -1 between panes.
int live_feed_at(const LiveLayout& l, float x, float y) {
  if (x < 0 || x >= 768 || y < 0 || y >= 763)
    return -1;
  if (y < l.c.nose_height)
    return 0;
  if (y < l.c.tail_top)
    return -1;
  if (!l.c.split_bottom)
    return 1;
  const float pane = live_split_pane(l);
  return x < pane ? 1 : x >= pane + l.c.bottom_gap ? 2 : -1;
}
// Guide points as the compositor draws them (camera_compositor_d3d12.hpp):
// the nose dot mirrored about the centre; in each tail pane the left bracket
// from the pane's left edge and the right one from its right edge. Each entry:
// guide, working x, y.
struct LiveGuidePoint {
  int guide;
  float x, y;
};
std::vector<LiveGuidePoint> live_guide_points(const LiveLayout& l, const win::Settings& s) {
  std::vector<LiveGuidePoint> points;
  if (!l.guides)
    return points;
  points.push_back({0, s.nose_dot[0] * 768, s.nose_dot[1] * l.c.nose_height});
  points.push_back({0, 768 - s.nose_dot[0] * 768, s.nose_dot[1] * l.c.nose_height});
  const float span = l.c.split_bottom ? live_split_pane(l) : 768.f, tail_height = 763.f - l.c.tail_top;
  const std::array<std::array<float, 2>, 3> tail{s.tail_upper, s.tail_corner, s.tail_inner};
  const std::array<std::array<float, 2>, 3> right{s.tail_upper_right, s.tail_corner_right, s.tail_inner_right};
  for (const float offset : l.c.split_bottom ? std::vector<float>{0, span + l.c.bottom_gap} : std::vector<float>{0}) {
    for (int i = 0; i < 3; ++i)
      points.push_back({i + 1, offset + tail[i][0] * span, l.c.tail_top + tail[i][1] * tail_height});
    for (int i = 0; i < 3; ++i)
      points.push_back({i + 4, offset + span - right[i][0] * span, l.c.tail_top + right[i][1] * tail_height});
  }
  return points;
}
// Texture pixels on the picture, in display area mode.
POINT live_texture_point(const RECT& picture, unsigned x, unsigned y) {
  const auto tw = std::max<unsigned>(live_view.texture_width, 1), th = std::max<unsigned>(live_view.texture_height, 1);
  return {picture.left + MulDiv(static_cast<int>(x), picture.right - picture.left, static_cast<int>(tw)),
          picture.top + MulDiv(static_cast<int>(y), picture.bottom - picture.top, static_cast<int>(th))};
}
// The area shown: the one being dragged, else the side's area now.
profiles::DisplayRect live_area_now(const profiles::AircraftProfile& profile) {
  if (live_view.drag == LiveView::Drag::area)
    return live_view.area;
  return profiles::side_rect(profile, live_view.side, live_view.texture_width, live_view.texture_height);
}
// Display area mode with a picture to edit the area on.
bool live_area_shown() {
  return live_view.area_mode && live_view.image && live_view.texture_width && live_view.texture_height;
}
LiveLayout live_layout(HWND hwnd, const profiles::AircraftProfile* profile) {
  LiveLayout l;
  if (profile) {
    l.c = profile->composition;
    l.feeds = profile->composition.split_bottom != 0 ? 3 : 2;
    l.guides = profile->reference_guides;
  }
  RECT client{};
  GetClientRect(hwnd, &client);
  const RECT area{scale(16), scale(92), client.right - scale(336), client.bottom - scale(16)};
  // The camera area of the display texture, at its own aspect; in area mode
  // the whole texture.
  const bool whole = profile && live_area_shown();
  double aspect = 768.0 / 763.0;
  if (whole)
    aspect = double(live_view.texture_width) / double(live_view.texture_height);
  else if (profile && live_view.texture_width && live_view.texture_height) {
    const auto inner = profiles::side_content_rect(*profile, live_view.side, live_view.texture_width, live_view.texture_height);
    if (inner.right > inner.left && inner.bottom > inner.top)
      aspect = double(inner.right - inner.left) / double(inner.bottom - inner.top);
  }
  const int aw = std::max<int>(area.right - area.left, 1), ah = std::max<int>(area.bottom - area.top, 1);
  const int w = std::min(aw, static_cast<int>(ah * aspect)), h = std::min(ah, static_cast<int>(aw / aspect));
  l.picture = {area.left + (aw - w) / 2, area.top + (ah - h) / 2, area.left + (aw - w) / 2 + w, area.top + (ah - h) / 2 + h};
  l.drawn = l.picture;
  if (whole) {
    // The camera inset of the area, where the composition is drawn (at the
    // cameras' shape on a side they were not shaped for).
    const auto a = live_area_now(*profile);
    auto inner = profiles::fit_rect(profiles::inset_rect(a, profile->camera_padding),
                                    live_view.side < MaxDisplaySides ? profile->fit_aspect[live_view.side] : 0);
    if (inner.right <= inner.left || inner.bottom <= inner.top)
      inner = a;
    const auto from = live_texture_point(l.picture, inner.left, inner.top), to = live_texture_point(l.picture, inner.right, inner.bottom);
    l.drawn = {from.x, from.y, std::max(to.x, from.x + 1), std::max(to.y, from.y + 1)};
  }
  return l;
}
void live_button(HDC dc, const RECT& r, const wchar_t* label, int action, bool selected = false, bool primary = false) {
  live_buttons.push_back({r, action});
  HBRUSH brush = CreateSolidBrush(primary ? Accent : selected ? RGB(30, 64, 63) : Card);
  HPEN pen = CreatePen(PS_SOLID, scale(1), selected || primary ? Accent : Border);
  const auto oldb = SelectObject(dc, brush), oldp = SelectObject(dc, pen);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, scale(9), scale(9));
  SelectObject(dc, oldb);
  SelectObject(dc, oldp);
  DeleteObject(brush);
  DeleteObject(pen);
  SelectObject(dc, normal);
  SetTextColor(dc, primary ? Background : selected ? Accent : Text);
  SetBkMode(dc, TRANSPARENT);
  auto bounds = r;
  DrawTextW(dc, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void live_text(HDC dc, const wchar_t* value, RECT r, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
  SelectObject(dc, font);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, value, -1, &r, flags | DT_END_ELLIPSIS);
}
const wchar_t* live_feed_name(unsigned feeds, unsigned feed) {
  return feed == 0 ? L"Nose" : feeds == 3 ? (feed == 1 ? L"Left wing" : L"Right wing") : L"Tail";
}
// Corner and edge handles of the area: 0-3 corners, 4-7 edge midpoints.
std::array<POINT, 8> live_area_handles(const LiveLayout& l, const profiles::DisplayRect& a) {
  const unsigned cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
  const auto& p = l.picture;
  return {live_texture_point(p, a.left, a.top),    live_texture_point(p, a.right, a.top), live_texture_point(p, a.right, a.bottom),
          live_texture_point(p, a.left, a.bottom), live_texture_point(p, cx, a.top),      live_texture_point(p, a.right, cy),
          live_texture_point(p, cx, a.bottom),     live_texture_point(p, a.left, cy)};
}
// What a point of the picture would drag: a display area handle (index 0-7)
// or its frame (8, to move it), a guide point (its guide), or a camera pane
// (its feed). Clicks and the pointer shape both use it.
struct LiveHit {
  enum class Kind { none, area, guide, pane } kind{};
  int index = -1;
};
LiveHit live_hit(const LiveLayout& l, const win::Settings& s, const profiles::AircraftProfile& profile, POINT p) {
  const auto grabs = [&](POINT q) { return std::abs(q.x - p.x) <= scale(9) && std::abs(q.y - p.y) <= scale(9); };
  const bool area = live_area_shown();
  const auto a = area ? live_area_now(profile) : profiles::DisplayRect{};
  if (area) {
    const auto handles = live_area_handles(l, a);
    for (int i = 0; i < 8; ++i)
      if (grabs(handles[static_cast<unsigned>(i)]))
        return {LiveHit::Kind::area, i};
  }
  for (const auto& point : live_guide_points(l, s))
    if (grabs(live_point(l, point.x, point.y)))
      return {LiveHit::Kind::guide, point.guide};
  const auto w = live_working(l, p);
  const int feed = live_feed_at(l, w[0], w[1]);
  if (area) {
    // The frame: the rectangle's edges, and inside it wherever no camera is
    // drawn (the camera inset and the gaps between cameras).
    const auto from = live_texture_point(l.picture, a.left, a.top), to = live_texture_point(l.picture, a.right, a.bottom);
    const int reach = scale(6);
    const bool around = p.x >= from.x - reach && p.x <= to.x + reach && p.y >= from.y - reach && p.y <= to.y + reach;
    const bool edge = std::abs(p.x - from.x) <= reach || std::abs(p.x - to.x) <= reach || std::abs(p.y - from.y) <= reach ||
                      std::abs(p.y - to.y) <= reach;
    if (around && (edge || feed < 0))
      return {LiveHit::Kind::area, 8};
  }
  if (feed >= 0)
    return {LiveHit::Kind::pane, feed};
  return {};
}
void paint_live_view(HWND hwnd, HDC dc) {
  live_buttons.clear();
  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, background_brush);
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  const auto l = live_layout(hwnd, profile);
  live_view.feed = std::min(live_view.feed, l.feeds - 1);
  // Toolbar: camera, step size, display area, undo and defaults.
  int x = scale(16);
  for (unsigned feed = 0; feed < l.feeds; ++feed) {
    live_button(dc, {x, scale(14), x + scale(118), scale(50)}, live_feed_name(l.feeds, feed), live_feed + static_cast<int>(feed),
                feed == live_view.feed);
    x += scale(126);
  }
  const wchar_t* steps[]{L"Step: Fine", L"Step: Normal", L"Step: Coarse"};
  live_button(dc, {x + scale(8), scale(14), x + scale(160), scale(50)}, steps[live_view.step], live_step);
  live_button(dc, {x + scale(168), scale(14), x + scale(320), scale(50)}, L"Display area", live_area, live_view.area_mode);
  // Changes apply live and are saved when the window closes. Undo returns to
  // the saved settings from when it opened; Defaults to the profile's.
  live_button(dc, {client.right - scale(166), scale(14), client.right - scale(16), scale(50)}, L"Undo changes", live_undo, false,
              live_view.changed);
  live_button(dc, {client.right - scale(306), scale(14), client.right - scale(176), scale(50)}, L"Defaults", live_defaults);
  // Caption: which display, and how fresh its picture is.
  const auto now = GetTickCount64();
  std::wstring caption;
  if (!profile)
    caption = L"No aircraft profile selected.";
  else if (!fresh_status(sample) || !sample.graphics_ready)
    caption = L"Choose Connect with a flight loaded to see the cameras.";
  else if (!live_view.id)
    caption = L"Waiting for a display to be found for this aircraft.";
  else {
    caption = std::wstring(display_side_title(s, live_view.side)) +
              (live_view.area_mode ? L" · drag a white handle to resize the camera area, or its frame to move it"
                                   : L" · drag in a camera picture to aim it, wheel to zoom");
    // Without a fresh picture, say what the bridge is doing (cameras starting,
    // waiting for the display) rather than showing a blank view.
    const bool stale = !live_view.image || (live_view.taken_ms && now - live_view.taken_ms > 3000);
    const auto bridge = widen(std::string(sample.message, strnlen(sample.message, sizeof(sample.message))).c_str());
    if (stale && !bridge.empty())
      caption = bridge;
    else if (live_view.taken_ms && now - live_view.taken_ms > 3000)
      caption += L" · picture " + std::to_wstring((now - live_view.taken_ms) / 1000) + L" s old";
  }
  if (!live_view.note.empty() && now - live_view.note_ms < 5000)
    caption = live_view.note;
  live_text(dc, caption.c_str(), {scale(16), scale(58), client.right - scale(336), scale(84)}, small, Muted);
  // Picture: the display's camera area, stretched as the compositor draws it;
  // in display area mode the whole texture.
  const bool area = profile && live_area_shown();
  HBRUSH fill = CreateSolidBrush(Sidebar);
  FillRect(dc, &l.picture, fill);
  DeleteObject(fill);
  bool shown = false;
  if (profile && live_view.image && live_view.texture_width && live_view.texture_height) {
    int sx = 0, sy = 0, sw = live_view.width, sh = live_view.height;
    if (!area) {
      const auto inner = profiles::side_content_rect(*profile, live_view.side, live_view.texture_width, live_view.texture_height);
      const int tw = static_cast<int>(live_view.texture_width), th = static_cast<int>(live_view.texture_height);
      sw = 0;
      if (inner.right > inner.left && inner.bottom > inner.top && inner.right <= live_view.texture_width &&
          inner.bottom <= live_view.texture_height) {
        sx = MulDiv(static_cast<int>(inner.left), live_view.width, tw);
        sy = MulDiv(static_cast<int>(inner.top), live_view.height, th);
        sw = std::max(1, MulDiv(static_cast<int>(inner.right), live_view.width, tw) - sx);
        sh = std::max(1, MulDiv(static_cast<int>(inner.bottom), live_view.height, th) - sy);
      }
    }
    if (sw > 0) {
      HDC memory = CreateCompatibleDC(dc);
      const auto old_bitmap = SelectObject(memory, live_view.image);
      SetStretchBltMode(dc, HALFTONE);
      SetBrushOrgEx(dc, 0, 0, nullptr);
      StretchBlt(dc, l.picture.left, l.picture.top, l.picture.right - l.picture.left, l.picture.bottom - l.picture.top, memory, sx, sy, sw,
                 sh, SRCCOPY);
      SelectObject(memory, old_bitmap);
      DeleteDC(memory);
      shown = true;
    }
  }
  if (!shown) {
    auto r = l.picture;
    live_text(dc,
              !live_view.id         ? L"No picture"
              : live_view.area_mode ? L"Waiting for the display picture…"
                                    : L"Waiting for the camera picture…",
              r, normal, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  if (profile) {
    // The selected camera's pane.
    const float pane = live_split_pane(l);
    std::array<float, 4> box{0, 0, 768, l.c.nose_height};
    if (live_view.feed == 1)
      box = {0, l.c.tail_top, l.c.split_bottom ? pane : 768.f, 763};
    else if (live_view.feed == 2)
      box = {pane + l.c.bottom_gap, l.c.tail_top, 768, 763};
    const auto a = live_point(l, box[0], box[1]), b = live_point(l, box[2], box[3]);
    HPEN pen = CreatePen(PS_SOLID, scale(2), Accent);
    const auto old_pen = SelectObject(dc, pen);
    const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, a.x, a.y, b.x, b.y);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    // Guides, drawn here too so they follow a drag before the picture does.
    if (l.guides) {
      const COLORREF guide = RGB(UINT(s.guide_color[0] * 255), UINT(s.guide_color[1] * 255), UINT(s.guide_color[2] * 255));
      HPEN line = CreatePen(PS_SOLID, scale(2), guide);
      SelectObject(dc, line);
      const auto points = live_guide_points(l, s);
      // Tail brackets: upper - corner - inner. After the two nose points come
      // six per pane: the left bracket's three, then the right one's.
      for (std::size_t base = 2; base + 3 <= points.size(); base += 3) {
        const POINT bracket[]{live_point(l, points[base].x, points[base].y), live_point(l, points[base + 1].x, points[base + 1].y),
                              live_point(l, points[base + 2].x, points[base + 2].y)};
        Polyline(dc, bracket, 3);
      }
      SelectObject(dc, old_pen);
      DeleteObject(line);
      // Handles: every guide point can be dragged.
      HBRUSH handle = CreateSolidBrush(RGB(255, 255, 255));
      HPEN outline = CreatePen(PS_SOLID, scale(2), guide);
      SelectObject(dc, handle);
      SelectObject(dc, outline);
      // While a dragged point is snapped, a dotted line joins it to its
      // partner and the partner is drawn as large as the dragged one.
      const int partner = live_view.guide > 0 ? (live_view.guide >= 4 ? live_view.guide - 3 : live_view.guide + 3) : -1;
      const bool snapped = live_view.drag == LiveView::Drag::guide && (live_view.snapped[0] || live_view.snapped[1]);
      if (snapped) {
        HPEN dotted = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        const auto before = SelectObject(dc, dotted);
        for (const auto& a : points)
          for (const auto& b : points)
            if (a.guide == live_view.guide && b.guide == partner && std::abs(a.y - b.y) < 400) {
              const auto pa = live_point(l, a.x, a.y), pb = live_point(l, b.x, b.y);
              MoveToEx(dc, pa.x, pa.y, nullptr);
              LineTo(dc, pb.x, pb.y);
            }
        SelectObject(dc, before);
        DeleteObject(dotted);
      }
      for (const auto& point : points) {
        const auto p = live_point(l, point.x, point.y);
        const int r = scale(point.guide == live_view.guide || (snapped && point.guide == partner) ? 7 : 5);
        Ellipse(dc, p.x - r, p.y - r, p.x + r, p.y + r);
      }
      SelectObject(dc, old_pen);
      DeleteObject(outline);
      DeleteObject(handle);
    }
    if (area) {
      // The display area: its frame, with square handles at the corners and
      // edge midpoints.
      const auto a = live_area_now(*profile);
      const auto from = live_texture_point(l.picture, a.left, a.top), to = live_texture_point(l.picture, a.right, a.bottom);
      HPEN frame = CreatePen(PS_SOLID, scale(1), RGB(255, 255, 255));
      SelectObject(dc, frame);
      SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, from.x, from.y, to.x, to.y);
      HPEN outline = CreatePen(PS_SOLID, scale(1), Accent);
      SelectObject(dc, outline);
      SelectObject(dc, GetStockObject(WHITE_BRUSH));
      for (const auto& h : live_area_handles(l, a))
        Rectangle(dc, h.x - scale(5), h.y - scale(5), h.x + scale(6), h.y + scale(6));
      SelectObject(dc, old_pen);
      DeleteObject(frame);
      DeleteObject(outline);
    }
    SelectObject(dc, old_brush);
  }
  // Side panel: in display area mode which display and its area first; then
  // the selected camera's mount, with step buttons.
  const int px = client.right - scale(320), pw = scale(304);
  int y = scale(92);
  if (live_view.area_mode) {
    live_text(dc, L"Display area", {px, y, px + pw, y + scale(30)}, heading, Text);
    const unsigned sides = profile ? std::min<unsigned>(profile->sides, MaxDisplaySides) : 0;
    const int width = sides ? (pw - static_cast<int>(sides - 1) * scale(8)) / static_cast<int>(sides) : pw;
    for (unsigned side = 0; side < sides; ++side) {
      const int bx = px + static_cast<int>(side) * (width + scale(8));
      live_button(dc, {bx, y + scale(36), bx + width, y + scale(70)}, display_side_title(s, side), live_area_side + static_cast<int>(side),
                  side == live_view.side);
    }
    if (area) {
      const auto a = live_area_now(*profile);
      wchar_t value[96];
      std::swprintf(value, 96, L"From %u, %u · %u × %u of %u × %u", a.left, a.top, a.right - a.left, a.bottom - a.top,
                    live_view.texture_width, live_view.texture_height);
      live_text(dc, value, {px, y + scale(74), px + pw, y + scale(100)}, small, Text);
    }
    live_button(dc, {px, y + scale(104), px + pw, y + scale(138)}, L"Reset area", live_area_reset);
    y += scale(152);
  }
  live_text(dc, (std::wstring(live_feed_name(l.feeds, live_view.feed)) + L" camera").c_str(), {px, y, px + pw, y + scale(30)}, heading,
            Text);
  const auto& mount = s.mounts[live_view.feed];
  for (int j = 0; j < 6; ++j) {
    const int row = y + scale(40 + j * 44);
    wchar_t value[48];
    std::swprintf(value, 48, L"%.2f %ls", mount[j], LiveFieldUnits[j]);
    live_text(dc, LiveFieldNames[j], {px, row, px + scale(90), row + scale(40)}, normal, Muted);
    live_text(dc, value, {px + scale(90), row, px + scale(200), row + scale(40)}, normal, Text);
    live_button(dc, {px + scale(204), row + scale(2), px + scale(250), row + scale(38)}, L"−", live_adjust + j * 2);
    live_button(dc, {px + scale(256), row + scale(2), px + pw, row + scale(38)}, L"+", live_adjust + j * 2 + 1);
  }
  y += scale(40 + 6 * 44 + 8);
  live_text(dc,
            live_view.area_mode
                ? L"The white rectangle is where this display's camera is drawn on its texture. Drag a handle to resize it or "
                  L"its frame to move it; it applies when you let go. A new shape renders the cameras at that shape, which "
                  L"takes a few seconds; the other display shows them at that shape too."
                : L"Drag a camera picture to aim it; the wheel zooms. Arrows move right and forward, Page Up/Down up and down, "
                  L"Ctrl+arrows aim, + and − zoom, Tab changes camera, Shift is fine. Drag the white points to move the guides; "
                  L"a tail point snaps level with the other side's, and to the same distance from its edge (hold Alt to place "
                  L"it freely).\n\nChanges show live and are saved when you close this window. Undo changes returns to how they "
                  L"were saved; Defaults restores this camera's and the guides' starting values.",
            {px, y, px + pw, client.bottom - scale(16)}, small, Muted, DT_LEFT | DT_WORDBREAK | DT_TOP);
}
// Applies a live change: in bounds, published, marked unsaved, and shown in
// the main window's fields. False if the settings were refused.
bool live_apply(win::Settings s) {
  for (auto& m : s.mounts) {
    for (int j = 0; j < 3; ++j)
      m[j] = std::clamp(m[j], -500.0, 500.0);
    m[3] = std::clamp(m[3], -89.0, 89.0);
    m[4] = m[4] > 180 ? m[4] - 360 : m[4] < -180 ? m[4] + 360 : m[4];
    m[5] = std::clamp(m[5], 0.05, 1.55);
  }
  for (auto* guide :
       {&s.nose_dot, &s.tail_upper, &s.tail_corner, &s.tail_inner, &s.tail_upper_right, &s.tail_corner_right, &s.tail_inner_right}) {
    (*guide)[0] = std::clamp((*guide)[0], 0.f, 0.5f);
    (*guide)[1] = std::clamp((*guide)[1], 0.f, 1.f);
  }
  if (!win::valid_settings(s))
    return false;
  publish(s);
  dirty_notice();
  live_view.changed = true;
  refreshing = true;
  if (page == 1)
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < 6; ++j)
        if (HWND field = GetDlgItem(window, static_cast<int>(300 + i * 10 + j))) {
          wchar_t text[64];
          std::swprintf(text, 64, L"%.10g", s.mounts[i][j]);
          SetWindowTextW(field, text);
        }
  if (page == 5) {
    const std::array<float, 2> guides[]{s.nose_dot,         s.tail_upper,        s.tail_corner,     s.tail_inner,
                                        s.tail_upper_right, s.tail_corner_right, s.tail_inner_right};
    for (unsigned i = 0; i < 7; ++i)
      for (unsigned axis = 0; axis < 2; ++axis)
        if (HWND field = GetDlgItem(window, static_cast<int>((i < 4 ? 360 + i * 2 : 380 + (i - 4) * 2) + axis))) {
          wchar_t text[64];
          std::swprintf(text, 64, L"%.1f", guides[i][axis] * 100.);
          SetWindowTextW(field, text);
        }
  }
  refreshing = false;
  if (live_view.window)
    InvalidateRect(live_view.window, nullptr, FALSE);
  return true;
}
void live_adjust_field(unsigned field, double direction) {
  auto s = draft();
  const double step = LiveSteps[field] * LiveStepScale[static_cast<unsigned>(live_view.step)] * direction;
  s.mounts[live_view.feed][field] += step;
  live_apply(s);
}
// The area as the user's choice for the side: the profile's own region (or
// a set-up aircraft's whole texture) is no choice.
void live_commit_area(const profiles::DisplayRect& area) {
  auto s = draft();
  const auto* base = profiles::base_profile(s.profile);
  if (!base || live_view.side >= MaxDisplaySides)
    return;
  const auto own = base->fit_texture ? profiles::DisplayRect{0, 0, live_view.texture_width, live_view.texture_height}
                                     : profiles::display_rect(*base, live_view.side);
  const bool same = area.left == own.left && area.top == own.top && area.right == own.right && area.bottom == own.bottom;
  s.display_override.areas[live_view.side] = same ? profiles::DisplayRect{} : area;
  if (!live_apply(s)) {
    live_view.note = L"That area leaves no room for the camera, so it was not applied.";
    live_view.note_ms = GetTickCount64();
  }
  if (live_view.window)
    InvalidateRect(live_view.window, nullptr, FALSE);
}
void live_click(HWND hwnd, POINT p) {
  for (const auto& button : live_buttons) {
    if (!PtInRect(&button.rect, p))
      continue;
    const auto* profile = profiles::find(draft().profile);
    if (button.action >= live_adjust)
      live_adjust_field(static_cast<unsigned>(button.action - live_adjust) / 2, (button.action - live_adjust) % 2 ? 1 : -1);
    else if (button.action >= live_feed && button.action < live_feed + 3)
      live_view.feed = static_cast<unsigned>(button.action - live_feed);
    else if (button.action == live_step)
      live_view.step = (live_view.step + 1) % 3;
    else if (button.action == live_area) {
      live_view.area_mode = !live_view.area_mode;
      live_view.area_side = live_view.side;
    } else if (button.action >= live_area_side && button.action < live_area_side + 3) {
      live_view.area_side = static_cast<unsigned>(button.action - live_area_side);
    } else if (button.action == live_area_reset) {
      auto s = draft();
      if (live_view.side < MaxDisplaySides)
        s.display_override.areas[live_view.side] = {};
      live_apply(s);
    } else if (button.action == live_undo) {
      auto s = draft();
      s.mounts = live_view.saved.mounts;
      for (auto member : {&win::Settings::nose_dot, &win::Settings::tail_upper, &win::Settings::tail_corner, &win::Settings::tail_inner,
                          &win::Settings::tail_upper_right, &win::Settings::tail_corner_right, &win::Settings::tail_inner_right})
        s.*member = live_view.saved.*member;
      s.display_override = live_view.saved.display_override;
      live_apply(s);
      live_view.changed = false;
    } else if (button.action == live_defaults && profile) {
      auto s = draft();
      s.mounts[live_view.feed] = profile->mounts[live_view.feed];
      const auto color = s.guide_color;
      win::reset_guide_settings(s, *profile);
      s.guide_color = color;  // Defaults move the guides; the colour is the user's.
      live_apply(s);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return;
  }
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  if (!profile)
    return;
  const auto l = live_layout(hwnd, profile);
  const auto hit = live_hit(l, s, *profile, p);
  if (hit.kind == LiveHit::Kind::area) {
    live_view.area = live_view.area_start = live_area_now(*profile);
    live_view.drag = LiveView::Drag::area;
    live_view.area_handle = hit.index;
    live_view.area_from = p;
  } else if (hit.kind == LiveHit::Kind::guide) {
    live_view.drag = LiveView::Drag::guide;
    live_view.guide = hit.index;
  } else if (hit.kind == LiveHit::Kind::pane) {
    live_view.feed = static_cast<unsigned>(hit.index);
    live_view.drag = LiveView::Drag::aim;
    live_view.last = p;
  } else {
    return;
  }
  SetCapture(hwnd);
  InvalidateRect(hwnd, nullptr, FALSE);
}
void live_drag(HWND hwnd, POINT p) {
  auto s = draft();
  const auto* profile = profiles::find(s.profile);
  if (!profile || live_view.drag == LiveView::Drag::none)
    return;
  const auto l = live_layout(hwnd, profile);
  if (live_view.drag == LiveView::Drag::area) {
    // Pointer movement in texture pixels, applied to the edges the handle moves.
    const int tw = static_cast<int>(live_view.texture_width), th = static_cast<int>(live_view.texture_height);
    const int dx = MulDiv(p.x - live_view.area_from.x, tw, std::max<int>(l.picture.right - l.picture.left, 1));
    const int dy = MulDiv(p.y - live_view.area_from.y, th, std::max<int>(l.picture.bottom - l.picture.top, 1));
    const auto& a0 = live_view.area_start;
    int left = static_cast<int>(a0.left), top = static_cast<int>(a0.top), right = static_cast<int>(a0.right),
        bottom = static_cast<int>(a0.bottom);
    const int h = live_view.area_handle;
    // Large enough to keep a camera inside the profile's padding (and a
    // catalog profile's whole-display minimum, valid_override).
    const auto& pad = profile->camera_padding;
    const int least = profile->fit_texture ? 16 : 64;
    const int min_width = std::max(least, static_cast<int>(pad.left + pad.right) + 16);
    const int min_height = std::max(least, static_cast<int>(pad.top + pad.bottom) + 16);
    if (h == 8) {
      const int w = right - left, ht = bottom - top;
      left = std::clamp(left + dx, 0, std::max(tw - w, 0));
      top = std::clamp(top + dy, 0, std::max(th - ht, 0));
      right = left + w;
      bottom = top + ht;
    } else {
      if (h == 0 || h == 3 || h == 7)
        left = std::clamp(left + dx, 0, std::max(right - min_width, 0));
      if (h == 1 || h == 2 || h == 5)
        right = std::clamp(right + dx, std::min(left + min_width, tw), tw);
      if (h == 0 || h == 1 || h == 4)
        top = std::clamp(top + dy, 0, std::max(bottom - min_height, 0));
      if (h == 2 || h == 3 || h == 6)
        bottom = std::clamp(bottom + dy, std::min(top + min_height, th), th);
    }
    live_view.area = {static_cast<unsigned>(std::max(left, 0)), static_cast<unsigned>(std::max(top, 0)),
                      static_cast<unsigned>(std::max(right, 0)), static_cast<unsigned>(std::max(bottom, 0))};
    InvalidateRect(hwnd, nullptr, FALSE);
    return;
  }
  if (live_view.drag == LiveView::Drag::guide) {
    const auto w = live_working(l, p);
    std::array<float, 2> value{};
    if (live_view.guide == 0) {
      value = {std::min(w[0], 768 - w[0]) / 768, w[1] / std::max(l.c.nose_height, 1.f)};
    } else {
      float x = w[0], span = 768;
      if (l.c.split_bottom) {
        span = live_split_pane(l);
        if (x >= span)
          x -= span + l.c.bottom_gap;
      }
      // Left points from the pane's left edge, right ones from its right edge.
      value = {(live_view.guide >= 4 ? span - x : x) / span, (w[1] - l.c.tail_top) / std::max(763 - l.c.tail_top, 1.f)};
    }
    std::array<std::array<float, 2>*, 7> guides{&s.nose_dot,         &s.tail_upper,        &s.tail_corner,     &s.tail_inner,
                                                &s.tail_upper_right, &s.tail_corner_right, &s.tail_inner_right};
    // A tail point snaps to the mirror of the same point on the other side,
    // each axis within 8 px, so a symmetric bracket is easy; Alt places it
    // freely. Both are measured from their own edge, so mirror = same value.
    live_view.snapped = {};
    if (live_view.guide > 0 && GetKeyState(VK_MENU) >= 0) {
      const auto& partner = *guides[static_cast<unsigned>(live_view.guide >= 4 ? live_view.guide - 3 : live_view.guide + 3)];
      const float span = l.c.split_bottom ? live_split_pane(l) : 768.f;
      const float width = span * float(l.drawn.right - l.drawn.left) / 768.f;
      const float height = (763 - l.c.tail_top) * float(l.drawn.bottom - l.drawn.top) / 763.f;
      const std::array<float, 2> reach{scale(8) / std::max(width, 1.f), scale(8) / std::max(height, 1.f)};
      for (unsigned axis = 0; axis < 2; ++axis)
        if (std::abs(value[axis] - partner[axis]) <= reach[axis]) {
          value[axis] = partner[axis];
          live_view.snapped[axis] = true;
        }
    }
    *guides[static_cast<unsigned>(live_view.guide)] = value;
    live_apply(s);
    return;
  }
  // Aim: the picture follows the pointer. The lens is the vertical field of
  // view, so a pane's height in pixels spans that many degrees.
  const float pane_rows = live_view.feed == 0 ? l.c.nose_height
                          : l.c.split_bottom  ? (l.c.bottom_pane_height > 0 ? l.c.bottom_pane_height : live_split_pane(l))
                                              : 763 - l.c.tail_top;
  const double rows_on_screen = std::max(1.0, pane_rows * double(l.drawn.bottom - l.drawn.top) / 763.0);
  auto& mount = s.mounts[live_view.feed];
  const double degrees_per_pixel = mount[5] * 180.0 / 3.14159265358979323846 / rows_on_screen;
  mount[4] -= (p.x - live_view.last.x) * degrees_per_pixel;
  mount[3] += (p.y - live_view.last.y) * degrees_per_pixel;
  live_view.last = p;
  live_apply(s);
}
LRESULT CALLBACK live_view_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  switch (message) {
    case WM_CREATE: {
      BOOL dark = TRUE;
      DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
      SetTimer(hwnd, 1, 50, nullptr);
      return 0;
    }
    case WM_TIMER:
      service_live_view();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_GETMINMAXINFO:
      reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {scale(1060), scale(640)};
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      HDC memory = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
      const auto old = SelectObject(memory, bitmap);
      paint_live_view(hwnd, memory);
      BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      live_click(hwnd, {static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))});
      return 0;
    case WM_MOUSEMOVE:
      live_drag(hwnd, {static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))});
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
      // Every drag ends here: the pointer is released (else every later click,
      // in any window, would still come here) and a moved display area applies.
      const auto drag = live_view.drag;
      live_view.drag = LiveView::Drag::none;
      live_view.guide = -1;
      if (message == WM_LBUTTONUP && GetCapture() == hwnd)
        ReleaseCapture();
      if (drag == LiveView::Drag::area && message == WM_LBUTTONUP)
        live_commit_area(live_view.area);
      if (drag != LiveView::Drag::none)
        InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      ScreenToClient(hwnd, &p);
      auto s = draft();
      const auto* profile = profiles::find(s.profile);
      if (!profile)
        return 0;
      const auto layout = live_layout(hwnd, profile);
      const auto at = live_working(layout, p);
      const int feed = live_feed_at(layout, at[0], at[1]);
      if (feed >= 0)
        live_view.feed = static_cast<unsigned>(feed);
      // Wheel up narrows the lens (zooms in).
      const double notches = static_cast<short>(HIWORD(w)) / double(WHEEL_DELTA);
      s.mounts[live_view.feed][5] *= std::pow(0.95, notches);
      live_apply(s);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        // The pointer shows what a drag from here does: resize or move the
        // display area, move a guide, or aim a camera.
        LPCWSTR cursor = nullptr;
        const auto s = draft();
        if (std::any_of(live_buttons.begin(), live_buttons.end(), [&](const auto& button) { return PtInRect(&button.rect, p); }))
          cursor = IDC_HAND;
        else if (const auto* profile = profiles::find(s.profile)) {
          const auto hit = live_hit(live_layout(hwnd, profile), s, *profile, p);
          const LPCWSTR area[]{IDC_SIZENWSE, IDC_SIZENESW, IDC_SIZENWSE, IDC_SIZENESW, IDC_SIZENS,
                               IDC_SIZEWE,   IDC_SIZENS,   IDC_SIZEWE,   IDC_SIZEALL};
          if (hit.kind == LiveHit::Kind::area && hit.index >= 0 && hit.index < 9)
            cursor = area[hit.index];
          else if (hit.kind == LiveHit::Kind::guide)
            cursor = IDC_SIZEALL;
          else if (hit.kind == LiveHit::Kind::pane)
            cursor = IDC_CROSS;
        }
        if (cursor) {
          SetCursor(LoadCursorW(nullptr, cursor));
          return TRUE;
        }
      }
      break;
    case WM_KEYDOWN: {
      const bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
      const int saved_step = live_view.step;
      if (shift)
        live_view.step = 0;
      switch (w) {
        case VK_ESCAPE:
          DestroyWindow(hwnd);
          return 0;
        case VK_TAB: {
          const auto* profile = profiles::find(draft().profile);
          const unsigned feeds = profile && profile->composition.split_bottom != 0 ? 3 : 2;
          live_view.feed = (live_view.feed + 1) % feeds;
          InvalidateRect(hwnd, nullptr, FALSE);
          break;
        }
        case VK_LEFT:
          live_adjust_field(control ? 4 : 0, -1);
          break;
        case VK_RIGHT:
          live_adjust_field(control ? 4 : 0, 1);
          break;
        case VK_UP:
          live_adjust_field(control ? 3 : 2, 1);
          break;
        case VK_DOWN:
          live_adjust_field(control ? 3 : 2, -1);
          break;
        case VK_PRIOR:
          live_adjust_field(1, 1);
          break;
        case VK_NEXT:
          live_adjust_field(1, -1);
          break;
        case VK_ADD:
        case VK_OEM_PLUS:
          live_adjust_field(5, -1);
          break;
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
          live_adjust_field(5, 1);
          break;
        default:
          break;
      }
      live_view.step = saved_step;
      return 0;
    }
    case WM_DESTROY: {
      KillTimer(hwnd, 1);
      release_live_image();
      live_view.window = nullptr;
      live_view.serial = 0;
      // The camera control from before the window turned a camera on comes
      // back, unless the cameras were changed meanwhile. First, so closing
      // saves the user's control, not the window's.
      if (live_view.camera_taken) {
        auto s = draft();
        if (s.manual_mask == live_view.manual_set) {
          s.follow_taxi = live_view.follow_before;
          s.manual_mask = live_view.manual_before;
          if (win::valid_settings(s)) {
            publish(s);
            refresh_side_toggles(s);
          }
        }
        live_view.camera_taken = false;
      }
      // Closing saves what was adjusted, like Save changes.
      if (live_view.changed)
        apply();
      live_view.changed = false;
      return 0;
    }
  }
  return DefWindowProcW(hwnd, message, w, l);
}
void open_live_view() {
  if (live_view.window) {
    ShowWindow(live_view.window, SW_SHOWNORMAL);
    SetForegroundWindow(live_view.window);
    return;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpfnWndProc = live_view_procedure;
    type.lpszClassName = LiveViewClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassExW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  release_live_image();
  live_view = {};
  // Undo returns to the saved settings, not to unsaved edits.
  {
    const auto current = draft();
    win::Settings saved;
    const bool set_up = current.profile == profiles::DiscoveredProfileId;
    live_view.saved =
        win::load_settings(saved, installation, current.profile, set_up ? std::string_view(current.discovered.key) : std::string_view{})
            ? saved
            : current;
  }
  live_view.window = registered ? CreateWindowExW(0, LiveViewClass, L"Taxi Cam — Camera adjustment", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                                  CW_USEDEFAULT, CW_USEDEFAULT, scale(1100), scale(760), window, nullptr, instance, nullptr)
                                : nullptr;
  if (!live_view.window) {
    notice = L"Could not open the camera adjustment window.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  SendMessageW(live_view.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
  SendMessageW(live_view.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
  // The first timer turns on the camera of the display shown (live_keep_camera).
  service_live_view();
}
void open_aircraft_setup() {
  auto s = draft();
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  // The selected built-in profile's own aircraft: change its displays.
  // Otherwise set up the aircraft loaded (or change a set-up aircraft's).
  setup.built_in = !unsupported_aircraft(sample) && s.profile != profiles::DiscoveredProfileId ? s.profile : 0;
  setup.key = {};  // Start again from the displays in use.
  // The bridge tracks every panel texture while the setup is open.
  s.panel_setup = 1;
  publish(s);
  page = SetupPage;
  build_controls();
}
// A built-in profile's display choices: saved with the profile, applied at
// once, the setup closed.
void save_built_in_displays(bool built_in_only) {
  const auto* base = profiles::base_profile(setup.built_in);
  if (!base)
    return;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  auto s = setup.trial ? setup.before : draft();
  setup.trial = false;
  setup.testing = 0;
  s.display_override = built_in_choices(*base, s.display_override);
  if (built_in_only) {
    for (auto& texture : s.display_override.textures)
      std::fill(std::begin(texture), std::end(texture), '\0');
    win::update_target_assignment(s.left_id, s.right_id, s.route_request, 0, 0);
  } else {
    route_picks(s, setup_items(sample));
  }
  s.panel_setup = 0;
  if (!win::valid_settings(s) || !win::save_settings(s)) {
    notice = L"Could not save these displays. Check access to your local settings folder.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  publish(s);
  dirty = false;
  page = 0;
  build_controls();
  notice = built_in_only ? L"The profile's own displays are back." : L"Saved. These displays are used for this aircraft from now on.";
  InvalidateRect(window, nullptr, FALSE);
}
// Learns the layout from the picks and the bridge's panel map, saves it for
// this variant and selects it, then opens Camera views with previews on so
// the cameras can be positioned from their starting mounts.
void save_aircraft_setup() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  sync_setup(sample);
  if (const auto blocker = setup_blocker(sample); !blocker.empty()) {
    notice = blocker;
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (setup.built_in) {
    save_built_in_displays(false);
    return;
  }
  std::array<const char*, MaxDisplaySides> names{};
  for (unsigned side = 0; side < setup.sides; ++side)
    names[side] = setup.names[side].c_str();
  profiles::DiscoveredLayout layout;
  switch (win::learn_layout(sample, names, setup.sides, layout)) {
    case win::LayoutLearning::learned:
      break;
    case win::LayoutLearning::unknown_panel:
      notice = L"Pick a panel for every display.";
      InvalidateRect(window, nullptr, FALSE);
      return;
    default:
      notice = L"These panels cannot be saved for this aircraft. Pick other panels.";
      InvalidateRect(window, nullptr, FALSE);
      return;
  }
  // A cockpit test is not the user's profile: keep the one from before it.
  if (setup.trial ? !win::save_settings(setup.before) : !apply())
    return;
  setup.trial = false;
  setup.testing = 0;
  const auto s = draft();
  win::Settings next;
  if (!win::load_settings(next, installation, profiles::DiscoveredProfileId, layout.key, &layout)) {
    notice = L"Could not prepare this aircraft's settings.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  next.enabled = s.enabled;
  if (!win::prepare_profile_selection(next, s, s.auto_profile != 0) || !win::save_settings(next)) {
    notice = L"Could not save this aircraft. Check access to your local settings folder.";
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  // Previews draw every camera on the picked displays while positioning.
  next.manual_mask = (1u << layout.sides) - 1;
  next.panel_setup = 0;
  next.left_id = s.left_id;
  next.right_id = s.right_id;
  next.lower_id = 0;
  next.route_request = s.route_request;
  route_picks(next, setup_items(sample));
  publish(next);
  dirty = false;
  setup.positioning = true;
  page = 1;
  build_controls();
  open_live_view();
  notice = L"Saved. Position each camera in the camera adjustment window, then choose Save.";
  InvalidateRect(window, nullptr, FALSE);
}
// Set-up aircraft (Overview): every saved variant, with Remove.
std::vector<std::string> setup_list;
void fill_setup_list(HWND hwnd) {
  setup_list = win::saved_discovered_variants();
  HWND list = GetDlgItem(hwnd, 801);
  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  const auto s = draft();
  constexpr std::string_view prefix = "SimObjects/";
  for (const auto& key : setup_list) {
    auto family = profiles::variant_family(key);
    if (family.substr(0, prefix.size()) == prefix)
      family.remove_prefix(prefix.size());
    auto label = widen(std::string(profiles::variant_name(key)).c_str()) + L"   ·   " + widen(std::string(family).c_str());
    if (s.profile == profiles::DiscoveredProfileId && profiles::same_variant(key, s.discovered.key))
      label += L"   (in use)";
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
  EnableWindow(GetDlgItem(hwnd, 802), !setup_list.empty());
  SetDlgItemTextW(hwnd, 803, setup_list.empty() ? L"No aircraft has been set up yet." : L"");
  // The aircraft loaded now, which Copy to this aircraft sets up.
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto loaded = loaded_variant(sample);
  const auto line = loaded[0] ? L"Loaded now: " + widen(std::string(profiles::variant_name(loaded.data())).c_str())
                              : std::wstring(L"Load an aircraft to copy a setup to it.");
  SetDlgItemTextW(hwnd, 805, line.c_str());
  EnableWindow(GetDlgItem(hwnd, 804), !setup_list.empty() && loaded[0]);
}
void remove_setup_variant(HWND hwnd) {
  const auto index = SendDlgItemMessageW(hwnd, 801, LB_GETCURSEL, 0, 0);
  if (index < 0 || static_cast<std::size_t>(index) >= setup_list.size()) {
    SetDlgItemTextW(hwnd, 803, L"Select an aircraft to remove.");
    return;
  }
  const auto key = setup_list[static_cast<std::size_t>(index)];
  const auto name = widen(std::string(profiles::variant_name(key)).c_str());
  const auto question = L"Remove " + name + L"? Its displays and camera positions are deleted. You can set it up again.";
  if (MessageBoxW(hwnd, question.c_str(), L"Remove set-up aircraft", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
    return;
  const auto s = draft();
  const bool in_use = s.profile == profiles::DiscoveredProfileId && profiles::same_variant(key, s.discovered.key);
  if (!win::remove_discovered_variant(key)) {
    SetDlgItemTextW(hwnd, 803, L"Could not remove it. Check access to your local settings folder.");
    return;
  }
  saved_variant = {};  // Automatic selection must not find it in its cache.
  // The aircraft in use is no longer set up: the default profile takes over
  // until it is set up again.
  win::Settings next;
  if (in_use && win::load_settings(next, installation, profiles::A380.id)) {
    next.enabled = s.enabled;
    if (win::prepare_profile_selection(next, s, s.auto_profile != 0) && win::save_settings(next))
      publish(next);
  }
  fill_setup_list(hwnd);
  SetDlgItemTextW(hwnd, 803, (name + L" removed.").c_str());
}
// Copies the selected set-up aircraft to the aircraft loaded now (another
// preset of it, say), saves it and selects it, so it does not start again.
void copy_setup_variant(HWND hwnd) {
  const auto index = SendDlgItemMessageW(hwnd, 801, LB_GETCURSEL, 0, 0);
  if (index < 0 || static_cast<std::size_t>(index) >= setup_list.size()) {
    SetDlgItemTextW(hwnd, 803, L"Select the aircraft to copy from.");
    return;
  }
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto source = setup_list[static_cast<std::size_t>(index)];
  const auto target = loaded_variant(sample);
  if (!target[0]) {
    SetDlgItemTextW(hwnd, 803, L"Load the aircraft to copy to first.");
    return;
  }
  if (profiles::same_variant(target.data(), source)) {
    SetDlgItemTextW(hwnd, 803, L"That is the aircraft loaded now. Select another one to copy from.");
    return;
  }
  const auto from = widen(std::string(profiles::variant_name(source)).c_str());
  const auto to = widen(std::string(profiles::variant_name(target.data())).c_str());
  const bool replaces =
      std::any_of(setup_list.begin(), setup_list.end(), [&](const auto& key) { return profiles::same_variant(target.data(), key); });
  const bool other = profiles::variant_family(source) != profiles::variant_family(target.data());
  auto question = L"Copy " + from + L"'s displays, cameras and guides to " + to + L"?";
  if (replaces)
    question += L" This replaces " + to + L"'s own setup.";
  if (other)
    question += L" It is a different aircraft, so its panel names may not match: check the displays with Test in cockpit.";
  if (MessageBoxW(hwnd, question.c_str(), L"Copy set-up aircraft", MB_OKCANCEL | (replaces || other ? MB_ICONWARNING : MB_ICONQUESTION)) !=
      IDOK)
    return;
  // Keep edits to the profile in use before it changes.
  if (!apply())
    return;
  const auto s = draft();
  win::Settings next;
  if (!win::copy_discovered_variant(source, target.data(), installation, next)) {
    SetDlgItemTextW(hwnd, 803, L"Could not copy it. Check access to your local settings folder.");
    return;
  }
  next.enabled = s.enabled;
  if (win::prepare_profile_selection(next, s, s.auto_profile != 0) && win::save_settings(next))
    publish(next);
  saved_variant = {};
  fill_setup_list(hwnd);
  SetDlgItemTextW(hwnd, 803, (to + L" now starts from " + from + L"'s setup and is selected.").c_str());
}
INT_PTR CALLBACK setup_list_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM) {
  if (message == WM_INITDIALOG) {
    place_dialog(hwnd, L"Taxi Cam — Set-up aircraft", 640, 460);
    dialog_control(hwnd, L"STATIC", L"Set-up aircraft", -1, 20, 17, 600, 28, 0, heading);
    dialog_control(hwnd, L"STATIC",
                   L"Aircraft you set up, one per variant. Copy to this aircraft starts the aircraft loaded now from the "
                   L"selected one; Remove deletes a variant's displays and camera positions.",
                   -1, 20, 50, 600, 44, 0, small);
    dialog_control(hwnd, L"LISTBOX", L"", 801, 20, 100, 600, 210, WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP);
    dialog_control(hwnd, L"STATIC", L"", 805, 20, 318, 600, 24);
    dialog_control(hwnd, L"STATIC", L"", 803, 20, 346, 600, 44, 0, small);
    dialog_control(hwnd, L"BUTTON", L"Copy to this aircraft", 804, 20, 406, 220, 34, WS_TABSTOP);
    dialog_control(hwnd, L"BUTTON", L"Remove", 802, 400, 406, 110, 34, WS_TABSTOP);
    const auto close = dialog_control(hwnd, L"BUTTON", L"Close", IDCANCEL, 520, 406, 100, 34, WS_TABSTOP | BS_DEFPUSHBUTTON);
    fill_setup_list(hwnd);
    SetFocus(close);
    return FALSE;
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORLISTBOX)
    return dialog_colors(w);
  if (message == WM_COMMAND && LOWORD(w) == 802) {
    remove_setup_variant(hwnd);
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == 804) {
    copy_setup_variant(hwnd);
    return TRUE;
  }
  if ((message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) || message == WM_CLOSE) {
    EndDialog(hwnd, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}
void show_setup_list() {
  const DialogTemplate layout;
  if (DialogBoxIndirectParamW(instance, &layout.dialog, window, setup_list_dialog, 0) == -1)
    notice = L"Could not open the set-up aircraft list.";
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
  // Pages by id (100 + page), listed top to bottom in the order they are used:
  // the displays are routed before the cameras are set.
  const wchar_t* names[]{L"Overview", L"Camera views", L"Display", L"PFD routing", L"Diagnostics", L"Reference guides"};
  constexpr int order[]{0, 3, 1, 2, 4, 5};
  for (int slot = 0; slot < 6; ++slot)
    navigation.push_back(button(names[order[slot]], 100 + order[slot], 20, 156 + slot * 49, 166, 40));
  const auto* nav_profile = profiles::find(s.profile);
  HWND guides_nav = GetDlgItem(window, 105);
  const bool guides_enabled = !nav_profile || nav_profile->reference_guides;
  EnableWindow(guides_nav, guides_enabled);
  InvalidateRect(guides_nav, nullptr, FALSE);
  const auto donate_button = button(L"Donate", 513, 24, 590, 110, 40);
  const auto report_button = button(L"Report a bug", 512, 24, 638, 40, 40);
  const auto version_link = button(L"v" TAXI_CAM_VERSION_WIDE, 514, 24, 692, 155, 22);
  SendMessageW(version_link, WM_SETFONT, reinterpret_cast<WPARAM>(version_font), TRUE);
  const HWND whats_new_link = button(changelog_fetcher.busy() ? L"Loading…" : L"What's new", 515, 76, 647, 110, 22);
  SendMessageW(whats_new_link, WM_SETFONT, reinterpret_cast<WPARAM>(version_font), TRUE);
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
    tip.uId = reinterpret_cast<UINT_PTR>(whats_new_link);
    tip.lpszText = const_cast<wchar_t*>(L"See what changed in this version and earlier ones");
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
  }
  button(L"Menu", 602, 930, 37, 80, 34);
  // The aircraft setup saves with its own button in Save changes' place:
  // Save changes would keep the settings but not the picks.
  if (page != SetupPage)
    button(L"Save changes", 500, 835, 686, 175, 42);
  button(L"Hide to tray", 501, page == SetupPage ? 515 : 650, 686, 165, 42);
  if (page == 0) {
    HWND combo = child(L"COMBOBOX", L"", 210, 262, 310, 425, 220, CBS_DROPDOWNLIST | WS_VSCROLL);
    for (const auto* profile : profiles::Catalog)
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile->name));
    for (size_t i = 0; i < profiles::Catalog.size(); ++i)
      if (profiles::Catalog[i]->id == s.profile)
        SendMessageW(combo, CB_SETCURSEL, i, 0);
    // The set-up aircraft in use follows the catalog.
    if (const auto* set_up = s.profile == profiles::DiscoveredProfileId ? profiles::find(s.profile) : nullptr) {
      const auto label = std::wstring(set_up->name) + L" (set up)";
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      SendMessageW(combo, CB_SETCURSEL, profiles::Catalog.size(), 0);
    }
    toggle(L"Auto aircraft", 230, s.auto_profile, 697, 306, 140);
    button(L"Set-up aircraft", 646, 847, 306, 145);
    toggle(L"Auto-connect", 240, auto_connect.load(std::memory_order_acquire), 427, 210, 155);
    button(win::connection_button_label(connection_requested.load(std::memory_order_acquire)), 241, 262, 210, 155);
    const auto* profile = profiles::find(s.profile);
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    toggle(pmdg_cam_control(s) ? L"CAM button" : L"TAXI buttons", 221, s.follow_taxi && !manual, 812, 392, 180);
    EnableWindow(GetDlgItem(window, 221), !manual);
    button(L"Shortcuts and buttons…", 645, 597, 392, 205);
    button(L"Refresh", 409, 882, 499, 110);
    ShowWindow(button(L"Set up this aircraft", SetupOpenId, 787, 138, 205, 36), SW_HIDE);
    update_setup_buttons();
  } else if (page == 1) {
    const auto* profile = profiles::find(s.profile);
    const int feed_count = profile && profile->composition.split_bottom != 0 ? 3 : 2;
    for (int i = 0; i < feed_count; ++i) {
      // One card per camera across the column (draw_page): fields three to a
      // row, step buttons two to a row.
      const int card = feed_count == 3 ? 246 : 376, x = 244 + i * (card + 14) + 18, inner = card - 36;
      const int field = (inner - 20) / 3, half = (inner - 10) / 2;
      for (int j = 0; j < 6; ++j)
        edit(s.mounts[i][j], 300 + i * 10 + j, x + (j % 3) * (field + 10), 222 + (j / 3) * 64, field);
      button(L"Lower 0.25 m", 330 + i * 10, x, 358, half);
      button(L"Raise 0.25 m", 331 + i * 10, x + half + 10, 358, half);
      button(L"Aft 1 m", 332 + i * 10, x, 404, half);
      button(L"Forward 1 m", 333 + i * 10, x + half + 10, 404, half);
    }
    button(L"Reset camera mounts", 359, 244, 472, 240);
    button(L"Adjust with live view…", 358, 494, 472, 250);
  } else if (page == 2) {
    pending_camera_mode = s.camera_mode < kCameraModeCount ? s.camera_mode : static_cast<unsigned>(kDefaultCameraMode);
    for (unsigned slot = 0; slot < kCameraModeCount; ++slot) {
      const auto mode = static_cast<unsigned>(CameraModeOrder[slot]);
      button(CameraModeNames[mode], CameraModeCardId + static_cast<int>(mode), 262 + static_cast<int>(slot) * 148, 196, 138, 102);
    }
    HWND target = child(TRACKBAR_CLASSW, L"", CameraTargetId, 420, 308, 462, 32, TBS_HORZ | TBS_NOTICKS);
    SendMessageW(target, TBM_SETRANGE, FALSE, MAKELPARAM(kMinimumCameraRate, kMaximumCameraRate));
    SendMessageW(target, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(target, TBM_SETPOS, TRUE, std::clamp(s.camera_rate, kMinimumCameraRate, kMaximumCameraRate));
    EnableWindow(target, pending_camera_mode == static_cast<unsigned>(CameraMode::custom));
    edit(s.day_brightness, 201, 872, 440, 120);
    edit(s.night_brightness, 202, 872, 536, 120);
    button(L"Ground-speed colour", 231, 244, 606, 235);
    const auto* display_profile = profiles::find(s.profile);
    EnableWindow(GetDlgItem(window, 231), !display_profile || display_profile->ground_speed);
  } else if (page == 3) {
    button(L"Clear choices", 406, 522, 133, 120);
    button(L"Swap left / right", 403, 652, 133, 165);
    EnableWindow(GetDlgItem(window, 403), !single_display_profile(s));
    toggle(L"Auto select", 223, s.auto_detect, 827, 133, 165);
    button(L"Refresh pictures", 402, 640, 336, 160);
    button(L"‹", 407, 872, 336, 56);
    button(L"›", 408, 936, 336, 56);
    const unsigned lists = routing_lists(s);
    for (unsigned slot = 0; slot < GalleryColumns; ++slot)
      for (unsigned list = 0; list < lists; ++list)
        ShowWindow(button(list_action(s, list), static_cast<int>(420 + slot * 2 + list),
                          268 + static_cast<int>(slot) * 185 + static_cast<int>(list) * 84, 556, lists == 1 ? 163 : 79, 28),
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
    toggle(L"Scene test", 229, s.scene_test, 262, 318, 200);
    toggle(L"First camera only", 228, s.single_camera, 472, 318, 200);
    edit(s.calibration_budget, 203, 872, 392, 120);
    button(L"Open log folder", 510, 244, 454, 210);
    button(L"Stop camera tests", 511, 464, 454, 210);
  } else if (page == 5) {
    const auto* guide_profile = profiles::find(s.profile);
    const bool draw_guides = !guide_profile || guide_profile->reference_guides;
    const std::array<float, 2> guides[]{s.nose_dot, s.tail_upper, s.tail_corner, s.tail_inner};
    const std::array<float, 2> right[]{s.tail_upper_right, s.tail_corner_right, s.tail_inner_right};
    // Nose X/Y, then each tail point's left X/Y and right X/Y.
    for (unsigned i = 0; i < 4; ++i) {
      const int y = i ? 316 + static_cast<int>(i - 1) * 64 : 190;
      edit(guides[i][0] * 100., 360 + static_cast<int>(i * 2), 452, y, 80);
      edit(guides[i][1] * 100., 361 + static_cast<int>(i * 2), 540, y, 80);
      EnableWindow(GetDlgItem(window, 360 + static_cast<int>(i * 2)), draw_guides);
      EnableWindow(GetDlgItem(window, 361 + static_cast<int>(i * 2)), draw_guides);
      if (!i)
        continue;
      edit(right[i - 1][0] * 100., 380 + static_cast<int>((i - 1) * 2), 632, y, 80);
      edit(right[i - 1][1] * 100., 381 + static_cast<int>((i - 1) * 2), 720, y, 80);
      EnableWindow(GetDlgItem(window, 380 + static_cast<int>((i - 1) * 2)), draw_guides);
      EnableWindow(GetDlgItem(window, 381 + static_cast<int>((i - 1) * 2)), draw_guides);
    }
    button(L"Apply live", 370, 244, 586, 200);
    button(L"Reset guides", 371, 454, 586, 200);
    button(L"Marking colour", 372, 664, 586, 200);
    EnableWindow(GetDlgItem(window, 370), draw_guides);
    EnableWindow(GetDlgItem(window, 371), draw_guides);
    EnableWindow(GetDlgItem(window, 372), draw_guides);
  } else if (page == SetupPage) {
    button(L"Refresh pictures", SetupRefreshId, 640, 342, 160);
    button(L"‹", SetupPreviousId, 872, 342, 56);
    button(L"›", SetupNextId, 936, 342, 56);
    const int width = setup.sides == 3 ? 51 : 79;
    for (unsigned slot = 0; slot < GalleryColumns; ++slot)
      for (unsigned side = 0; side < MaxDisplaySides; ++side)
        ShowWindow(button(setup_side_action(side), static_cast<int>(SetupCardId + slot * 3 + side),
                          268 + static_cast<int>(slot) * 185 + static_cast<int>(side) * (width + 5), 562, width, 28),
                   SW_HIDE);
    if (setup.built_in)
      button(L"Use built-in displays", SetupBuiltInId, 244, 628, 240);
    else
      toggle(L"Lower display", SetupSidesId, setup.sides == 3, 244, 628, 210);
    const int slot_width = (730 - 10 * (static_cast<int>(setup.sides) - 1)) / static_cast<int>(setup.sides);
    for (unsigned side = 0; side < setup.sides; ++side)
      toggle(setup.sides == 3 ? L"Test" : L"Test in cockpit", SetupTestId + static_cast<int>(side), (setup.testing >> side) & 1,
             362 + static_cast<int>(side) * (slot_width + 10), 266, setup.sides == 3 ? 120 : 200, 28);
    button(setup.built_in ? L"Save displays" : L"Save and position cameras", SetupSaveId, 700, 686, 310, 42);
    update_setup_buttons();
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
// After an update, the first time Settings is on screen opens What's new by
// itself (a start in the tray waits until the window is opened).
bool whats_new_offered{};
void offer_whats_new() {
  if (!whats_new || whats_new_offered || preview_ui || !IsWindowVisible(window))
    return;
  whats_new_offered = true;
  request_whats_new();
}
void show() {
  ShowWindow(window, SW_SHOW);
  ShowWindow(window, SW_RESTORE);
  SetForegroundWindow(window);
  offer_whats_new();
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
  const wchar_t* titles[]{L"Taxi camera", L"Camera views",     L"Display",        L"PFD routing",
                          L"Diagnostics", L"Reference guides", L"Set up aircraft"};
  const wchar_t* subtitles[]{L"Your taxi cameras, controlled from the flight deck.",
                             setup.positioning ? L"Step 2 of 2: the cameras start from default positions. Move each one, then save."
                                               : L"Fine-tune each camera independently. Changes stay with this aircraft.",
                             L"Balance visibility, colour and camera update rate.",
                             L"Connect each TAXI button to the correct display.",
                             L"Live status and the controls used during camera testing.",
                             L"Move the guide points, preview them live, then save for this aircraft.",
                             setup.built_in ? L"Pick the cockpit display each camera is drawn on."
                                            : L"Step 1 of 2: pick the cockpit display each camera is drawn on."};
  text(dc, titles[page], 244, 30, 740, 48, title_font);
  text(dc, subtitles[page], 247, 84, 758, 30, normal, setup.positioning && page == 1 ? Accent : Muted);
  win::Status sample;
  std::wstring live;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
    live = connection;
  }
  const bool has_displays = sample.candidate_count != 0 || sample.left_id != 0 || sample.right_id != 0 || sample.lower_id != 0;
  // Every page's cards share one grid: the content column 244-1010, cards
  // from y 124 with 14 between them and 18 inside (content 262-992), buttons
  // 36 high and right-aligned to 992; rows below the cards start at 244.
  if (page == 0) {
    panel(dc, 244, 124, 766, 140);
    const bool unsupported = unsupported_aircraft(sample);
    text(dc,
         connection_disconnected.load(std::memory_order_acquire) ? L"Disconnected"
         : !sample.graphics_ready                                ? L"Waiting for the simulator"
         : unsupported                                           ? L"This aircraft is not set up yet"
         : has_displays                                          ? L"Native bridge connected"
                                                                 : L"Native bridge connected — waiting for cockpit displays",
         262, 140, 510, 30, heading, unsupported || (sample.graphics_ready && has_displays) ? Accent : Text);
    const bool late_empty_pfds = sample.graphics_ready && !has_displays && !unsupported;
    const auto line = late_empty_pfds    ? std::wstring(
                                               L"Waiting for cockpit displays to be drawn. "
                                               L"Restart Flight only if the list stays empty.")
                      : sample.heartbeat ? widen(sample.message)
                                         : live;
    text(dc, line.c_str(), 262, 172, 730, 36, normal, late_empty_pfds ? Accent : Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 278, 766, 90);
    text(dc, L"Aircraft profile", 262, 286, 350, 22, small, Muted);
    panel(dc, 244, 382, 766, 96);
    text(dc, L"Flight-deck control", 262, 395, 300, 30, heading);
    const auto* profile = profiles::find(draft().profile);
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    const bool cam = profile && profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam;
    text(dc,
         manual ? L"Use Shortcuts and buttons or PFD routing previews for this aircraft."
         : cam  ? L"Select L INBD, R INBD or LWR CTR, then press CAM. Press CAM again with that display selected to turn it off."
                : L"Left and right EFIS TAXI buttons activate their own PFD.",
         262, 436, 730, 36, small, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 492, 766, 180);
    text(dc, L"Cockpit displays", 262, 502, 300, 30, heading);
    text(dc, L"Where each camera is drawn. Select one to change it.", 436, 502, 436, 30, small, Muted);
    draw_display_strip(dc, sample);
  } else if (page == 1) {
    constexpr const wchar_t* labels[]{L"Right (m)", L"Up (m)", L"Forward (m)", L"Pitch (deg)", L"Yaw (deg)", L"Lens (rad)"};
    const auto* profile = profiles::find(draft().profile);
    const bool split = profile && profile->composition.split_bottom != 0;
    const int feed_count = split ? 3 : 2;
    const wchar_t* titles[]{L"Nose-wheel camera", split ? L"Left wing camera" : L"Tail camera", L"Right wing camera"};
    const wchar_t* views[]{L"UPPER VIEW", split ? L"LOWER LEFT" : L"LOWER VIEW", L"LOWER RIGHT"};
    for (int i = 0; i < feed_count; ++i) {
      const int card = split ? 246 : 376, x = 244 + i * (card + 14), inner = card - 36, field = (inner - 20) / 3;
      panel(dc, x, 124, card, 334);
      text(dc, titles[i], x + 18, 140, inner, 30, heading);
      const auto& dimensions = (profile ? *profile : profiles::A380).camera_panes[i];
      wchar_t view_label[96];
      std::swprintf(view_label, 96, L"%ls · %d × %d", views[i], dimensions[0], dimensions[1]);
      text(dc, view_label, x + 18, 172, inner, 20, small, Muted);
      for (int j = 0; j < 6; ++j)
        text(dc, labels[j], x + 18 + (j % 3) * (field + 10), 200 + (j / 3) * 64, field, 20, small, Muted);
      text(dc, L"Position relative to the aircraft datum", x + 18, 328, inner, 20, small, Muted);
    }
  } else if (page == 2) {
    // The camera images take the main view's lighting; brightness is the
    // user's offset on top of it.
    const bool custom = selected_camera_mode() == static_cast<unsigned>(CameraMode::custom);
    panel(dc, 244, 124, 766, 276);
    text(dc, L"Camera frame rate", 262, 136, 400, 29, heading);
    text(dc, L"Frames per second for each taxi camera, parked or moving. Auto keeps the simulator within 10% of its frame rate.", 262, 166,
         730, 22, small, Muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, L"Custom target", 262, 312, 150, 24, normal, custom ? Text : Muted);
    wchar_t target_label[32];
    std::swprintf(target_label, 32, L"%u fps", selected_camera_target());
    text(dc, target_label, 892, 312, 100, 24, normal, custom ? Text : Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
      text(dc, line.c_str(), 262, 350, 730, 36, small, Muted, DT_LEFT | DT_WORDBREAK);
    }
    // The camera images take the main view's lighting; brightness is the
    // user's offset on top of it.
    const int ys[]{414, 510};
    const wchar_t* names[]{L"Daytime brightness", L"Night brightness"};
    const wchar_t* descriptions[]{L"EV added to the main view's exposure in daylight, −4 to +2 in 0.25 steps. 0 matches the main view.",
                                  L"EV added at night, −4 to +2 in 0.25 steps. Dusk blends the two with the ambient light."};
    for (int i = 0; i < 2; ++i) {
      panel(dc, 244, ys[i], 766, 82);
      text(dc, names[i], 262, ys[i] + 12, 515, 29, heading);
      text(dc, descriptions[i], 262, ys[i] + 42, 580, 34, small, Muted, DT_LEFT | DT_WORDBREAK);
    }
  } else if (page == 3) {
    panel(dc, 244, 124, 766, 190);
    text(dc, L"Assigned displays", 262, 134, 255, 34, heading);
    draw_routing_slots(dc, sample);
    panel(dc, 244, 328, 766, 280);
    text(dc, L"Display textures", 262, 338, 180, 32, heading);
    text(dc, L"Select a picture to enlarge it.", 446, 338, 190, 32, small, Muted);
    draw_gallery(dc, sample);
    const auto s = draft();
    const auto* profile = profiles::find(s.profile);
    const unsigned sides = profile && profile->sides > 2 ? 3 : 2;
    text(dc, L"Preview", 244, 622, 60, 36, small, Muted);
    text(dc, L"Calibrate", side_toggle_x(1, sides) - 76, 622, 72, 36, small, Muted);
    text(dc,
         pmdg_cam_control(s) ? L"Previews add displays on top of the CAM button. Calibration bars identify each screen and turn CAM off."
         : profile && profile->taxi_control == profiles::TaxiControl::manual_only
             ? L"Previews are this aircraft's camera control. Calibration bars identify each screen."
             : L"Previews and calibration turn off TAXI-button control. Calibration bars identify each screen.",
         244, 662, 766, 20, small, Muted);
  } else if (page == 4) {
    panel(dc, 244, 124, 766, 166);
    wchar_t data[1024];
    std::swprintf(data, 1024,
                  L"Bridge                 %s\nCamera pair         %s\nLeft / right PFD    %llu / %llu\nCaptured frames  "
                  L"%llu\nCompositions       %llu\nPFD writes            %llu\nHook failures        %llu",
                  sample.graphics_ready ? (has_displays ? L"Connected" : L"Waiting for displays") : L"Waiting",
                  sample.scene_ready ? L"Ready" : L"Waiting", static_cast<unsigned long long>(sample.left_id),
                  static_cast<unsigned long long>(sample.right_id), static_cast<unsigned long long>(sample.captures),
                  static_cast<unsigned long long>(sample.composed), static_cast<unsigned long long>(sample.stamps),
                  static_cast<unsigned long long>(sample.hook_failures));
    text(dc, data, 262, 140, 360, 136, normal, Text, DT_LEFT | DT_WORDBREAK);
    std::swprintf(data, 1024,
                  L"Probe CPU: %.2f ms (max %.2f)\nManager %.3f   Pool %.3f\nLifecycle %.3f   Entries %.3f\nView 1 %.3f   View 2 "
                  L"%.3f\nHandoff %.3f   Pose %.3f\nActivation %.3f   Publish %.3f\n\nExcludes engine rendering and GPU time.",
                  sample.probe_cpu_ms, sample.probe_max_ms, sample.stage_ms[0], sample.stage_ms[1], sample.stage_ms[2], sample.stage_ms[3],
                  sample.stage_ms[4], sample.stage_ms[5], sample.stage_ms[6], sample.stage_ms[7], sample.stage_ms[8], sample.stage_ms[9]);
    text(dc, data, 637, 140, 355, 136, small, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 304, 766, 136);
    text(dc, L"Scene test renders without PFD delivery.", 262, 362, 730, 22, small, Muted, DT_LEFT | DT_WORDBREAK);
    const auto line = sample.heartbeat ? widen(sample.message) : live;
    text(dc, L"Calibration batches per 50 ms (64–16384)", 262, 395, 550, 24, small, Muted);
    text(dc, line.c_str(), 244, 504, 766, 38, small, Muted, DT_LEFT | DT_WORDBREAK);
  } else if (page == SetupPage) {
    draw_setup(dc, sample);
  } else if (page == 5) {
    panel(dc, 244, 124, 766, 118);
    panel(dc, 244, 256, 766, 259);
    text(dc, L"Nose-wheel view", 262, 136, 250, 30, heading);
    const auto* guide_profile = profiles::find(draft().profile);
    const bool draw_guides = !guide_profile || guide_profile->reference_guides;
    const bool nose_squares = (guide_profile ? guide_profile : &profiles::A380)->composition.square_nose_markers != 0;
    text(dc, nose_squares ? L"Nose squares" : L"Nose dot", 262, 191, 225, 28, normal);
    text(dc, L"Tail view", 262, 267, 250, 30, heading);
    const wchar_t* labels[]{L"Upper endpoint", L"Outside corner", L"Inner endpoint"};
    for (int i = 0; i < 3; ++i)
      text(dc, labels[i], 262, 316 + i * 64, 185, 30, normal);
    text(dc, L"X (%)", 452, 162, 80, 23, small, Muted);
    text(dc, L"Y (%)", 540, 162, 80, 23, small, Muted);
    text(dc, L"Left X (%)", 452, 289, 80, 23, small, Muted);
    text(dc, L"Left Y (%)", 540, 289, 80, 23, small, Muted);
    text(dc, L"Right X (%)", 632, 289, 80, 23, small, Muted);
    text(dc, L"Right Y (%)", 720, 289, 80, 23, small, Muted);
    text(dc,
         draw_guides ? L"X from the nearest side, 0–50%; Y from the top, 0–100% of each camera view. The nose guide mirrors."
                     : L"This aircraft does not use alignment markers.",
         244, 529, 766, 26, small, Muted);
    text(dc,
         draw_guides ? L"Preview is temporary until saved. Reset restores guide positions and colour."
                     : L"No marker calibration is required for this profile.",
         244, 554, 766, 23, small, Muted);
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
      text(dc, L"Preview", 811, 141, 181, 23, small, Muted);
      for (const bool right : {false, true}) {
        if (nose_squares) {
          const auto nose = point(preview.nose_dot, right, 177, 45);
          const RECT marker{nose.x - scale(7), nose.y - scale(7), nose.x + scale(7), nose.y + scale(7)};
          FillRect(dc, &marker, brush);
        } else {
          dot(point(preview.nose_dot, right, 177, 45), 6);
        }
        const auto a = point(right ? preview.tail_upper_right : preview.tail_upper, right, 312, 158),
                   b = point(right ? preview.tail_corner_right : preview.tail_corner, right, 312, 158),
                   c = point(right ? preview.tail_inner_right : preview.tail_inner, right, 312, 158);
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
  text(dc, notice.c_str(), 244, 687, page == SetupPage ? 261 : 396, 43, small, dirty ? Accent : Muted, DT_LEFT | DT_WORDBREAK);
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
      const bool selected =
          !disabled &&
          ((id >= 100 && id < 106 && id - 100 == page) || is_on(id, draft()) || (id >= 420 && id < 428 && gallery_button_routed(id)) ||
           (id >= SetupCardId && id < SetupCardId + 12 && setup_button_picked(id)) || (id == SetupSidesId && setup.sides == 3) ||
           (id >= SetupTestId && id < SetupTestId + 3 && ((setup.testing >> (id - SetupTestId)) & 1)));
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
      const bool primary = id == 500 || id == SetupSaveId;
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
      update_setup_buttons();
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
      if (id == SetupOpenId) {
        open_aircraft_setup();
        return 0;
      }
      if (id == 358) {
        open_live_view();
        return 0;
      }
      if (id == 646) {
        show_setup_list();
        return 0;
      }
      if (id >= SetupCardId && id < SetupCardId + 12) {
        pick_setup_panel(static_cast<unsigned>(id - SetupCardId) / 3, static_cast<unsigned>(id - SetupCardId) % 3);
        update_setup_buttons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id == SetupPreviousId || id == SetupNextId) {
        if (id == SetupPreviousId && setup.gallery_page > 0)
          --setup.gallery_page;
        if (id == SetupNextId)
          ++setup.gallery_page;
        update_setup_buttons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id == SetupSidesId) {
        setup.sides = setup.sides == 3 ? 2 : 3;
        if (setup.sides == 2)
          setup.names[2].clear();
        setup.testing &= (1u << setup.sides) - 1;
        if (setup.testing)
          apply_setup_trial();
        build_controls();
        return 0;
      }
      if (id >= SetupTestId && id < SetupTestId + 3) {
        setup.testing ^= 1u << (id - SetupTestId);
        apply_setup_trial();
        build_controls();
        return 0;
      }
      if (id == SetupRefreshId) {
        thumbnails.fresh_after = GetTickCount64();
        service_thumbnails();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (id == SetupSaveId) {
        save_aircraft_setup();
        return 0;
      }
      if (id == SetupBuiltInId) {
        save_built_in_displays(true);
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
          notice = L"Your choices are cleared. Named and detected displays apply again.";
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
        // Leaving the setup closes the bridge's panel tracking for it and ends
        // an unsaved cockpit test.
        if (page == SetupPage)
          end_setup_trial(s);
        s.panel_setup = 0;
        setup.positioning = setup.positioning && page == 1 && id == 101;
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
        const auto selected = draft();
        const bool set_up = static_cast<size_t>(index) == profiles::Catalog.size() && selected.profile == profiles::DiscoveredProfileId;
        if (index < 0 || (static_cast<size_t>(index) >= profiles::Catalog.size() && !set_up))
          return 0;
        if (!apply())
          return 0;
        win::Settings next;
        if (!win::load_settings(next, installation, set_up ? profiles::DiscoveredProfileId : profiles::Catalog[index]->id,
                                set_up ? std::string_view(selected.discovered.key) : std::string_view{})) {
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
        // On the setup page, saving means saving the aircraft setup.
        if (page == SetupPage)
          save_aircraft_setup();
        else
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
        if (const auto* profile = profiles::find(s.profile))
          s.mounts = profile->mounts;
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
          notice = L"Left and right swapped for this flight. Clear choices goes back to automatic selection.";
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
  if (current.profile == profiles::DiscoveredProfileId)
    profiles::publish_discovered(current.discovered);
  profiles::publish_override(profiles::base_profile(current.profile), current.display_override);
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
    offer_whats_new();
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
