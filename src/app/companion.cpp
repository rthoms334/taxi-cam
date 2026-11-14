#include <windows.h>
#include <objidl.h>
#include <algorithm>
// GDI+ draws the antialiased shapes; its headers use the min/max macros.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <commctrl.h>
#include <commdlg.h>
#include "../camera/aircraft_identity.hpp"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cwchar>
#include <functional>
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
#include "../shared/status_messages.hpp"
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
#include "localization.hpp"
#include "legacy_display_names.hpp"
#include "../setup/marks_package.hpp"
#include "area_edit.hpp"

namespace {
using namespace taxi_camera;
namespace win = standalone;
using win::decimal, win::tr, win::tr_format;
constexpr UINT TrayMessage = WM_APP + 1, StatusMessage = WM_APP + 2, WhatsNewMessage = WM_APP + 3, WhatsNewImageMessage = WM_APP + 4,
               ExitWithSimulatorMessage = WM_APP + 5, AreaPickerMessage = WM_APP + 6, LvarMessage = WM_APP + 7;
constexpr win::ChangelogVersion InstalledVersion{TAXI_CAM_VERSION_MAJOR, TAXI_CAM_VERSION_MINOR, TAXI_CAM_VERSION_PATCH};
constexpr wchar_t WindowClass[] = L"380TaxiCamera.Settings";
constexpr wchar_t DonationUrl[] = L"https://www.paypal.com/donate/?hosted_button_id=EPVELD44P6NXW";
constexpr wchar_t GithubUrl[] = L"https://github.com/rthoms334/taxi-cam";
// The palette: three stacked surfaces (picture wells, the page, its cards),
// the raised fill of a control, hairlines, and the text, state and accent
// colours. The accent is the taxiway sign's yellow, with dark text on it.
constexpr COLORREF Background = RGB(18, 20, 23), Well = RGB(11, 12, 14), Card = RGB(26, 29, 34), Raised = RGB(43, 48, 56),
                   RaisedHot = RGB(54, 60, 70), Border = RGB(35, 39, 46), BorderStrong = RGB(62, 69, 80), Text = RGB(244, 245, 247),
                   Muted = RGB(169, 177, 191), Faint = RGB(111, 120, 133), Accent = RGB(242, 194, 48), AccentHot = RGB(247, 212, 104),
                   AccentTint = RGB(54, 46, 21), Warning = RGB(255, 138, 91), Blue = RGB(126, 176, 255), Ok = RGB(91, 214, 160),
                   NoColor = CLR_INVALID;
// The settings window's client area and its bands, in logical pixels.
constexpr int ClientWidth = 1000, ClientHeight = 680, HeaderHeight = 56, PageLeft = 24, PageWidth = 952;
HINSTANCE instance{};
HWND window{}, sidebar_tooltip{}, shortcut_window{};
HFONT normal{}, small{}, title_font{}, heading{}, version_font{}, mono{};
HBRUSH background_brush{}, card_brush{};
HICON icon{};
UINT dpi = 96, taskbar_created{};
int page = 0;
std::vector<HWND> controls;
std::vector<HWND> navigation;
native_camera::AutoProfileSelection profile_selection;
std::wstring installation, expected_simulator, notice;
std::wstring language_preference;  // Empty: follow the Windows display language.
std::mutex app_mutex;
win::Settings current;
win::Status status;
bool received_bridge_status{};  // Guarded by app_mutex; retained across simulator sessions.
// Guarded by app_mutex. The connection worker takes unseen bridge events from
// every status sample; the UI thread shows them (Shell_NotifyIcon from the
// window thread only) and clears the queue.
NotificationReader notification_reader;
std::vector<SimEvent> pending_notifications;
// Guarded by app_mutex. English, so the connection worker can set it; shown with text().
win::UiMessage connection{ui_text(L"Waiting for Microsoft Flight Simulator 2024")};
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
int button_capture = -1;                             // Action waiting for a controller button in the editor.
std::vector<win::CameraButton> button_capture_down;  // Went down while waiting; the first to come up is set.
win::Updater updater;
ULONGLONG next_update_check{};
bool update_prompt{};
// An update downloaded while the simulator blocked installing it.
bool update_after_simulator{};
// The pilot chose to keep the app open; it no longer quits with the simulator.
bool stay_after_simulator{};
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
// Antialiased shapes, in device pixels. GDI+ starts on first use and stays
// for the life of the process.
bool shapes_ready() {
  static const bool ready = [] {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token{};
    return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }();
  return ready;
}
Gdiplus::Color shape_color(COLORREF color, BYTE alpha = 255) {
  return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}
COLORREF blend(COLORREF from, COLORREF to, int percent) {
  const auto mix = [&](int a, int b) { return static_cast<BYTE>(a + (b - a) * percent / 100); };
  return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)), mix(GetBValue(from), GetBValue(to)));
}
void round_path(Gdiplus::GraphicsPath& path, float left, float top, float right, float bottom, float radius) {
  const float d = std::max(0.f, std::min({radius * 2, right - left, bottom - top}));
  if (d <= 0) {
    path.AddRectangle(Gdiplus::RectF(left, top, right - left, bottom - top));
    return;
  }
  path.AddArc(left, top, d, d, 180, 90);
  path.AddArc(right - d, top, d, d, 270, 90);
  path.AddArc(right - d, bottom - d, d, d, 0, 90);
  path.AddArc(left, bottom - d, d, d, 90, 90);
  path.CloseFigure();
}
// A rounded rectangle: its fill, then an outline of `width` pixels inside
// its bounds. NoColor leaves either out.
void round_box(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF outline = NoColor, int width = 1) {
  if (!shapes_ready() || r.right <= r.left || r.bottom <= r.top)
    return;
  Gdiplus::Graphics graphics(dc);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  graphics.TranslateTransform(-0.5f, -0.5f);  // GDI+ puts pixel centres on whole coordinates.
  const float left = static_cast<float>(r.left), top = static_cast<float>(r.top), right = static_cast<float>(r.right),
              bottom = static_cast<float>(r.bottom);
  if (fill != NoColor) {
    Gdiplus::GraphicsPath path;
    round_path(path, left, top, right, bottom, static_cast<float>(radius));
    Gdiplus::SolidBrush brush(shape_color(fill));
    graphics.FillPath(&brush, &path);
  }
  if (outline != NoColor && width > 0) {
    const float half = width / 2.f;
    Gdiplus::GraphicsPath path;
    round_path(path, left + half, top + half, right - half, bottom - half, std::max(0.f, radius - half));
    Gdiplus::Pen pen(shape_color(outline), static_cast<float>(width));
    graphics.DrawPath(&pen, &path);
  }
}
void disc(HDC dc, int cx, int cy, int radius, COLORREF fill, COLORREF outline = NoColor, int width = 1) {
  round_box(dc, {cx - radius, cy - radius, cx + radius, cy + radius}, radius, fill, outline, width);
}
// Round-capped line strokes for the drawn icons: {x1, y1, x2, y2} each.
void strokes(HDC dc, std::initializer_list<std::array<float, 4>> lines, float width, COLORREF color) {
  if (!shapes_ready())
    return;
  Gdiplus::Graphics graphics(dc);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  graphics.TranslateTransform(-0.5f, -0.5f);  // GDI+ puts pixel centres on whole coordinates.
  Gdiplus::Pen pen(shape_color(color), width);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  for (const auto& line : lines)
    graphics.DrawLine(&pen, line[0], line[1], line[2], line[3]);
}
// The cards painted on the settings page this frame, so a control knows the
// surface it sits on (surface_under).
struct Surface {
  RECT bounds{};
  COLORREF color{};
};
std::vector<Surface> surfaces;
void panel(HDC dc, int x, int y, int w, int h, COLORREF color = Card) {
  const auto r = rectangle(x, y, w, h);
  surfaces.push_back({r, color});
  // A card has a hairline; a well set into a card has a fainter edge of its own colour.
  round_box(dc, r, scale(color == Card ? 12 : 10), color, color == Card ? Border : blend(color, Text, 6));
}
// The colour behind a child control of the settings window.
COLORREF surface_under(HWND control) {
  RECT r{};
  GetWindowRect(control, &r);
  MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
  const POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  for (auto surface = surfaces.rbegin(); surface != surfaces.rend(); ++surface)
    if (PtInRect(&surface->bounds, centre))
      return surface->color;
  return Background;
}
// A switch: its track and knob, `height` pixels tall and twice as wide less a little.
int switch_width(int height) {
  return height * 9 / 5;
}
void draw_switch(HDC dc, int x, int y, int height, bool on, bool disabled, COLORREF surface) {
  const int width = switch_width(height), knob = height / 2 - std::max(2, height / 7);
  const COLORREF track = disabled ? blend(surface, Text, 8) : on ? Accent : blend(surface, Text, 18);
  round_box(dc, {x, y, x + width, y + height}, height / 2, track);
  disc(dc, on ? x + width - height / 2 : x + height / 2, y + height / 2, knob, disabled ? Faint : on ? Well : Text);
}
// The one button look. A primary button carries the accent; a chosen one
// (the current choice of a set) is tinted; the rest are quiet raised fills
// that lift under the pointer.
struct ButtonLook {
  bool primary{}, chosen{}, hot{}, down{}, disabled{}, focus{};
};
void button_box(HDC dc, const RECT& r, const ButtonLook& look, COLORREF surface, int radius = 8) {
  const bool filled = look.primary || look.chosen;
  const COLORREF fill = look.disabled ? blend(surface, Well, 40)
                        : filled      ? (look.down  ? blend(Accent, Well, 18)
                                         : look.hot ? AccentHot
                                                    : Accent)
                        : look.down   ? blend(Raised, Well, 35)
                        : look.hot    ? RaisedHot
                                      : Raised;
  const COLORREF outline = look.disabled ? blend(surface, Text, 5)
                           : filled      ? fill
                           : look.focus  ? Accent
                           : look.hot    ? BorderStrong
                                         : blend(Raised, Text, 7);
  round_box(dc, r, scale(radius), fill, outline, scale(1));
}
COLORREF button_text(const ButtonLook& look) {
  return look.disabled ? Faint : look.primary || look.chosen ? Well : Text;
}
// The button under the pointer, so its painter can lift it.
HWND hot_button{};
LRESULT CALLBACK hover_tracking(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_MOUSEMOVE && hot_button != control) {
    const HWND before = hot_button;
    hot_button = control;
    if (before && IsWindow(before))
      InvalidateRect(before, nullptr, FALSE);
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
    TrackMouseEvent(&track);
    InvalidateRect(control, nullptr, FALSE);
  } else if (message == WM_MOUSELEAVE && hot_button == control) {
    hot_button = nullptr;
    InvalidateRect(control, nullptr, FALSE);
  } else if (message == WM_NCDESTROY) {
    if (hot_button == control)
      hot_button = nullptr;
    RemoveWindowSubclass(control, hover_tracking, id);
  }
  return DefSubclassProc(control, message, w, l);
}
void track_hover(HWND control, const wchar_t* type) {
  if (control && !lstrcmpiW(type, L"BUTTON"))
    SetWindowSubclass(control, hover_tracking, 7, 0);
}
// Advanced's two pages (texture routing and guide values) keep the layout
// they were drawn with, a 766-wide column from (244, 172), moved to the
// middle of the page under its subtitle: their controls and their drawing
// are shifted by this much.
constexpr int LegacyShiftX = (ClientWidth - 766) / 2 - 244, LegacyShiftY = 96 - 172;
int origin_x{}, origin_y{};
HWND child(const wchar_t* type, const wchar_t* label, int id, int x, int y, int w, int h, DWORD style = 0) {
  HWND value = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, scale(x + origin_x), scale(y + origin_y),
                               scale(w), scale(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
  SendMessageW(value, WM_SETFONT, reinterpret_cast<WPARAM>(normal), TRUE);
  SetWindowTheme(value, L"DarkMode_Explorer", nullptr);
  track_hover(value, type);
  controls.push_back(value);
  return value;
}
HWND button(const wchar_t* label, int id, int x, int y, int w = 130, int h = 36) {
  return child(L"BUTTON", label, id, x, y, w, h, BS_OWNERDRAW);
}
// A drop-down list keeps the height Windows gives it for its font, centred
// on the row of controls beside it: logical y and height.
void center_combo(HWND combo, int y, int height) {
  RECT box{};
  GetWindowRect(combo, &box);
  MapWindowPoints(nullptr, GetParent(combo), reinterpret_cast<POINT*>(&box), 2);
  SetWindowPos(combo, nullptr, box.left, scale(y) + (scale(height) - (box.bottom - box.top)) / 2, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
// The Displays swap button: a round button on the slot group's card, like the
// slots it sits between, with two opposed arrows (drawn, not a font glyph).
void draw_swap_button(const DRAWITEMSTRUCT& item) {
  const auto& r = item.rcItem;
  HDC dc = item.hDC;
  HBRUSH surround = CreateSolidBrush(surface_under(item.hwndItem));
  FillRect(dc, &r, surround);
  DeleteObject(surround);
  const bool down = (item.itemState & ODS_SELECTED) != 0, focus = (item.itemState & ODS_FOCUS) != 0, hot = hot_button == item.hwndItem;
  const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, size = r.right - r.left;
  disc(dc, cx, cy, size / 2, down ? AccentTint : hot ? RaisedHot : Raised, down || focus ? Accent : hot ? BorderStrong : Border, scale(1));
  const float arm = size * 0.26f, offset = size / 9.f, head = size / 9.f, pen = std::max(1.f, scale(3) / 2.f);
  // To the right above, to the left below.
  for (const int direction : {1, -1}) {
    const float y = cy - direction * offset, tip = cx + direction * arm;
    strokes(dc, {{cx - direction * arm, y, tip, y}, {tip, y, tip - direction * head, y - head}, {tip, y, tip - direction * head, y + head}},
            pen, Accent);
  }
}
void frame_field(HWND control);
void edit(double value, int id, int x, int y, int w = 110) {
  wchar_t buffer[64];
  std::swprintf(buffer, 64,
                (id >= 360 && id <= 367) || (id >= 380 && id <= 385) ? L"%.1f"
                : id == 201 || id == 202                             ? L"%+.2f"
                                                                     : L"%.10g",
                value);
  auto h = child(L"EDIT", buffer, id, x, y, w, 30, ES_AUTOHSCROLL | ES_LEFT);
  SendMessageW(h, EM_SETLIMITTEXT, 32, 0);
  frame_field(h);
}
// A toggle button's label: its translated name and state.
std::wstring switch_label(const wchar_t* name, bool on) {
  return on ? tr_format(L"{0}: On", {name}) : tr_format(L"{0}: Off", {name});
}
void make_fonts() {
  for (HFONT f : {normal, small, title_font, heading, version_font, mono})
    if (f)
      DeleteObject(f);
  auto make = [](int size, int weight, bool underline = false) {
    return CreateFontW(-scale(size), 0, 0, 0, weight, FALSE, underline, FALSE, win::active_language->charset, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, win::active_language->typeface);
  };
  normal = make(15, FW_NORMAL);
  small = make(13, FW_NORMAL);
  title_font = make(22, FW_SEMIBOLD);
  heading = make(18, FW_SEMIBOLD);
  version_font = make(13, FW_NORMAL, true);
  // Shortcuts and measurements, in a fixed-width face every Windows has.
  mono = CreateFontW(-scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
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
  profiles::publish_override(profiles::base_profile(value.profile), value.display_override, value.door_side);
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
  SetDlgItemTextW(window, 241, tr(win::connection_button_label(connection_requested.load(std::memory_order_acquire))));
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
// Side 2, Lower, is the lower ECAM on Airbus aircraft and the lower DU on the
// 777: the display a door camera usually shows on.
const wchar_t* side_toggle_name(const win::Settings& s, unsigned side) {
  (void)s;
  return side == 0 ? tr(L"Left") : side == 1 ? tr(L"Right") : tr(L"Lower");
}
// The door camera's six mount values (Door camera, Advanced).
constexpr int DoorFieldId = 480;
// Overview's camera toggles and Diagnostics' calibration bars, from the
// current request.
void refresh_side_toggles(const win::Settings& s) {
  constexpr int previews[]{224, 225, 232}, calibrations[]{226, 227, 233};
  for (unsigned side = 0; side < 3; ++side) {
    const auto name = side_toggle_name(s, side);
    SetDlgItemTextW(window, previews[side], switch_label(name, s.manual_mask & (1u << side)).c_str());
    SetDlgItemTextW(window, calibrations[side], switch_label(name, s.calibration_mask & (1u << side)).c_str());
  }
}
void request_connection(win::ConnectCommand command) {
  {
    const std::lock_guard lock(app_mutex);
    const bool disconnect = command == win::ConnectCommand::disconnect;
    if (!disconnect && !win::begin_connection(current)) {
      notice = tr(L"Restart Taxi Cam to begin a new connection.");
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
    connection_disconnected.store(disconnect, std::memory_order_release);
    connection_requested.store(!disconnect, std::memory_order_release);
    if (disconnect)
      win::apply_connection_command(current, command);
    if (disconnect) {
      status = {};
      connection = ui_text(L"Disconnected. Choose Connect to enable the cameras again.");
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
  notice = command == win::ConnectCommand::disconnect ? tr(L"Disconnected. Camera output and temporary requests are off.")
                                                      : tr(L"Connect requested. Cameras will enable when the bridge is ready.");
  refresh_connection_button();
  if (command == win::ConnectCommand::disconnect) {
    SetDlgItemTextW(window, 229, switch_label(tr(L"Scene test"), false).c_str());
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
    MessageBoxW(window, tr(L"Could not open your browser. You can also find the PayPal donation link in the Taxi Cam README."),
                tr(L"Donate to Taxi Cam"), MB_OK | MB_ICONWARNING);
}
// For GitHub's file picker: Ctrl+V into its File name box attaches the file.
void copy_text(const std::wstring& text) {
  if (!OpenClipboard(window))
    return;
  EmptyClipboard();
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  if (const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    auto* target = static_cast<wchar_t*>(GlobalLock(memory));
    if (target) {
      std::memcpy(target, text.c_str(), bytes);
      GlobalUnlock(memory);
    }
    if (!target || !SetClipboardData(CF_UNICODETEXT, memory))
      GlobalFree(memory);
  }
  CloseClipboard();
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
  const auto cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  const auto details = win::system_details(simulator_pid.load(), expected_simulator);
  win::BugReportForm form{
      details.simulator, win::bug_report_aircraft(context), win::bug_report_environment(details, win::bug_report_created(context.now)), {}};
  const auto bundle = win::write_report_bundle(win::settings_directory(), installation, details,
                                               win::bug_report_info(form, win::bug_report_snapshot(context)));
  form.logs = bundle.path.empty()
                  ? "Taxi Cam could not save the report file (" + bundle.error + "). Attach bridge.log from Help > Open log folder instead."
                  : "Report file: " + bundle.name +
                        "\nDrag it into this box from the File Explorer window Taxi Cam opened, or click the attach "
                        "bar below and paste (Ctrl+V) into File name: Taxi Cam copied its location.";
  // Select the file first; the form then opens in front of File Explorer.
  if (!bundle.path.empty()) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (auto* item = ILCreateFromPathW(bundle.path.c_str())) {
      SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
      ILFree(item);
    }
    if (SUCCEEDED(com))
      CoUninitialize();
    copy_text(bundle.path);
  }
  const auto url = win::bug_report_url(context, form, !bundle.path.empty());
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  SetCursor(cursor);
  if (result <= 32) {
    MessageBoxW(window,
                tr(L"Could not open your browser. Open github.com/rthoms334/taxi-cam/issues and choose Bug report. "
                   L"Attach logs from Help > Open log folder and include your Taxi Cam version."),
                tr(L"Taxi Cam bug report"), MB_OK | MB_ICONWARNING);
    return;
  }
  notice = bundle.path.empty() ? tr(L"Bug report opened, but the report file could not be saved. Attach bridge.log from Open log folder.")
                               : tr(L"Bug report opened. Drag the selected report file from File Explorer into the form's Logs box.");
  InvalidateRect(window, nullptr, FALSE);
}
// Every change saves by itself (there is no Save button): an edit saves once
// typing pauses (autosave), so a number is not saved half typed.
constexpr ULONGLONG AutosaveDelayMs = 700;
ULONGLONG autosave_due = 0;
void dirty_notice() {
  dirty = true;
  autosave_due = GetTickCount64() + AutosaveDelayMs;
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
  return profile && profile->display_layout == profiles::DisplayLayout::shared_texture;
}
// Indexed by CameraMode; CameraModeOrder is the left-to-right card order.
constexpr const wchar_t* CameraModeNames[]{ui_text(L"Performance"), ui_text(L"Balanced"), ui_text(L"Smooth"), ui_text(L"Custom"),
                                           ui_text(L"Auto")};
constexpr const wchar_t* CameraModeHints[]{ui_text(L"Lowest cost"), ui_text(L"Medium cost"), ui_text(L"Highest cost"),
                                           ui_text(L"Your target"), ui_text(L"Adapts to fps")};
constexpr CameraMode CameraModeOrder[]{CameraMode::automatic, CameraMode::performance, CameraMode::balanced, CameraMode::smooth,
                                       CameraMode::custom};
static_assert(std::size(CameraModeNames) == kCameraModeCount && std::size(CameraModeHints) == kCameraModeCount &&
              std::size(CameraModeOrder) == kCameraModeCount);
// Display page mode cards (270 + CameraMode) and the custom target slider
// (205). The chosen card saves like the other fields (autosave).
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
  const unsigned feeds = profile && profile->composition.split_bottom != 0 ? 3u : 2u;
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
  const bool hot = hot_button == item.hwndItem;
  round_box(dc, item.rcItem, scale(10),
            chosen ? (hot ? AccentHot : Accent)
            : down ? Well
            : hot  ? RaisedHot
                   : Raised,
            chosen ? (hot ? AccentHot : Accent)
            : hot  ? BorderStrong
                   : blend(Raised, Text, 7),
            scale(1));
  // On the accent, the text is dark.
  const COLORREF ink = chosen ? Well : Text, quiet = chosen ? blend(Well, Accent, 30) : Muted;
  SetBkMode(dc, TRANSPARENT);
  const auto line = [&](const wchar_t* value, int top, int height, HFONT font, COLORREF color) {
    RECT r{item.rcItem.left + scale(10), item.rcItem.top + scale(top), item.rcItem.right - scale(4), item.rcItem.top + scale(top + height)};
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
  };
  line(tr(CameraModeNames[mode]), 9, 26, heading, ink);
  const auto label = auto_live && per_frame >= feeds ? std::wstring(tr(L"Every camera, every frame"))
                     : auto_live && per_frame == 1   ? std::wstring(tr(L"1 camera per frame"))
                     : auto_live                     ? tr_format(L"{0} cameras per frame", {std::to_wstring(per_frame)})
                     : automatic                     ? std::wstring(tr(L"Even frames"))
                                                     : tr_format(L"{0} fps", {std::to_wstring(target)});
  line(label.c_str(), 36, 20, small, quiet);
  const bool show_now = std::isfinite(reached) && (!automatic || auto_live);
  const auto now =
      show_now ? tr_format(L"≈ {0} fps now", {decimal(reached, reached < 9.95 ? 1 : 0)}) : std::wstring(tr(CameraModeHints[mode]));
  line(now.c_str(), 57, 22, normal, show_now ? ink : quiet);
  // Frame cost follows the images rendered per second (Auto: per frame).
  const int bars = automatic                          ? static_cast<int>(auto_live ? std::min(per_frame, 3u) : 1u)
                   : target <= kPerformanceCameraRate ? 1
                   : target <= 20                     ? 2
                                                      : 3;
  for (int i = 0; i < 3; ++i) {
    RECT bar{item.rcItem.right - scale(12 + (3 - i) * 16) + scale(4), item.rcItem.bottom - scale(16),
             item.rcItem.right - scale(12 + (2 - i) * 16), item.rcItem.bottom - scale(11)};
    round_box(dc, bar, scale(3), i < bars ? (chosen ? Well : Muted) : (chosen ? blend(Accent, Well, 25) : BorderStrong));
  }
  RECT cost{item.rcItem.left + scale(10), item.rcItem.bottom - scale(22), item.rcItem.right - scale(64), item.rcItem.bottom - scale(4)};
  SelectObject(dc, small);
  SetTextColor(dc, quiet);
  DrawTextW(dc, tr(L"Cost"), -1, &cost, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}
// Cockpit L:vars for the selected aircraft (Taxi cameras and Door camera): a
// name and a trigger per camera action, and an Equals trigger's value, saved
// in the profile's [bindings].
constexpr int LvarNameId = 760, LvarTriggerId = 770, LvarStatusId = 780, LvarValueId = 870;
// The trigger button's width, and its width beside the Equals value field.
constexpr int LvarTriggerWidth = 100, LvarEqualsWidth = 56, LvarValueWidth = 40;
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
  for (unsigned j = 0; j < 6; ++j)
    settings.mounts[profiles::DoorFeed][j] = number(DoorFieldId + static_cast<int>(j), settings.mounts[profiles::DoorFeed][j], ok);
  // A name that is not an L:var stops the step, with its own message.
  for (unsigned i = 0; i < win::LvarBindingCount; ++i) {
    HWND name = GetDlgItem(window, LvarNameId + static_cast<int>(i));
    if (!name)
      continue;
    wchar_t text[96]{};
    GetWindowTextW(name, text, 96);
    win::LvarBinding binding{};
    if (!win::parse_lvar_name(text, binding.name)) {
      ok = false;
      if (error)
        *error = ui_text(L"Use letters, digits, underscores and dots for L:var names, for example S_PED_COCKPIT_DOOR_VIDEO.");
      continue;
    }
    // The trigger button's user data is the trigger; an equals value is the
    // whole number typed beside it.
    const auto trigger =
        static_cast<std::uint32_t>(GetWindowLongPtrW(GetDlgItem(window, LvarTriggerId + static_cast<int>(i)), GWLP_USERDATA));
    binding.trigger = win::valid_lvar_trigger(trigger) ? trigger : static_cast<std::uint32_t>(win::LvarTrigger::momentary);
    if (win::lvar_trigger_kind(binding.trigger) == win::LvarTrigger::equals) {
      wchar_t digits[16]{};
      GetDlgItemTextW(window, LvarValueId + static_cast<int>(i), digits, 16);
      wchar_t* end{};
      const auto value = std::wcstoul(digits, &end, 10);
      if (!digits[0] || *end || value > win::LvarEqualsMax) {
        ok = false;
        if (error)
          *error = ui_text(L"Equals takes a whole number from 0 to 16777215, such as the page number the line under it reads.");
        continue;
      }
      binding.trigger = win::lvar_equals_trigger(static_cast<std::uint32_t>(value));
    }
    settings.lvar_bindings[i] = binding;
  }
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
    // Status lines speak only when something needs attention: a shortcut that
    // works, or none, says nothing.
    const bool editing = hotkey_editor_focused && GetFocus() == GetDlgItem(shortcut_window, 620 + static_cast<int>(i));
    // A shortcut saves once its editor loses focus (autosave).
    const auto state = editing ? std::wstring(tr(L"Editing — shortcuts paused"))
                       : !hotkey_editor_focused && hotkey_registration.unavailable(i)
                           ? tr_format(hotkey_registration.status_wording(i), {std::to_wstring(hotkey_registration.error(i))})
                           : std::wstring();
    SetDlgItemTextW(shortcut_window, 650 + i, state.c_str());
    const auto binding = button_draft[i];
    const auto device = binding.button ? win::CameraButtonInput::device_name(binding.vendor, binding.product) : std::wstring();
    const bool capturing = button_capture == static_cast<int>(i);
    // A controller without a product name is "Game controller", translated here.
    const auto controller = win::camera_button_controller(binding, device);
    const auto label =
        capturing ? std::wstring(tr(L"Press and release a controller button…"))
                  : tr_format(win::camera_button_wording(binding, device), {std::to_wstring(binding.button), tr(controller.c_str())});
    SetDlgItemTextW(shortcut_window, 680 + i, label.c_str());
    const auto button_state = capturing                       ? std::wstring(tr(L"Waiting — × cancels"))
                              : !binding.button || preview_ui ? std::wstring()
                              : !button_input.enabled()       ? tr_format(L"Windows error {0}", {std::to_wstring(button_input.error())})
                              : device.empty()                ? std::wstring(tr(L"Controller not connected"))
                                                              : std::wstring();
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
// The binding fields (shortcut, controller button, cockpit L:var): a dark
// well on the card with a rounded edge like the buttons', accented in use.
void draw_field_frame(HDC dc, const RECT& r, bool active) {
  FillRect(dc, &r, card_brush);
  round_box(dc, r, scale(8), Background, active ? Accent : BorderStrong, scale(1));
}
int line_height(HWND control) {
  HDC dc = GetDC(control);
  const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  SelectObject(dc, old);
  ReleaseDC(control, dc);
  return metrics.tmHeight;
}
// Shortcut and L:var fields draw that frame in their non-client area, with
// the text centred inside it.
LRESULT CALLBACK field_frame(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_NCCALCSIZE && w) {
    auto& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0];
    const int inset = std::max<int>(scale(2), (r.bottom - r.top - line_height(control)) / 2);
    r = {r.left + scale(8), r.top + inset, r.right - scale(8), r.bottom - inset};
    return 0;
  }
  if (message == WM_NCPAINT) {
    HDC dc = GetWindowDC(control);
    RECT bounds{}, inner{};
    GetWindowRect(control, &bounds);
    GetClientRect(control, &inner);
    MapWindowPoints(control, nullptr, reinterpret_cast<POINT*>(&inner), 2);
    OffsetRect(&inner, -bounds.left, -bounds.top);
    OffsetRect(&bounds, -bounds.left, -bounds.top);
    ExcludeClipRect(dc, inner.left, inner.top, inner.right, inner.bottom);
    draw_field_frame(dc, bounds, GetFocus() == control);
    ReleaseDC(control, dc);
    return 0;
  }
  if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
    const auto result = DefSubclassProc(control, message, w, l);
    RedrawWindow(control, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    return result;
  }
  if (message == WM_NCDESTROY)
    RemoveWindowSubclass(control, field_frame, id);
  return DefSubclassProc(control, message, w, l);
}
void frame_field(HWND control) {
  SetWindowSubclass(control, field_frame, 2, 0);
  SetWindowPos(control, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
// A shortcut as the native hotkey control writes it, in Windows' own key
// names: Ctrl + Shift + L.
std::wstring hotkey_label(WORD value) {
  const UINT key = LOBYTE(value), flags = HIBYTE(value);
  if (!key)
    return tr(L"None");
  const auto name = [](UINT vk, bool extended) {
    wchar_t text[64]{};
    const LONG code = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16) | (extended ? 1L << 24 : 0L);
    return GetKeyNameTextW(code, text, 64) > 0 ? std::wstring(text) : std::to_wstring(vk);
  };
  constexpr std::pair<UINT, UINT> modifiers[]{{HOTKEYF_CONTROL, VK_CONTROL}, {HOTKEYF_SHIFT, VK_SHIFT}, {HOTKEYF_ALT, VK_MENU}};
  std::wstring label;
  for (const auto& [flag, vk] : modifiers)
    if (flags & flag)
      label += name(vk, false) + L" + ";
  return label + name(key, (flags & HOTKEYF_EXT) != 0);
}
LRESULT CALLBACK shortcut_editor(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_SETFOCUS) {
    // RegisterHotKey consumes its chord before the native editor sees it. Release
    // our bindings while editing so existing shortcuts can be captured as well.
    hotkey_editor_focused = true;
    hotkey_registration.clear();
    refresh_shortcut_status();
    // The native caret would sit over the label drawn below.
    const auto result = DefSubclassProc(control, message, w, l);
    DestroyCaret();
    return result;
  } else if (message == WM_ERASEBKGND) {
    return 1;
  } else if (message == WM_PAINT) {
    // The native control paints white: the label goes on the field's well.
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(control, &paint);
    RECT r{};
    GetClientRect(control, &r);
    FillRect(dc, &r, background_brush);
    const auto label = hotkey_label(static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0)));
    const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    SetTextColor(dc, GetFocus() == control ? Accent : Text);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, label.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    EndPaint(control, &paint);
    return 0;
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
  track_hover(control, type);
  return control;
}
// WM_SETCURSOR: the hand over a window's own buttons, as over links.
bool button_cursor(HWND hwnd, WPARAM w, LPARAM l) {
  const auto control = reinterpret_cast<HWND>(w);
  wchar_t type[16]{};
  if (!control || control == hwnd || GetParent(control) != hwnd || LOWORD(l) != HTCLIENT || !GetClassNameW(control, type, 16) ||
      lstrcmpiW(type, L"Button") != 0)
    return false;
  SetCursor(LoadCursorW(nullptr, IDC_HAND));
  return true;
}
// A dialog's buttons without the dotted focus square until the keyboard is
// used (Tab shows it again).
void hide_focus_cues(HWND hwnd) {
  SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
}
INT_PTR dialog_colors(WPARAM w) {
  const auto dc = reinterpret_cast<HDC>(w);
  SetTextColor(dc, Text);
  SetBkColor(dc, Card);
  return reinterpret_cast<INT_PTR>(card_brush);
}
// The door camera action needs a two-camera aircraft; the others need the
// displays they turn on.
bool lvar_action_available(const profiles::AircraftProfile* profile, unsigned action) {
  return profile && (action == win::CameraDoor || win::camera_action_sides(*profile, action) != 0);
}
// A trigger button's label: Momentary, Latched or Equals (its value in the
// field beside it).
const wchar_t* lvar_trigger_label(std::uint32_t trigger) {
  switch (win::lvar_trigger_kind(trigger)) {
    case win::LvarTrigger::latched:
      return tr(L"Latched");
    case win::LvarTrigger::equals:
      return tr(L"Equals");
    default:
      return tr(L"Momentary");
  }
}
void refresh_lvar_status(HWND hwnd) {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  for (unsigned i = 0; i < win::LvarBindingCount; ++i) {
    const auto& read = sample.lvars[i];
    std::wstring line;
    if (!lvar_action_available(profile, i)) {
      line = i == win::CameraDoor ? tr(L"This aircraft has no door camera.") : tr(L"This aircraft has no lower display camera.");
    } else if (i == win::CameraDoor && !win::door_camera_set_up(s)) {
      line = tr(L"No display chosen yet");
    } else if (!s.lvar_bindings[i].name[0]) {
      line.clear();
    } else if (!sample.heartbeat || !sample.lvar_generation || !read.read) {
      line = tr(L"Not read yet");
    } else {
      wchar_t value[32];
      std::swprintf(value, 32, L"%.4g", read.value);
      line = tr_format(L"Reads {0}", {value});
    }
    SetDlgItemTextW(hwnd, LvarStatusId + static_cast<int>(i), line.c_str());
  }
}
// The --preview UI reads What's new and its pictures from the repository
// checkout it runs in, so notes not yet on main can be checked.
std::wstring preview_notes_root() {
  wchar_t directory[MAX_PATH]{};
  const DWORD n = GetCurrentDirectoryW(MAX_PATH, directory);
  return n && n < MAX_PATH ? std::wstring(directory) : std::wstring(L".");
}
// What's new (open_whats_new): the installed version's notes, then notes main
// carries for later versions, then earlier ones (whats_new_blocks). The
// installed release's features are cards with their
// pictures, fetched one at a time from the repository while the window is
// open; a picture that cannot be loaded is left out.
constexpr wchar_t WhatsNewClass[] = L"TaxiCamWhatsNew";
constexpr int WhatsNewWidth = 880, WhatsNewHeight = 720, WhatsNewBar = 68, WhatsNewPad = 28, WhatsNewCardPad = 18;
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
    round_box(dc, {x, top + shift, x + w, top + shift + h}, radius / 2, fill, line, scale(1));
  };
  const auto rule = [&](int x, int top, int w) {
    if (!draw)
      return;
    RECT line{x, top + shift, x + w, top + shift + std::max(1, scale(1))};
    HBRUSH b = CreateSolidBrush(Border);
    FillRect(dc, &line, b);
    DeleteObject(b);
  };
  using Kind = win::WhatsNewBlock::Kind;
  const int pad = scale(WhatsNewCardPad), indent = scale(22);
  for (std::size_t index = 0; index < v.blocks.size(); ++index) {
    const auto& b = v.blocks[index];
    // Headings and notes around the release notes are interface wording.
    const auto label = b.wording ? tr_format(b.wording, {b.value}) : b.text;
    if (b.kind == Kind::heading) {
      // The app's icon, then the version over its date.
      const int mark = scale(48), x = left + mark + scale(16), w = width - mark - scale(16);
      if (draw)
        DrawIconEx(dc, left, y + shift + scale(4), icon, mark, mark, 0, nullptr, DI_NORMAL);
      put(tr(L"WHAT'S NEW"), small, Muted, x, y, w, scale(18), DT_SINGLELINE);
      y += scale(18);
      const int h = measure(label, title_font, w);
      put(label, title_font, Text, x, y, w, h);
      y += h;
      if (!b.detail.empty()) {
        put(b.detail, small, Muted, x, y, w, scale(20), DT_SINGLELINE);
        y += scale(20);
      }
      y = std::max(y, scale(26) + mark + scale(8));
    } else if (b.kind == Kind::section) {
      y += scale(22);
      const int h = measure(label, heading, width);
      put(label, heading, Text, left, y, width, h);
      y += h + scale(10);
    } else if (b.kind == Kind::feature) {
      // A card: the picture across it, then the title and text.
      const int title_h = measure(label, heading, inner), text_h = measure(b.detail, normal, inner);
      const WhatsNewPicture* picture{};
      for (const auto& candidate : v.pictures)
        if (!b.image.empty() && candidate.path == b.image)
          picture = &candidate;
      const int picture_h = !picture || picture->failed ? 0 : picture->bitmap ? picture->height : inner * 9 / 16;
      const int card_h = 2 * pad + (picture_h ? picture_h + scale(14) : 0) + title_h + scale(4) + text_h;
      box(left, y, width, card_h, Card, Border, scale(24));
      int top = y + pad;
      const int x = left + pad;
      if (picture_h) {
        if (draw && picture->bitmap) {
          // Centred, its corners rounded like the card's, with a hairline so
          // dark screenshots keep an edge.
          const int px = x + (inner - picture->width) / 2, radius = scale(8);
          const RECT frame{px, top + shift, px + picture->width, top + shift + picture->height};
          const int saved = SaveDC(dc);
          HRGN clip = CreateRoundRectRgn(frame.left, frame.top, frame.right + 1, frame.bottom + 1, radius * 2, radius * 2);
          ExtSelectClipRgn(dc, clip, RGN_AND);
          HDC memory = CreateCompatibleDC(dc);
          const auto old = SelectObject(memory, picture->bitmap);
          BitBlt(dc, frame.left, frame.top, picture->width, picture->height, memory, 0, 0, SRCCOPY);
          SelectObject(memory, old);
          DeleteDC(memory);
          RestoreDC(dc, saved);
          DeleteObject(clip);
          round_box(dc, frame, radius, NoColor, BorderStrong, scale(1));
        } else if (draw) {
          box(x, top, inner, picture_h, Well, Border, scale(16));
          put(tr(L"Loading picture…"), small, Muted, x, top, inner, picture_h, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        top += picture_h + scale(14);
      }
      put(label, heading, Text, x, top, inner, title_h);
      top += title_h + scale(4);
      put(b.detail, normal, Muted, x, top, inner, text_h);
      y += card_h + scale(14);
    } else if (b.kind == Kind::bullet) {
      // A run of fixes or changes is one card, a row each, ruled apart.
      std::size_t last = index;
      while (last + 1 < v.blocks.size() && v.blocks[last + 1].kind == Kind::bullet)
        ++last;
      const int row_w = inner - indent, gap = scale(12);
      int card_h = gap;
      for (std::size_t k = index; k <= last; ++k)
        card_h += measure(v.blocks[k].wording ? tr_format(v.blocks[k].wording, {v.blocks[k].value}) : v.blocks[k].text, normal, row_w) +
                  2 * gap + (k < last ? std::max(1, scale(1)) : 0);
      card_h -= gap;
      box(left, y, width, card_h, Card, Border, scale(24));
      int top = y;
      for (std::size_t k = index; k <= last; ++k) {
        const auto row = v.blocks[k].wording ? tr_format(v.blocks[k].wording, {v.blocks[k].value}) : v.blocks[k].text;
        const int h = measure(row, normal, row_w);
        top += gap;
        if (draw)
          disc(dc, left + pad + scale(4), top + shift + scale(10), scale(3), Accent);
        put(row, normal, Text, left + pad + indent, top, row_w, h);
        top += h + gap;
        if (k < last) {
          rule(left + pad, top, inner);
          top += std::max(1, scale(1));
        }
      }
      y += card_h + scale(14);
      index = last;
    } else if (b.kind == Kind::release) {
      y += scale(14);
      put(label, heading, Text, left, y, width, scale(26), DT_SINGLELINE);
      if (!b.detail.empty()) {
        RECT title{0, 0, width, 0};
        SelectObject(dc, heading);
        DrawTextW(dc, label.c_str(), -1, &title, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        put(b.detail, small, Muted, left + title.right + scale(12), y + scale(4), width, scale(22), DT_SINGLELINE);
      }
      y += scale(34);
    } else {
      const int h = measure(label, normal, width);
      put(label, normal, Muted, left, y, width, h);
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
      FillRect(memory, &strip, background_brush);
      RECT line{0, bar, client.right, bar + scale(1)};
      HBRUSH edge = CreateSolidBrush(Border);
      FillRect(memory, &line, edge);
      DeleteObject(edge);
      v.close = {client.right - scale(WhatsNewPad) - scale(120), bar + scale(16), client.right - scale(WhatsNewPad), bar + scale(52)};
      ButtonLook close_look;
      close_look.primary = true;
      button_box(memory, v.close, close_look, Background);
      SelectObject(memory, normal);
      SetTextColor(memory, button_text(close_look));
      SetBkMode(memory, TRANSPARENT);
      auto label = v.close;
      DrawTextW(memory, tr(L"Close"), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
  v.window = CreateWindowExW(0, WhatsNewClass, tr(L"Taxi Cam — What's new"), style, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
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
// The status line from before What's new started loading, shown again once
// it opens.
std::wstring notice_before_whats_new;
void request_whats_new() {
  if (changelog_fetcher.busy())
    return;
  if (preview_ui)
    changelog_fetcher.set_local_root(preview_notes_root());
  if (!changelog_fetcher.begin(window, WhatsNewMessage)) {
    notice = tr(L"Could not start loading What's new. Try again.");
  } else {
    SetDlgItemTextW(window, 515, tr(L"Loading…"));
    notice_before_whats_new = notice;
    notice = tr(L"Loading What's new from GitHub…");
  }
  InvalidateRect(window, nullptr, FALSE);
}
// Texture routing: a one-shot picture of a tracked display texture, with the
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
  const bool single = profile.display_layout == profiles::DisplayLayout::shared_texture;
  return {sample.left_id, single ? sample.left_id : sample.right_id,
          profiles::own_lower_texture(profile) ? sample.lower_id : sample.left_id};
}
const wchar_t* snapshot_side_name(const win::Settings& s, unsigned side) {
  return side_toggle_name(s, side);
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
  win::UiMessage message;
  if (!answered)
    message.add(ui_text(L"Texture #{0}."), {std::to_wstring(snapshot_view.id)})
        .add(GetTickCount64() - snapshot_view.requested_ms > 3000
                 ? ui_text(L"Waiting for the simulator. Connect Taxi Cam with a flight loaded.")
                 : ui_text(L"Requesting a snapshot..."));
  else
    message
        .add(ui_text(L"Texture #{0} | {1} x {2} | format {3}."),
             {std::to_wstring(sample.snapshot_id), std::to_wstring(sample.snapshot_width), std::to_wstring(sample.snapshot_height),
              std::to_wstring(sample.snapshot_format)})
        .add(result == win::DisplaySnapshotResult::ready && !snapshot_view.image ? ui_text(L"The saved image could not be opened.")
                                                                                 : win::display_snapshot_text(result));
  const auto caption = message.text();
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
    // Only a profile the catalog does not know has no name to show.
    const auto* named = profiles::find(s.profile);
    const std::wstring name = named ? named->name : L"";
    legend = !named     ? tr(L"This texture is not the size of the selected profile's display texture, so no display rectangles are drawn.")
             : !profile ? tr_format(L"This texture is not the size of a {0} display texture, so no display rectangles are drawn.", {name})
             : bound.empty()
                 ? tr_format(L"Not routed to a display on {0}. Dotted outlines show where its displays would be drawn.", {name})
                 : tr_format(L"Routed to {0} on {1}. Solid outlines are routed display rectangles; dotted ones are not.", {bound, name});
  }
  if (legend != snapshot_view.legend) {
    snapshot_view.legend = legend;
    SetDlgItemTextW(hwnd, 702, legend.c_str());
  }
  if (caption != snapshot_view.caption) {
    snapshot_view.caption = caption;
    SetDlgItemTextW(hwnd, 701, caption.c_str());
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}
void paint_snapshot(HDC dc) {
  const auto area = rectangle(20, 92, 860, 560);
  HBRUSH fill = CreateSolidBrush(Well);
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
  SetBkColor(dc, Well);
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
INT_PTR CALLBACK snapshot_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_INITDIALOG) {
    place_dialog(hwnd, tr(L"Taxi Cam — Display texture snapshot"), 900, 760);
    dialog_control(hwnd, L"STATIC", tr(L"Display texture snapshot"), -1, 20, 17, 860, 28, 0, heading);
    dialog_control(hwnd, L"STATIC", L"", 701, 20, 54, 860, 30);
    dialog_control(hwnd, L"STATIC", L"", 702, 20, 662, 860, 44, 0, small);
    dialog_control(hwnd, L"BUTTON", tr(L"Take again"), 703, 652, 714, 120, 34, WS_TABSTOP);
    const auto close = dialog_control(hwnd, L"BUTTON", tr(L"Close"), IDCANCEL, 782, 714, 98, 34, WS_TABSTOP | BS_DEFPUSHBUTTON);
    SetTimer(hwnd, 1, 200, nullptr);
    update_snapshot_dialog(hwnd);
    SetFocus(close);
    hide_focus_cues(hwnd);
    return FALSE;
  }
  if (message == WM_SETCURSOR && button_cursor(hwnd, w, l)) {
    SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
    return TRUE;
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
    notice = tr(L"Could not open the display snapshot.");
    InvalidateRect(window, nullptr, FALSE);
  }
  snapshot_dialog_open = false;
}
// Displays, Texture routing, Taxi cameras, Door camera and Overview cards:
// small pictures of the display textures, taken one at a time through the
// snapshot request while one of those pages is on screen. Never periodic: a
// picture is retaken when a page opens and it is older than ThumbnailFreshMs,
// when the user asks for a refresh, or once a camera turned on or off on its
// display has had ThumbnailCameraMs to draw.
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
  ULONGLONG requested_ms{}, fresh_after{}, retake_ms{};
  // The cameras on the displays (live_cameras_on, then the door camera's bit).
  unsigned cameras{};
  bool showing{};
} thumbnails;
constexpr unsigned ThumbnailEdge = 512;
// A drawn texture answers in well under a second (0.3-0.4 s on the Fenix,
// 2026-10-31); one that is not drawn would hold every other picture up to
// the bridge's 5 s timeout, so the next request replaces it sooner.
constexpr ULONGLONG ThumbnailWaitMs = 2500, ThumbnailRetryMs = 15000, ThumbnailFreshMs = 120000, ThumbnailCameraMs = 2000;
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
  return separate_lower_profile(s) ? (list ? tr(L"Lower display") : tr(L"Navigation displays"))
         : routing_lists(s) == 1   ? tr(L"Display texture")
         : list                    ? tr(L"Right PFD")
                                   : tr(L"Left PFD");
}
const wchar_t* list_action(const win::Settings& s, unsigned list) {
  return separate_lower_profile(s) ? (list ? tr(L"Lower") : tr(L"NDs"))
         : routing_lists(s) == 1   ? tr(L"Use")
         : list                    ? tr(L"Right")
                                   : tr(L"Left");
}
const wchar_t* display_side_title(const win::Settings& s, unsigned side) {
  // The PMDG 777 display select panel's own labels.
  if (pmdg_cam_control(s))
    return side == 0 ? L"L INBD" : side == 1 ? L"R INBD" : L"LWR CTR";
  return side == 0 ? tr(L"Left PFD") : side == 1 ? tr(L"Right PFD") : tr(L"Lower display");
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
  if (!candidate)
    return L"#" + std::to_wstring(id);
  return tr_format(L"#{0} · {1} × {2} · {3} mips", {std::to_wstring(id), std::to_wstring(candidate->width),
                                                    std::to_wstring(candidate->height), std::to_wstring(candidate->mips)});
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
// routed state. The buttons exist from build_controls on Texture routing only.
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
    notice = result != win::TargetAssignmentResult::duplicate ? tr(L"Display assignment request limit reached. Restart Taxi Cam.")
             : separate ? tr(L"Choose a lower DU texture different from the navigation display texture, or Automatic.")
                        : tr(L"Choose different textures for left and right, or Automatic.");
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
    notice = tr_format(L"{0} uses #{1} for this flight. Clear choices goes back to automatic selection.",
                       {list_title(s, list), std::to_wstring(id)});
}
// Aircraft setup (SetupPage): an aircraft no profile matches is set up by
// picking, by picture, the cockpit panel each camera side draws on. The layout
// is learned from the bridge's panel map (discovered_setup.hpp); the cameras
// are then positioned from their starting mounts on Taxi cameras.
// Pages: Overview, Options and Diagnostics, and the Setup steps:
// Displays (the aircraft setup, with texture routing as its Advanced page),
// Taxi cameras (with the guide values as its Advanced page) and Door camera.
// Each camera's step has the controls that turn it on.
// The window's pages. Home is Overview; Aircraft is Setup's three steps
// (Displays with its texture routing, Taxi cameras with its guide values,
// Door camera) and Picture; Options is PreferencesPage; Help is DiagnosticsPage.
constexpr int OverviewPage = 0, CamerasPage = 1, PicturePage = 2, RoutingPage = 3, DiagnosticsPage = 4, GuidesPage = 5, SetupPage = 6,
              DoorPage = 7, PreferencesPage = 8;
constexpr int SetupPreviousId = 452, SetupNextId = 453, SetupRefreshId = 456, SetupSwapId = 457, SetupTestId = 458, SetupBuiltInId = 461,
              SetupCardButtonId = 750, SetupPlaceId = 754;
// The step pills (Overview and every step), Advanced, and a step's Back and
// Next.
constexpr int StepPillId = 710, AdvancedId = 716, BackId = 717, NextId = 718, DoorToggleId = 719;
// Options: its notifications switch and its language button.
constexpr int NotificationsId = 606, LanguageId = 607, BrightnessSliderId = 206;
// Home's action for an aircraft that needs setting up, and Options' update check.
constexpr int HomeActionId = 609, UpdatesId = 608;
// Displays' display area, Taxi cameras' and Door camera's Adjust live view and
// Defaults per feed, the Advanced pages' openers, the door camera's display
// (none, then each side) and picture, and Overview's way to choose its display.
constexpr int LiveFeedId = 720, DisplayAreaId = 724, DefaultsId = 725, RoutingOpenId = 729, GuidesOpenId = 730, DoorWhereId = 740,
              DoorSnapshotId = 744, DoorLiveId = 745, DoorChooseId = 746;
// Power buttons on the live pictures: each taxi camera card's (790 + feed)
// turns the taxi cameras on or off, the door camera step's the door camera.
constexpr int CameraPowerId = 790, DoorPowerId = 793;
// Look icons on the same pictures (+ camera view): film grain and black and
// white for that camera.
constexpr int LookGrainId = 850, LookMonoId = 860;
// The door camera step's Display area: its display's area in the live view.
constexpr int DoorAreaId = 747;
// A step's content starts under its subtitle and ends above its footer. The
// camera cards were sized for a 766-wide column; wide() spreads such a width
// across the page.
constexpr int StepTop = 96, StepBottom = 600;
constexpr int wide(int width) {
  return width * PageWidth / 766;
}
// Taxi cameras: its camera cards, Advanced's guide row and the controls card
// below with a row per camera action (draw_page and build_controls share
// them).
constexpr int CameraCardHeight = 186, CameraCardAdvancedHeight = 204, CameraGuidesHeight = 40, ControlRowHeight = 54,
              ControlRowAdvancedHeight = 42;
// Overview's display cards, as the design draws them: the picture across the
// card, the display's name with its panel's pill, then the cameras' switches
// (draw_display_strip and build_controls share them).
constexpr int DisplayCardTop = 228, DisplayCardHeight = 310, DisplayCardPicture = 226, DisplayCardSwitches = 246;
// Above them the status card and the aircraft's card; under them the
// in-flight switches.
constexpr int HomeStatusTop = 72, HomeStatusHeight = 76, HomeAircraftTop = 160, HomeAircraftHeight = 56, HomeFlightTop = 550,
              HomeFlightHeight = 48;
// Displays: the slot cards (name and pill, picture, panel, button), the hint
// under them and the gallery below.
constexpr int SetupSlotTop = 96, SetupSlotHeight = 206, SetupSlotPicture = 104, SetupGalleryTop = 352, SetupGalleryCards = 394,
              SetupGalleryCardHeight = 160;
constexpr const wchar_t* ControlsActionNames[]{ui_text(L"Left"), ui_text(L"Right"), ui_text(L"Both"), ui_text(L"Lower"), ui_text(L"Door")};
constexpr unsigned SetupSteps = 3;
bool advanced = false;
// Where Taxi cameras' controls card starts, below its cards.
int controls_top() {
  return StepTop + (advanced ? CameraCardAdvancedHeight + 10 + CameraGuidesHeight : CameraCardHeight) + 14;
}
// The step a page belongs to, or -1 outside Setup.
int setup_step(int shown) {
  switch (shown) {
    case SetupPage:
    case RoutingPage:
      return 0;
    case CamerasPage:
    case GuidesPage:
      return 1;
    case DoorPage:
      return 2;
    default:
      return -1;
  }
}
// Aircraft's pages: what is saved for the aircraft loaded.
bool aircraft_page(int shown) {
  return setup_step(shown) >= 0 || shown == PicturePage;
}
const wchar_t* step_name(unsigned step) {
  return step == 0 ? tr(L"Displays") : step == 1 ? tr(L"Taxi cameras") : tr(L"Door camera");
}
struct AircraftSetup {
  profiles::VariantKey key{};
  std::array<std::string, MaxDisplaySides> names;
  // Every aircraft shows all three displays; an empty lower one is none.
  unsigned sides = MaxDisplaySides, gallery_page = 0;
  // The picks were loaded for this opening of Displays (sync_setup).
  bool synced{};
  std::array<std::uint64_t, GalleryColumns> ids{};
  std::array<std::string, GalleryColumns> card_names;
  bool positioning{};  // Taxi cameras shows the positioning step after a save.
  // Displays saved since Displays opened (each drop that completes them
  // saves), and the cameras the user had on before the first of those saves.
  bool stored{};
  unsigned manual_before{}, door_on_before{};
  // A drop not saved yet: the panel map was not ready (a save restarts the
  // cameras and pairs the names again), so it is saved once it is.
  bool unsaved{};
  // Leaving Displays was refused once for it; the next try leaves anyway.
  bool leave_warned{};
  // The panel picked from the gallery for a display (Place here), and a drag
  // of it from its card onto a display. picked_texture is the picture's own
  // texture, and textures what each display was given by picture: this
  // flight's routes, right even where the flight's panel names are off.
  std::string picked;
  std::uint64_t picked_texture{};
  std::array<std::uint64_t, MaxDisplaySides> textures{};
  // Each display's area on its panel (Display area picker), empty for the
  // display's own. Saved with the picks (apply_setup_areas).
  std::array<profiles::DisplayRect, MaxDisplaySides> areas{};
  bool dragging{};
  int drag_slot = -1;
  POINT drag_from{};
  // The display under the cursor while dragging, -1 for none.
  int drop_side = -1;
  std::array<RECT, MaxDisplaySides> slot_areas{};
  // Cockpit test: sides showing the calibration pattern, and the settings in
  // use before the picks ran as an unsaved trial profile.
  unsigned testing{};
  bool trial{};
  win::Settings before{};
  // A built-in profile whose displays are being changed (0: setting up an
  // aircraft of its own). Its choices save as the profile's DisplayOverride.
  std::uint32_t built_in{};
} setup;
// The Displays slots side by side over 730 logical pixels, the pilot
// displays held apart by the swap button's gap. Returns a slot's x and width.
constexpr int SetupSlotGap = 16, SetupSwapGap = 52, SetupSwapSize = 36;
std::pair<int, int> setup_slot_x(unsigned side) {
  const int sides = std::max(1, static_cast<int>(setup.sides)), swap = sides >= 2 ? SetupSwapGap - SetupSlotGap : 0;
  const int width = (PageWidth - SetupSlotGap * (sides - 1) - swap) / sides;
  return {PageLeft + static_cast<int>(side) * (width + SetupSlotGap) + (side >= 1 ? swap : 0), width};
}
bool fresh_status(const win::Status& sample) {
  const auto now = GetTickCount64();
  return sample.heartbeat && now >= sample.heartbeat && now - sample.heartbeat <= 3000;
}
// A loaded aircraft no profile matches; the bridge tracks its panels.
bool unsupported_aircraft(const win::Status& sample) {
  return fresh_status(sample) && sample.panel_discovery && !sample.detected_profile && sample.identity_sample_ms;
}
// The built-in profile a setup changes: the selected one, unless the aircraft
// loaded has no built-in profile. Auto aircraft keeps the last profile while
// such an aircraft is loaded, and the bridge still reports one set up earlier
// in this session as set up after its settings are gone (FSLabs A321 under
// the FlyByWire A380X, 2026-11-03).
std::uint32_t setup_built_in(const win::Settings& s, const win::Status& sample) {
  if (s.profile == profiles::DiscoveredProfileId)
    return 0;
  const bool loaded_none = fresh_status(sample) && sample.identity_sample_ms &&
                           (!sample.detected_profile || sample.detected_profile == profiles::DiscoveredProfileId);
  return loaded_none ? 0 : s.profile;
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
// Every cockpit display the bridge tracks while Displays is open, for set-up
// and built-in aircraft alike (placing one is checked by fits_built_in).
std::vector<win::PanelTexture> setup_items(const win::Status& sample) {
  std::vector<win::PanelTexture> items;
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
  std::erase_if(items, [](const auto& item) { return !item.id; });
  return items;
}
// A built-in profile's own displays are regions of its display shape, so they
// use only panels of that shape; a set-up aircraft, and a lower display added
// to a pilot pair, use any panel.
bool fits_built_in(const win::PanelTexture& panel) {
  const auto* built_in = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  return !built_in || profiles::named_display(*built_in, panel.width, panel.height, panel.mips, profiles::DiscoveredFormat);
}
// Picks belong to one variant: another aircraft starts again, and an aircraft
// already set up starts from its saved displays. They load when Displays
// opens, before its controls are made, whether or not the simulator reports
// the aircraft yet.
void sync_setup(const win::Status& sample) {
  const auto key = loaded_variant(sample);
  // An aircraft report that comes and goes changes nothing: the picks being
  // made stay. So does the report of the aircraft already loaded.
  if (setup.synced && (!key[0] || key == setup.key))
    return;
  const auto s = draft();
  // The aircraft reported decides again whether the built-in profile is the
  // one set up.
  const auto built_in = setup_built_in(s, sample);
  if (setup.synced && !setup.key[0] && built_in == setup.built_in &&
      (setup.built_in || (s.profile == profiles::DiscoveredProfileId && profiles::same_variant(key.data(), s.discovered.key)))) {
    setup.key = key;
    return;
  }
  setup.synced = true;
  setup.key = key;
  setup.built_in = built_in;
  // Saves and the cameras to give back belong to the aircraft they were made
  // for: another one starts without them.
  setup.stored = setup.unsaved = setup.leave_warned = false;
  setup.manual_before = setup.door_on_before = 0;
  setup.names = {};
  setup.areas = {};
  // Each display starts on the texture it uses in this flight: the one the
  // user placed, else the bridge's.
  setup.textures = {s.left_id ? s.left_id : sample.left_id, s.right_id ? s.right_id : sample.right_id, sample.lower_id};
  setup.picked_texture = 0;
  setup.sides = MaxDisplaySides;
  setup.gallery_page = 0;
  // A built-in profile starts from the displays it uses now. Every aircraft
  // has a lower display: a pilot pair's is the one the user places there
  // (named_lower_side), where the door camera goes.
  if (const auto* built_in = setup.built_in ? profiles::find(setup.built_in) : nullptr) {
    for (unsigned side = 0; side < MaxDisplaySides; ++side)
      setup.names[side] = side < built_in->sides && built_in->panel_textures[side] ? built_in->panel_textures[side] : "";
    if (s.profile == setup.built_in)
      setup.areas = std::to_array(s.display_override.areas);
    return;
  }
  // The selected set-up aircraft, when it is the one loaded or none is yet.
  if (s.profile == profiles::DiscoveredProfileId && (!key[0] || profiles::same_variant(key.data(), s.discovered.key)))
    for (unsigned side = 0; side < s.discovered.sides && side < MaxDisplaySides; ++side) {
      setup.names[side] = s.discovered.textures[side];
      setup.areas[side] = s.display_override.areas[side];
    }
}
// A set-up aircraft's displays: the lower one only when a panel is on it.
unsigned placed_sides() {
  return setup.names[MaxDisplaySides - 1].empty() ? MaxDisplaySides - 1 : MaxDisplaySides;
}
// The aircraft a step sets up: on Displays the one loaded, else the selected
// profile's.
std::wstring setup_aircraft_name() {
  if (page == SetupPage && !setup.built_in && setup.key[0])
    return widen(std::string(profiles::variant_name(setup.key.data())).c_str());
  const auto* profile = profiles::find(draft().profile);
  return profile ? std::wstring(profile->name) : std::wstring(tr(L"this aircraft"));
}
// What stops the setup now; empty when the picks can be saved.
// The bridge pictured cockpit panels and none carried a mark: the panel
// marks package is not loaded in this simulator session.
bool package_conflict() noexcept;
std::wstring packages_in_the_way();
// The other add-ons that change the cockpit files Taxi Cam's package uses,
// by folder name, after a message about the package: without the names the
// pilot can't tell which add-on to move out (issue 200). Nothing is added
// while there is none.
std::wstring with_packages_in_the_way(std::wstring text) {
  const auto names = packages_in_the_way();
  if (!names.empty())
    text += L" " + tr_format(L"Add-ons in your Community folder that change the same cockpit files: {0}.", {names});
  return text;
}
// Another add-on's copy of the cockpit page Taxi Cam can't add to: the package
// was left out, so restarting or reinstalling won't help.
std::wstring package_conflict_text() {
  return with_packages_in_the_way(
      tr(L"Another add-on in your Community folder changes the cockpit page Taxi Cam uses to recognise your displays, in a way "
         L"Taxi Cam can't add to. Taxi Cam can't recognise your displays while that add-on is installed."));
}
std::wstring package_inactive_text() {
  if (package_conflict())
    return package_conflict_text();
  return with_packages_in_the_way(
      tr(L"Taxi Cam's display package is not active in the simulator, so it can't recognise the cockpit displays. Restart the "
         L"simulator; if that doesn't help, run the Taxi Cam installer again."));
}
bool package_inactive(const win::Status& sample) {
  return fresh_status(sample) && static_cast<win::PanelMapState>(sample.panel_state) == win::PanelMapState::inactive;
}
std::wstring setup_blocker(const win::Status& sample) {
  // Pictures and the panel list need the bridge connection.
  if (!fresh_status(sample) || !sample.graphics_ready || !draft().enabled)
    return tr(L"Choose Connect on Home with this aircraft loaded in a flight.");
  if (!setup.key[0])
    return tr(L"Waiting for the simulator to report which aircraft is loaded.");
  if (!sample.panel_discovery)
    return tr(L"Waiting for the bridge to track this cockpit's panels.");
  if (package_inactive(sample))
    return package_inactive_text();
  // Panels are named by the marks their pages paint when they load.
  if (static_cast<win::PanelMapState>(sample.panel_state) != win::PanelMapState::marked)
    return tr(L"Reading this cockpit's panels. This takes a few seconds after the flight has loaded.");
  return {};
}
// A built-in profile's choices from the picks: a pick that is the profile's
// own panel is no choice, so the profile stays as it comes.
profiles::DisplayOverride built_in_choices(const profiles::AircraftProfile& base, profiles::DisplayOverride choices) {
  // A pilot pair's side 2: the lower display the user placed.
  const unsigned sides = base.sides == 2 && profiles::pilot_pair(base) ? MaxDisplaySides : base.sides;
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    std::fill(std::begin(choices.textures[side]), std::end(choices.textures[side]), '\0');
    const auto& pick = setup.names[side];
    const std::string own = side < base.sides && base.panel_textures[side] ? base.panel_textures[side] : "";
    if (side < sides && !pick.empty() && pick != own && pick.size() < profiles::MaxPanelName)
      pick.copy(choices.textures[side], pick.size());
  }
  return choices;
}
std::uint64_t picked_id(const std::vector<win::PanelTexture>& items, unsigned side) {
  // The picture the user gave the display this flight.
  if (side < MaxDisplaySides && setup.textures[side])
    return setup.textures[side];
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
// Setup previews on the placed displays: a taxi camera on each, but the door
// camera on its own display, as in flight, rather than a taxi camera there
// until Door is picked in the live view. A door camera set to no display (the
// user's choice in Setup → Door camera) leaves that display a taxi camera.
void preview_cameras(win::Settings& s, unsigned placed) {
  const unsigned door = s.door_side < MaxDisplaySides && (placed & (1u << s.door_side)) ? 1u << s.door_side : 0u;
  s.manual_mask = placed & ~door;
  s.door_on = door ? 1u : 0u;
}
// Cockpit test: the picks run as an unsaved trial profile with the
// calibration pattern on each tested side, so the cockpit shows which screen
// a panel is even where panel names do not say. A side not picked yet uses
// a picked side's panel. Leaving the setup unsaved restores the settings.
// An aircraft being set up runs the trial from its first placed display,
// with the cameras on every placed display not tested: they show before the
// setup is saved, and saving that layout keeps them running (no restart).
// The displays' areas from the picker, onto settings about to be saved or
// tried: a display without a panel has none.
void apply_setup_areas(win::Settings& s) {
  for (unsigned side = 0; side < MaxDisplaySides; ++side)
    s.display_override.areas[side] = setup.names[side].empty() ? profiles::DisplayRect{} : setup.areas[side];
}
void apply_setup_trial() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  auto s = draft();
  if (!setup.testing && setup.built_in) {
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
    apply_setup_areas(s);
    win::fit_door_camera(s);
    s.calibration_mask = setup.testing & ((1u << profiles::override_sides(*base, s.display_override)) - 1);
    s.manual_mask = s.taxi_selected_mask = s.taxi_desired_mask = 0;
    s.panel_setup = 1;
    route_picks(s, setup_items(sample));
    if (!win::valid_settings(s)) {
      setup.testing = 0;
      notice = tr(L"The cockpit test could not start.");
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
    publish(s);
    setup.trial = true;
    notice = tr(L"Test bars show on the tested panels in the cockpit after a few seconds.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  std::array<const char*, MaxDisplaySides> names{};
  const char* fallback = nullptr;
  const unsigned sides = placed_sides();
  for (unsigned side = 0; side < sides; ++side)
    if (!setup.names[side].empty())
      fallback = fallback ? fallback : setup.names[side].c_str();
  for (unsigned side = 0; side < sides; ++side)
    names[side] = setup.names[side].empty() ? fallback : setup.names[side].c_str();
  profiles::DiscoveredLayout layout;
  if (!fallback || win::learn_layout(sample, names, sides, layout) != win::LayoutLearning::learned) {
    // Without a test asked for, the cameras wait for a placed display, and a
    // test just turned off takes its bars down (the map may be pairing again).
    if (!setup.testing) {
      if (setup.trial && s.calibration_mask) {
        s.calibration_mask = 0;
        publish(s);
      }
      return;
    }
    setup.testing = 0;
    notice = fallback ? tr(L"The cockpit test needs the panel list. Wait for the pictures, then try again.") : tr(L"Pick a panel first.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  unsigned placed = 0;
  for (unsigned side = 0; side < sides; ++side)
    placed |= setup.names[side].empty() ? 0u : 1u << side;
  if (!setup.trial)
    setup.before = s;
  s.profile = profiles::DiscoveredProfileId;
  s.discovered = layout;
  apply_setup_areas(s);
  win::fit_door_camera(s);
  s.calibration_mask = setup.testing & ((1u << sides) - 1);
  preview_cameras(s, placed & ~s.calibration_mask);
  s.taxi_selected_mask = s.taxi_desired_mask = 0;
  s.lower_id = 0;
  route_picks(s, setup_items(sample));
  s.panel_setup = 1;
  if (!win::valid_settings(s)) {
    if (setup.testing)
      notice = tr(L"The cockpit test could not start.");
    setup.testing = 0;
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  publish(s);
  setup.trial = true;
  if (setup.testing)
    notice = tr(L"Test bars show on the tested panels in the cockpit after a few seconds.");
  InvalidateRect(window, nullptr, FALSE);
}
// Leaving the setup without saving: the settings from before the test,
// keeping the displays placed as this flight's routes until Taxi Cam closes.
void end_setup_trial(win::Settings& s) {
  if (setup.trial) {
    const auto placed = s;
    s = setup.before;
    s.left_id = placed.left_id;
    s.right_id = placed.right_id;
    s.route_request = placed.route_request;
  }
  setup.trial = false;
  setup.testing = 0;
  s.panel_setup = 0;
}
// The displays placed, by picture, as this flight's routes (Texture
// routing's choices): kept until Taxi Cam closes, saved or not.
void route_placed_displays() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  auto s = draft();
  const auto left = s.left_id, right = s.right_id;
  route_picks(s, setup_items(sample));
  if (s.left_id != left || s.right_id != right)
    publish(s);
}
bool store_aircraft_setup(const win::Status& sample, bool quiet);
bool save_setup_picks(const win::Status& sample, bool quiet);
void apply_setup_picks();
// Displays: a display's area, picked over a large picture of its panel. Each
// gauge the panel's marks name is outlined and labelled; a click on one makes
// it the area, and the frame's handles resize it and its inside moves it.
// Done keeps it for the display (setup.areas), saved with the picks.
struct AreaPicker {
  unsigned side{};
  bool ready{}, dragging{}, done{};
  profiles::DisplayRect area{}, start{};
  int handle = -1;
  POINT from{};
  unsigned texture_width{}, texture_height{};
  win::area_edit::Limits limits{};
  std::vector<std::pair<std::wstring, profiles::DisplayRect>> gauges;
} area_picker;
constexpr int AreaPickerWholeId = 713, AreaPickerInfoId = 712;
// The picture's place in the dialog: the snapshot fitted into its well.
RECT area_picker_picture() {
  const auto well = rectangle(20, 92, 860, 520);
  if (!snapshot_view.image || snapshot_view.width <= 0 || snapshot_view.height <= 0)
    return well;
  const int ww = well.right - well.left, wh = well.bottom - well.top;
  const double fit = std::min(double(ww) / snapshot_view.width, double(wh) / snapshot_view.height);
  const int w = std::max(1, int(snapshot_view.width * fit)), h = std::max(1, int(snapshot_view.height * fit));
  const int left = well.left + (ww - w) / 2, top = well.top + (wh - h) / 2;
  return {left, top, left + w, top + h};
}
// The display's own area when nothing is chosen: a built-in profile's region
// on its own panel, else the whole panel.
profiles::DisplayRect own_setup_area(unsigned side, unsigned tw, unsigned th) {
  const auto* base = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  if (base && side < base->sides && base->panel_textures[side] && setup.names[side] == base->panel_textures[side])
    return profiles::display_rect(*base, side);
  return {0, 0, tw, th};
}
// The texture a display's picture is taken from.
std::uint64_t setup_side_texture(unsigned side) {
  if (side < MaxDisplaySides && setup.textures[side])
    return setup.textures[side];
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  return picked_id(setup_items(sample), side);
}
// Once the picture is in: the texture's size, its gauges, and the area to
// start from (the one chosen, else the display's own, else a lone gauge).
void prepare_area_picker(const win::Status& sample) {
  auto& p = area_picker;
  p.texture_width = sample.snapshot_width;
  p.texture_height = sample.snapshot_height;
  const int tw = static_cast<int>(p.texture_width), th = static_cast<int>(p.texture_height);
  p.gauges.clear();
  for (std::uint32_t i = 0; i < sample.panel_count && i < win::MaxPanelTextures; ++i) {
    if (sample.panels[i].id != snapshot_view.id)
      continue;
    for (std::uint32_t g = 0; g < sample.gauge_count && g < win::MaxPanelGauges; ++g) {
      const auto& gauge = sample.gauges[g];
      if (gauge.panel != i)
        continue;
      p.gauges.push_back({widen(std::string(gauge.name, strnlen(gauge.name, sizeof(gauge.name))).c_str()),
                          {gauge.x, gauge.y, static_cast<unsigned>(gauge.x + gauge.width), static_cast<unsigned>(gauge.y + gauge.height)}});
    }
  }
  const auto* base = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  p.limits = base ? win::area_edit::limits(*base) : win::area_edit::Limits{};
  auto start = setup.areas[p.side];
  if (profiles::empty_area(start)) {
    const auto own = own_setup_area(p.side, p.texture_width, p.texture_height);
    const bool whole = own.left == 0 && own.top == 0 && own.right == p.texture_width && own.bottom == p.texture_height;
    start = whole && p.gauges.size() == 1 ? p.gauges[0].second : own;
  }
  p.area = win::area_edit::fit(start, tw, th, p.limits);
  p.ready = tw > 0 && th > 0;
}
void update_area_picker_info(HWND hwnd) {
  const auto& p = area_picker;
  if (!p.ready)
    return;
  win::UiMessage message;
  message.add(ui_text(L"From {0}, {1} · {2} × {3} of {4} × {5}"),
              {std::to_wstring(p.area.left), std::to_wstring(p.area.top), std::to_wstring(p.area.right - p.area.left),
               std::to_wstring(p.area.bottom - p.area.top), std::to_wstring(p.texture_width), std::to_wstring(p.texture_height)});
  SetDlgItemTextW(hwnd, AreaPickerInfoId, message.text().c_str());
}
void paint_area_picker(HDC dc) {
  const auto well = rectangle(20, 92, 860, 520);
  HBRUSH fill = CreateSolidBrush(Well);
  FillRect(dc, &well, fill);
  DeleteObject(fill);
  if (!snapshot_view.image || !area_picker.ready)
    return;
  const auto picture = area_picker_picture();
  HDC memory = CreateCompatibleDC(dc);
  const auto old_bitmap = SelectObject(memory, snapshot_view.image);
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  StretchBlt(dc, picture.left, picture.top, picture.right - picture.left, picture.bottom - picture.top, memory, 0, 0, snapshot_view.width,
             snapshot_view.height, SRCCOPY);
  SelectObject(memory, old_bitmap);
  DeleteDC(memory);
  const auto& p = area_picker;
  const auto saved = SaveDC(dc);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  SelectObject(dc, small);
  SetBkMode(dc, OPAQUE);
  SetBkColor(dc, Well);
  HPEN gauge_pen = CreatePen(PS_DOT, 1, Blue);
  SelectObject(dc, gauge_pen);
  SetTextColor(dc, Blue);
  for (const auto& [name, rect] : p.gauges) {
    auto box = win::area_edit::to_screen(picture, p.texture_width, p.texture_height, rect);
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    InflateRect(&box, -scale(4), -scale(3));
    DrawTextW(dc, name.c_str(), -1, &box, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  HPEN frame = CreatePen(PS_SOLID, scale(2), Accent);
  SelectObject(dc, frame);
  const auto a = win::area_edit::to_screen(picture, p.texture_width, p.texture_height, p.area);
  Rectangle(dc, a.left, a.top, a.right, a.bottom);
  HPEN outline = CreatePen(PS_SOLID, scale(1), Accent);
  SelectObject(dc, outline);
  SelectObject(dc, GetStockObject(WHITE_BRUSH));
  for (const auto& h : win::area_edit::handles(picture, p.texture_width, p.texture_height, p.area))
    Rectangle(dc, h.x - scale(5), h.y - scale(5), h.x + scale(6), h.y + scale(6));
  RestoreDC(dc, saved);
  DeleteObject(gauge_pen);
  DeleteObject(frame);
  DeleteObject(outline);
}
// The gauge under a point of the picture, -1 for none: the smallest one, as
// gauges can lie inside others.
int area_picker_gauge_at(POINT point) {
  const auto& p = area_picker;
  const auto picture = area_picker_picture();
  int best = -1;
  std::uint64_t best_size = UINT64_MAX;
  for (std::size_t i = 0; i < p.gauges.size(); ++i) {
    const auto box = win::area_edit::to_screen(picture, p.texture_width, p.texture_height, p.gauges[i].second);
    const auto& r = p.gauges[i].second;
    // A gauge filling the panel is the Whole panel button's area.
    if (r.left == 0 && r.top == 0 && r.right >= p.texture_width && r.bottom >= p.texture_height)
      continue;
    const std::uint64_t size = std::uint64_t{r.right - r.left} * (r.bottom - r.top);
    if (PtInRect(&box, point) && size < best_size) {
      best = static_cast<int>(i);
      best_size = size;
    }
  }
  return best;
}
INT_PTR CALLBACK area_picker_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  auto& p = area_picker;
  if (message == WM_INITDIALOG) {
    place_dialog(hwnd, tr(L"Taxi Cam — Display area"), 900, 700);
    win::UiMessage title;
    title.add(ui_text(L"Display area: {0}"), {display_side_title(draft(), p.side)});
    dialog_control(hwnd, L"STATIC", title.text().c_str(), -1, 20, 17, 860, 28, 0, heading);
    dialog_control(hwnd, L"STATIC",
                   tr(L"Click a gauge to use it, or drag the frame and its handles. The cameras are drawn inside this area."), -1, 20, 54,
                   860, 30);
    dialog_control(hwnd, L"STATIC", L"", AreaPickerInfoId, 20, 530, 860, 24, 0, small);
    dialog_control(hwnd, L"BUTTON", tr(L"Whole panel"), AreaPickerWholeId, 20, 650, 140, 34, WS_TABSTOP);
    dialog_control(hwnd, L"BUTTON", tr(L"Cancel"), IDCANCEL, 652, 650, 110, 34, WS_TABSTOP);
    const auto done = dialog_control(hwnd, L"BUTTON", tr(L"Done"), IDOK, 772, 650, 108, 34, WS_TABSTOP | BS_DEFPUSHBUTTON);
    SetTimer(hwnd, 1, 200, nullptr);
    SetFocus(done);
    hide_focus_cues(hwnd);
    return FALSE;
  }
  if (message == WM_SETCURSOR) {
    if (button_cursor(hwnd, w, l)) {
      SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
      return TRUE;
    }
    if (p.ready && reinterpret_cast<HWND>(w) == hwnd && LOWORD(l) == HTCLIENT) {
      POINT point{};
      GetCursorPos(&point);
      ScreenToClient(hwnd, &point);
      const int hit = win::area_edit::hit(area_picker_picture(), p.texture_width, p.texture_height, p.area, point, scale(9), scale(6));
      const int gauge = area_picker_gauge_at(point);
      LPCWSTR shape = p.dragging   ? win::area_edit::cursor(p.handle)
                      : hit >= 0   ? win::area_edit::cursor(hit)
                      : gauge >= 0 ? IDC_HAND
                                   : nullptr;
      if (shape) {
        SetCursor(LoadCursorW(nullptr, shape));
        SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
        return TRUE;
      }
    }
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC)
    return dialog_colors(w);
  if (message == WM_TIMER) {
    // Loads the picture once it is taken (the snapshot dialog's loader).
    update_snapshot_dialog(hwnd);
    if (!p.ready && snapshot_view.image) {
      win::Status sample;
      {
        const std::lock_guard lock(app_mutex);
        sample = status;
      }
      prepare_area_picker(sample);
      update_area_picker_info(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return TRUE;
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(hwnd, &paint);
    paint_area_picker(dc);
    EndPaint(hwnd, &paint);
    return TRUE;
  }
  if (message == WM_LBUTTONDOWN && p.ready) {
    const POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
    const int hit = win::area_edit::hit(area_picker_picture(), p.texture_width, p.texture_height, p.area, point, scale(9), scale(6));
    // On the frame or a handle the frame moves or resizes; outside it a click
    // on a gauge makes that gauge the area.
    const int gauge = hit < 0 ? area_picker_gauge_at(point) : -1;
    if (hit >= 0) {
      p.dragging = true;
      p.handle = hit;
      p.start = p.area;
      p.from = point;
      SetCapture(hwnd);
    } else if (gauge >= 0) {
      p.area = win::area_edit::fit(p.gauges[static_cast<std::size_t>(gauge)].second, static_cast<int>(p.texture_width),
                                   static_cast<int>(p.texture_height), p.limits);
      update_area_picker_info(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return TRUE;
  }
  if (message == WM_MOUSEMOVE && p.dragging) {
    const POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
    const auto picture = area_picker_picture();
    const int tw = static_cast<int>(p.texture_width), th = static_cast<int>(p.texture_height);
    const int dx = MulDiv(point.x - p.from.x, tw, std::max<int>(picture.right - picture.left, 1));
    const int dy = MulDiv(point.y - p.from.y, th, std::max<int>(picture.bottom - picture.top, 1));
    p.area = win::area_edit::drag(p.start, p.handle, dx, dy, tw, th, p.limits);
    update_area_picker_info(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    return TRUE;
  }
  if ((message == WM_LBUTTONUP || message == WM_CAPTURECHANGED) && p.dragging) {
    p.dragging = false;
    if (message == WM_LBUTTONUP)
      ReleaseCapture();
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == AreaPickerWholeId && p.ready) {
    p.area = win::area_edit::fit({0, 0, p.texture_width, p.texture_height}, static_cast<int>(p.texture_width),
                                 static_cast<int>(p.texture_height), p.limits);
    update_area_picker_info(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == IDOK) {
    p.done = p.ready;
    EndDialog(hwnd, IDOK);
    return TRUE;
  }
  if ((message == WM_COMMAND && LOWORD(w) == IDCANCEL) || message == WM_CLOSE) {
    EndDialog(hwnd, IDCANCEL);
    return TRUE;
  }
  if (message == WM_DESTROY) {
    KillTimer(hwnd, 1);
    release_snapshot_image();
  }
  return FALSE;
}
// Opens the area picker for a display; Done saves its area with the picks.
void show_area_picker(unsigned side) {
  if (side >= setup.sides || setup.names[side].empty())
    return;
  const auto id = setup_side_texture(side);
  if (!id) {
    notice = tr(L"Place a panel on this display first.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  release_snapshot_image();
  snapshot_view = {};
  snapshot_view.id = id;
  snapshot_view.serial = request_display_snapshot(id);
  snapshot_view.requested_ms = GetTickCount64();
  area_picker = {};
  area_picker.side = side;
  snapshot_dialog_open = true;
  const DialogTemplate layout;
  const auto result = DialogBoxIndirectParamW(instance, &layout.dialog, window, area_picker_dialog, 0);
  snapshot_dialog_open = false;
  if (result == -1) {
    notice = tr(L"Could not open the display area.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (!area_picker.done)
    return;
  // The display's own area is no choice.
  const auto own = own_setup_area(side, area_picker.texture_width, area_picker.texture_height);
  setup.areas[side] = win::area_edit::same(area_picker.area, own) ? profiles::DisplayRect{} : area_picker.area;
  apply_setup_picks();
  InvalidateRect(window, nullptr, FALSE);
}
void refresh_setup_picking();
// One panel per side: placing another side's panel swaps the two.
void place_setup_panel(unsigned side) {
  if (side >= setup.sides || setup.picked.empty())
    return;
  const auto name = setup.picked;
  // A built-in profile's own displays are found by name on panels of their
  // shape: another shape is never drawn on. A lower display added to a pilot
  // pair may be any panel (lower_any_panel).
  const auto* base = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  if (base && side < base->sides) {
    win::Status sample;
    {
      const std::lock_guard lock(app_mutex);
      sample = status;
    }
    const auto items = setup_items(sample);
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& panel) { return panel_name(panel) == name; });
    if (item == items.end() || !fits_built_in(*item)) {
      setup.picked.clear();
      setup.picked_texture = 0;
      notice = tr(L"This aircraft draws its cameras only on panels the size of its own displays. Pick another.");
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
  }
  // Built-in profiles may share a texture between sides (the PMDG 777's DUS).
  for (unsigned other = 0; other < MaxDisplaySides && !setup.built_in; ++other)
    if (other != side && setup.names[other] == name) {
      setup.names[other] = setup.names[side];
      setup.textures[other] = setup.textures[side];
      setup.areas[other] = setup.areas[side];
    }
  // A new panel starts from its own area; the picker opens to choose it.
  if (setup.names[side] != name)
    setup.areas[side] = {};
  setup.names[side] = name;
  setup.textures[side] = setup.picked_texture;
  setup.picked.clear();
  setup.picked_texture = 0;
  apply_setup_picks();
  // Once the drop is handled; only where the user is looking.
  if (IsWindowVisible(window))
    PostMessageW(window, AreaPickerMessage, side, 0);
}
// Swaps the two pilot displays: their pictures look alike, so the setup can
// put each on the other's side (FSLabs A321, 2026-11-03).
void swap_pilot_displays() {
  if (setup.sides < 2 || setup.names[0].empty() || setup.names[1].empty())
    return;
  std::swap(setup.names[0], setup.names[1]);
  std::swap(setup.textures[0], setup.textures[1]);
  std::swap(setup.areas[0], setup.areas[1]);
  setup.testing = (setup.testing & ~3u) | ((setup.testing & 1u) << 1) | ((setup.testing >> 1) & 1u);
  apply_setup_picks();
  refresh_setup_picking();
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
// The picks changed (a drop or a swap): they save at once, like every other
// change, so an aircraft's cameras run there and stay whichever step comes
// next. Picks that cannot save yet stay unsaved and save once they can
// (retry_unsaved_setup, leaving Displays), a cockpit test's after it ends.
// Meanwhile an aircraft being set up runs them as the trial, and a built-in
// profile's are this flight's routes.
void apply_setup_picks() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  setup.unsaved = true;
  setup.leave_warned = false;
  if (!setup.testing && save_setup_picks(sample, true))
    return;
  if (!setup.built_in || setup.testing)
    apply_setup_trial();
  else
    route_placed_displays();
}
// The displays' Test in cockpit, or Place here while a panel is picked.
void refresh_setup_picking() {
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    const bool shown = side < setup.sides;
    if (HWND test = GetDlgItem(window, SetupTestId + static_cast<int>(side)))
      ShowWindow(test, shown && setup.picked.empty() ? SW_SHOWNA : SW_HIDE);
    if (HWND place = GetDlgItem(window, SetupPlaceId + static_cast<int>(side)))
      ShowWindow(place, shown && !setup.picked.empty() ? SW_SHOWNA : SW_HIDE);
  }
  // The swap needs both pilot displays placed.
  if (HWND swap = GetDlgItem(window, SetupSwapId))
    ShowWindow(swap, setup.sides >= 2 && !setup.names[0].empty() && !setup.names[1].empty() ? SW_SHOWNA : SW_HIDE);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot)
    if (HWND card = GetDlgItem(window, SetupCardButtonId + static_cast<int>(slot)))
      InvalidateRect(card, nullptr, FALSE);
  InvalidateRect(window, nullptr, FALSE);
}
// The setup page's gallery cards for the page of panels shown.
void update_setup_buttons() {
  if (page != SetupPage || !GetDlgItem(window, SetupCardButtonId))
    return;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  sync_setup(sample);
  const auto items = setup_items(sample);
  const unsigned pages = std::max(1u, static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns));
  setup.gallery_page = std::min(setup.gallery_page, pages - 1);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto index = setup.gallery_page * GalleryColumns + slot;
    setup.ids[slot] = index < items.size() ? items[index].id : 0;
    setup.card_names[slot] = index < items.size() ? panel_name(items[index]) : std::string();
    HWND card = GetDlgItem(window, SetupCardButtonId + static_cast<int>(slot));
    const bool visible = setup.ids[slot] != 0;
    if (card && (IsWindowVisible(card) != FALSE) != visible)
      ShowWindow(card, visible ? SW_SHOWNA : SW_HIDE);
    if (card && visible)
      InvalidateRect(card, nullptr, FALSE);
  }
  EnableWindow(GetDlgItem(window, SetupPreviousId), setup.gallery_page > 0);
  EnableWindow(GetDlgItem(window, SetupNextId), setup.gallery_page + 1 < pages);
}
void show_drag_ghost();
void move_drag_ghost();
void hide_drag_ghost();
// Dragging a gallery card onto a display places its panel there: past the
// drag distance the main window takes the mouse until the button comes up.
LRESULT CALLBACK card_drag(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_LBUTTONDOWN) {
    setup.drag_slot = GetDlgCtrlID(control) - SetupCardButtonId;
    setup.drag_from = {static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
  } else if (message == WM_MOUSEMOVE && (w & MK_LBUTTON) && setup.drag_slot >= 0 && !setup.dragging) {
    const POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
    const auto slot = static_cast<unsigned>(setup.drag_slot);
    if ((std::abs(point.x - setup.drag_from.x) > GetSystemMetrics(SM_CXDRAG) ||
         std::abs(point.y - setup.drag_from.y) > GetSystemMetrics(SM_CYDRAG)) &&
        slot < GalleryColumns && !setup.card_names[slot].empty()) {
      setup.picked = setup.card_names[slot];
      setup.picked_texture = setup.ids[slot];
      setup.dragging = true;
      setup.drop_side = -1;
      SendMessageW(control, BM_SETSTATE, FALSE, 0);
      SetCapture(window);
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      refresh_setup_picking();
      // The displays show as drop targets, the card as picked, and a copy of
      // the card follows the cursor.
      RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
      show_drag_ghost();
      return 0;
    }
  } else if (message == WM_LBUTTONUP) {
    setup.drag_slot = -1;
  } else if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(control, card_drag, id);
  }
  return DefSubclassProc(control, message, w, l);
}
// Ends a drag: over a display, the panel is placed there; elsewhere it stays
// picked for Place here.
// The display a drag is over, -1 for none.
int drop_side_at(POINT point) {
  for (unsigned side = 0; side < setup.sides && side < MaxDisplaySides; ++side)
    if (PtInRect(&setup.slot_areas[side], point))
      return static_cast<int>(side);
  return -1;
}
void end_card_drag(POINT point) {
  setup.dragging = false;
  setup.drag_slot = -1;
  setup.drop_side = -1;
  hide_drag_ghost();
  ReleaseCapture();
  if (const int side = drop_side_at(point); side >= 0)
    place_setup_panel(static_cast<unsigned>(side));
  refresh_setup_picking();
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
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
  // A camera asked for (the window opened, or turned one on): its display's
  // pictures wait until the bridge has drawn cameras since (Status::stamps),
  // so the bare display does not stand in for a camera still starting.
  bool awaiting{};
  std::uint64_t stamps_from{}, request_stamps{};
  ULONGLONG awaiting_ms{};
  HBITMAP image{};
  int width{}, height{};
  unsigned texture_width{}, texture_height{}, side{};
  // The camera control from before this window turned a camera on, restored
  // when it closes unless the cameras were changed meanwhile.
  bool camera_taken{};
  decltype(win::Settings::follow_taxi) follow_before{};
  decltype(win::Settings::manual_mask) manual_before{}, manual_set{};
  // A setup's door camera preview, which ends with the other previews.
  bool door_preview{};
  decltype(win::Settings::door_on) door_on_before_preview{};
  // The door camera, turned on live while it is selected, as it was before,
  // and the aircraft and flight it was taken in.
  bool door_taken{};
  decltype(win::Settings::door_on) door_on_before{}, door_live_before{};
  decltype(win::Settings::profile) door_profile{};
  decltype(win::Settings::aircraft_session_epoch) door_session{};
  decltype(win::Settings::discovered.key) door_variant{};  // A set-up aircraft's.
  // The aircraft's saved settings when the window opened: Undo returns there.
  // Changes are saved when the window closes.
  win::Settings saved{};
  bool changed{};
  unsigned feed{};  // Selected camera: 0 nose, 1 tail or left wing, 2 right wing or the door camera.
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
// The window shows "Waiting for the camera picture…" until a camera has drawn.
void live_await_camera(std::uint64_t stamps) {
  live_view.awaiting = true;
  live_view.stamps_from = stamps;
  live_view.awaiting_ms = GetTickCount64();
  live_view.taken_ms = 0;
  release_live_image();
}
// A camera that never draws (refused, stopped) shows the display after this.
constexpr ULONGLONG LiveAwaitMs = 20000;
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
  if (side >= MaxDisplaySides || !fresh_status(sample) || !sample.graphics_ready || s.calibration_mask || s.scene_test)
    return;
  auto next = s;
  if (s.profile == profiles::DiscoveredProfileId && !live_view.camera_taken) {
    // A set-up aircraft previews every display it was set up on, the door
    // camera on its own display, as its setup does (one PFD only showed a
    // camera, 2026-11-03). Once: a camera turned off meanwhile stays off,
    // and afterwards only the display shown is kept on, as below.
    preview_cameras(next, profiles::side_mask(profile));
    if (next.manual_mask == s.manual_mask && next.door_on == s.door_on) {
      // Already all on: taken as they are, so this pass is the only one.
      live_view.camera_taken = true;
      live_view.follow_before = s.follow_taxi;
      live_view.manual_before = live_view.manual_set = s.manual_mask;
      live_view.door_preview = false;
      live_view.door_on_before_preview = s.door_on;
      return;
    }
  } else {
    if (live_cameras_on(s, sample, profile) & (1u << side))
      return;
    if (next.follow_taxi && profile.taxi_control != profiles::TaxiControl::manual_only &&
        profile.taxi_control != profiles::TaxiControl::pmdg_dsp_cam) {
      next.manual_mask = sample.taxi_mask & profiles::side_mask(profile);
      next.follow_taxi = 0;
    }
    next.manual_mask |= 1u << side;
  }
  if (!win::valid_settings(next))
    return;
  if (!live_view.camera_taken) {
    live_view.camera_taken = true;
    live_view.follow_before = s.follow_taxi;
    live_view.manual_before = s.manual_mask;
    live_view.door_preview = next.door_on != s.door_on;
    live_view.door_on_before_preview = s.door_on;
  }
  live_view.manual_set = next.manual_mask;
  live_await_camera(sample.stamps);
  publish(next);
  refresh_side_toggles(next);
  InvalidateRect(window, nullptr, FALSE);
}
// The door camera is the selected camera.
bool live_door(const profiles::AircraftProfile& profile) {
  (void)profile;
  return live_view.feed == profiles::DoorFeed;
}
// The aircraft and flight the door camera was taken in are still the ones
// loaded (a set-up aircraft by its variant too).
bool door_hold_aircraft(const win::Settings& s) {
  return live_view.door_taken && s.profile == live_view.door_profile &&
         std::memcmp(s.discovered.key, live_view.door_variant, sizeof(live_view.door_variant)) == 0;
}
bool door_hold_current(const win::Settings& s) {
  return door_hold_aircraft(s) && s.aircraft_session_epoch == live_view.door_session;
}
// Ends the window's hold on the door camera, giving back what it still holds:
// on the aircraft it was taken on, its saved Live or Snapshot, and in the same
// flight its on state. A door camera turned off meanwhile (a shortcut, an
// L:var, a flight change) stays off. Returns the settings to publish.
win::Settings release_door_hold(win::Settings s) {
  if (door_hold_aircraft(s)) {
    if (s.door_live)
      s.door_live = live_view.door_live_before;
    if (s.door_on && door_hold_current(s))
      s.door_on = live_view.door_on_before;
  }
  live_view.door_taken = false;
  return s;
}
// While it is selected the door camera is on, rendering live, on its display.
// Another aircraft or flight ends the hold on the previous one, which is then
// taken afresh from the values loaded now.
void live_keep_door(const win::Settings& loaded) {
  auto s = loaded;
  if (live_view.door_taken && !door_hold_current(s))
    s = release_door_hold(s);
  if (!win::door_camera_set_up(s) || (s.door_on && s.door_live)) {
    if (s.door_live != loaded.door_live && win::valid_settings(s))
      publish(s);
    return;
  }
  auto next = s;
  next.door_on = next.door_live = 1;
  if (!win::valid_settings(next))
    return;
  if (!live_view.door_taken) {
    live_view.door_taken = true;
    live_view.door_on_before = s.door_on;
    live_view.door_live_before = s.door_live;
    live_view.door_profile = s.profile;
    live_view.door_session = s.aircraft_session_epoch;
    std::memcpy(live_view.door_variant, s.discovered.key, sizeof(live_view.door_variant));
  }
  {
    const std::lock_guard lock(app_mutex);
    live_await_camera(status.stamps);
  }
  publish(next);
  InvalidateRect(window, nullptr, FALSE);
}
// The display side shown: the door camera's display while it is selected;
// otherwise the first with a camera on, else the first routed.
std::pair<unsigned, std::uint64_t> live_view_target(const win::Settings& s,
                                                    const win::Status& sample,
                                                    const profiles::AircraftProfile& profile) {
  const auto routed = routed_texture_ids(sample, profile);
  if (live_view.area_mode && live_view.area_side < profile.sides && live_view.area_side < MaxDisplaySides)
    return {live_view.area_side, routed[live_view.area_side]};
  if (live_door(profile) && win::door_camera_set_up(s))
    return {s.door_side, routed[s.door_side]};
  // A taxi camera: never the door camera's display while that shows the door
  // page, unless it is the only one.
  const unsigned door = s.door_on && s.door_side < MaxDisplaySides ? 1u << s.door_side : 0u;
  const unsigned on = live_cameras_on(s, sample, profile) & ~door;
  for (unsigned side = 0; side < profile.sides && side < MaxDisplaySides; ++side)
    if (routed[side] && (on & (1u << side)))
      return {side, routed[side]};
  for (const unsigned skip : {door, 0u})
    for (unsigned side = 0; side < profile.sides && side < MaxDisplaySides; ++side)
      if (routed[side] && !(skip & (1u << side)))
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
      const bool drawn = !live_view.awaiting || live_view.area_mode || live_view.request_stamps > live_view.stamps_from ||
                         now - live_view.awaiting_ms > LiveAwaitMs;
      if (image && !drawn) {
        DeleteObject(image);  // Taken before any camera drew: the bare display.
      } else if (image && GetObjectW(image, sizeof(bitmap), &bitmap) && bitmap.bmWidth > 0 && bitmap.bmHeight) {
        live_view.awaiting = false;
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
    // Another display (the door camera's after a taxi camera's): its picture
    // is loading, so the last one does not stand in for it.
    if (id != live_view.id) {
      release_live_image();
      live_view.taken_ms = live_view.requested_ms = 0;
    }
    live_view.id = id;
    live_view.side = side;
    InvalidateRect(live_view.window, nullptr, FALSE);
  }
  if (profile && live_door(*profile))
    live_keep_door(s);
  else if (profile && id)
    live_keep_camera(s, sample, *profile, side);
  if (!id || !fresh_status(sample) || !sample.graphics_ready || now - live_view.requested_ms < LiveViewIntervalMs)
    return;
  live_view.serial = request_display_snapshot(id);
  live_view.request_stamps = sample.stamps;
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
  const bool showing = IsWindowVisible(window) && !IsIconic(window) &&
                       (page == OverviewPage || page == RoutingPage || page == SetupPage || page == CamerasPage || page == DoorPage);
  if (showing && !thumbnails.showing)
    thumbnails.fresh_after = std::max(thumbnails.fresh_after, now > ThumbnailFreshMs ? now - ThumbnailFreshMs : 0);
  thumbnails.showing = showing;
  const auto s = draft();
  const auto routed = routed_lists(s, sample);
  const auto* camera_profile = profiles::find(s.profile);
  // Every display's texture, the lower display's too: Overview's cards and the
  // door camera step show it.
  std::array<std::uint64_t, MaxDisplaySides> displays{};
  if (camera_profile)
    displays = routed_texture_ids(sample, *camera_profile);
  const auto display = [&](std::uint64_t id) {
    return id && (id == routed[0] || id == routed[1] || std::find(displays.begin(), displays.end(), id) != displays.end());
  };
  const unsigned cameras = camera_profile ? live_cameras_on(s, sample, *camera_profile) | (s.door_on ? 1u << MaxDisplaySides : 0u) : 0u;
  if (cameras != thumbnails.cameras) {
    thumbnails.cameras = cameras;
    thumbnails.retake_ms = now + ThumbnailCameraMs;
  }
  if (thumbnails.retake_ms && now >= thumbnails.retake_ms) {
    thumbnails.retake_ms = 0;
    for (auto& t : thumbnails.items)
      if (display(t.id))
        t.taken_ms = t.tried_ms = 0;
  }
  std::vector<std::uint64_t> wanted;
  const auto want = [&](std::uint64_t id) {
    if (id && std::find(wanted.begin(), wanted.end(), id) == wanted.end())
      wanted.push_back(id);
  };
  want(routed[0]);
  want(routed[1]);
  for (unsigned side = 0; camera_profile && side < camera_profile->sides && side < MaxDisplaySides; ++side)
    want(displays[side]);
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
    const bool gone = !display(t.id) && !find_candidate(sample, t.id) && !listed;
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
  std::uint64_t id{};  // Picture to enlarge; 0 opens Displays.
  int side = -1;       // A Displays slot's picture: opens its area picker.
};
std::vector<HitArea> hit_areas;
const HitArea* hit_area_at(POINT point) {
  for (const auto& area : hit_areas)
    if (PtInRect(&area.bounds, point))
      return &area;
  return nullptr;
}
// The logical width chip() draws `label` at.
int chip_width(HDC dc, const wchar_t* label) {
  SelectObject(dc, small);
  SIZE size{};
  GetTextExtentPoint32W(dc, label, static_cast<int>(std::wcslen(label)), &size);
  return MulDiv(size.cx + scale(16), 96, static_cast<int>(dpi));
}
// Returns the chip's logical width; `limit` (logical, 0 for none) shortens a
// long label with an ellipsis.
int chip(HDC dc, int x, int y, const wchar_t* label, COLORREF color, int limit = 0) {
  SelectObject(dc, small);
  SIZE size{};
  GetTextExtentPoint32W(dc, label, static_cast<int>(std::wcslen(label)), &size);
  RECT r{scale(x), scale(y), scale(x) + size.cx + scale(16), scale(y + 22)};
  if (limit > 0)
    r.right = std::min<LONG>(r.right, scale(x + limit));
  round_box(dc, r, (r.bottom - r.top) / 2, blend(Well, color, 20));
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  RECT inner = r;
  InflateRect(&inner, -scale(8), 0);
  DrawTextW(dc, label, -1, &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  return MulDiv(r.right - r.left, 96, static_cast<int>(dpi));
}
// A route's colour: named, the user's choice, detected, or none found.
COLORREF route_color(RouteSource source) {
  return source == RouteSource::named ? Ok : source == RouteSource::chosen ? Blue : source == RouteSource::none ? Warning : Muted;
}
// How a display was found, in words: the pilot needs that, not the panel's
// internal name.
const wchar_t* route_label(RouteSource source) {
  switch (source) {
    case RouteSource::named:
      return tr(L"Found");
    case RouteSource::chosen:
      return tr(L"Your choice");
    case RouteSource::detected:
      return tr(L"Detected");
    case RouteSource::none:
      break;
  }
  return tr(L"Not found");
}
int route_chip(HDC dc, int x, int y, RouteSource source) {
  return chip(dc, x, y, route_label(source), route_color(source));
}
// Draws a texture's picture, or the part `crop` (texture pixels) of it,
// fitted and centred in box. Returns the drawn rectangle; empty without one.
RECT draw_thumbnail(HDC dc, const RECT& well, std::uint64_t id, const wchar_t* missing, const profiles::DisplayRect* crop = nullptr) {
  // The design's picture well: a dark rounded box, the picture centred in it
  // in its own shape.
  round_box(dc, well, scale(8), Well, Border, scale(1));
  RECT box = well;
  InflateRect(&box, -scale(2), -scale(2));
  const auto* t = find_thumbnail(id);
  if (!id || !t || !t->image || t->width <= 0 || t->height <= 0) {
    const wchar_t* label = !id                                                   ? missing
                           : thumbnails.serial && thumbnails.id == id            ? tr(L"Taking a picture…")
                           : t && t->failed                                      ? tr(L"Not being drawn")
                           : t && t->tried_ms && !t->image && !thumbnails.serial ? tr(L"No picture")
                                                                                 : tr(L"Waiting…");
    // Centred, wrapped to the picture's width.
    SelectObject(dc, small);
    SetTextColor(dc, Muted);
    SetBkMode(dc, TRANSPARENT);
    RECT lines = box;
    DrawTextW(dc, label, -1, &lines, DT_CALCRECT | DT_CENTER | DT_WORDBREAK);
    RECT r = box;
    r.top += std::max<LONG>(0, (box.bottom - box.top - (lines.bottom - lines.top)) / 2);
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
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
// The card a drag carries: a translucent copy under the cursor with the
// panel's picture and name. The mouse passes through it to the displays.
constexpr wchar_t DragGhostClass[] = L"380TaxiCamera.DragGhost";
constexpr int DragGhostWidth = 168, DragGhostHeight = 124;
HWND drag_ghost{};
LRESULT CALLBACK drag_ghost_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCHITTEST)
    return HTTRANSPARENT;
  if (message == WM_ERASEBKGND)
    return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(hwnd, &paint);
    RECT r{};
    GetClientRect(hwnd, &r);
    FillRect(dc, &r, background_brush);
    round_box(dc, r, scale(10), Card, Accent, scale(2));
    // The picture alone: panel names stay internal.
    draw_thumbnail(dc, rectangle(8, 8, DragGhostWidth - 16, DragGhostHeight - 16), setup.picked_texture, L"");
    EndPaint(hwnd, &paint);
    return 0;
  }
  return DefWindowProcW(hwnd, message, w, l);
}
void move_drag_ghost() {
  if (!drag_ghost)
    return;
  POINT cursor{};
  GetCursorPos(&cursor);
  SetWindowPos(drag_ghost, HWND_TOPMOST, cursor.x + scale(14), cursor.y + scale(14), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void show_drag_ghost() {
  if (!drag_ghost) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = drag_ghost_procedure;
    type.hInstance = instance;
    type.lpszClassName = DragGhostClass;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return;
    drag_ghost = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, DragGhostClass,
                                 L"", WS_POPUP, 0, 0, 0, 0, window, nullptr, instance, nullptr);
    if (!drag_ghost)
      return;
    SetLayeredWindowAttributes(drag_ghost, 0, 215, LWA_ALPHA);
  }
  SetWindowPos(drag_ghost, HWND_TOPMOST, 0, 0, scale(DragGhostWidth), scale(DragGhostHeight), SWP_NOMOVE | SWP_NOACTIVATE);
  InvalidateRect(drag_ghost, nullptr, TRUE);
  move_drag_ghost();
}
void hide_drag_ghost() {
  if (drag_ghost)
    ShowWindow(drag_ghost, SW_HIDE);
}
// Overview's Cockpit displays: a card per display side, and the door
// camera's own (side MaxDisplaySides) while it has no display.
struct DisplayCard {
  int x{}, width{};
  unsigned side{};
};
std::vector<DisplayCard> display_cards(const win::Settings& s) {
  const auto* profile = profiles::find(s.profile);
  if (!profile)
    return {};
  std::vector<unsigned> sides;
  for (unsigned side = 0; side < profile->sides && side < MaxDisplaySides; ++side)
    sides.push_back(side);
  const int count = static_cast<int>(sides.size()), gap = 16, width = (PageWidth - gap * (count - 1)) / count;
  std::vector<DisplayCard> cards;
  for (int i = 0; i < count; ++i)
    cards.push_back({PageLeft + i * (width + gap), width, sides[static_cast<size_t>(i)]});
  return cards;
}
// Overview: a card per display. Its picture is what the display's camera
// draws into, cropped from the texture routed to that side, with how it was
// found in a pill on it; under it the display's name and shortcut. The
// switches of the cameras drawn there are at the right (build_controls).
std::wstring hotkey_label(WORD value);
void draw_display_strip(HDC dc, const win::Status& sample) {
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  if (!profile)
    return;
  const auto routed = routed_texture_ids(sample, *profile);
  const int y = DisplayCardTop, row = y + DisplayCardSwitches;
  for (const auto& card : display_cards(s)) {
    const int x = card.x, width = card.width;
    const auto picture = rectangle(x + 12, y + 12, width - 24, DisplayCardPicture);
    panel(dc, x, y, width, DisplayCardHeight);
    const auto id = routed[card.side];
    const auto* t = find_thumbnail(id);
    // A set-up aircraft's side is the whole texture: nothing to crop.
    // A placed lower display's region is in its own panel's pixels, whatever
    // that panel's size (lower_any_panel).
    const bool fits = t && !profile->fit_texture &&
                      ((t->texture_width == profile->width && t->texture_height == profile->height) ||
                       (card.side == MaxDisplaySides - 1 && profiles::lower_any_panel(*profile)));
    const auto crop = profiles::display_rect(*profile, card.side);
    draw_thumbnail(dc, picture, id, tr(L"No display"), fits ? &crop : nullptr);
    hit_areas.push_back({picture, 0});
    // How it was found, once the simulator's displays can be seen at all.
    if (sample.graphics_ready) {
      const auto source = route_source(s, sample, card.side, id);
      chip(dc, x + 20, y + 20, route_label(source), route_color(source), width - 40);
    }
    // Left of its switch.
    const int names = width - 28 - 56;
    const unsigned action = card.side == 0 ? win::CameraLeft : card.side == 1 ? win::CameraRight : win::CameraSd;
    const auto keys = static_cast<WORD>(win::hotkey_control_value(hotkey_saved[action]));
    const bool bound = LOBYTE(keys) != 0;
    text(dc, display_side_title(s, card.side), x + 16, row + (bound ? 4 : 15), names, 22, heading, Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (bound)
      text(dc, hotkey_label(keys).c_str(), x + 16, row + 27, names, 18, mono, Muted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}
// A taxi camera's pane in the 768 x 763 working image (live_feed_at): left,
// top, right, bottom.
std::array<float, 4> pane_box(const profiles::Composition& c, unsigned feed) {
  const float half = (768.f - c.bottom_gap) * 0.5f;
  if (feed == 1)
    return {0, c.tail_top, c.split_bottom ? half : 768.f, 763};
  if (feed == 2)
    return {half + c.bottom_gap, c.tail_top, 768, 763};
  return {0, 0, 768, c.nose_height};
}
// Taxi cameras: a camera's live picture, its pane cropped from the first
// display showing the taxi cameras; nothing while they are off.
void draw_feed_picture(HDC dc, const win::Status& sample, unsigned feed, const RECT& box) {
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  std::uint64_t id = 0;
  unsigned side = 0, on = 0;
  if (profile) {
    const auto routed = routed_texture_ids(sample, *profile);
    on = live_cameras_on(s, sample, *profile);
    // The door camera has its display while it is on.
    for (unsigned k = 0; k < profile->sides && k < MaxDisplaySides && !id; ++k)
      if (routed[k] && (on & (1u << k)) && !(s.door_on && s.door_side == k)) {
        id = routed[k];
        side = k;
      }
  }
  const auto* t = find_thumbnail(id);
  profiles::DisplayRect crop{};
  if (profile && t && t->texture_width && t->texture_height &&
      (profile->fit_texture || (t->texture_width == profile->width && t->texture_height == profile->height) ||
       (side == MaxDisplaySides - 1 && profiles::lower_any_panel(*profile)))) {
    const auto area = profiles::side_content_rect(*profile, side, t->texture_width, t->texture_height);
    const auto b = pane_box(profile->composition, feed);
    const double w = area.right - area.left, h = area.bottom - area.top;
    crop = {area.left + static_cast<unsigned>(b[0] * w / 768), area.top + static_cast<unsigned>(b[1] * h / 763),
            area.left + static_cast<unsigned>(b[2] * w / 768), area.top + static_cast<unsigned>(b[3] * h / 763)};
  }
  draw_thumbnail(dc, box, id, on ? tr(L"No display") : tr(L"Camera off"), crop.right > crop.left ? &crop : nullptr);
  if (id)
    hit_areas.push_back({box, id});
}
// Texture routing: the texture behind each routing list, with the display
// rectangles it serves outlined.
void draw_routing_slots(HDC dc, const win::Status& sample) {
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  const auto routed = routed_lists(s, sample);
  const unsigned lists = routing_lists(s);
  for (unsigned list = 0; list < lists; ++list) {
    const int x = 262 + static_cast<int>(list) * 370, y = 224, width = lists == 1 ? 730 : 360;
    const auto id = routed[list];
    panel(dc, x, y, width, 120, Background);
    const auto box = rectangle(x + 7, y + 7, 106, 106);
    const auto drawn = draw_thumbnail(dc, box, id, tr(L"Not found"));
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
         !id            ? tr(L"Choose a texture below, or wait for detection.")
         : name.empty() ? tr(L"Unnamed texture")
                        : name.c_str(),
         tx, y + 38, tw, 24, normal, id && !name.empty() ? Text : Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (id)
      text(dc, candidate_details(id, candidate).c_str(), tx, y + 62, tw, 22, small, Muted);
    const unsigned side = list && separate_lower_profile(s) ? 2 : list;
    route_chip(dc, tx, y + 88, route_source(s, sample, side, id));
  }
}
// Texture routing gallery: one card per tracked display texture on this page.
void draw_gallery(HDC dc, const win::Status& sample) {
  const auto s = draft();
  const auto items = gallery_items(s, sample);
  const auto routed = routed_lists(s, sample);
  const bool any_named = std::any_of(items.begin(), items.end(), [](const auto& c) { return c.name[0] != 0; });
  if (items.empty()) {
    text(dc,
         sample.graphics_ready ? tr(L"Waiting for cockpit displays to be drawn. Restart Flight only if this stays empty.")
                               : tr(L"Connect Taxi Cam with a flight loaded to see the display textures."),
         262, 488, 730, 60, normal, Muted, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    return;
  }
  const unsigned pages = static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns);
  wchar_t page_label[32];
  std::swprintf(page_label, 32, L"%u / %u", std::min(gallery_page, pages - 1) + 1, pages);
  text(dc, page_label, 806, 386, 60, 32, small, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
    const auto id = gallery_ids[slot];
    const auto* candidate = find_candidate(sample, id);
    if (!id || !candidate)
      continue;
    const int x = 262 + static_cast<int>(slot) * 185, y = 430;
    const bool in_use = id == routed[0] || id == routed[1];
    panel(dc, x, y, 175, 208, Background);
    const auto box = rectangle(x + 6, y + 6, 163, 120);
    draw_thumbnail(dc, box, id, L"");
    hit_areas.push_back({box, id});
    if (in_use)
      chip(dc, x + 12, y + 12, routing_lists(s) == 1 ? tr(L"In use") : list_action(s, id == routed[0] ? 0 : 1), Accent);
    const auto name = candidate_name(candidate);
    text(dc, name.empty() ? tr(L"Unnamed") : name.c_str(), x + 10, y + 130, 155, 22, normal, name.empty() && any_named ? Muted : Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    wchar_t details[64];
    std::swprintf(details, 64, L"#%llu · %u × %u · m%u", static_cast<unsigned long long>(id), candidate->width, candidate->height,
                  candidate->mips);
    text(dc, details, x + 10, y + 152, 155, 20, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}
// Displays: each display with the panel placed on it, then the gallery of
// the cockpit's panels (gallery cards draw themselves: draw_page_button).
void draw_setup(HDC dc, const win::Status& sample) {
  sync_setup(sample);
  const auto items = setup_items(sample);
  const auto s = draft();
  const auto* built_in = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr;
  const auto blocker = setup_blocker(sample);
  const auto picked = widen(setup.picked.c_str());
  const auto hint = !blocker.empty()        ? blocker
                    : !setup.picked.empty() ? tr_format(
                                                  L"Choose Place here on a display for {0}, or drag its picture there. "
                                                  L"Select the picture again to cancel.",
                                                  {picked})
                    : built_in ? std::wstring(tr(L"Select a panel's picture below, then choose its display, or drag it onto one. "
                                                 L"This aircraft's default displays come back with Use default displays (Advanced)."))
                               : std::wstring(tr(L"Select a panel's picture below, then choose its display, or drag it onto one. "
                                                 L"It is saved for this aircraft variant only."));
  // Under the displays, as in the design.
  text(dc, hint.c_str(), PageLeft, SetupSlotTop + SetupSlotHeight + 6, PageWidth, 34, small,
       !blocker.empty()        ? Warning
       : !setup.picked.empty() ? Accent
                               : Muted,
       DT_LEFT | DT_WORDBREAK);
  const int sides = static_cast<int>(setup.sides);
  for (int side = 0; side < sides; ++side) {
    const auto unsigned_side = static_cast<unsigned>(side);
    const auto [x, width] = setup_slot_x(unsigned_side);
    const int y = SetupSlotTop, inner = width - 20;
    setup.slot_areas[unsigned_side] = rectangle(x, y, width, SetupSlotHeight);
    // A picked panel marks every display as a drop target; the one a drag is
    // over lights up.
    const bool over = setup.dragging && setup.drop_side == side;
    panel(dc, x, y, width, SetupSlotHeight, over ? AccentTint : setup.picked.empty() ? Card : blend(Card, AccentTint, 55));
    if (!setup.picked.empty()) {
      // A drop target is outlined in the accent; heavier under the drag.
      round_box(dc, setup.slot_areas[unsigned_side], scale(10), NoColor, over ? Accent : blend(Background, Accent, 55),
                scale(over ? 2 : 1));
    }
    // The design's slot: the display's name and pill, its picture, the panel
    // placed there, then the button (build_controls).
    const auto& name = setup.names[unsigned_side];
    const std::string own = built_in && unsigned_side < built_in->sides && built_in->panel_textures[unsigned_side]
                                ? built_in->panel_textures[unsigned_side]
                            : !built_in && s.profile == profiles::DiscoveredProfileId && unsigned_side < s.discovered.sides
                                ? s.discovered.textures[unsigned_side]
                                : "";
    const wchar_t* state = name.empty() ? tr(L"Not placed") : name == own ? (built_in ? tr(L"Default") : tr(L"Saved")) : tr(L"Your choice");
    const COLORREF state_color = name.empty() ? Warning : name == own ? Muted : Blue;
    const int pill = std::min(chip_width(dc, state), inner / 2);
    chip(dc, x + 10 + inner - pill, y + 10, state, state_color, pill);
    text(dc, display_side_title(s, unsigned_side), x + 10, y + 10, inner - pill - 8, 22, heading, Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const auto id = picked_id(items, unsigned_side);
    const auto box = rectangle(x + 10, y + 40, inner, SetupSlotPicture);
    const auto drawn = draw_thumbnail(dc, box, id, L"");
    // The display's area on its panel, chosen in the area picker.
    if (const auto* thumbnail = id && !name.empty() ? find_thumbnail(id) : nullptr; thumbnail && thumbnail->texture_width) {
      const auto area = profiles::empty_area(setup.areas[unsigned_side])
                            ? own_setup_area(unsigned_side, thumbnail->texture_width, thumbnail->texture_height)
                            : setup.areas[unsigned_side];
      const auto r = win::area_edit::to_screen(drawn, thumbnail->texture_width, thumbnail->texture_height, area);
      HPEN pen = CreatePen(PS_SOLID, scale(2), Accent);
      const auto old_pen = SelectObject(dc, pen);
      const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, r.left, r.top, r.right, r.bottom);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
    }
    if (id)
      hit_areas.push_back({box, id, name.empty() ? -1 : static_cast<int>(unsigned_side)});
    // The picture says which display it is; panel names stay internal.
    if (name.empty())
      text(dc, tr(L"Nothing placed yet"), x + 10, y + 46 + SetupSlotPicture, inner, 18, small, Muted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  panel(dc, PageLeft, SetupGalleryTop, PageWidth, 608 - SetupGalleryTop);
  text(dc, tr(L"All cockpit displays"), 42, SetupGalleryTop + 8, 300, 30, heading);
  if (items.empty()) {
    text(dc, tr(L"No panel pictures yet. Panels appear once the cockpit has been drawn."), 42, SetupGalleryCards + 20, 916, 60, normal,
         Muted, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
  } else {
    const unsigned pages = static_cast<unsigned>((items.size() + GalleryColumns - 1) / GalleryColumns);
    wchar_t page_label[32];
    std::swprintf(page_label, 32, L"%u / %u", std::min(setup.gallery_page, pages - 1) + 1, pages);
    text(dc, page_label, 786, SetupGalleryTop + 8, 60, 30, mono, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}
// A gallery card of Displays: the panel's picture, the displays it is
// placed on, and its size. The picked panel is outlined.
bool draw_page_button(const DRAWITEMSTRUCT& item) {
  const int id = static_cast<int>(item.CtlID);
  if (id < SetupCardButtonId || id >= SetupCardButtonId + static_cast<int>(GalleryColumns))
    return false;
  const auto slot = static_cast<unsigned>(id - SetupCardButtonId);
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto s = draft();
  const auto items = setup_items(sample);
  const auto found = std::find_if(items.begin(), items.end(), [&](const auto& panel) { return panel.id == setup.ids[slot]; });
  const auto& r = item.rcItem;
  HBRUSH surround = CreateSolidBrush(Card);
  FillRect(item.hDC, &r, surround);
  DeleteObject(surround);
  const bool picked = !setup.picked.empty() && setup.picked == setup.card_names[slot];
  const bool hot = hot_button == item.hwndItem;
  round_box(item.hDC, r, scale(10), hot && !picked ? Raised : Background,
            picked ? Accent
            : hot  ? BorderStrong
                   : Border,
            scale(picked ? 2 : 1));
  if (found == items.end())
    return true;
  const RECT box{r.left + scale(6), r.top + scale(6), r.right - scale(6), r.bottom - scale(52)};
  draw_thumbnail(item.hDC, box, setup.ids[slot], L"");
  int chip_y = 12;
  for (unsigned side = 0; side < setup.sides; ++side)
    if (setup.names[side] == panel_name(*found)) {
      chip(item.hDC, 12, chip_y, display_side_title(s, side), Accent);
      chip_y += 26;
    }
  // The picture says which display it is: panel names (an iniBuilds gauge is
  // "ini-crash-report") stay internal, so only its size is written.
  SetBkMode(item.hDC, TRANSPARENT);
  wchar_t details[64];
  std::swprintf(details, 64, L"%u × %u", found->width, found->height);
  SelectObject(item.hDC, small);
  SetTextColor(item.hDC, Muted);
  RECT details_box{r.left + scale(10), r.bottom - scale(48), r.right - scale(10), r.bottom - scale(6)};
  DrawTextW(item.hDC, details, -1, &details_box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  return true;
}
// Selected states of the steps' choice buttons.
bool page_button_selected(int id) {
  const auto s = draft();
  if (id >= DoorWhereId && id <= DoorWhereId + static_cast<int>(MaxDisplaySides))
    return id == DoorWhereId ? s.door_side == win::DoorSideNone : s.door_side == static_cast<std::uint32_t>(id - DoorWhereId - 1);
  if (id == DoorSnapshotId || id == DoorLiveId)
    return (id == DoorLiveId) == (s.door_live != 0);
  return false;
}
void show_whats_new() {
  win::ChangelogFetchResult result;
  if (!changelog_fetcher.take(result))
    return;
  SetDlgItemTextW(window, 515, tr(L"What's new"));
  std::vector<win::ChangelogRelease> releases;
  if (!result.ok || !win::parse_changelog(result.body, releases, nullptr, win::ChangelogParse::lenient)) {
    notice = result.ok ? tr(L"What's new could not be read. Try again later.")
                       : tr(L"Could not load What's new from GitHub. Check your connection and try again.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (!open_whats_new(win::whats_new_blocks(releases, InstalledVersion))) {
    notice = tr(L"Could not open What's new.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  // Opened by itself once per installed version; the link stays.
  whats_new = false;
  notice = notice_before_whats_new;
  if (!preview_ui && !win::record_whats_new_seen(win::settings_directory(), InstalledVersion))
    notice = tr(L"Could not remember that What's new was read. It will show again next launch.");
  InvalidateRect(window, nullptr, FALSE);
}
bool apply(bool save = true) {
  auto settings = draft();
  const wchar_t* field_error{};
  if (!read_fields(settings, &field_error)) {
    notice = field_error ? tr(field_error)
             : page == 5 ? tr(L"Guide X must be 0–50%; Y must be 0–100%. Enter finite numbers.")
                         : tr(L"Check the values: brightness −4 to +2 EV in 0.25 steps, lens 0.05–1.55.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  // While the live view holds the door camera on this aircraft, the file
  // keeps the aircraft's own Live or Snapshot (the user's latest pick), not
  // the view's forced Live (an aircraft switch saves before the hold ends).
  auto saved = settings;
  if (door_hold_aircraft(saved) && saved.door_live)
    saved.door_live = live_view.door_live_before;
  // Every change is saved; a setup's unsaved trial is not the user's profile.
  if (!setup.trial && !win::save_settings(saved)) {
    notice = tr(L"Could not save settings. Check access to your local settings folder.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  publish(settings);
  dirty = false;
  autosave_due = 0;
  // save: the user changed a setting (else a switch applied pending fields).
  notice = save ? tr(L"Saved. Adjustments apply while the cameras are running.") : tr(L"Settings updated.");
  if (save && hotkey_registration.conflicts())
    notice = tr(L"Saved. Some shortcuts are unavailable; check Aircraft > Taxi cameras.");
  InvalidateRect(window, nullptr, FALSE);
  return true;
}
// Runs on the UI thread, including when hidden to the tray.
// Display choices saved before panel marks name displays by panel.cfg
// texture= (legacy_display_names.hpp); a catalog profile can too. Converted
// once per aircraft from its own panel.cfg files and saved, so the bridge
// finds them by their marks.
bool legacy_display_name(const char* name) {
  return name && name[0] && !mark_identity::is_key(name);
}
// Whether the displays in use still name a panel.cfg texture=.
bool legacy_displays(const win::Settings& s) {
  const auto* base = profiles::base_profile(s.profile);
  const bool set_up = s.profile == profiles::DiscoveredProfileId;
  for (unsigned side = 0; side < MaxDisplaySides; ++side)
    if (legacy_display_name(s.display_override.textures[side]) ||
        (set_up && side < s.discovered.sides && legacy_display_name(s.discovered.textures[side])) ||
        (!set_up && base && side < base->sides && legacy_display_name(base->panel_textures[side]) && !s.display_override.textures[side][0]))
      return true;
  return false;
}
std::string legacy_attempt(const win::Status& sample, std::uint32_t profile) {
  return std::string(sample.aircraft_path, strnlen(sample.aircraft_path, sizeof(sample.aircraft_path))) + "|" + std::to_string(profile);
}
// The aircraft and profile last converted.
std::string legacy_tried;
// A conversion left displays to choose: a pop-up says so once, the next time
// the window is on screen (prompt_choose_displays).
bool choose_displays_prompt{};
// The loaded aircraft's displays could not all be converted (an encrypted
// Marketplace panel.cfg, the iniBuilds A380): until the user chooses them,
// Overview says so.
bool displays_to_choose(const win::Status& sample) {
  if (!fresh_status(sample) || !sample.identity_sample_ms || legacy_tried.empty())
    return false;
  const auto s = draft();
  return legacy_tried == legacy_attempt(sample, s.profile) && legacy_displays(s);
}
// The attached simulator's Community folder, once the panel marks package
// has been checked for it (check_marks_package, on the connection worker).
std::mutex marks_community_mutex;
std::filesystem::path marks_community;
// The other packages between Taxi Cam and the cockpit pages at that check
// (marks_package::in_the_way), comma separated.
std::wstring marks_in_the_way;
std::wstring packages_in_the_way() {
  const std::lock_guard lock(marks_community_mutex);
  return marks_in_the_way;
}
void convert_legacy_displays() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  if (!fresh_status(sample) || !sample.identity_sample_ms)
    return;
  const auto s = draft();
  const auto* base = profiles::base_profile(s.profile);
  const bool set_up = s.profile == profiles::DiscoveredProfileId;
  const auto legacy = legacy_display_name;
  if (!legacy_displays(s))
    return;
  // Only from the panel.cfg of the aircraft these settings are for: a profile
  // kept over a newly loaded aircraft (Auto aircraft waits for a second
  // sample, finds no profile, or is off) is converted once its own aircraft
  // is loaded.
  const bool own =
      set_up ? profiles::same_variant(std::string_view(sample.aircraft_path, strnlen(sample.aircraft_path, sizeof(sample.aircraft_path))),
                                      s.discovered.key)
             : sample.detected_profile && profiles::base_profile(sample.detected_profile) == base;
  if (!own)
    return;
  const std::string path(sample.aircraft_path, strnlen(sample.aircraft_path, sizeof(sample.aircraft_path)));
  const auto attempt = legacy_attempt(sample, s.profile);
  if (attempt == legacy_tried)
    return;
  legacy_tried = attempt;
  // Community panel.cfg files come from the attached simulator's folder.
  std::filesystem::path community;
  {
    const std::lock_guard lock(marks_community_mutex);
    community = marks_community;
  }
  if (community.empty())
    community = win::marks_package::community_folder(std::filesystem::path(expected_simulator).parent_path());
  const auto sections = win::legacy_names::aircraft_sections(community, path);
  auto next = s;
  bool changed = false, missing = false;
  const auto put = [&](char (&name)[profiles::MaxPanelName], const char* from) {
    const auto key = win::legacy_names::key_for(sections, from);
    if (!key[0]) {
      missing = true;
      return;
    }
    std::fill(std::begin(name), std::end(name), '\0');
    std::strncpy(name, key.data(), profiles::MaxPanelName - 1);
    changed = true;
  };
  for (unsigned side = 0; side < MaxDisplaySides; ++side) {
    if (legacy(next.display_override.textures[side]))
      put(next.display_override.textures[side], s.display_override.textures[side]);
    else if (!set_up && base && side < base->sides && legacy(base->panel_textures[side]) && !s.display_override.textures[side][0])
      put(next.display_override.textures[side], base->panel_textures[side]);
    if (set_up && side < next.discovered.sides && legacy(next.discovered.textures[side]))
      put(next.discovered.textures[side], s.discovered.textures[side]);
  }
  if (!changed || !win::valid_settings(next) || !win::save_settings(next)) {
    notice = tr(L"Taxi Cam could not convert this aircraft's saved displays. Choose them again in Displays.");
    choose_displays_prompt = true;
    build_controls();
    return;
  }
  publish(next);
  notice = missing ? tr(L"Some of this aircraft's saved displays could not be converted. Check them in Displays.")
                   : tr(L"This aircraft's saved displays now use the new display names.");
  choose_displays_prompt = missing;
  build_controls();
}
// The panel marks package (marks_package.hpp), checked once a run on the
// connection worker for the simulator Taxi Cam attaches to; a new or updated
// package takes effect at the next simulator start.
// The Community folder is the attached simulator's own: Steam and Store
// builds on one PC have one each (Steam test, 2026-11-04: the Store package
// was checked while Steam ran without one).
std::atomic<int> marks_package_outcome{-1};
bool package_conflict() noexcept {
  return marks_package_outcome.load(std::memory_order_acquire) == static_cast<int>(win::marks_package::Outcome::conflict);
}
// The check's result and any other package that replaces the files every
// cockpit page loads, in launcher.log and bridge.log: bridge.log is the file
// reporters attach, and a package that is not active leaves no other trace.
// The profile folder and account names are replaced as in report files.
void log_marks_package(win::marks_package::Outcome outcome, const std::filesystem::path& community) noexcept {
  try {
    // Without a settings folder there are no logs to write, as for the bridge.
    const auto directory = win::settings_directory();
    if (directory.empty())
      return;
    const auto names = [](const std::vector<std::wstring>& list) {
      std::wstring out;
      for (const auto& name : list)
        out += (out.empty() ? L"" : L",") + name;
      return out.empty() ? std::wstring(L"none") : out;
    };
    // mode: the file ours replaces (page, or a script while another package
    // takes the page); carrier: the script the package on disk replaces;
    // *_packages: packages with a copy; *_takers: those whose manifest lists
    // it.
    const auto overrides = win::marks_package::other_page_overrides(community);
    const auto taken = win::marks_package::takers(community);
    const auto carrier = win::marks_package::written_carrier(community);
    const auto line = std::wstring(L"Display package: outcome=") + widen(win::marks_package::outcome_name(outcome)) + L" community=" +
                      (community.empty() ? std::wstring(L"none") : community.wstring()) + L" mode=" +
                      widen(win::marks_package::mode_name(win::marks_package::mode_for(taken))) + L" other_page_packages=" +
                      names(overrides.page) + L" page_takers=" + names(taken.page) + L" script_packages=" + names(overrides.script) +
                      L" script_takers=" + names(taken.script) + L" base_instrument_packages=" + names(overrides.base_instrument) +
                      L" logic_packages=" + names(overrides.logic) + L" carrier=" + (carrier.empty() ? L"none" : widen(carrier.c_str()));
    const auto text = win::redact(win::report_utf8(line), win::report_redactions());
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring redacted(static_cast<std::size_t>(wide > 0 ? wide : 0), L'\0');
    if (wide > 0)
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), redacted.data(), wide);
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t stamp[32]{};
    std::swprintf(stamp, 32, L"%04u-%02u-%02uT%02u:%02u:%02uZ ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    win::append_launcher_log(directory, stamp + redacted + L"\r\n");
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "%llu pid=%lu tid=%lu companion=1 |  | ", static_cast<unsigned long long>(GetTickCount64()),
                  GetCurrentProcessId(), GetCurrentThreadId());
    win::append_rotating_log(directory + L"\\bridge.log", prefix + text + "\r\n", win::BridgeLogBytes);
  } catch (...) {
    // Diagnostics never change the check's result.
  }
}
void check_marks_package(const std::wstring& simulator_exe) {
  if (marks_package_outcome.load(std::memory_order_acquire) >= 0 || simulator_exe.empty())
    return;
  auto outcome = win::marks_package::Outcome::failed;
  std::filesystem::path community;
  try {
    const auto simulator = std::filesystem::path(simulator_exe).parent_path();
    community = win::marks_package::community_folder(simulator);
    {
      const std::lock_guard lock(marks_community_mutex);
      marks_community = community;
    }
    outcome = win::marks_package::ensure(simulator, community, win::marks_package::helper_text());
    std::wstring names;
    for (const auto& name : win::marks_package::in_the_way(community))
      names += (names.empty() ? L"" : L", ") + name;
    const std::lock_guard lock(marks_community_mutex);
    marks_in_the_way = std::move(names);
  } catch (...) {
  }
  log_marks_package(outcome, community);
  marks_package_outcome.store(static_cast<int>(outcome), std::memory_order_release);
}
void show_alert(std::wstring title, std::wstring body, std::wstring primary, std::wstring secondary, std::function<void()> act);
// The package is loaded with the simulator's add-ons, before the simulator
// starts Taxi Cam: a package written or changed now, or one that could not be
// written, means no display can be recognised until the simulator starts
// again, so the pilot is told in front of the simulator.
void service_marks_package_notice() {
  static bool shown = false;
  const auto outcome = marks_package_outcome.load(std::memory_order_acquire);
  if (shown || outcome < 0)
    return;
  shown = true;
  using win::marks_package::Outcome;
  const wchar_t* title = tr(L"Taxi Cam can't recognise your displays");
  switch (static_cast<Outcome>(outcome)) {
    case Outcome::current:
      return;
    case Outcome::written:
      title = tr(L"Restart the simulator once");
      notice =
          tr(L"Taxi Cam has just set up the package that lets it recognise your cockpit displays, but the simulator had already "
             L"loaded its add-ons. Restart the simulator so Taxi Cam can find your displays.");
      break;
    case Outcome::removed:
    case Outcome::page_changed:
      title = tr(L"Update Taxi Cam");
      notice =
          tr(L"This simulator version changed the cockpit page Taxi Cam uses to recognise your displays, so it can't find them. "
             L"Update Taxi Cam to find them again.");
      break;
    case Outcome::no_community:
      notice = tr(L"Taxi Cam could not find the simulator's Community folder for its display package. Run the Taxi Cam installer again.");
      break;
    case Outcome::conflict:
      notice = package_conflict_text();
      break;
    default:
      notice = tr(L"Taxi Cam could not add its display package to the simulator's Community folder. Run the Taxi Cam installer again.");
      break;
  }
  show_alert(title, notice, tr(L"OK"), L"", {});
  build_controls();
}
// The package is not loaded in this simulator session although Taxi Cam did
// not have to write it (another package's cockpit page loaded after it, or a
// file removed by hand): an alert once a run, unless the restart alert above
// already said so.
void service_package_inactive_alert() {
  static bool shown = false;
  if (shown || preview_ui)
    return;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  if (!package_inactive(sample))
    return;
  shown = true;
  using win::marks_package::Outcome;
  const auto outcome = marks_package_outcome.load(std::memory_order_acquire);
  if (outcome >= 0 && static_cast<Outcome>(outcome) != Outcome::current)
    return;
  show_alert(tr(L"Taxi Cam can't recognise your displays"), package_inactive_text(), tr(L"OK"), L"", {});
}
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
  SetDlgItemTextW(window, 229, switch_label(tr(L"Scene test"), false).c_str());
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
  if (action == win::CameraDoor) {
    if (!win::toggle_door_camera(s)) {
      notice = tr(L"This aircraft has no door camera display. Choose one in Aircraft > Door camera.");
      InvalidateRect(window, nullptr, FALSE);
      return;
    }
    publish(s);
    notice = !s.enabled  ? std::wstring(tr(L"Camera request updated. Choose Connect to enable camera output."))
             : s.door_on ? std::wstring(tr(L"Door camera requested on."))
                         : std::wstring(tr(L"Door camera requested off."));
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  if (const auto* profile = profiles::find(s.profile); profile && !win::camera_action_sides(*profile, action)) {
    notice = tr(L"This aircraft has no lower display camera.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  const auto result = win::request_camera_hotkey(s, action, sample, now);
  if (result == win::CameraHotkeyResult::unavailable) {
    notice = s.enabled ? tr(L"Waiting for current aircraft TAXI-button state. Try the shortcut again when connected.")
                       : tr(L"Choose Connect before using aircraft camera shortcuts.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  publish(s);
  dirty_notice();
  // One whole sentence per camera, so its state is never a lone word.
  win::UiMessage request;
  request.add(s.manual_mask & 1 ? ui_text(L"Left camera requested on.") : ui_text(L"Left camera requested off."));
  request.add(s.manual_mask & 2 ? ui_text(L"Right camera requested on.") : ui_text(L"Right camera requested off."));
  if (const auto* profile = profiles::find(s.profile); profile && profile->sides > 2) {
    request.add(s.manual_mask & 4 ? ui_text(L"Lower camera requested on.") : ui_text(L"Lower camera requested off."));
  }
  if (pmdg_cam_control(s) && s.follow_taxi)
    request.add(ui_text(L"Displays selected with CAM stay on until CAM is pressed again."));
  notice = s.enabled ? request.text() : std::wstring(tr(L"Camera request updated. Choose Connect to enable camera output."));
  refresh_side_toggles(s);
  SetDlgItemTextW(window, 221, switch_label(pmdg_cam_control(s) ? tr(L"CAM button") : tr(L"TAXI buttons"), s.follow_taxi).c_str());
  SetDlgItemTextW(window, 229, switch_label(tr(L"Scene test"), false).c_str());
  InvalidateRect(window, nullptr, FALSE);
}
// Bound cockpit L:vars act like their shortcuts: a momentary press toggles the
// action, a latched variable turns its displays on or off when it changes.
win::LvarBindingTracker lvar_tracker;
void service_lvar_bindings() {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  std::array<win::LvarCommand, win::LvarBindingCount> commands{};
  const auto count = lvar_tracker.update(draft().lvar_bindings, sample.heartbeat ? sample.lvar_generation : 0, sample.lvars, commands);
  if (preview_ui)
    return;
  for (std::size_t i = 0; i < count; ++i) {
    const auto command = commands[i];
    if (command.kind != win::LvarCommandKind::toggle &&
        win::camera_action_requested(draft(), sample, command.action) == (command.kind == win::LvarCommandKind::on))
      continue;
    toggle_camera_from_hotkey(command.action);
  }
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
  notice = tr(L"Aircraft detected. Its saved displays and calibration are active.");
  build_controls();
}
// Camera adjustment window. Everything is painted here and hit-tested, so
// the keyboard always reaches the window. Changes apply live like the Camera
// views buttons and save by themselves (autosave); the main window's fields
// follow them so a later save cannot write back older values. Undo returns
// to the values from when the window opened.
constexpr wchar_t LiveViewClass[] = L"TaxiCamLiveView";
// Mount steps (right, up, forward m; pitch, yaw deg; lens rad), scaled by
// live_view.step: fine x0.2, normal x1, coarse x5.
constexpr std::array<double, 6> LiveSteps{0.1, 0.1, 0.5, 0.5, 0.5, 0.02};
constexpr std::array<double, 3> LiveStepScale{0.2, 1, 5};
constexpr std::array<const wchar_t*, 6> LiveFieldNames{ui_text(L"Right"), ui_text(L"Up"),  ui_text(L"Forward"),
                                                       ui_text(L"Pitch"), ui_text(L"Yaw"), ui_text(L"Lens")};
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
  // The page of the selected camera: the taxi cameras', or the door camera's
  // own (one pane over the whole picture).
  profiles::Composition c{};
  unsigned feeds = 2;
  bool guides{}, split{}, door{};
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
  if (l.door)
    return static_cast<int>(profiles::DoorFeed);
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
  return win::area_edit::to_screen(picture, live_view.texture_width, live_view.texture_height, x, y);
}
// The area shown: the one being dragged, else the side's area now.
profiles::DisplayRect live_area_now(const profiles::AircraftProfile& profile) {
  if (live_view.drag == LiveView::Drag::area)
    return live_view.area;
  return profiles::side_rect(profile, live_view.side, live_view.texture_width, live_view.texture_height);
}
// The display shown draws the door camera, over its whole camera area.
bool live_door_page() {
  const auto s = draft();
  return s.door_on && s.door_side == live_view.side;
}
// Display area mode with a picture to edit the area on.
bool live_area_shown() {
  return live_view.area_mode && live_view.image && live_view.texture_width && live_view.texture_height;
}
LiveLayout live_layout(HWND hwnd, const profiles::AircraftProfile* profile) {
  LiveLayout l;
  if (profile) {
    l.c = profile->composition;
    l.feeds = profiles::CameraViews;
    l.split = profile->composition.split_bottom != 0;
    l.door = live_door(*profile);
    l.guides = profile->reference_guides && !l.door;
    if (l.door)
      l.c.nose_height = l.c.divider_top = l.c.divider_bottom = l.c.tail_top = 763;
  }
  RECT client{};
  GetClientRect(hwnd, &client);
  const RECT area{scale(16), scale(92), client.right - scale(336), client.bottom - scale(16)};
  // The camera area of the display texture, at its own aspect; in area mode
  // the whole texture.
  const bool whole = profile && live_area_shown(), door_page = live_door_page();
  double aspect = 768.0 / 763.0;
  if (whole)
    aspect = double(live_view.texture_width) / double(live_view.texture_height);
  else if (profile && live_view.texture_width && live_view.texture_height) {
    const auto inner = profiles::side_content_rect(*profile, live_view.side, live_view.texture_width, live_view.texture_height, door_page);
    if (inner.right > inner.left && inner.bottom > inner.top)
      aspect = double(inner.right - inner.left) / double(inner.bottom - inner.top);
  }
  const int aw = std::max<int>(area.right - area.left, 1), ah = std::max<int>(area.bottom - area.top, 1);
  const int w = std::min(aw, static_cast<int>(ah * aspect)), h = std::min(ah, static_cast<int>(aw / aspect));
  l.picture = {area.left + (aw - w) / 2, area.top + (ah - h) / 2, area.left + (aw - w) / 2 + w, area.top + (ah - h) / 2 + h};
  l.drawn = l.picture;
  if (whole) {
    // The camera inset of the area, where the composition is drawn (at the
    // cameras' shape on a side they were not shaped for; the door camera
    // fills it).
    const auto a = live_area_now(*profile);
    auto inner = profiles::fit_rect(profiles::inset_rect(a, profile->camera_padding),
                                    live_view.side < MaxDisplaySides && !door_page ? profile->fit_aspect[live_view.side] : 0);
    if (inner.right <= inner.left || inner.bottom <= inner.top)
      inner = a;
    const auto from = live_texture_point(l.picture, inner.left, inner.top), to = live_texture_point(l.picture, inner.right, inner.bottom);
    l.drawn = {from.x, from.y, std::max(to.x, from.x + 1), std::max(to.y, from.y + 1)};
  }
  return l;
}
void live_button(HDC dc, const RECT& r, const wchar_t* label, int action, bool selected = false, bool primary = false) {
  live_buttons.push_back({r, action});
  ButtonLook look;
  look.primary = primary;
  look.chosen = selected;
  button_box(dc, r, look, Background);
  SelectObject(dc, normal);
  SetTextColor(dc, button_text(look));
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
// The cameras the window offers: the taxi cameras and the door camera, not
// the idle right wing slot of a two-camera aircraft.
bool live_feed_offered(bool split, unsigned feed) {
  return feed < profiles::CameraViews && (split || feed != 2);
}
unsigned live_next_feed(bool split, unsigned feed) {
  do
    feed = (feed + 1) % profiles::CameraViews;
  while (!live_feed_offered(split, feed));
  return feed;
}
const wchar_t* live_feed_name(bool split, unsigned feed) {
  return feed == profiles::DoorFeed ? tr(L"Door")
         : feed == 0                ? tr(L"Nose")
         : split                    ? (feed == 1 ? tr(L"Left wing") : tr(L"Right wing"))
                                    : tr(L"Tail");
}
const wchar_t* live_camera_title(bool split, unsigned feed) {
  return feed == profiles::DoorFeed ? tr(L"Door camera")
         : feed == 0                ? tr(L"Nose camera")
         : split                    ? (feed == 1 ? tr(L"Left wing camera") : tr(L"Right wing camera"))
                                    : tr(L"Tail camera");
}
// Corner and edge handles of the area: 0-3 corners, 4-7 edge midpoints.
std::array<POINT, 8> live_area_handles(const LiveLayout& l, const profiles::DisplayRect& a) {
  return win::area_edit::handles(l.picture, live_view.texture_width, live_view.texture_height, a);
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
    // The whole rectangle, its edges and all inside it (cameras included),
    // moves it: the area is placed here, cameras are aimed outside this mode.
    const auto from = live_texture_point(l.picture, a.left, a.top), to = live_texture_point(l.picture, a.right, a.bottom);
    const int reach = scale(6);
    if (p.x >= from.x - reach && p.x <= to.x + reach && p.y >= from.y - reach && p.y <= to.y + reach)
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
  if (!live_feed_offered(l.split, live_view.feed))
    live_view.feed = 0;
  // Toolbar: camera, step size, display area, undo and defaults. The camera
  // buttons narrow to keep clear of Defaults (four with a split bottom).
  int offered = 0;
  for (unsigned feed = 0; feed < l.feeds; ++feed)
    offered += live_feed_offered(l.split, feed) ? 1 : 0;
  const int defaults_left = client.right - scale(306);
  const int feed_width = offered ? std::clamp((defaults_left - scale(314)) / offered - scale(8), scale(96), scale(118)) : scale(118);
  int x = scale(16);
  for (unsigned feed = 0; feed < l.feeds; ++feed) {
    if (!live_feed_offered(l.split, feed))
      continue;
    live_button(dc, {x, scale(14), x + feed_width, scale(50)}, live_feed_name(l.split, feed), live_feed + static_cast<int>(feed),
                feed == live_view.feed);
    x += feed_width + scale(8);
  }
  const wchar_t* steps[]{tr(L"Step: Fine"), tr(L"Step: Normal"), tr(L"Step: Coarse")};
  live_button(dc, {x + scale(8), scale(14), x + scale(144), scale(50)}, steps[live_view.step], live_step);
  live_button(dc, {x + scale(152), scale(14), x + scale(288), scale(50)}, tr(L"Display area"), live_area, live_view.area_mode);
  // Changes apply live and are saved when the window closes. Undo returns to
  // the saved settings from when it opened; Defaults to the profile's.
  live_button(dc, {client.right - scale(166), scale(14), client.right - scale(16), scale(50)}, tr(L"Undo changes"), live_undo, false,
              live_view.changed);
  live_button(dc, {client.right - scale(306), scale(14), client.right - scale(176), scale(50)}, tr(L"Defaults"), live_defaults);
  // Caption: which display, and how fresh its picture is.
  const auto now = GetTickCount64();
  std::wstring caption;
  if (!profile)
    caption = tr(L"No aircraft profile selected.");
  else if (!fresh_status(sample) || !sample.graphics_ready)
    caption = tr(L"Choose Connect with a flight loaded to see the cameras.");
  // The door camera without a display: the picture stays on a taxi camera's.
  // With a lower display placed, none is the user's choice in Door camera.
  else if (live_door(*profile) && !win::door_camera_set_up(s))
    caption = profiles::side_mask(*profile) & (1u << (MaxDisplaySides - 1))
                  ? tr(L"Choose the display it fills. The lower display is the usual place.")
                  : tr(L"Place the lower display in Aircraft > Displays to show the door camera there.");
  else if (!live_view.id)
    caption = tr(L"Waiting for a display to be found for this aircraft.");
  else {
    const auto side = display_side_title(s, live_view.side);
    caption = live_view.area_mode ? tr_format(L"{0} · drag a white handle to resize the camera area, or inside it to move it", {side})
                                  : tr_format(L"{0} · drag in a camera picture to aim it, wheel to zoom", {side});
    // Without a fresh picture, say what the bridge is doing (cameras starting,
    // waiting for the display) rather than showing a blank view.
    const bool stale = !live_view.image || (live_view.taken_ms && now - live_view.taken_ms > 3000);
    const auto bridge = widen(std::string(sample.message, strnlen(sample.message, sizeof(sample.message))).c_str());
    if (stale && !bridge.empty())
      caption = bridge;
    else if (live_view.taken_ms && now - live_view.taken_ms > 3000)
      caption = tr_format(L"{0} · picture {1} s old", {caption, std::to_wstring((now - live_view.taken_ms) / 1000)});
  }
  if (!live_view.note.empty() && now - live_view.note_ms < 5000)
    caption = live_view.note;
  live_text(dc, caption.c_str(), {scale(16), scale(58), client.right - scale(336), scale(84)}, small, Muted);
  // Picture: the display's camera area, stretched as the compositor draws it;
  // in display area mode the whole texture.
  const bool area = profile && live_area_shown();
  HBRUSH fill = CreateSolidBrush(Well);
  FillRect(dc, &l.picture, fill);
  DeleteObject(fill);
  bool shown = false;
  if (profile && live_view.image && live_view.texture_width && live_view.texture_height) {
    int sx = 0, sy = 0, sw = live_view.width, sh = live_view.height;
    if (!area) {
      const auto inner =
          profiles::side_content_rect(*profile, live_view.side, live_view.texture_width, live_view.texture_height, live_door_page());
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
              !live_view.id         ? tr(L"No picture")
              : live_view.area_mode ? tr(L"Waiting for the display picture…")
                                    : tr(L"Waiting for the camera picture…"),
              r, normal, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  if (profile) {
    // The selected camera's pane.
    const auto box = l.door ? std::array<float, 4>{0, 0, 768, 763} : pane_box(l.c, live_view.feed);
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
    live_text(dc, tr(L"Display area"), {px, y, px + pw, y + scale(30)}, heading, Text);
    const unsigned sides = profile ? std::min<unsigned>(profile->sides, MaxDisplaySides) : 0;
    const int width = sides ? (pw - static_cast<int>(sides - 1) * scale(8)) / static_cast<int>(sides) : pw;
    for (unsigned side = 0; side < sides; ++side) {
      const int bx = px + static_cast<int>(side) * (width + scale(8));
      live_button(dc, {bx, y + scale(36), bx + width, y + scale(70)}, display_side_title(s, side), live_area_side + static_cast<int>(side),
                  side == live_view.side);
    }
    if (area) {
      const auto a = live_area_now(*profile);
      const auto value =
          tr_format(L"From {0}, {1} · {2} × {3} of {4} × {5}",
                    {std::to_wstring(a.left), std::to_wstring(a.top), std::to_wstring(a.right - a.left), std::to_wstring(a.bottom - a.top),
                     std::to_wstring(live_view.texture_width), std::to_wstring(live_view.texture_height)});
      live_text(dc, value.c_str(), {px, y + scale(74), px + pw, y + scale(100)}, small, Text);
    }
    live_button(dc, {px, y + scale(104), px + pw, y + scale(138)}, tr(L"Reset area"), live_area_reset);
    y += scale(152);
  }
  live_text(dc, live_camera_title(l.split, live_view.feed), {px, y, px + pw, y + scale(30)}, heading, Text);
  const auto& mount = s.mounts[live_view.feed];
  for (int j = 0; j < 6; ++j) {
    const int row = y + scale(40 + j * 44);
    wchar_t value[48];
    std::swprintf(value, 48, L"%.2f %ls", mount[j], LiveFieldUnits[j]);
    live_text(dc, tr(LiveFieldNames[j]), {px, row, px + scale(90), row + scale(40)}, normal, Muted);
    live_text(dc, value, {px + scale(90), row, px + scale(200), row + scale(40)}, normal, Text);
    live_button(dc, {px + scale(204), row + scale(2), px + scale(250), row + scale(38)}, L"−", live_adjust + j * 2);
    live_button(dc, {px + scale(256), row + scale(2), px + pw, row + scale(38)}, L"+", live_adjust + j * 2 + 1);
  }
  y += scale(40 + 6 * 44 + 8);
  live_text(dc,
            live_view.area_mode && live_door_page()
                ? tr(L"The white rectangle is where the door camera is drawn on its display. Drag a handle to resize it or "
                     L"inside it to move it; it applies when you let go. The door camera then renders at that shape, which takes a few "
                     L"seconds.")
            : live_view.area_mode
                ? tr(L"The white rectangle is where this display's camera is drawn on its texture. Drag a handle to resize it or "
                     L"inside it to move it; it applies when you let go. A new shape renders the cameras at that shape, which "
                     L"takes a few seconds; the other display shows them at that shape too.")
                : tr(L"Drag a camera picture to aim it; the wheel zooms. Arrows move right and forward, Page Up/Down up and down, "
                     L"Ctrl+arrows aim, + and − zoom, Tab changes camera, Shift is fine. Drag the white points to move the guides; "
                     L"a tail point snaps level with the other side's, and to the same distance from its edge (hold Alt to place "
                     L"it freely).\n\nChanges show live and are saved at once. Undo changes returns to how they were when this "
                     L"window opened; Defaults restores this camera's and the guides' starting values."),
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
  if (page == CamerasPage || page == DoorPage)
    for (unsigned i = 0; i < profiles::CameraViews; ++i)
      for (unsigned j = 0; j < 6; ++j)
        if (HWND field = GetDlgItem(window, static_cast<int>(i == profiles::DoorFeed ? DoorFieldId + j : 300 + i * 10 + j))) {
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
  auto& mount = s.mounts[live_view.feed];
  if (field == 0 || field == 2) {
    // Right and forward as the camera faces, to the nearest quarter turn: an
    // aft-looking door camera moves the way its picture shows.
    const int quarter = static_cast<int>(std::lround(mount[4] / 90.0)) & 3;
    const bool along = (field == 2) != (quarter % 2 != 0);  // Along the aircraft's forward axis.
    const double sign = quarter == 2 || (quarter == 1 && field == 0) || (quarter == 3 && field == 2) ? -1.0 : 1.0;
    mount[along ? 2 : 0] += step * sign;
  } else {
    mount[field] += step;
  }
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
    live_view.note = tr(L"That area leaves no room for the camera, so it was not applied.");
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
    else if (button.action >= live_feed && button.action < live_feed + static_cast<int>(profiles::CameraViews))
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
    live_view.area = win::area_edit::drag(live_view.area_start, live_view.area_handle, dx, dy, tw, th, win::area_edit::limits(*profile));
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
  // Aim: the camera turns the way the pointer moves, as the simulator's mouse
  // look and Ctrl+arrows do (drag right to look right, down to look down).
  // The lens is the vertical field of view, so a pane's height in pixels
  // spans that many degrees.
  const float pane_rows = live_view.feed == 0 || l.door ? l.c.nose_height
                          : l.c.split_bottom            ? (l.c.bottom_pane_height > 0 ? l.c.bottom_pane_height : live_split_pane(l))
                                                        : 763 - l.c.tail_top;
  const double rows_on_screen = std::max(1.0, pane_rows * double(l.drawn.bottom - l.drawn.top) / 763.0);
  auto& mount = s.mounts[live_view.feed];
  const double degrees_per_pixel = mount[5] * 180.0 / 3.14159265358979323846 / rows_on_screen;
  mount[4] += (p.x - live_view.last.x) * degrees_per_pixel;
  mount[3] -= (p.y - live_view.last.y) * degrees_per_pixel;
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
          if (hit.kind == LiveHit::Kind::area && hit.index >= 0 && hit.index < 9)
            cursor = win::area_edit::cursor(hit.index);
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
          live_view.feed = live_next_feed(profile && profile->composition.split_bottom != 0, live_view.feed);
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
          if (live_view.door_preview)
            s.door_on = live_view.door_on_before_preview;
          if (win::valid_settings(s)) {
            publish(s);
            refresh_side_toggles(s);
          }
        }
        live_view.camera_taken = false;
      }
      // The door camera too: closing never saves the live picture it was
      // given while selected (release_door_hold).
      if (live_view.door_taken) {
        const auto s = release_door_hold(draft());
        if (win::valid_settings(s))
          publish(s);
      }
      // Closing saves what is still waiting for the autosave.
      if (live_view.changed)
        apply();
      live_view.changed = false;
      return 0;
    }
  }
  return DefWindowProcW(hwnd, message, w, l);
}
// Opens the camera adjustment window on a camera, or with a display area to
// adjust: area_side's (the door camera's display), else the display shown.
void open_live_view(unsigned feed = 0, bool area = false, unsigned area_side = MaxDisplaySides) {
  if (live_view.window) {
    live_view.feed = feed;
    live_view.area_mode = area;
    live_view.area_side = area_side < MaxDisplaySides ? area_side : live_view.side;
    InvalidateRect(live_view.window, nullptr, FALSE);
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
  live_view.feed = feed < profiles::CameraViews ? feed : 0;
  live_view.area_mode = area;
  // The cameras may still be starting (opened right after a setup's save).
  if (!area) {
    const std::lock_guard lock(app_mutex);
    live_await_camera(status.stamps);
  }
  if (area_side < MaxDisplaySides)
    live_view.area_side = area_side;
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
  live_view.window = registered ? CreateWindowExW(0, LiveViewClass, tr(L"Taxi Cam — Camera adjustment"), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                                  CW_USEDEFAULT, CW_USEDEFAULT, scale(1100), scale(760), window, nullptr, instance, nullptr)
                                : nullptr;
  if (!live_view.window) {
    notice = tr(L"Could not open the camera adjustment window.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  SendMessageW(live_view.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
  SendMessageW(live_view.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
  // The first timer turns on the camera of the display shown (live_keep_camera).
  service_live_view();
}
void open_aircraft_setup() {
  // No camera adjustment window stays open through the setup: it gives its
  // cameras back now, before a trial or a save records them, and positioning
  // opens a window of its own.
  if (live_view.window)
    DestroyWindow(live_view.window);
  auto s = draft();
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  // The selected built-in profile's own aircraft: change its displays.
  // Otherwise set up the aircraft loaded (or change a set-up aircraft's).
  setup.built_in = setup_built_in(s, sample);
  setup.key = {};  // Start again from the displays in use.
  setup.synced = false;
  setup.stored = setup.unsaved = setup.leave_warned = false;
  // The bridge tracks every panel texture while the setup is open.
  s.panel_setup = 1;
  publish(s);
  page = SetupPage;
  build_controls();
}
// A built-in profile's display choices: saved with the profile and applied
// at once. Saved picks go on to Taxi cameras; the profile's own displays stay
// on Displays to show them.
// stay: a drop's save, which keeps Displays open and the bridge tracking every
// panel for its gallery. False when nothing was saved.
bool save_built_in_displays(bool built_in_only, bool stay = false) {
  const auto* base = profiles::base_profile(setup.built_in);
  if (!base)
    return false;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  auto s = setup.trial ? setup.before : draft();
  setup.trial = false;
  setup.testing = 0;
  const unsigned sides_before = profiles::override_sides(*base, s.display_override);
  s.display_override = built_in_choices(*base, s.display_override);
  apply_setup_areas(s);
  if (built_in_only) {
    // The displays as shipped, with the lower display the aircraft comes
    // with (the iniBuilds A380's system display, say) and its area.
    auto shipped = s;
    if (!win::reset_settings(shipped, win::SettingsReset::displays)) {
      notice = tr(L"Could not save these displays. Check access to your local settings folder.");
      InvalidateRect(window, nullptr, FALSE);
      return false;
    }
    s.display_override = shipped.display_override;
    setup.areas = {};
    win::update_target_assignment(s.left_id, s.right_id, s.route_request, 0, 0);
  } else {
    route_picks(s, setup_items(sample));
  }
  // A newly placed lower display is where the door camera goes. None with the
  // lower display already placed is the user's choice and stays (a drop or
  // swap saves too).
  if (s.door_side == win::DoorSideNone && profiles::override_sides(*base, s.display_override) > base->sides && sides_before <= base->sides)
    s.door_side = MaxDisplaySides - 1;
  // Without its placed lower display the door camera has none.
  win::fit_door_camera(s);
  s.panel_setup = stay ? 1 : 0;
  if (!win::valid_settings(s) || !win::save_settings(s)) {
    notice = tr(L"Could not save these displays. Check access to your local settings folder.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  publish(s);
  dirty = false;
  setup.unsaved = false;
  if (built_in_only) {
    setup.key = {};  // The picks start again from the profile's own.
    setup.synced = false;
    s.panel_setup = 1;
    publish(s);
  } else if (!stay) {
    page = CamerasPage;
  }
  if (!stay)
    build_controls();
  notice =
      built_in_only ? tr(L"The profile's own displays are back.") : tr(L"Saved. These displays are used for this aircraft from now on.");
  InvalidateRect(window, nullptr, FALSE);
  return true;
}
// Learns the layout from the picks and the bridge's panel map, saves it for
// this variant and selects it with the cameras on every placed display.
// Quiet (a drop on Displays) leaves out the notices of picks not finished and
// keeps the bridge tracking every panel for the gallery. False when nothing
// was saved.
bool store_aircraft_setup(const win::Status& sample, bool quiet) {
  std::array<const char*, MaxDisplaySides> names{};
  const unsigned sides = placed_sides();
  for (unsigned side = 0; side < sides; ++side)
    names[side] = setup.names[side].c_str();
  profiles::DiscoveredLayout layout;
  const auto learned = win::learn_layout(sample, names, sides, layout);
  switch (learned) {
    case win::LayoutLearning::learned:
      break;
    // Not placed yet, or the map is pairing again: a quiet save waits.
    case win::LayoutLearning::unknown_panel:
    case win::LayoutLearning::not_paired:
      if (!quiet) {
        notice = learned == win::LayoutLearning::not_paired
                     ? tr(L"This cockpit's panel list is not complete. Wait a few seconds after the cockpit has loaded.")
                     : tr(L"Pick a panel for every display.");
        InvalidateRect(window, nullptr, FALSE);
      }
      return false;
    default:
      setup.unsaved = false;  // These picks never save: the user picks again.
      notice = tr(L"These panels cannot be saved for this aircraft. Pick other panels.");
      InvalidateRect(window, nullptr, FALSE);
      return false;
  }
  // An open adjustment window gives its cameras back first (its own close
  // rules), so the cameras recorded below are the user's, not its previews.
  if (live_view.window)
    DestroyWindow(live_view.window);
  // A cockpit test is not the user's profile: keep the one from before it.
  const auto earlier = setup.trial ? setup.before : draft();
  if (setup.trial ? !win::save_settings(setup.before) : !apply())
    return false;
  setup.trial = false;
  setup.testing = 0;
  const auto s = draft();
  win::Settings next;
  if (!win::load_settings(next, installation, profiles::DiscoveredProfileId, layout.key, &layout)) {
    notice = tr(L"Could not prepare this aircraft's settings.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  next.enabled = s.enabled;
  apply_setup_areas(next);
  const bool same = earlier.profile == profiles::DiscoveredProfileId && profiles::same_variant(layout.key, earlier.discovered.key);
  // A newly placed lower display is where the door camera goes, as on a
  // built-in profile: saved first without one, the variant kept no door
  // display (FSLabs A321, 2026-11-03). A door camera set to none with the
  // lower display already placed is the user's choice and stays.
  if (next.door_side == win::DoorSideNone && layout.sides == MaxDisplaySides && (!same || earlier.discovered.sides < MaxDisplaySides))
    next.door_side = MaxDisplaySides - 1;
  if (!win::prepare_layout_save(next, s, s.auto_profile != 0) || !win::save_settings(next)) {
    notice = tr(L"Could not save this aircraft. Check access to your local settings folder.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  // The cameras the user had on come back when positioning ends: only those
  // from before this setup's first save (later saves start from its previews).
  if (!setup.stored) {
    setup.manual_before = same ? earlier.manual_mask : 0;
    setup.door_on_before = same ? earlier.door_on : 0;
  }
  setup.stored = true;
  setup.unsaved = false;
  // Previews draw every camera on the picked displays while positioning.
  preview_cameras(next, (1u << layout.sides) - 1);
  next.panel_setup = quiet ? 1 : 0;
  next.left_id = s.left_id;
  next.right_id = s.right_id;
  next.lower_id = 0;
  next.route_request = s.route_request;
  route_picks(next, setup_items(sample));
  publish(next);
  dirty = false;
  notice = tr(L"Saved. These displays are used for this aircraft from now on.");
  InvalidateRect(window, nullptr, FALSE);
  return true;
}
// Taxi cameras with the previews on and the camera adjustment window open, so
// the cameras can be positioned from their starting mounts.
void start_positioning() {
  setup.positioning = true;
  // Always a window of its own, so what it gives back is only the setup's.
  if (live_view.window)
    DestroyWindow(live_view.window);
  auto s = draft();
  s.panel_setup = 0;
  publish(s);
  page = CamerasPage;
  build_controls();
  open_live_view();
  // The previews are the window's, as a camera it turns on is: closing it
  // turns them off again (a lower display kept its taxi cameras), back to the
  // cameras from before the setup's first save. Its first pass has just taken
  // the previews as if they were the user's.
  if (live_view.window) {
    live_view.camera_taken = true;
    live_view.follow_before = s.follow_taxi;
    live_view.manual_before = setup.manual_before;
    live_view.manual_set = s.manual_mask;
    live_view.door_preview = true;
    live_view.door_on_before_preview = setup.door_on_before;
  }
  notice = tr(L"The cameras start from default positions. Adjust each one in the live view.");
  InvalidateRect(window, nullptr, FALSE);
}
// A drop saved late, on Displays: once nothing blocks it (the map pairs again
// after a save restarts the cameras). Picks not complete stay the trial.
// Saves the picks where nothing blocks them: a built-in profile's display
// choices, or the aircraft being set up. Quiet: a drop's or a retry's save,
// which stays on Displays. False when nothing was saved.
bool save_setup_picks(const win::Status& sample, bool quiet) {
  if (!setup_blocker(sample).empty())
    return false;
  return setup.built_in ? save_built_in_displays(false, true) : store_aircraft_setup(sample, quiet);
}
void retry_unsaved_setup() {
  if (!setup.unsaved || page != SetupPage || setup.testing || setup.dragging)
    return;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  save_setup_picks(sample, true);
}
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
  if (store_aircraft_setup(sample, false))
    start_positioning();
}
// Leaves the page shown for next: unfinished fields keep it. Leaving the
// aircraft setup ends an unsaved cockpit test and the bridge's panel tracking.
bool leave_page(int next) {
  auto s = draft();
  const wchar_t* field_error{};
  if (!read_fields(s, &field_error)) {
    notice = field_error ? tr(field_error) : tr(L"Finish the current values before changing pages.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  // Placed displays not saved yet (a drop while the map paired again) are
  // saved before Displays is left. If they cannot be now, the first try says
  // why and stays; the next one leaves without them.
  if (page == SetupPage && next != SetupPage && !setup.built_in && setup.unsaved && !setup.names[0].empty() && !setup.names[1].empty() &&
      !setup.leave_warned) {
    win::Status sample;
    {
      const std::lock_guard lock(app_mutex);
      sample = status;
    }
    const auto blocker = setup_blocker(sample);
    if (!blocker.empty() || !store_aircraft_setup(sample, false)) {
      if (!blocker.empty())
        notice = blocker;
      setup.leave_warned = true;
      InvalidateRect(window, nullptr, FALSE);
      return false;
    }
    s = draft();
  }
  // A built-in profile's picks not saved yet (made during a cockpit test)
  // save on the way out; where something blocks that, they stay this
  // flight's routes, and leaving is never held up.
  if (page == SetupPage && next != SetupPage && setup.built_in && setup.unsaved) {
    win::Status sample;
    {
      const std::lock_guard lock(app_mutex);
      sample = status;
    }
    if (save_setup_picks(sample, true))
      s = draft();
  }
  if (page == SetupPage && next != SetupPage)
    end_setup_trial(s);
  if (next != SetupPage)
    s.panel_setup = 0;
  setup.positioning = setup.positioning && next == CamerasPage;
  publish(s);
  return true;
}
void show_page(int next) {
  if (next == page || !leave_page(next))
    return;
  page = next;
  build_controls();
}
// A Setup step's own page; Displays sets up the aircraft loaded.
void open_step(unsigned step) {
  const int next = step == 0 ? SetupPage : step == 1 ? CamerasPage : DoorPage;
  if (next == page || !leave_page(next))
    return;
  if (next == SetupPage) {
    open_aircraft_setup();
    return;
  }
  // Displays saved as they were placed: their cameras are positioned next.
  if (page == SetupPage && next == CamerasPage && setup.stored && !setup.built_in) {
    setup.stored = false;
    start_positioning();
    return;
  }
  page = next;
  build_controls();
}
// Whether Next has displays to save: picks that differ from the ones in use,
// or an aircraft not set up yet.
bool display_picks_changed() {
  const auto s = draft();
  if (const auto* base = setup.built_in ? profiles::base_profile(setup.built_in) : nullptr) {
    const auto choices = built_in_choices(*base, s.display_override);
    for (unsigned side = 0; side < MaxDisplaySides; ++side)
      if (std::strcmp(choices.textures[side], s.display_override.textures[side]) != 0)
        return true;
    return false;
  }
  if (s.profile != profiles::DiscoveredProfileId || !setup.key[0] || !profiles::same_variant(setup.key.data(), s.discovered.key) ||
      placed_sides() != s.discovered.sides)
    return true;
  for (unsigned side = 0; side < placed_sides(); ++side)
    if (setup.names[side] != s.discovered.textures[side])
      return true;
  return false;
}
bool save_controls();
// Next saves this step and goes on; the last step returns to Overview.
// Displays saves only new picks (the setup goes on to Taxi cameras itself);
// without the simulator there are none to save.
void next_step() {
  const int step = setup_step(page);
  if (step < 0)
    return;
  if (page == SetupPage) {
    win::Status sample;
    {
      const std::lock_guard lock(app_mutex);
      sample = status;
    }
    sync_setup(sample);
    if (setup_blocker(sample).empty() && (display_picks_changed() || setup.trial))
      save_aircraft_setup();
    else
      open_step(1);
    return;
  }
  if ((page == CamerasPage || page == DoorPage) && !save_controls())
    return;
  if (!apply())
    return;
  if (step + 1 < static_cast<int>(SetupSteps))
    open_step(static_cast<unsigned>(step) + 1);
  else
    show_page(OverviewPage);
}
// Back goes to the step before without saving; Cancel leaves Displays.
void back_step() {
  const int step = setup_step(page);
  if (step == 0)
    show_page(OverviewPage);
  else if (step > 0)
    open_step(static_cast<unsigned>(step) - 1);
}
// Saved aircraft (Overview): the aircraft that come with Taxi Cam and every
// variant set up, in one list by name. Each can be changed in Setup; only
// the ones set up can be removed or copied.
struct SavedAircraft {
  std::uint32_t profile{};  // A catalog profile, or 0 for a set-up variant.
  std::string key;          // The set-up variant.
  std::wstring label;
  bool in_use{};
};
std::vector<SavedAircraft> saved_list;
const SavedAircraft* selected_saved(HWND hwnd) {
  const auto index = SendDlgItemMessageW(hwnd, 801, LB_GETCURSEL, 0, 0);
  return index >= 0 && static_cast<std::size_t>(index) < saved_list.size() ? &saved_list[static_cast<std::size_t>(index)] : nullptr;
}
// The set-up variant selected in the list, or nothing for a catalog profile.
const std::string* selected_setup(HWND hwnd) {
  const auto* selected = selected_saved(hwnd);
  return selected && !selected->profile ? &selected->key : nullptr;
}
// Remove and Copy to this aircraft follow the selection, and so do the
// resets of an aircraft that comes with Taxi Cam.
void update_saved_buttons(HWND hwnd) {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto* selected = selected_saved(hwnd);
  const bool set_up = selected && !selected->profile, built_in = selected && selected->profile;
  const bool enable[]{set_up, set_up && loaded_variant(sample)[0] != 0, built_in, built_in};
  const int buttons[]{802, 804, 806, 807};
  // A button about to be disabled gives the keyboard focus to the list first
  // (after a Remove, say), so the dialog keeps answering Tab, Enter and Esc.
  for (std::size_t i = 0; i < std::size(buttons); ++i)
    if (!enable[i] && GetFocus() == GetDlgItem(hwnd, buttons[i]))
      SendMessageW(hwnd, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(hwnd, 801)), TRUE);
  for (std::size_t i = 0; i < std::size(buttons); ++i)
    EnableWindow(GetDlgItem(hwnd, buttons[i]), enable[i]);
  SetDlgItemTextW(
      hwnd, 803,
      built_in ? tr(L"Included with Taxi Cam, so it cannot be removed. Reset displays brings back its displays and door camera as they "
                    L"come; Factory reset brings back all of its settings.")
               : L"");
}
void fill_saved_list(HWND hwnd) {
  const auto s = draft();
  saved_list.clear();
  for (const auto* profile : profiles::Catalog) {
    std::wstring label = profile->name;
    const bool in_use = s.profile == profile->id;
    if (in_use)
      label = tr_format(L"{0}   (in use)", {label});
    saved_list.push_back({profile->id, {}, std::move(label), in_use});
  }
  constexpr std::string_view prefix = "SimObjects/";
  for (auto& key : win::saved_discovered_variants()) {
    auto family = profiles::variant_family(key);
    if (family.substr(0, prefix.size()) == prefix)
      family.remove_prefix(prefix.size());
    auto label = widen(std::string(profiles::variant_name(key)).c_str()) + L"   ·   " + widen(std::string(family).c_str());
    const bool in_use = s.profile == profiles::DiscoveredProfileId && profiles::same_variant(key, s.discovered.key);
    if (in_use)
      label = tr_format(L"{0}   (in use)", {label});
    saved_list.push_back({0, std::move(key), std::move(label), in_use});
  }
  std::stable_sort(saved_list.begin(), saved_list.end(),
                   [](const auto& a, const auto& b) { return lstrcmpiW(a.label.c_str(), b.label.c_str()) < 0; });
  // The aircraft in use starts selected.
  HWND list = GetDlgItem(hwnd, 801);
  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  for (std::size_t i = 0; i < saved_list.size(); ++i) {
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(saved_list[i].label.c_str()));
    if (saved_list[i].in_use)
      SendMessageW(list, LB_SETCURSEL, i, 0);
  }
  // The aircraft loaded now, which Copy to this aircraft sets up.
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto loaded = loaded_variant(sample);
  const auto line = loaded[0] ? tr_format(L"Loaded now: {0}", {widen(std::string(profiles::variant_name(loaded.data())).c_str())})
                              : std::wstring(tr(L"Load an aircraft to copy a setup to it."));
  SetDlgItemTextW(hwnd, 805, line.c_str());
  update_saved_buttons(hwnd);
}
void remove_setup_variant(HWND hwnd) {
  const auto* selected = selected_setup(hwnd);
  if (!selected) {
    SetDlgItemTextW(hwnd, 803, tr(L"Select an aircraft you set up to remove it."));
    return;
  }
  const auto key = *selected;
  const auto name = widen(std::string(profiles::variant_name(key)).c_str());
  const auto question = tr_format(L"Remove {0}? Its displays and camera positions are deleted. You can set it up again.", {name});
  if (MessageBoxW(hwnd, question.c_str(), tr(L"Remove saved aircraft"), MB_OKCANCEL | MB_ICONWARNING) != IDOK)
    return;
  const auto s = draft();
  const bool in_use = s.profile == profiles::DiscoveredProfileId && profiles::same_variant(key, s.discovered.key);
  if (!win::remove_discovered_variant(key)) {
    SetDlgItemTextW(hwnd, 803, tr(L"Could not remove it. Check access to your local settings folder."));
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
  fill_saved_list(hwnd);
  SetDlgItemTextW(hwnd, 803, tr_format(L"{0} removed.", {name}).c_str());
}
// Copies the selected set-up aircraft to the aircraft loaded now (another
// preset of it, say), saves it and selects it, so it does not start again.
void copy_setup_variant(HWND hwnd) {
  const auto* selected = selected_setup(hwnd);
  if (!selected) {
    SetDlgItemTextW(hwnd, 803, tr(L"Select an aircraft you set up to copy from."));
    return;
  }
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto source = *selected;
  const auto target = loaded_variant(sample);
  if (!target[0]) {
    SetDlgItemTextW(hwnd, 803, tr(L"Load the aircraft to copy to first."));
    return;
  }
  if (profiles::same_variant(target.data(), source)) {
    SetDlgItemTextW(hwnd, 803, tr(L"That is the aircraft loaded now. Select another one to copy from."));
    return;
  }
  const auto from = widen(std::string(profiles::variant_name(source)).c_str());
  const auto to = widen(std::string(profiles::variant_name(target.data())).c_str());
  const bool replaces = std::any_of(saved_list.begin(), saved_list.end(), [&](const auto& aircraft) {
    return !aircraft.profile && profiles::same_variant(target.data(), aircraft.key);
  });
  const bool other = profiles::variant_family(source) != profiles::variant_family(target.data());
  win::UiMessage question;
  question.add(ui_text(L"Copy {0}'s displays, cameras and guides to {1}?"), {from, to});
  if (replaces)
    question.add(ui_text(L"This replaces {0}'s own setup."), {to});
  if (other)
    question.add(ui_text(L"It is a different aircraft, so its panel names may not match: check the displays with Test in cockpit."));
  if (MessageBoxW(hwnd, question.text().c_str(), tr(L"Copy saved aircraft"),
                  MB_OKCANCEL | (replaces || other ? MB_ICONWARNING : MB_ICONQUESTION)) != IDOK)
    return;
  // Keep edits to the profile in use before it changes.
  if (!apply())
    return;
  const auto s = draft();
  win::Settings next;
  if (!win::copy_discovered_variant(source, target.data(), installation, next)) {
    SetDlgItemTextW(hwnd, 803, tr(L"Could not copy it. Check access to your local settings folder."));
    return;
  }
  next.enabled = s.enabled;
  if (win::prepare_profile_selection(next, s, s.auto_profile != 0) && win::save_settings(next)) {
    // The copied setup is this aircraft's now: a live view's door hold on it
    // ends without giving back the old values, and is taken again from these.
    live_view.door_taken = false;
    publish(next);
  }
  saved_variant = {};
  fill_saved_list(hwnd);
  SetDlgItemTextW(hwnd, 803, tr_format(L"{0} now starts from {1}'s setup and is selected.", {to, from}).c_str());
}
// Resets the selected aircraft that comes with Taxi Cam (win::SettingsReset):
// Reset displays brings back its displays and door camera as shipped, Factory
// reset all of its settings. One not in use is reset in its own file and stays
// unselected.
void reset_saved_aircraft(HWND hwnd, win::SettingsReset kind) {
  const auto* selected = selected_saved(hwnd);
  if (!selected || !selected->profile)
    return;
  const auto* profile = profiles::find(selected->profile);
  if (!profile)
    return;
  const bool factory = kind == win::SettingsReset::factory;
  const std::wstring name = profile->name;
  const auto question =
      factory ? tr_format(
                    L"Factory reset {0}? All of its settings go back to how they come with Taxi Cam, camera positions and guides "
                    L"included.",
                    {name})
              : tr_format(
                    L"Reset {0}'s displays and door camera to how they come with Taxi Cam? Camera positions, guides and your other "
                    L"settings are kept.",
                    {name});
  if (MessageBoxW(hwnd, question.c_str(), factory ? tr(L"Factory reset") : tr(L"Reset displays"), MB_OKCANCEL | MB_ICONWARNING) != IDOK)
    return;
  const bool in_use = draft().profile == selected->profile;
  win::Settings next;
  if (in_use) {
    // An adjustment window gives its cameras back first, and pending edits
    // save, so the reset starts from what is in use.
    if (live_view.window)
      DestroyWindow(live_view.window);
    if (!apply())
      return;
    next = draft();
    // The displays placed this flight (Setup → Displays, Texture routing) are
    // routes of their own: back to automatic, as Use default displays does,
    // so the cameras do not go back to them when the bridge routes again.
    win::update_target_assignment(next.left_id, next.right_id, next.route_request, 0, 0, next.lower_id, 0);
  } else if (!win::load_settings(next, installation, selected->profile) &&
             !(factory && win::load_settings(next, installation, selected->profile, {}, nullptr, true))) {
    // A saved file that cannot be read still takes a factory reset, which
    // needs nothing from it; Reset displays keeps the rest of it, so refuses.
    SetDlgItemTextW(hwnd, 803,
                    factory ? tr(L"Could not reset it. Check access to your local settings folder.")
                            : tr_format(L"{0}'s saved settings cannot be read, so only Factory reset can bring it back.", {name}).c_str());
    return;
  }
  if (!win::reset_settings(next, kind) || !win::save_settings(next, in_use)) {
    SetDlgItemTextW(hwnd, 803, tr(L"Could not reset it. Check access to your local settings folder."));
    return;
  }
  if (in_use) {
    // The reset is this aircraft's setup now: a live view's door hold on it
    // ends without giving back the old values.
    live_view.door_taken = false;
    publish(next);
  }
  // The list is unchanged, so the reset aircraft stays selected.
  SetDlgItemTextW(hwnd, 803,
                  (factory ? tr_format(L"{0} is back to how it comes with Taxi Cam.", {name})
                           : tr_format(L"{0}'s displays and door camera are back to how they come with Taxi Cam.", {name}))
                      .c_str());
}
// The list's buttons draw like the main window's, so a disabled Remove stays
// dark instead of the native light grey.
void draw_dialog_button(const DRAWITEMSTRUCT& item) {
  const bool disabled = (item.itemState & ODS_DISABLED) != 0, down = !disabled && (item.itemState & ODS_SELECTED) != 0;
  const bool focus = (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT);
  FillRect(item.hDC, &item.rcItem, card_brush);
  ButtonLook look;
  look.disabled = disabled;
  look.down = down;
  look.focus = focus;
  look.hot = hot_button == item.hwndItem;
  button_box(item.hDC, item.rcItem, look, Card);
  wchar_t label[160]{};
  GetWindowTextW(item.hwndItem, label, 160);
  RECT r = item.rcItem;
  InflateRect(&r, -scale(10), 0);
  const auto old_font = SelectObject(item.hDC, normal);
  SetTextColor(item.hDC, disabled ? Muted : Text);
  SetBkMode(item.hDC, TRANSPARENT);
  DrawTextW(item.hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(item.hDC, old_font);
}
INT_PTR CALLBACK saved_list_dialog(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_INITDIALOG) {
    place_dialog(hwnd, tr(L"Taxi Cam — Saved aircraft"), 640, 504);
    dialog_control(hwnd, L"STATIC", tr(L"Saved aircraft"), -1, 20, 17, 600, 28, 0, heading);
    dialog_control(hwnd, L"STATIC",
                   tr(L"Every aircraft Taxi Cam has a setup for. Change one in Aircraft while it is loaded. Copy to this aircraft "
                      L"starts the aircraft loaded now from one you set up; Remove deletes one you set up."),
                   -1, 20, 50, 600, 64, 0, small);
    dialog_control(hwnd, L"LISTBOX", L"", 801, 20, 118, 600, 192,
                   WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP);
    dialog_control(hwnd, L"STATIC", L"", 805, 20, 318, 600, 24);
    dialog_control(hwnd, L"STATIC", L"", 803, 20, 346, 600, 44, 0, small);
    // A set-up aircraft's Copy and Remove, then an included aircraft's resets.
    dialog_control(hwnd, L"BUTTON", tr(L"Copy to this aircraft"), 804, 20, 406, 220, 34, WS_TABSTOP | BS_OWNERDRAW);
    dialog_control(hwnd, L"BUTTON", tr(L"Remove"), 802, 250, 406, 220, 34, WS_TABSTOP | BS_OWNERDRAW);
    dialog_control(hwnd, L"BUTTON", tr(L"Reset displays"), 806, 20, 450, 220, 34, WS_TABSTOP | BS_OWNERDRAW);
    dialog_control(hwnd, L"BUTTON", tr(L"Factory reset"), 807, 250, 450, 220, 34, WS_TABSTOP | BS_OWNERDRAW);
    const auto close = dialog_control(hwnd, L"BUTTON", tr(L"Close"), IDCANCEL, 520, 450, 100, 34, WS_TABSTOP | BS_OWNERDRAW);
    fill_saved_list(hwnd);
    SetFocus(close);
    hide_focus_cues(hwnd);
    return FALSE;
  }
  if (message == WM_SETCURSOR && button_cursor(hwnd, w, l)) {
    SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
    return TRUE;
  }
  if (message == WM_CTLCOLORLISTBOX) {
    // The list is a well in the dialog, like a picture's.
    SetBkColor(reinterpret_cast<HDC>(w), Well);
    static const HBRUSH well = CreateSolidBrush(Well);
    return reinterpret_cast<INT_PTR>(well);
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC)
    return dialog_colors(w);
  if (message == WM_MEASUREITEM && reinterpret_cast<MEASUREITEMSTRUCT*>(l)->CtlType == ODT_LISTBOX) {
    reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight = static_cast<UINT>(scale(30));
    SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
    return TRUE;
  }
  if (message == WM_DRAWITEM && reinterpret_cast<const DRAWITEMSTRUCT*>(l)->CtlType == ODT_LISTBOX) {
    // A row: the chosen one tinted, with the accent at its left edge.
    const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(l);
    HBRUSH well = CreateSolidBrush(Well);
    FillRect(item.hDC, &item.rcItem, well);
    DeleteObject(well);
    if (item.itemID != static_cast<UINT>(-1)) {
      const bool chosen = (item.itemState & ODS_SELECTED) != 0;
      RECT row = item.rcItem;
      InflateRect(&row, -scale(4), -scale(2));
      if (chosen) {
        round_box(item.hDC, row, scale(6), blend(Well, Accent, 16));
        const int middle = (row.top + row.bottom) / 2;
        round_box(item.hDC, {row.left, middle - scale(7), row.left + scale(3), middle + scale(7)}, scale(2), Accent);
      }
      std::wstring label(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0))) + 1, L'\0');
      SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
      RECT words{row.left + scale(12), row.top, row.right - scale(8), row.bottom};
      const auto old_font = SelectObject(item.hDC, normal);
      SetBkMode(item.hDC, TRANSPARENT);
      SetTextColor(item.hDC, chosen ? Text : blend(Muted, Text, 60));
      DrawTextW(item.hDC, label.c_str(), -1, &words, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      SelectObject(item.hDC, old_font);
    }
    SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
    return TRUE;
  }
  if (message == WM_DRAWITEM && reinterpret_cast<const DRAWITEMSTRUCT*>(l)->CtlType == ODT_BUTTON) {
    draw_dialog_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(l));
    SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == 801 && HIWORD(w) == LBN_SELCHANGE) {
    update_saved_buttons(hwnd);
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == 802) {
    remove_setup_variant(hwnd);
    return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(w) == 804) {
    copy_setup_variant(hwnd);
    return TRUE;
  }
  if (message == WM_COMMAND && (LOWORD(w) == 806 || LOWORD(w) == 807)) {
    reset_saved_aircraft(hwnd, LOWORD(w) == 807 ? win::SettingsReset::factory : win::SettingsReset::displays);
    return TRUE;
  }
  // Owner-drawn buttons never become the default button, so Enter (IDOK)
  // presses the focused Remove, Copy or reset here; anywhere else it closes.
  if (message == WM_COMMAND && LOWORD(w) == IDOK) {
    const int focused = GetDlgCtrlID(GetFocus());
    if (focused == 802 || focused == 804) {
      focused == 802 ? remove_setup_variant(hwnd) : copy_setup_variant(hwnd);
      return TRUE;
    }
    if (focused == 806 || focused == 807) {
      reset_saved_aircraft(hwnd, focused == 807 ? win::SettingsReset::factory : win::SettingsReset::displays);
      return TRUE;
    }
  }
  if ((message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) || message == WM_CLOSE) {
    EndDialog(hwnd, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}
void show_saved_list() {
  const DialogTemplate layout;
  if (DialogBoxIndirectParamW(instance, &layout.dialog, window, saved_list_dialog, 0) == -1)
    notice = tr(L"Could not open the saved aircraft list.");
  build_controls();
}
// Where a row of controls goes: its shortcut (field, ×), controller button
// (field to select and set, ×) and cockpit L:var (name, trigger), each part
// with its status line below when status_height is not 0.
struct ControlPlace {
  int x{}, y{}, w{};
};
// A camera action's controls: its shortcut and controller button (all
// aircraft) and its cockpit L:var (this aircraft). The first a Setup page
// shows starts the shortcut and button drafts from the saved ones; the drafts
// are kept from step to step.
void control_row(const win::Settings& s,
                 unsigned action,
                 ControlPlace keyboard,
                 ControlPlace controller,
                 ControlPlace lvar_place,
                 int status_height) {
  if (shortcut_window != window) {
    shortcut_window = window;
    hotkey_draft = hotkey_saved;
    button_draft = button_saved;
    button_capture = -1;
  }
  const int i = static_cast<int>(action);
  const auto status = [&](int id, ControlPlace place) {
    if (status_height > 0)
      SendMessageW(child(L"STATIC", L"", id, place.x, place.y + 32, place.w, status_height, SS_ENDELLIPSIS), WM_SETFONT,
                   reinterpret_cast<WPARAM>(small), TRUE);
  };
  auto field = child(HOTKEY_CLASSW, L"", 620 + i, keyboard.x, keyboard.y, keyboard.w - 38, 30);
  SendMessageW(field, WM_SETFONT, reinterpret_cast<WPARAM>(small), TRUE);
  SendMessageW(field, HKM_SETHOTKEY, win::hotkey_control_value(hotkey_draft[action]), 0);
  SetWindowSubclass(field, shortcut_editor, 1, 0);
  frame_field(field);
  button(tr(L"Clear"), 630 + i, keyboard.x + keyboard.w - 32, keyboard.y, 32, 30);
  status(650 + i, keyboard);
  button(L"", 680 + i, controller.x, controller.y, controller.w - 38, 30);
  button(tr(L"Clear"), 690 + i, controller.x + controller.w - 32, controller.y, 32, 30);
  status(675 + i, controller);
  const auto* profile = profiles::find(s.profile);
  const bool available = lvar_action_available(profile, action);
  const auto& binding = s.lvar_bindings[action];
  const std::wstring name(binding.name, binding.name + std::strlen(binding.name));
  auto lvar = child(L"EDIT", name.c_str(), LvarNameId + i, lvar_place.x, lvar_place.y, lvar_place.w - 106, 30, ES_AUTOHSCROLL);
  SendMessageW(lvar, EM_SETLIMITTEXT, win::LvarNameSize + 1, 0);
  frame_field(lvar);
  // The trigger: a button that steps through Momentary, Latched and Equals
  // (its user data, the trigger), narrower with Equals' value field beside it.
  const bool equals = win::lvar_trigger_kind(binding.trigger) == win::LvarTrigger::equals;
  const int trigger_x = lvar_place.x + lvar_place.w - LvarTriggerWidth;
  auto trigger = button(lvar_trigger_label(binding.trigger), LvarTriggerId + i, trigger_x, lvar_place.y,
                        equals ? LvarEqualsWidth : LvarTriggerWidth, 30);
  SetWindowLongPtrW(trigger, GWLP_USERDATA, static_cast<LONG_PTR>(binding.trigger));
  auto value = child(L"EDIT", equals ? std::to_wstring(win::lvar_equals_value(binding.trigger)).c_str() : L"", LvarValueId + i,
                     trigger_x + LvarTriggerWidth - LvarValueWidth, lvar_place.y, LvarValueWidth, 30, ES_NUMBER | ES_CENTER);
  SendMessageW(value, EM_SETLIMITTEXT, 8, 0);
  frame_field(value);
  ShowWindow(value, equals ? SW_SHOW : SW_HIDE);
  if (sidebar_tooltip) {
    TOOLINFOW tip{};
    tip.cbSize = sizeof(tip);
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window;
    tip.uId = reinterpret_cast<UINT_PTR>(trigger);
    tip.lpszText =
        const_cast<wchar_t*>(tr(L"Momentary: each press. Latched: on while it is set. Equals: on while it reads the number in "
                                L"the field beside it, such as a display's page. The line below shows what it reads now."));
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    tip.uId = reinterpret_cast<UINT_PTR>(value);
    tip.lpszText = const_cast<wchar_t*>(tr(L"The value it turns on at, such as a page number"));
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
  }
  EnableWindow(lvar, available);
  EnableWindow(trigger, available);
  EnableWindow(value, available);
  status(LvarStatusId + i, lvar_place);
}
// The taxi camera actions this aircraft has: Left, Right and Both, and Lower
// where it has a lower display.
std::vector<unsigned> taxi_actions(const profiles::AircraftProfile* profile) {
  std::vector<unsigned> actions{win::CameraLeft, win::CameraRight, win::CameraBoth};
  if (profile && win::camera_action_sides(*profile, win::CameraSd))
    actions.push_back(win::CameraSd);
  return actions;
}
// Leaving Setup drops its unsaved shortcut and button edits.
void close_controls_page() {
  if (shortcut_window != window)
    return;
  shortcut_window = nullptr;
  hotkey_editor_focused = false;
  hotkey_draft = hotkey_saved;
  button_draft = button_saved;
  button_capture = -1;
  register_camera_hotkeys();
  update_camera_button_input();
}
// The camera steps' control commands; false for every other command.
bool handle_controls_command(int id, int code) {
  if (shortcut_window != window)
    return false;
  const int actions = static_cast<int>(win::CameraHotkeyNames.size());
  if (id >= 620 && id < 620 + actions) {
    if (code == EN_CHANGE) {
      hotkey_draft[id - 620] = win::hotkey_from_control(static_cast<WORD>(SendDlgItemMessageW(window, id, HKM_GETHOTKEY, 0, 0)));
      refresh_shortcut_status();
    }
    return true;
  }
  if ((id >= 630 && id < 630 + actions) || id == 640) {
    if (id == 640)
      hotkey_draft = win::DefaultCameraHotkeys;
    else
      hotkey_draft[id - 630] = {};
    for (unsigned i = 0; i < win::CameraHotkeyNames.size(); ++i)
      SendDlgItemMessageW(window, 620 + static_cast<int>(i), HKM_SETHOTKEY, win::hotkey_control_value(hotkey_draft[i]), 0);
    refresh_shortcut_status();
    return true;
  }
  if (id >= 680 && id < 680 + actions) {
    start_button_capture(id - 680);
    update_camera_button_input();
    notice = button_input.enabled()
                 ? tr(L"MSFS also receives controller buttons: leave a button you set here unassigned in the simulator.")
                 : tr_format(L"Could not listen for controller buttons (Windows error {0}).", {std::to_wstring(button_input.error())});
    if (!button_input.enabled())
      button_capture = -1;
    InvalidateRect(window, nullptr, FALSE);
    refresh_shortcut_status();
    return true;
  }
  if (id >= 690 && id < 690 + actions) {
    if (button_capture == id - 690)
      button_capture = -1;
    button_draft[id - 690] = {};
    update_camera_button_input();
    refresh_shortcut_status();
    return true;
  }
  if (id >= LvarTriggerId && id < LvarTriggerId + actions) {
    const auto trigger = GetDlgItem(window, id);
    const auto current = static_cast<std::uint32_t>(GetWindowLongPtrW(trigger, GWLP_USERDATA));
    auto next = static_cast<std::uint32_t>(win::LvarTrigger::momentary);
    if (win::lvar_trigger_kind(current) == win::LvarTrigger::momentary) {
      next = static_cast<std::uint32_t>(win::LvarTrigger::latched);
    } else if (win::lvar_trigger_kind(current) == win::LvarTrigger::latched) {
      next = win::lvar_equals_trigger(0);
    }
    // Equals shows its value field: what the L:var reads now, else what was
    // typed there before, else 1.
    const auto value = GetDlgItem(window, LvarValueId + (id - LvarTriggerId));
    const bool equals = win::lvar_trigger_kind(next) == win::LvarTrigger::equals;
    if (equals && value) {
      win::Status sample;
      {
        const std::lock_guard lock(app_mutex);
        sample = status;
      }
      const auto& read = sample.lvars[static_cast<unsigned>(id - LvarTriggerId)];
      const bool known = sample.heartbeat && sample.lvar_generation && read.read && std::isfinite(read.value) && read.value >= 0 &&
                         read.value <= static_cast<float>(win::LvarEqualsMax);
      if (known)
        SetWindowTextW(value, std::to_wstring(std::lround(read.value)).c_str());
      else if (!GetWindowTextLengthW(value))
        SetWindowTextW(value, L"1");
    }
    if (value)
      ShowWindow(value, equals ? SW_SHOW : SW_HIDE);
    SetWindowPos(trigger, nullptr, 0, 0, scale(equals ? LvarEqualsWidth : LvarTriggerWidth), scale(30), SWP_NOMOVE | SWP_NOZORDER);
    SetWindowLongPtrW(trigger, GWLP_USERDATA, static_cast<LONG_PTR>(next));
    SetWindowTextW(trigger, lvar_trigger_label(next));
    dirty_notice();
    return true;
  }
  if (id >= LvarValueId && id < LvarValueId + actions) {
    if (code == EN_CHANGE)
      dirty_notice();
    return true;
  }
  if (id >= LvarNameId && id < LvarNameId + actions) {
    if (code == EN_CHANGE)
      dirty_notice();
    return true;
  }
  return false;
}
// Next saves the shortcuts and buttons, for all aircraft (the L:vars save
// with the step's other values); one that cannot be saved keeps the step.
bool save_controls() {
  if (shortcut_window != window)
    return true;
  std::wstring error;
  if (!win::valid_camera_hotkeys(hotkey_draft, &error) || !win::valid_camera_buttons(button_draft, &error)) {
    notice = tr(error.c_str());
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  if (!win::save_camera_hotkeys(hotkey_draft, win::settings_directory())) {
    notice = tr(L"Could not save shortcuts. Check access to the local settings folder.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  hotkey_saved = hotkey_draft;
  button_capture = -1;
  // buttons.ini is created only once a button has been set.
  if (button_draft != button_saved && !win::save_camera_buttons(button_draft, win::settings_directory())) {
    notice = tr(L"Keyboard shortcuts saved. Could not save controller buttons. Check access to the local settings folder.");
    InvalidateRect(window, nullptr, FALSE);
    return false;
  }
  button_saved = button_draft;
  register_camera_hotkeys();
  update_camera_button_input();
  refresh_shortcut_status();
  return true;
}
// Saves every change by itself (main window timer): settings once edits pause
// (dirty_notice), shortcuts and controller buttons once no editor is open. A
// setup's unsaved trial is not saved, and shortcuts refused once (a clash)
// wait for the next edit rather than repeating the notice.
win::CameraHotkeys refused_hotkeys{};
win::CameraButtons refused_buttons{};
bool controls_refused{};
void autosave() {
  if (dirty && autosave_due && GetTickCount64() >= autosave_due && !setup.trial) {
    autosave_due = 0;
    apply();
  }
  if (shortcut_window != window || hotkey_editor_focused || button_capture >= 0 ||
      (hotkey_draft == hotkey_saved && button_draft == button_saved) ||
      (controls_refused && hotkey_draft == refused_hotkeys && button_draft == refused_buttons))
    return;
  controls_refused = !save_controls();
  refused_hotkeys = hotkey_draft;
  refused_buttons = button_draft;
}
// The power icon in a live picture's top left corner (device pixels).
RECT power_icon_rect(const RECT& well) {
  return {well.left + scale(6), well.top + scale(6), well.left + scale(34), well.top + scale(34)};
}
// The look icons in its top right corner: black and white in the corner (slot
// 0), film grain beside it (slot 1).
RECT look_icon_rect(const RECT& well, int slot) {
  const int right = well.right - scale(6) - slot * scale(32);
  return {right - scale(28), well.top + scale(6), right, well.top + scale(34)};
}
void icon_tooltip(int id, const RECT& bounds, const wchar_t* tip) {
  if (!sidebar_tooltip)
    return;
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.uFlags = TTF_SUBCLASS;
  info.hwnd = window;
  info.uId = static_cast<UINT_PTR>(id);
  info.rect = bounds;
  info.lpszText = const_cast<wchar_t*>(tip);
  SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}
// Its tooltip, for a picture well at these logical coordinates.
void power_tooltip(int id, int x, int y, int w, int h, const wchar_t* tip) {
  icon_tooltip(id, power_icon_rect(rectangle(x, y, w, h)), tip);
}
bool camera_look_on(int id) {
  const auto s = draft();
  return id >= LookMonoId ? (s.black_and_white >> (id - LookMonoId) & 1u) != 0 : (s.film_grain >> (id - LookGrainId) & 1u) != 0;
}
// A look icon's tooltip: the option, whether it is on, and what a click does.
const wchar_t* look_tooltip_text(int id) {
  const bool on = camera_look_on(id);
  if (id >= LookMonoId)
    return on ? tr(L"Black and white: On. Click to bring the colour back.") : tr(L"Black and white: Off. Click to take the colour out.");
  return on ? tr(L"Film grain: On. Grain and darker edges, like a camera. Click to turn it off.")
            : tr(L"Film grain: Off. Click to add grain and darker edges, like a camera.");
}
// The look icons' tooltips, for camera view `view`'s picture well.
void look_tooltips(unsigned view, int x, int y, int w, int h) {
  const auto well = rectangle(x, y, w, h);
  const int grain = LookGrainId + static_cast<int>(view), mono = LookMonoId + static_cast<int>(view);
  icon_tooltip(grain, look_icon_rect(well, 1), look_tooltip_text(grain));
  icon_tooltip(mono, look_icon_rect(well, 0), look_tooltip_text(mono));
}
// On for the taxi cameras when the left and right cameras both are, as the
// Both action turns them off then; for the door camera while it is on.
bool camera_power_on(int id) {
  const auto s = draft();
  if (id == DoorPowerId)
    return s.door_on != 0;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  const auto* profile = profiles::find(s.profile);
  const unsigned both = profile ? win::camera_action_sides(*profile, win::CameraBoth) : 0u;
  return both && (live_cameras_on(s, sample, *profile) & both) == both;
}
// The live pictures' power icons drawn by draw_page, in device pixels.
struct PowerArea {
  RECT bounds{};
  int id{};
};
std::vector<PowerArea> power_areas;
const PowerArea* power_area_at(POINT point) {
  for (const auto& area : power_areas)
    if (PtInRect(&area.bounds, point))
      return &area;
  return nullptr;
}
// The power symbol in a live picture's top left corner, on the picture with
// no background: the accent while on, light while off, outlined in black so
// it shows on any picture.
void draw_power_icon(HDC dc, const RECT& well, int id) {
  const auto r = power_icon_rect(well);
  power_areas.push_back({r, id});
  const bool on = camera_power_on(id);
  const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2 + scale(1), radius = scale(8);
  const int dx = radius / 2, dy = radius * 866 / 1000;
  const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  for (const bool outline : {true, false}) {
    HPEN pen = CreatePen(PS_SOLID, outline ? scale(5) : scale(2), outline ? RGB(0, 0, 0) : on ? Accent : Text);
    const auto old_pen = SelectObject(dc, pen);
    // The ring, open at the top (counterclockwise from upper left to upper
    // right), and the stroke through the opening.
    Arc(dc, cx - radius, cy - radius, cx + radius, cy + radius, cx - dx, cy - dy, cx + dx, cy - dy);
    MoveToEx(dc, cx, cy - radius - scale(2), nullptr);
    LineTo(dc, cx, cy - scale(1));
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }
  SelectObject(dc, old_brush);
}
// A look icon flips its camera's film grain or black and white at once; it
// saves with the step's other values (Next). Its tooltip follows.
void toggle_camera_look(int id) {
  if (!apply(false))
    return;
  auto s = draft();
  if (id >= LookMonoId)
    s.black_and_white ^= 1u << (id - LookMonoId);
  else
    s.film_grain ^= 1u << (id - LookGrainId);
  publish(s);
  dirty_notice();
  if (sidebar_tooltip) {
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = window;
    info.uId = static_cast<UINT_PTR>(id);
    info.lpszText = const_cast<wchar_t*>(look_tooltip_text(id));
    SendMessageW(sidebar_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
  }
}
// Camera view `view`'s look icons in its picture's top right corner, drawn
// like the power icon: scattered grain, and a circle half filled for black
// and white; the accent while on, light while off, outlined in black.
void draw_look_icons(HDC dc, const RECT& well, unsigned view) {
  const int grain_id = LookGrainId + static_cast<int>(view), mono_id = LookMonoId + static_cast<int>(view);
  const auto grain = look_icon_rect(well, 1), mono = look_icon_rect(well, 0);
  power_areas.push_back({grain, grain_id});
  power_areas.push_back({mono, mono_id});
  const auto dot = [&](int x, int y, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
  };
  // Grain: seven specks, each on a black one a little larger.
  constexpr int specks[][2]{{-6, -6}, {2, -7}, {7, -1}, {-1, -1}, {-7, 4}, {4, 6}, {-2, 7}};
  const int gx = (grain.left + grain.right) / 2, gy = (grain.top + grain.bottom) / 2;
  const COLORREF grain_color = camera_look_on(grain_id) ? Accent : Text;
  for (const bool outline : {true, false})
    for (const auto& s : specks)
      dot(gx + scale(s[0]), gy + scale(s[1]), outline ? scale(3) : scale(2), outline ? RGB(0, 0, 0) : grain_color);
  // Black and white: a ring with its left half filled.
  const int mx = (mono.left + mono.right) / 2, my = (mono.top + mono.bottom) / 2, radius = scale(8);
  const COLORREF mono_color = camera_look_on(mono_id) ? Accent : Text;
  const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  for (const bool outline : {true, false}) {
    HPEN pen = CreatePen(PS_SOLID, outline ? scale(5) : scale(2), outline ? RGB(0, 0, 0) : mono_color);
    const auto old_pen = SelectObject(dc, pen);
    Ellipse(dc, mx - radius, my - radius, mx + radius, my + radius);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }
  HBRUSH fill = CreateSolidBrush(mono_color);
  SelectObject(dc, fill);
  const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
  // Counterclockwise from the top to the bottom: the left half.
  Chord(dc, mx - radius, my - radius, mx + radius + 1, my + radius + 1, mx, my - radius, mx, my + radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(fill);
}
// An aircraft set up for the first time goes through Setup's steps in order,
// with Back and Next; one that has a setup opens any of them directly.
bool guided_setup(const win::Status& sample) {
  return unsupported_aircraft(sample) || setup.positioning;
}
// What the pages were last built for, so a change of either rebuilds them.
bool built_with_displays{}, built_guided{};
void build_controls() {
  refreshing = true;
  if (setup_step(page) < 0)
    close_controls_page();
  if (sidebar_tooltip) {
    DestroyWindow(sidebar_tooltip);
    sidebar_tooltip = nullptr;
  }
  for (HWND h : controls)
    DestroyWindow(h);
  controls.clear();
  navigation.clear();
  const auto s = draft();
  const int step = setup_step(page);
  sidebar_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
  const auto tooltip = [&](HWND control, const wchar_t* words) {
    if (!sidebar_tooltip || !control)
      return;
    TOOLINFOW tip{};
    tip.cbSize = sizeof(tip);
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window;
    tip.uId = reinterpret_cast<UINT_PTR>(control);
    tip.lpszText = const_cast<wchar_t*>(words);
    SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
  };
  win::Status built_for;
  {
    const std::lock_guard lock(app_mutex);
    built_for = status;
  }
  built_with_displays = built_for.graphics_ready != 0;
  built_guided = guided_setup(built_for);
  // The header, the same on every page: the four places, then the menu.
  // Pages open by id (100 + page); Aircraft opens Displays.
  const wchar_t* places[]{tr(L"Home"), tr(L"Aircraft"), tr(L"Options"), tr(L"Help")};
  constexpr int place_ids[]{100 + OverviewPage, 100 + CamerasPage, 100 + PreferencesPage, 100 + DiagnosticsPage};
  for (int k = 0; k < 4; ++k)
    button(places[k], place_ids[k], 190 + k * 104, 10, 96, 36);
  tooltip(button(tr(L"Menu"), 602, 932, 10, 44, 36), tr(L"Menu"));
  if (aircraft_page(page)) {
    // Aircraft's sections, under the header. A step keeps Advanced at its
    // foot; an aircraft being set up for the first time also has Back and
    // Next there (the last finishes). Every change saves by itself.
    for (unsigned k = 0; k < SetupSteps; ++k)
      button(step_name(k), StepPillId + static_cast<int>(k), 352 + static_cast<int>(k) * 158, 62, 150, 32);
    button(tr(L"Picture"), 100 + PicturePage, 352 + 3 * 158, 62, 150, 32);
    if (step >= 0) {
      if (built_guided) {
        const auto back = step == 0 ? std::wstring(tr(L"Cancel")) : tr_format(L"Back: {0}", {step_name(static_cast<unsigned>(step) - 1)});
        button(back.c_str(), BackId, PageLeft, 627, 190, 42);
        const auto next = step + 1 < static_cast<int>(SetupSteps) ? tr_format(L"Next: {0}", {step_name(static_cast<unsigned>(step) + 1)})
                                                                  : std::wstring(tr(L"Finish"));
        button(next.c_str(), NextId, 786, 627, 190, 42);
      }
      button(switch_label(tr(L"Advanced"), advanced).c_str(), AdvancedId, built_guided ? 224 : PageLeft, 631, 140, 34);
    }
  }
  if (page == OverviewPage) {
    // The status card: what to do about an aircraft that needs setting up,
    // then Connect.
    const bool needs_setup = unsupported_aircraft(built_for), needs_displays = !needs_setup && displays_to_choose(built_for);
    button(tr(win::connection_button_label(connection_requested.load(std::memory_order_acquire))), 241, 838, HomeStatusTop + 20, 120, 36);
    if (needs_setup || needs_displays)
      button(needs_setup ? tr(L"Set up this aircraft") : tr(L"Choose displays"), HomeActionId, 628, HomeStatusTop + 20, 200, 36);
    // The aircraft's card: which aircraft, its sections, and the saved ones.
    HWND combo = child(L"COMBOBOX", L"", 210, 42, HomeAircraftTop + 10, 250, 340, CBS_DROPDOWNLIST | WS_VSCROLL);
    center_combo(combo, HomeAircraftTop + 10, 36);
    for (const auto* profile : profiles::Catalog)
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile->name));
    for (size_t i = 0; i < profiles::Catalog.size(); ++i)
      if (profiles::Catalog[i]->id == s.profile)
        SendMessageW(combo, CB_SETCURSEL, i, 0);
    // The set-up aircraft in use follows the catalog, named like the others.
    if (const auto* set_up = s.profile == profiles::DiscoveredProfileId ? profiles::find(s.profile) : nullptr) {
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(set_up->name));
      SendMessageW(combo, CB_SETCURSEL, profiles::Catalog.size(), 0);
    }
    for (unsigned k = 0; k < SetupSteps; ++k)
      button(step_name(k), StepPillId + static_cast<int>(k), 404 + static_cast<int>(k) * 130, HomeAircraftTop + 12, 124, 32);
    button(tr(L"Saved aircraft…"), 646, 798, HomeAircraftTop + 12, 160, 32);
    // Cockpit displays, once the simulator shows them: at the right of each
    // display's card, the switch of the cameras drawn there, as their
    // shortcuts turn them on.
    const auto* profile = profiles::find(s.profile);
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    constexpr int previews[]{224, 225, 232};
    if (built_with_displays)
      for (const auto& card : display_cards(s))
        button(switch_label(side_toggle_name(s, card.side), s.manual_mask & (1u << card.side)).c_str(), previews[card.side],
               card.x + card.width - 12 - 52, DisplayCardTop + DisplayCardSwitches + 2, 52, 44);
    // In flight: TAXI-button control and the door camera's switch, or a way
    // to choose its display.
    button(switch_label(pmdg_cam_control(s) ? tr(L"CAM button") : tr(L"TAXI buttons"), s.follow_taxi && !manual).c_str(), 221, 32,
           HomeFlightTop + 4, 460, 40);
    EnableWindow(GetDlgItem(window, 221), !manual);
    if (s.door_side < MaxDisplaySides)
      button(switch_label(tr(L"Door camera"), s.door_on).c_str(), DoorToggleId, 508, HomeFlightTop + 4, 460, 40);
    else
      button(tr(L"Choose display…"), DoorChooseId, 818, HomeFlightTop + 8, 150, 32);
    // The footer's links: the version, What's new, and at the right Report a
    // bug and Donate.
    const auto version_link = button(L"v" TAXI_CAM_VERSION_WIDE, 514, PageLeft, 645, 56, 22);
    const HWND whats_new_link = button(changelog_fetcher.busy() ? tr(L"Loading…") : tr(L"What's new"), 515, 84, 645, 150, 22);
    tooltip(version_link, tr(L"Open Taxi Cam on GitHub"));
    tooltip(whats_new_link, tr(L"See what changed in this version and earlier ones"));
    button(tr(L"Report a bug"), 512, 716, 645, 160, 22);
    tooltip(button(tr(L"Donate"), 513, 886, 645, 90, 22), tr(L"Donate via PayPal"));
  } else if (page == CamerasPage) {
    const auto* profile = profiles::find(s.profile);
    const int feed_count = profile ? static_cast<int>(profiles::taxi_feeds(*profile)) : 2;
    const int height = advanced ? CameraCardAdvancedHeight : CameraCardHeight;
    for (int i = 0; i < feed_count; ++i) {
      // One card per taxi camera across the column (draw_page); Advanced adds
      // its six values, three to a row.
      const int card = wide(feed_count == 3 ? 246 : 376), x = PageLeft + i * (card + wide(14)) + 18, inner = card - 36;
      if (advanced) {
        const int field = (inner - 20) / 3;
        for (int j = 0; j < 6; ++j)
          edit(s.mounts[i][j], 300 + i * 10 + j, x + (j % 3) * (field + 10), StepTop + 74 + (j / 3) * 50, field);
      }
      if (!advanced) {
        power_tooltip(CameraPowerId + i, x, StepTop + 40, inner, CameraCardHeight - 90, tr(L"Turn the left and right cameras on or off"));
        look_tooltips(static_cast<unsigned>(i), x, StepTop + 40, inner, CameraCardHeight - 90);
      }
      const int half = (inner - 10) / 2;
      button(tr(L"Adjust live view"), LiveFeedId + i, x, StepTop + height - 42, half, 32);
      button(tr(L"Defaults"), DefaultsId + i, x + half + 10, StepTop + height - 42, half, 32);
    }
    if (advanced) {
      const bool guides = !profile || profile->reference_guides;
      const int y = StepTop + height + 10;
      button(tr(L"Guide values…"), GuidesOpenId, 528, y + 4, 136, 32);
      button(tr(L"Marking colour"), 372, 674, y + 4, 136, 32);
      button(tr(L"Reset guides"), 371, 820, y + 4, 138, 32);
      for (const int id : {GuidesOpenId, 372, 371})
        EnableWindow(GetDlgItem(window, id), guides);
    }
    // Turn them on with: TAXI-button control, then a row per action.
    const int top = controls_top();
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    button(tr(L"Reset shortcuts"), 640, 648, top + 6, 150, 30);
    button(switch_label(pmdg_cam_control(s) ? tr(L"CAM button") : tr(L"TAXI buttons"), s.follow_taxi && !manual).c_str(), 221, 808, top + 6,
           150, 30);
    EnableWindow(GetDlgItem(window, 221), !manual);
    // Advanced's taller cards leave no room for the status lines.
    const int row_height = advanced ? ControlRowAdvancedHeight : ControlRowHeight, first = top + (advanced ? 58 : 78);
    int row = 0;
    for (const auto action : taxi_actions(profile)) {
      const int y = first + row++ * row_height;
      control_row(s, action, {150, y, 236}, {400, y, 254}, {668, y, 290}, advanced ? 0 : row_height - 34);
    }
    refresh_shortcut_status();
    refresh_lvar_status(window);
  } else if (page == DoorPage) {
    // Where it shows: none or one of the aircraft's displays; then its
    // picture, and Advanced's six values.
    const auto* profile = profiles::find(s.profile);
    const unsigned sides = profile ? std::min<unsigned>(profile->sides, MaxDisplaySides) : 2;
    const int width = (524 - 8 * static_cast<int>(sides)) / static_cast<int>(sides + 1);
    // Where on that display: its area, placed anywhere on the whole texture.
    button(tr(L"Display area"), DoorAreaId, 434, 104, 132, 30);
    EnableWindow(GetDlgItem(window, DoorAreaId), s.door_side < MaxDisplaySides);
    button(tr(L"None"), DoorWhereId, 42, 140, width);
    for (unsigned side = 0; side < sides; ++side)
      button(display_side_title(s, side), DoorWhereId + 1 + static_cast<int>(side), 42 + static_cast<int>(side + 1) * (width + 8), 140,
             width);
    button(tr(L"Snapshot"), DoorSnapshotId, 42, 276, 258);
    button(tr(L"Live"), DoorLiveId, 308, 276, 258);
    if (s.door_side < MaxDisplaySides) {
      power_tooltip(DoorPowerId, 618, 160, 340, advanced ? 150 : 276, tr(L"Turn the door camera on or off"));
      look_tooltips(profiles::DoorFeed, 618, 160, 340, advanced ? 150 : 276);
    }
    button(tr(L"Adjust live view"), LiveFeedId + static_cast<int>(profiles::DoorFeed), 618, 452, 166);
    button(tr(L"Defaults"), DefaultsId + static_cast<int>(profiles::DoorFeed), 792, 452, 166);
    if (advanced) {
      const auto& door = s.mounts[profiles::DoorFeed];
      for (int j = 0; j < 6; ++j)
        edit(door[j], DoorFieldId + j, 618 + (j % 3) * 116, 340 + (j / 3) * 60, 108);
    }
    // Turn it on with: the door camera's shortcut, controller button and
    // cockpit L:var, one to a row.
    control_row(s, win::CameraDoor, {160, 414, 406}, {160, 472, 406}, {160, 530, 406}, 20);
    refresh_shortcut_status();
    refresh_lvar_status(window);
  } else if (page == 2) {
    // Camera frame rate: a card per mode, then Custom's target.
    pending_camera_mode = s.camera_mode < kCameraModeCount ? s.camera_mode : static_cast<unsigned>(kDefaultCameraMode);
    for (unsigned slot = 0; slot < kCameraModeCount; ++slot) {
      const auto mode = static_cast<unsigned>(CameraModeOrder[slot]);
      button(tr(CameraModeNames[mode]), CameraModeCardId + static_cast<int>(mode), 42 + static_cast<int>(slot) * 185, 160, 176, 102);
    }
    HWND target = child(TRACKBAR_CLASSW, L"", CameraTargetId, 200, 268, 640, 28, TBS_HORZ | TBS_NOTICKS);
    SendMessageW(target, TBM_SETRANGE, FALSE, MAKELPARAM(kMinimumCameraRate, kMaximumCameraRate));
    SendMessageW(target, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(target, TBM_SETPOS, TRUE, std::clamp(s.camera_rate, kMinimumCameraRate, kMaximumCameraRate));
    EnableWindow(target, pending_camera_mode == static_cast<unsigned>(CameraMode::custom));
    // Each brightness as a slider in quarter EV steps beside its value,
    // which can also be typed; then the ground-speed colour.
    const float brightness[]{s.day_brightness, s.night_brightness};
    for (int i = 0; i < 2; ++i) {
      const int x = i ? 526 : 42;
      HWND slider = child(TRACKBAR_CLASSW, L"", BrightnessSliderId + i, x + 164, 366, 170, 30, TBS_HORZ | TBS_NOTICKS);
      SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 24));
      SendMessageW(slider, TBM_SETPAGESIZE, 0, 4);
      SendMessageW(slider, TBM_SETPOS, TRUE, std::clamp(static_cast<int>(std::lround(brightness[i] * 4)) + 16, 0, 24));
      edit(brightness[i], 201 + i, x + 344, 366, 76);
    }
    button(tr(L"Ground-speed colour"), 231, 42, 448, 220, 34);
    const auto* display_profile = profiles::find(s.profile);
    EnableWindow(GetDlgItem(window, 231), !display_profile || display_profile->ground_speed);
  } else if (page == PreferencesPage) {
    // What Taxi Cam does by itself, its language, and its updates: the same
    // for every aircraft.
    button(switch_label(tr(L"Auto-connect"), auto_connect.load(std::memory_order_acquire)).c_str(), 240, 32, 82, 936, 40);
    button(switch_label(tr(L"Auto aircraft"), s.auto_profile).c_str(), 230, 32, 124, 936, 40);
    button(switch_label(tr(L"Show notifications"), s.notifications).c_str(), NotificationsId, 32, 166, 936, 40);
    const auto* language = win::find_language(language_preference);
    button(language ? language->native_name : tr(L"Follow Windows"), LanguageId, 776, 211, 182, 34);
    button(updater.busy() ? tr(L"Checking for updates...") : tr(L"Check for updates"), UpdatesId, 738, 286, 220, 34);
    EnableWindow(GetDlgItem(window, UpdatesId), !preview_ui && !updater.busy() && !update_prompt);
  } else if (page == RoutingPage) {
    origin_x = LegacyShiftX;
    origin_y = LegacyShiftY;
    button(tr(L"Clear choices"), 406, 522, 181, 120);
    button(tr(L"Swap left / right"), 403, 652, 181, 165);
    EnableWindow(GetDlgItem(window, 403), !single_display_profile(s));
    button(switch_label(tr(L"Auto select"), s.auto_detect).c_str(), 223, 827, 181, 165);
    button(tr(L"Refresh pictures"), 402, 640, 384, 160);
    button(L"‹", 407, 872, 384, 56);
    button(L"›", 408, 936, 384, 56);
    const unsigned lists = routing_lists(s);
    for (unsigned slot = 0; slot < GalleryColumns; ++slot)
      for (unsigned list = 0; list < lists; ++list)
        ShowWindow(button(list_action(s, list), static_cast<int>(420 + slot * 2 + list),
                          268 + static_cast<int>(slot) * 185 + static_cast<int>(list) * 84, 604, lists == 1 ? 163 : 79, 28),
                   SW_HIDE);
    update_gallery_buttons();
  } else if (page == 4) {
    // What to do about a problem, then the camera tests.
    button(tr(L"Report a bug"), 512, 738, 82, 220, 34);
    button(tr(L"Open log folder"), 510, 738, 124, 220, 34);
    button(switch_label(tr(L"Scene test"), s.scene_test).c_str(), 229, 42, 232, 200);
    // Calibration bars identify each screen.
    const auto* side_profile = profiles::find(s.profile);
    const unsigned sides = side_profile && side_profile->sides > 2 ? 3 : 2;
    constexpr int calibrations[]{226, 227, 233};
    for (unsigned side = 0; side < sides; ++side)
      button(switch_label(side_toggle_name(s, side), s.calibration_mask & (1u << side)).c_str(), calibrations[side],
             536 + static_cast<int>(side) * 142, 232, 134);
    edit(s.calibration_budget, 203, 838, 300, 120);
    button(tr(L"Stop camera tests"), 511, 748, 340, 210);
  } else if (page == GuidesPage) {
    origin_x = LegacyShiftX;
    origin_y = LegacyShiftY;
    const auto* guide_profile = profiles::find(s.profile);
    const bool draw_guides = !guide_profile || guide_profile->reference_guides;
    const std::array<float, 2> guides[]{s.nose_dot, s.tail_upper, s.tail_corner, s.tail_inner};
    const std::array<float, 2> right[]{s.tail_upper_right, s.tail_corner_right, s.tail_inner_right};
    // Nose X/Y, then each tail point's left X/Y and right X/Y.
    for (unsigned i = 0; i < 4; ++i) {
      const int y = i ? 364 + static_cast<int>(i - 1) * 64 : 238;
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
    button(tr(L"Apply live"), 370, 244, 634, 200);
    button(tr(L"Reset guides"), 371, 454, 634, 200);
    button(tr(L"Marking colour"), 372, 664, 634, 200);
    EnableWindow(GetDlgItem(window, 370), draw_guides);
    EnableWindow(GetDlgItem(window, 371), draw_guides);
    EnableWindow(GetDlgItem(window, 372), draw_guides);
  } else if (page == SetupPage) {
    // The displays, each with its cockpit test or, while a picture is picked,
    // a Place here; then the gallery of the cockpit's panels.
    // The picks load first, so the buttons sit on the displays drawn.
    {
      win::Status sample;
      {
        const std::lock_guard lock(app_mutex);
        sample = status;
      }
      sync_setup(sample);
    }
    for (unsigned side = 0; side < setup.sides; ++side) {
      const auto [slot_x, slot_width] = setup_slot_x(side);
      const int x = slot_x + 10, w = slot_width - 20, y = SetupSlotTop + SetupSlotHeight - 40;
      button(switch_label(tr(L"Test in cockpit"), (setup.testing >> side) & 1).c_str(), SetupTestId + static_cast<int>(side), x, y, w, 30);
      button(tr(L"Place here"), SetupPlaceId + static_cast<int>(side), x, y, w, 30);
    }
    // In the gap between the pilot displays, level with their pictures
    // (painted round with a drawn icon: draw_swap_button).
    const auto [left_x, left_width] = setup_slot_x(0);
    if (HWND swap = button(tr(L"Swap the left and right PFDs"), SetupSwapId, left_x + left_width + (SetupSwapGap - SetupSwapSize) / 2,
                           SetupSlotTop + 40 + SetupSlotPicture / 2 - SetupSwapSize / 2, SetupSwapSize, SetupSwapSize)) {
      TOOLINFOW tip{};
      tip.cbSize = sizeof(tip);
      tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      tip.hwnd = window;
      tip.uId = reinterpret_cast<UINT_PTR>(swap);
      tip.lpszText = const_cast<wchar_t*>(tr(L"Swap the left and right PFDs"));
      SendMessageW(sidebar_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    button(tr(L"Refresh pictures"), SetupRefreshId, 626, SetupGalleryTop + 8, 150, 30);
    button(L"‹", SetupPreviousId, 852, SetupGalleryTop + 8, 50, 30);
    button(L"›", SetupNextId, 908, SetupGalleryTop + 8, 50, 30);
    for (unsigned slot = 0; slot < GalleryColumns; ++slot) {
      HWND card = child(L"BUTTON", L"", SetupCardButtonId + static_cast<int>(slot), 42 + static_cast<int>(slot) * 231, SetupGalleryCards,
                        223, SetupGalleryCardHeight, BS_OWNERDRAW | BS_NOTIFY);
      SetWindowSubclass(card, card_drag, 1, 0);
      ShowWindow(card, SW_HIDE);
    }
    if (advanced) {
      int x = 42;
      if (setup.built_in) {
        button(tr(L"Use default displays"), SetupBuiltInId, x, 564, 220, 32);
        x += 230;
      }
      button(tr(L"Texture routing…"), RoutingOpenId, x, 564, 190, 32);
      button(tr(L"Display area…"), DisplayAreaId, x + 200, 564, 190, 32);
    }
    update_setup_buttons();
    refresh_setup_picking();
  }
  origin_x = origin_y = 0;
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
  wcscpy_s(data.szTip, tr(L"Taxi Cam — Settings"));
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
  surface_notification(tr(L"Update downloaded. Close Microsoft Flight Simulator, then use Check for updates in the tray menu to install."),
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
// An alert in front of everything, the simulator included, for something the
// pilot must do before Taxi Cam can work. The companion often runs only in
// the tray, so a notice in its window would go unseen. The alert is shown
// without taking the keyboard from the simulator, centred on the monitor of
// the window in front (the simulator while flying). One at a time: a newer
// alert replaces the one on screen.
constexpr wchar_t AlertClass[] = L"380TaxiCamera.Alert";
constexpr int AlertWidth = 460, AlertPadding = 24, AlertTitleHeight = 28, AlertButtonHeight = 34, AlertButtonWidth = 150;
struct AlertView {
  HWND window{};
  std::wstring title, body, primary, secondary;
  std::function<void()> act;  // The primary button's action, after the alert closes.
  RECT primary_rect{}, secondary_rect{};
  int body_height{};
} alert_view;
void close_alert() {
  if (alert_view.window)
    DestroyWindow(alert_view.window);
  alert_view.window = nullptr;
}
void draw_alert_button(HDC dc, const RECT& r, const std::wstring& label, bool primary) {
  ButtonLook look;
  look.primary = primary;
  button_box(dc, r, look, Card);
  SelectObject(dc, normal);
  SetTextColor(dc, button_text(look));
  SetBkMode(dc, TRANSPARENT);
  auto bounds = r;
  DrawTextW(dc, label.c_str(), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
LRESULT CALLBACK alert_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  auto& v = alert_view;
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // A click works without taking the keyboard from the simulator.
    case WM_SETCURSOR: {
      POINT p{};
      GetCursorPos(&p);
      ScreenToClient(hwnd, &p);
      const bool over = PtInRect(&v.primary_rect, p) || (!v.secondary.empty() && PtInRect(&v.secondary_rect, p));
      SetCursor(LoadCursorW(nullptr, over ? IDC_HAND : IDC_ARROW));
      return TRUE;
    }
    case WM_LBUTTONUP: {
      const POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      if (PtInRect(&v.primary_rect, p)) {
        auto act = std::move(v.act);
        close_alert();
        if (act)
          act();
      } else if (!v.secondary.empty() && PtInRect(&v.secondary_rect, p)) {
        close_alert();
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT client;
      GetClientRect(hwnd, &client);
      HBRUSH fill = CreateSolidBrush(Card);
      FillRect(dc, &client, fill);
      DeleteObject(fill);
      // An amber frame: it needs the pilot, like the amber chips on Overview.
      HBRUSH frame = CreateSolidBrush(Warning);
      for (int i = 0; i < scale(2); ++i) {
        RECT edge{client.left + i, client.top + i, client.right - i, client.bottom - i};
        FrameRect(dc, &edge, frame);
      }
      DeleteObject(frame);
      SetBkMode(dc, TRANSPARENT);
      const int pad = scale(AlertPadding);
      RECT title{pad, pad, client.right - pad, pad + scale(AlertTitleHeight)};
      SelectObject(dc, heading);
      SetTextColor(dc, Text);
      DrawTextW(dc, v.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      RECT body{pad, title.bottom + scale(10), client.right - pad, title.bottom + scale(10) + v.body_height};
      SelectObject(dc, normal);
      SetTextColor(dc, Muted);
      DrawTextW(dc, v.body.c_str(), -1, &body, DT_LEFT | DT_WORDBREAK);
      draw_alert_button(dc, v.primary_rect, v.primary, true);
      if (!v.secondary.empty())
        draw_alert_button(dc, v.secondary_rect, v.secondary, false);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      if (v.window == hwnd)
        v.window = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd, message, w, l);
}
// primary closes the alert, then runs act; secondary (empty for none) only
// closes it.
void show_alert(std::wstring title, std::wstring body, std::wstring primary, std::wstring secondary, std::function<void()> act) {
  if (preview_ui)
    return;
  close_alert();
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpfnWndProc = alert_procedure;
    type.lpszClassName = AlertClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassExW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  if (!registered)
    return;
  auto& v = alert_view;
  v.title = std::move(title);
  v.body = std::move(body);
  v.primary = std::move(primary);
  v.secondary = std::move(secondary);
  v.act = std::move(act);
  const int width = scale(AlertWidth), pad = scale(AlertPadding);
  // The body's height at the alert's width.
  HDC screen = GetDC(nullptr);
  const auto old_font = SelectObject(screen, normal);
  RECT measure{0, 0, width - 2 * pad, 0};
  DrawTextW(screen, v.body.c_str(), -1, &measure, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
  SelectObject(screen, old_font);
  ReleaseDC(nullptr, screen);
  v.body_height = measure.bottom - measure.top;
  const int buttons_top = pad + scale(AlertTitleHeight) + scale(10) + v.body_height + scale(20);
  const int height = buttons_top + scale(AlertButtonHeight) + pad;
  v.primary_rect = {width - pad - scale(AlertButtonWidth), buttons_top, width - pad, buttons_top + scale(AlertButtonHeight)};
  v.secondary_rect = {v.primary_rect.left - scale(10) - scale(AlertButtonWidth), buttons_top, v.primary_rect.left - scale(10),
                      buttons_top + scale(AlertButtonHeight)};
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &monitor);
  const auto& area = monitor.rcWork;
  const int x = area.left + (area.right - area.left - width) / 2, y = area.top + (area.bottom - area.top - height) / 2;
  v.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, AlertClass, v.title.c_str(), WS_POPUP, x, y, width,
                             height, nullptr, nullptr, instance, nullptr);
  if (v.window)
    ShowWindow(v.window, SW_SHOWNOACTIVATE);
}
// After an update that could not convert this aircraft's displays, an alert
// says so once, in front of the simulator; Choose displays opens Displays.
void prompt_choose_displays() {
  if (!choose_displays_prompt || preview_ui)
    return;
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  if (!fresh_status(sample) || !sample.identity_sample_ms)
    return;  // Not known yet: ask once the aircraft is.
  choose_displays_prompt = false;
  if (!displays_to_choose(sample))
    return;
  show_alert(tr(L"Choose this aircraft's displays"),
             tr(L"Taxi Cam now recognises cockpit displays a new way and could not carry over this aircraft's saved displays. "
                L"Choose them once in Displays: Taxi Cam keeps your choice from then on."),
             tr(L"Choose displays"), tr(L"Later"), [] {
               // The pilot asked for Displays: no second check that could
               // leave them on another page.
               show();
               open_step(0);
             });
}
// Tray and Menu: Language. Unfinished fields are kept as a page change keeps
// them, then the controls are made again in the new language. Open windows
// other than Settings change when they are next opened or painted.
constexpr UINT LanguageMenuId = 610;  // Follow Windows; the languages follow it.
// The interface languages: Follow Windows, then each by its own name (native
// names are not translated), with the chosen one marked.
HMENU language_menu() {
  HMENU languages = CreatePopupMenu();
  AppendMenuW(languages, MF_STRING, LanguageMenuId, tr(L"Follow Windows"));
  for (UINT i = 0; i < std::size(win::UiLanguages); ++i)
    AppendMenuW(languages, MF_STRING, LanguageMenuId + 1 + i, win::UiLanguages[i].native_name);
  const auto* chosen = win::find_language(language_preference);
  CheckMenuRadioItem(languages, LanguageMenuId, LanguageMenuId + static_cast<UINT>(std::size(win::UiLanguages)),
                     chosen ? LanguageMenuId + 1 + static_cast<UINT>(chosen - win::UiLanguages) : LanguageMenuId, MF_BYCOMMAND);
  return languages;
}
// A global preference, saved with the other selections in settings.ini.
void toggle_notifications() {
  auto s = draft();
  s.notifications = s.notifications ? 0u : 1u;
  publish(s);
  if (!win::save_settings(draft()))
    notice = tr(L"Could not save settings. Check access to your local settings folder.");
  // Options' switch, where it is shown.
  if (GetDlgItem(window, NotificationsId))
    SetDlgItemTextW(window, NotificationsId, switch_label(tr(L"Show notifications"), s.notifications).c_str());
  InvalidateRect(window, nullptr, FALSE);
}
void choose_language(const wchar_t* preference) {
  auto s = draft();
  const wchar_t* field_error{};
  if (!read_fields(s, &field_error)) {
    notice = field_error ? tr(field_error) : tr(L"Finish the current values before changing pages.");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }
  publish(s);
  language_preference = preference;
  const bool saved = win::save_language(win::settings_directory(), language_preference);
  win::select_language(win::resolve_language(language_preference, win::windows_display_languages()));
  make_fonts();
  build_controls();
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = 1;
  data.uFlags = NIF_TIP;
  wcscpy_s(data.szTip, tr(L"Taxi Cam — Settings"));
  Shell_NotifyIconW(NIM_MODIFY, &data);
  notice = saved ? tr(L"Interface language updated.") : tr(L"Could not save the language preference.");
  InvalidateRect(window, nullptr, FALSE);
}
// The connection in a few words, with the colour of its dot: green once the
// cockpit's displays are seen, the warning colour when they cannot be
// recognised.
std::pair<const wchar_t*, COLORREF> connection_state(const win::Status& sample) {
  const bool has_displays = sample.candidate_count != 0 || sample.left_id != 0 || sample.right_id != 0 || sample.lower_id != 0;
  const bool unsupported = unsupported_aircraft(sample), inactive = package_inactive(sample);
  const wchar_t* state = connection_disconnected.load(std::memory_order_acquire) ? tr(L"Disconnected")
                         : !sample.graphics_ready                                ? tr(L"Waiting for the simulator")
                         : inactive                                              ? tr(L"Taxi Cam can't recognise your displays")
                         : unsupported                                           ? tr(L"This aircraft is not set up yet")
                                                                                 : tr(L"Cameras ready");
  const bool connected =
      sample.graphics_ready && has_displays && !inactive && !unsupported && !connection_disconnected.load(std::memory_order_acquire);
  return {state, inactive ? Warning : connected ? Ok : unsupported && sample.graphics_ready ? Accent : Faint};
}
void draw_page(HDC dc) {
  hit_areas.clear();
  power_areas.clear();
  surfaces.clear();
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, background_brush);
  const auto rule = [&](int x, int y, int w, int h) {
    RECT line{scale(x), scale(y), scale(x) + std::max(1, scale(w)), scale(y) + std::max(1, scale(h))};
    HBRUSH b = CreateSolidBrush(Border);
    FillRect(dc, &line, b);
    DeleteObject(b);
  };
  rule(0, HeaderHeight, ClientWidth, 1);
  win::Status sample;
  std::wstring live;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
    live = connection.text();
  }
  const bool has_displays = sample.candidate_count != 0 || sample.left_id != 0 || sample.right_id != 0 || sample.lower_id != 0;
  // A translation can outgrow a line: it then steps down a font size, and
  // shortens with an ellipsis only after that.
  const auto fitting = [&](const wchar_t* value, int width, HFONT font, HFONT smaller) {
    SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, value, static_cast<int>(std::wcslen(value)), &size);
    return size.cx > scale(width) ? smaller : font;
  };
  const int step = setup_step(page);
  const bool unsupported = unsupported_aircraft(sample);
  // Without the package nothing can be recognised: Home says so, first thing.
  const bool inactive = package_inactive(sample);
  // The header, the same on every page: the app, its places (build_controls),
  // and the connection in a few words.
  DrawIconEx(dc, scale(PageLeft), scale(12), icon, scale(32), scale(32), 0, nullptr, DI_NORMAL);
  text(dc, L"Taxi Cam", 66, 16, 104, 24, heading);
  rule(174, 18, 1, 20);
  if (page != OverviewPage) {
    const auto [state, state_color] = connection_state(sample);
    const int state_room = 920 - 622 - 14;
    const HFONT state_font = fitting(state, state_room, small, small);
    SelectObject(dc, state_font);
    SIZE state_size{};
    GetTextExtentPoint32W(dc, state, static_cast<int>(std::wcslen(state)), &state_size);
    const int state_width = std::min(state_room, MulDiv(state_size.cx, 96, static_cast<int>(dpi)) + 2);
    disc(dc, scale(920 - state_width - 10), scale(28), scale(4), state_color);
    text(dc, state, 920 - state_width, 16, state_width, 24, state_font, state_color == Warning ? Warning : Muted,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  if (page == OverviewPage) {
    const auto s = draft();
    const auto* profile = profiles::find(s.profile);
    const bool choose = !unsupported && displays_to_choose(sample);
    // The status card: one sentence for what is happening, one line for
    // what to do about it, and its buttons (build_controls).
    // A change that cannot finish lists no displays either: the bridge's own
    // line says so, where "waiting for displays" would hide it (issue 202).
    const bool change_paused = sample.heartbeat && !std::strcmp(sample.message, win::TransitionFailedMessage);
    const bool late_empty_pfds = sample.graphics_ready && !has_displays && !unsupported && !change_paused;
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    const bool paused = connection_disconnected.load(std::memory_order_acquire);
    const auto bridge_line = sample.heartbeat ? widen(sample.message) : std::wstring();
    const wchar_t* headline = paused ? tr(L"Cameras are paused")
                              : !sample.graphics_ready
                                  ? (simulator_pid.load() ? tr(L"Connecting to the simulator") : tr(L"Waiting for the simulator"))
                              : inactive        ? tr(L"Taxi Cam can't recognise your displays")
                              : change_paused   ? tr(L"Cameras are not running")
                              : unsupported     ? tr(L"This aircraft is not set up yet")
                              : choose          ? tr(L"Choose this aircraft's displays")
                              : late_empty_pfds ? tr(L"Waiting for the cockpit displays")
                                                : tr(L"Cameras ready");
    const std::wstring advice =
        paused                           ? std::wstring(tr(L"Choose Connect to turn them back on."))
        : !sample.graphics_ready         ? (!simulator_pid.load() && auto_connect.load(std::memory_order_acquire)
                                                ? std::wstring(tr(L"Start Microsoft Flight Simulator. Taxi Cam connects by itself."))
                                                : live)
        : inactive && package_conflict() ? std::wstring(tr(L"Another add-on's cockpit page stops Taxi Cam recognising your displays."))
        : inactive             ? std::wstring(tr(L"Display package not active. Restart the simulator, or run the installer again."))
        : change_paused        ? bridge_line
        : unsupported          ? std::wstring(tr(L"Set it up once: choose its displays, then aim its cameras."))
        : choose               ? std::wstring(tr(L"After this update, choose this aircraft's displays again in Displays."))
        : late_empty_pfds      ? std::wstring(tr(L"Waiting for cockpit displays to be drawn. Restart Flight only if the list stays empty."))
        : !bridge_line.empty() ? bridge_line
        : s.follow_taxi && !manual ? std::wstring(tr(L"Press TAXI in the cockpit to show them, or use a display's switch."))
                                   : std::wstring(tr(L"Use a display's switch or its shortcut to show them."));
    const bool trouble = inactive || change_paused;
    const bool ready = sample.graphics_ready && has_displays && !trouble && !unsupported && !choose && !paused;
    const COLORREF mood = trouble ? Warning : ready ? Ok : unsupported || choose ? Accent : Faint;
    panel(dc, PageLeft, HomeStatusTop, PageWidth, HomeStatusHeight);
    disc(dc, scale(50), scale(HomeStatusTop + 24), scale(6), mood);
    const int words = (unsupported || choose ? 628 : 838) - 12 - 70;
    text(dc, headline, 70, HomeStatusTop + 10, words, 28, heading, trouble ? Warning : Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, advice.c_str(), 70, HomeStatusTop + 38, words, 34, small, Muted, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    // The aircraft's card: a pill only when its setup needs something.
    panel(dc, PageLeft, HomeAircraftTop, PageWidth, HomeAircraftHeight);
    if (profile && (unsupported || choose))
      chip(dc, 302, HomeAircraftTop + 17, unsupported ? tr(L"Not set up") : tr(L"Choose displays"), Warning, 96);
    // The displays, once the simulator shows them; until then, what happens.
    if (built_with_displays) {
      draw_display_strip(dc, sample);
    } else {
      panel(dc, PageLeft, DisplayCardTop, PageWidth, DisplayCardHeight, Well);
      text(dc, tr(L"How it works"), 60, DisplayCardTop + 78, 880, 30, title_font, Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      const wchar_t* steps[]{tr(L"Start Microsoft Flight Simulator and load a flight."),
                             tr(L"Taxi Cam connects by itself and finds your cockpit displays."),
                             tr(L"Press TAXI in the cockpit, or use the switches here, to show the cameras.")};
      for (int k = 0; k < 3; ++k) {
        const int y = DisplayCardTop + 128 + k * 34;
        disc(dc, scale(292), scale(y + 13), scale(11), Raised);
        wchar_t number[4];
        std::swprintf(number, 4, L"%d", k + 1);
        text(dc, number, 281, y + 2, 22, 22, small, Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, steps[k], 316, y, 560, 26, normal, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
    }
    // In flight: the switches (build_controls), and the door camera's name
    // beside the way to choose its display while it has none.
    panel(dc, PageLeft, HomeFlightTop, PageWidth, HomeFlightHeight);
    if (s.door_side >= MaxDisplaySides)
      text(dc, tr(L"Door camera"), 516, HomeFlightTop, 290, HomeFlightHeight, normal);
    rule(0, 632, ClientWidth, 1);
  } else if (aircraft_page(page)) {
    // Aircraft is titled with the aircraft, left of its sections.
    const std::wstring title = setup_aircraft_name();
    text(dc, title.c_str(), PageLeft, 62, 316, 32, fitting(title.c_str(), 316, heading, small), Text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    rule(0, 616, ClientWidth, 1);
  }
  // Advanced's two pages are drawn where their controls are (LegacyShiftX);
  // what they register for the pointer and for their controls moves with them.
  const bool shifted = page == RoutingPage || page == GuidesPage;
  const size_t first_hit = hit_areas.size(), first_surface = surfaces.size();
  POINT viewport{};
  if (shifted)
    SetViewportOrgEx(dc, scale(LegacyShiftX), scale(LegacyShiftY), &viewport);
  if (page == OverviewPage) {
  } else if (page == CamerasPage) {
    const wchar_t* labels[]{tr(L"Right (m)"), tr(L"Up (m)"), tr(L"Forward (m)"), tr(L"Pitch (deg)"), tr(L"Yaw (deg)"), tr(L"Lens (rad)")};
    const auto s = draft();
    const auto* profile = profiles::find(s.profile);
    const bool split = profile && profile->composition.split_bottom != 0;
    const int feed_count = split ? 3 : 2, height = advanced ? CameraCardAdvancedHeight : CameraCardHeight;
    const wchar_t* titles[]{tr(L"Nose-wheel camera"), split ? tr(L"Left wing camera") : tr(L"Tail camera"), tr(L"Right wing camera")};
    const wchar_t* views[]{tr(L"UPPER VIEW"), split ? tr(L"LOWER LEFT") : tr(L"LOWER VIEW"), tr(L"LOWER RIGHT")};
    for (int i = 0; i < feed_count; ++i) {
      const int card = wide(feed_count == 3 ? 246 : 376), x = PageLeft + i * (card + wide(14)), inner = card - 36, field = (inner - 20) / 3;
      panel(dc, x, StepTop, card, height);
      // The design's header: the camera, and its view at the right.
      const auto& dimensions = (profile ? *profile : profiles::A380).camera_panes[i];
      wchar_t view_label[96];
      std::swprintf(view_label, 96, L"%ls · %d × %d", views[i], dimensions[0], dimensions[1]);
      SelectObject(dc, heading);
      SIZE title_size{};
      GetTextExtentPoint32W(dc, titles[i], static_cast<int>(std::wcslen(titles[i])), &title_size);
      const int title_width = std::min(inner, MulDiv(title_size.cx, 96, static_cast<int>(dpi)) + 4);
      text(dc, titles[i], x + 18, StepTop + 8, title_width, 26, heading, Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      if (inner - title_width >= 90)
        text(dc, view_label, x + 18 + title_width + 8, StepTop + 8, inner - title_width - 8, 26, mono, Muted,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      if (advanced) {
        for (int j = 0; j < 6; ++j)
          text(dc, labels[j], x + 18 + (j % 3) * (field + 10), StepTop + 56 + (j / 3) * 50, field, 18, small, Muted);
      } else {
        draw_feed_picture(dc, sample, static_cast<unsigned>(i), rectangle(x + 18, StepTop + 40, inner, CameraCardHeight - 90));
        draw_power_icon(dc, rectangle(x + 18, StepTop + 40, inner, CameraCardHeight - 90), CameraPowerId + i);
        draw_look_icons(dc, rectangle(x + 18, StepTop + 40, inner, CameraCardHeight - 90), static_cast<unsigned>(i));
      }
    }
    if (advanced) {
      const bool guides = profile && !profile->reference_guides ? false : true;
      const int y = StepTop + height + 10;
      panel(dc, PageLeft, y, PageWidth, CameraGuidesHeight);
      text(dc, tr(L"Reference guides"), 42, y, 290, CameraGuidesHeight, heading);
      if (!guides)
        text(dc, tr(L"This aircraft does not use alignment markers."), 42, y, 290, CameraGuidesHeight, small, Muted);
    }
    const int top = controls_top();
    const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
    const bool cam = profile && profile->taxi_control == profiles::TaxiControl::pmdg_dsp_cam;
    panel(dc, PageLeft, top, PageWidth, StepBottom - top);
    text(dc, tr(L"Turn them on with"), 42, top + 6, 400, 30, heading);
    if (!advanced)
      text(dc,
           manual ? tr(L"No cockpit camera control is known for this aircraft: bind cockpit L:vars below, or use the shortcuts.")
           : cam  ? tr(L"Select L INBD, R INBD or LWR CTR, then press CAM. Press CAM again with that display selected to turn it off.")
                  : tr(L"Left and right EFIS TAXI buttons activate their own PFD."),
           42, top + 34, 916, 22, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const wchar_t* columns[]{tr(L"CAMERA"), tr(L"KEYBOARD · ALL AIRCRAFT"), tr(L"CONTROLLER · ALL AIRCRAFT"),
                             tr(L"COCKPIT L:VAR · THIS AIRCRAFT")};
    const int column_x[]{42, 150, 400, 668}, column_w[]{100, 236, 254, 290};
    const int header = top + (advanced ? 36 : 56);
    for (int c = 0; c < 4; ++c)
      text(dc, columns[c], column_x[c], header, column_w[c], 20, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int row_height = advanced ? ControlRowAdvancedHeight : ControlRowHeight, first = top + (advanced ? 58 : 78);
    int row = 0;
    for (const auto action : taxi_actions(profile))
      text(dc, tr(ControlsActionNames[action]), 42, first + row++ * row_height, 100, 30, normal);
  } else if (page == DoorPage) {
    const auto s = draft();
    const auto* profile = profiles::find(s.profile);
    panel(dc, PageLeft, 96, 560, 124);
    text(dc, tr(L"Where it shows"), 42, 106, 380, 26, heading);
    const bool shares =
        profile && s.door_side < MaxDisplaySides && win::camera_action_sides(*profile, win::CameraSd) && s.door_side == MaxDisplaySides - 1;
    // A pilot pair without its lower display placed yet: that comes first.
    const auto* base = profiles::base_profile(s.profile);
    const bool place_lower = base && profile && profile->sides < MaxDisplaySides && profiles::pilot_pair(*base);
    text(dc,
         s.door_side == win::DoorSideNone && place_lower
             ? tr(L"Place the lower display in Aircraft > Displays to show the door camera there.")
         : s.door_side == win::DoorSideNone ? tr(L"Choose the display it fills. The lower display is the usual place.")
         : shares ? tr(L"While it is on it takes this display over from its taxi camera, and hands it back when it turns off.")
                  : tr(L"While it is on it fills this display; the other displays are unchanged."),
         42, 180, 524, 36, small, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, PageLeft, 234, 560, 122);
    text(dc, tr(L"Picture"), 42, 244, 400, 26, heading);
    text(dc, tr(L"Snapshot takes one picture each time it turns on. Live keeps rendering while it is on."), 42, 316, 524, 36, small, Muted,
         DT_LEFT | DT_WORDBREAK);
    panel(dc, PageLeft, 370, 560, 230);
    text(dc, tr(L"Turn it on with"), 42, 380, 260, 26, heading);
    const wchar_t* rows[]{tr(L"Keyboard"), tr(L"Controller"), tr(L"Cockpit L:var")};
    for (int row = 0; row < 3; ++row)
      text(dc, rows[row], 42, 414 + row * 58, 110, 30, normal, Muted);
    // As tall as its content: the note at the bottom sets the height.
    const auto* note = tr(L"It starts behind the cockpit door, looking aft. In the live view the arrows move it the way it faces.");
    const auto old_font = SelectObject(dc, small);
    RECT wrapped{0, 0, scale(340), 0};
    DrawTextW(dc, note, -1, &wrapped, DT_CALCRECT | DT_WORDBREAK);
    SelectObject(dc, old_font);
    const int note_height = MulDiv(wrapped.bottom, 96, static_cast<int>(dpi)) + 2;
    panel(dc, 600, 96, 376, 500 + note_height + 16 - 96);
    text(dc, tr(L"Door camera"), 618, 108, 200, 26, heading);
    wchar_t door_label[96];
    // The camera takes the shape of its display's area (shape_door).
    const auto& pane = profile ? profile->camera_panes[profiles::DoorFeed] : profiles::DoorPane;
    std::swprintf(door_label, 96, L"%ls · %d × %d", tr(L"OWN DISPLAY"), pane[0], pane[1]);
    text(dc, door_label, 618, 134, 340, 20, mono, Muted);
    // Its area on the display it shows on, as the cockpit draws it now.
    const auto box = rectangle(618, 160, 340, advanced ? 150 : 276);
    if (profile && s.door_side < MaxDisplaySides) {
      const auto id = routed_texture_ids(sample, *profile)[s.door_side];
      const auto* t = find_thumbnail(id);
      const bool fits = t && t->texture_width && t->texture_height &&
                        (profile->fit_texture || (t->texture_width == profile->width && t->texture_height == profile->height) ||
                         (s.door_side == MaxDisplaySides - 1 && profiles::lower_any_panel(*profile)));
      const auto crop =
          fits ? profiles::side_content_rect(*profile, s.door_side, t->texture_width, t->texture_height, true) : profiles::DisplayRect{};
      draw_thumbnail(dc, box, id, tr(L"No display"), crop.right > crop.left ? &crop : nullptr);
      if (id)
        hit_areas.push_back({box, id});
      draw_power_icon(dc, box, DoorPowerId);
      draw_look_icons(dc, box, profiles::DoorFeed);
    } else {
      draw_thumbnail(dc, box, 0, tr(L"No display chosen"));
    }
    if (advanced) {
      const wchar_t* labels[]{tr(L"Right (m)"), tr(L"Up (m)"), tr(L"Forward (m)"), tr(L"Pitch (deg)"), tr(L"Yaw (deg)"), tr(L"Lens (rad)")};
      for (int j = 0; j < 6; ++j)
        text(dc, labels[j], 618 + (j % 3) * 116, 320 + (j / 3) * 60, 108, 20, small, Muted,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    text(dc, note, 618, 500, 340, note_height, small, Muted, DT_LEFT | DT_WORDBREAK);
  } else if (page == 2) {
    // The camera images take the main view's lighting; brightness is the
    // user's offset on top of it.
    const bool custom = selected_camera_mode() == static_cast<unsigned>(CameraMode::custom);
    panel(dc, PageLeft, 96, PageWidth, 242);
    text(dc, tr(L"Camera frame rate"), 42, 104, 400, 28, heading);
    text(dc, tr(L"Frames per second for each taxi camera, parked or moving. Auto keeps the simulator within 10% of its frame rate."), 42,
         132, 916, 22, small, Muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, tr(L"Custom target"), 42, 270, 150, 24, normal, custom ? Text : Muted);
    const auto target_label = tr_format(L"{0} fps", {std::to_wstring(selected_camera_target())});
    text(dc, target_label.c_str(), 858, 270, 100, 24, mono, custom ? Text : Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (sample.heartbeat) {
      // Whole sentences for each limit, so no clause is added in English order.
      const auto target = std::to_wstring(sample.effective_rate);
      const bool capped = sample.rate_limits & kRateLimitPfdRefresh, limited = !capped && (sample.rate_limits & kRateLimitSimulator);
      win::UiMessage rate;
      if (sample.reachable_rate > 0) {
        std::vector<std::wstring> values{decimal(sample.reachable_rate, 1), target, decimal(sample.update_hz, 0)};
        rate.add(capped ? ui_text(L"Now: each camera about {0} fps (target {1} fps, capped at the PFD refresh rate), simulator {2} fps.")
                 : limited
                     ? ui_text(L"Now: each camera about {0} fps (target {1} fps, limited by the simulator frame rate), simulator {2} fps.")
                     : ui_text(L"Now: each camera about {0} fps (target {1} fps), simulator {2} fps."),
                 std::move(values));
      } else {
        rate.add(
            capped ? ui_text(L"Now: target {0} fps per camera (capped at the PFD refresh rate). Measuring the simulator's frame rate.")
            : limited
                ? ui_text(L"Now: target {0} fps per camera (limited by the simulator frame rate). Measuring the simulator's frame rate.")
                : ui_text(L"Now: target {0} fps per camera. Measuring the simulator's frame rate."),
            {target});
      }
      // Saved Auto: the update rate it keeps the simulator above.
      if (draft().camera_mode == static_cast<std::uint32_t>(CameraMode::automatic) && sample.auto_floor > 0)
        rate.add(ui_text(L"Auto keeps the simulator above {0} fps."), {decimal(sample.auto_floor, 0)});
      const auto line = rate.text();
      text(dc, line.c_str(), 42, 300, 916, 34, small, Muted, DT_LEFT | DT_WORDBREAK);
    }
    panel(dc, PageLeft, 352, PageWidth, 144);
    const wchar_t* names[]{tr(L"Daytime brightness"), tr(L"Night brightness")};
    const wchar_t* descriptions[]{tr(L"EV added to the main view's exposure in daylight, −4 to +2 in 0.25 steps. 0 matches the main view."),
                                  tr(L"EV added at night, −4 to +2 in 0.25 steps. Dusk blends the two with the ambient light.")};
    for (int i = 0; i < 2; ++i) {
      const int x = i ? 526 : 42;
      text(dc, names[i], x, 366, 160, 30, fitting(names[i], 160, normal, small), Text,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      text(dc, descriptions[i], x, 400, 420, 32, small, Muted, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
  } else if (page == PreferencesPage) {
    // The same for every aircraft: the switches' rows (build_controls), the
    // language, and this version with its update check.
    panel(dc, PageLeft, 72, PageWidth, 182);
    text(dc, tr(L"Language"), 40, 208, 400, 40, normal);
    panel(dc, PageLeft, 268, PageWidth, 70);
    text(dc, L"Taxi Cam " TAXI_CAM_VERSION_WIDE, 42, 268, 400, 70, heading);
  } else if (page == RoutingPage) {
    panel(dc, 244, 172, 766, 190);
    text(dc, tr(L"Assigned displays"), 262, 182, 255, 34, heading);
    draw_routing_slots(dc, sample);
    panel(dc, 244, 376, 766, 280);
    text(dc, tr(L"Display textures"), 262, 386, 180, 32, heading);
    text(dc, tr(L"Select a picture to enlarge it."), 446, 386, 190, 32, small, Muted);
    draw_gallery(dc, sample);
  } else if (page == 4) {
    // Whether each part is working, in plain words, beside what to do about
    // a problem (build_controls).
    panel(dc, PageLeft, 72, PageWidth, 96);
    const bool simulator = simulator_pid.load() != 0;
    const unsigned found = (sample.left_id != 0) + (sample.right_id != 0) + (sample.lower_id != 0);
    const wchar_t* parts[]{tr(L"Simulator"), tr(L"Cameras"), tr(L"Displays")};
    const bool working[]{simulator, sample.scene_ready != 0, found != 0};
    const std::wstring states[]{simulator ? tr(L"Connected") : tr(L"Waiting"), sample.scene_ready ? tr(L"Ready") : tr(L"Waiting"),
                                found ? std::to_wstring(found) : std::wstring(tr(L"Waiting for displays"))};
    for (int k = 0; k < 3; ++k) {
      const int y = 82 + k * 26;
      disc(dc, scale(50), scale(y + 12), scale(5), working[k] ? Ok : Faint);
      text(dc, parts[k], 68, y, 150, 24, normal, Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      text(dc, states[k].c_str(), 226, y, 480, 24, normal, working[k] ? Text : Muted,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // The numbers behind them, for a bug report.
    panel(dc, PageLeft, 402, PageWidth, 198);
    text(dc, tr(L"Technical details"), 42, 408, 400, 28, heading);
    const auto counts = tr_format(
        L"Bridge                 {0}\n"
        L"Camera pair         {1}\n"
        L"Left / right PFD    {2} / {3}\n"
        L"Captured frames  {4}\n"
        L"Compositions       {5}\n"
        L"PFD writes            {6}\n"
        L"Hook failures        {7}",
        {sample.graphics_ready ? (has_displays ? tr(L"Connected") : tr(L"Waiting for displays")) : tr(L"Waiting"),
         sample.scene_ready ? tr(L"Ready") : tr(L"Waiting"), std::to_wstring(sample.left_id), std::to_wstring(sample.right_id),
         std::to_wstring(sample.captures), std::to_wstring(sample.composed), std::to_wstring(sample.stamps),
         std::to_wstring(sample.hook_failures)});
    // A row per line: its name, then its value in one column. A line is
    // split at its run of spaces, or at its colon when it has none; a line
    // with neither is drawn whole.
    int row_y = 440;
    for (size_t start = 0; start <= counts.size();) {
      const auto end = std::min(counts.find(L'\n', start), counts.size());
      const auto row_text = counts.substr(start, end - start);
      const auto spaces = row_text.find(L"  ");
      const auto gap = spaces != std::wstring::npos ? spaces : row_text.find_first_of(L"\uFF1A:");
      const auto value_at = gap == std::wstring::npos ? gap : row_text.find_first_not_of(L" \uFF1A:", gap);
      if (gap == std::wstring::npos)
        text(dc, row_text.c_str(), 42, row_y, 420, 20, normal, Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      else
        text(dc, row_text.substr(0, gap).c_str(), 42, row_y, 170, 20, normal, Muted,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      if (value_at != std::wstring::npos)
        text(dc, row_text.substr(value_at).c_str(), 220, row_y, 240, 20, normal, Text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      row_y += 20;
      start = end + 1;
    }
    const auto stage = [&](unsigned i) { return decimal(sample.stage_ms[i], 3); };
    const auto timing = tr_format(L"Probe CPU: {0} ms (max {1})", {decimal(sample.probe_cpu_ms, 2), decimal(sample.probe_max_ms, 2)}) +
                        L"\n" +
                        tr_format(
                            L"Manager {0}   Pool {1}\nLifecycle {2}   Entries {3}\nView 1 {4}   View 2 {5}\nHandoff {6}   Pose {7}\n"
                            L"Activation {8}   Publish {9}",
                            {stage(0), stage(1), stage(2), stage(3), stage(4), stage(5), stage(6), stage(7), stage(8), stage(9)}) +
                        L"\n\n" + tr(L"Excludes engine rendering and GPU time.");
    text(dc, timing.c_str(), 500, 442, 458, 150, small, Muted, DT_LEFT | DT_WORDBREAK);
    // The tests, with Stop camera tests at the bottom right.
    panel(dc, PageLeft, 182, PageWidth, 206);
    text(dc, tr(L"Camera tests"), 42, 190, 400, 28, heading);
    text(dc, tr(L"Calibration bars"), 396, 232, 130, 36, small, Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, tr(L"Scene test renders without PFD delivery. Calibration bars identify each screen and pause TAXI-button control."), 42, 274,
         916, 22, small, Muted, DT_LEFT | DT_WORDBREAK);
    const auto line = sample.heartbeat ? widen(sample.message) : live;
    text(dc, tr(L"Calibration batches per 50 ms (64–16384)"), 42, 303, 700, 24, small, Muted);
    text(dc, line.c_str(), PageLeft, 606, PageWidth, 22, small, Muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else if (page == SetupPage) {
    draw_setup(dc, sample);
  } else if (page == GuidesPage) {
    panel(dc, 244, 172, 766, 118);
    panel(dc, 244, 304, 766, 259);
    text(dc, tr(L"Nose-wheel view"), 262, 184, 250, 30, heading);
    const auto* guide_profile = profiles::find(draft().profile);
    const bool draw_guides = !guide_profile || guide_profile->reference_guides;
    const bool nose_squares = (guide_profile ? guide_profile : &profiles::A380)->composition.square_nose_markers != 0;
    text(dc, nose_squares ? tr(L"Nose squares") : tr(L"Nose dot"), 262, 239, 225, 28, normal);
    text(dc, tr(L"Tail view"), 262, 315, 250, 30, heading);
    const wchar_t* labels[]{tr(L"Upper endpoint"), tr(L"Outside corner"), tr(L"Inner endpoint")};
    for (int i = 0; i < 3; ++i)
      text(dc, labels[i], 262, 364 + i * 64, 185, 30, normal);
    text(dc, L"X (%)", 452, 210, 80, 23, small, Muted);
    text(dc, L"Y (%)", 540, 210, 80, 23, small, Muted);
    text(dc, tr(L"Left X (%)"), 452, 337, 80, 23, small, Muted);
    text(dc, tr(L"Left Y (%)"), 540, 337, 80, 23, small, Muted);
    text(dc, tr(L"Right X (%)"), 632, 337, 80, 23, small, Muted);
    text(dc, tr(L"Right Y (%)"), 720, 337, 80, 23, small, Muted);
    text(dc,
         draw_guides ? tr(L"X from the nearest side, 0–50%; Y from the top, 0–100% of each camera view. The nose guide mirrors.")
                     : tr(L"This aircraft does not use alignment markers."),
         244, 577, 766, 26, small, Muted);
    text(dc, draw_guides ? tr(L"Reset restores guide positions and colour.") : tr(L"No marker calibration is required for this profile."),
         244, 602, 766, 23, small, Muted);
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
      text(dc, tr(L"Preview"), 811, 189, 181, 23, small, Muted);
      for (const bool right : {false, true}) {
        if (nose_squares) {
          const auto nose = point(preview.nose_dot, right, 225, 45);
          const RECT marker{nose.x - scale(7), nose.y - scale(7), nose.x + scale(7), nose.y + scale(7)};
          FillRect(dc, &marker, brush);
        } else {
          dot(point(preview.nose_dot, right, 225, 45), 6);
        }
        const auto a = point(right ? preview.tail_upper_right : preview.tail_upper, right, 360, 158),
                   b = point(right ? preview.tail_corner_right : preview.tail_corner, right, 360, 158),
                   c = point(right ? preview.tail_inner_right : preview.tail_inner, right, 360, 158);
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
  if (shifted) {
    SetViewportOrgEx(dc, viewport.x, viewport.y, nullptr);
    for (size_t k = first_hit; k < hit_areas.size(); ++k)
      OffsetRect(&hit_areas[k].bounds, scale(LegacyShiftX), scale(LegacyShiftY));
    for (size_t k = first_surface; k < surfaces.size(); ++k)
      OffsetRect(&surfaces[k].bounds, scale(LegacyShiftX), scale(LegacyShiftY));
  }
  // What just happened, at the foot of the page. On a step it gives way to
  // what the step is for while there is nothing to say: "Changes are saved"
  // is true of every page and says nothing.
  const bool quiet = notice.empty() || notice == tr(L"Changes are saved for this aircraft.");
  if (step >= 0) {
    const wchar_t* purpose =
        page == SetupPage     ? tr(L"Put each cockpit display in its place: the cameras are drawn there.")
        : page == RoutingPage ? tr(L"Advanced: the display texture behind each display, for this flight.")
        : page == CamerasPage
            ? (setup.positioning ? tr(L"The cameras start from default positions. Adjust each one in the live view.")
                                 : tr(L"Adjust each taxi camera in the live view: drag to aim, arrows to move, the wheel to zoom."))
        : page == GuidesPage ? tr(L"Advanced: move the guide points; they show live and are saved for this aircraft.")
                             : tr(L"A cabin camera on a cockpit display, turned on from the flight deck.");
    const int x = built_guided ? 376 : 176, width = (built_guided ? 776 : 976) - x;
    text(dc, quiet ? purpose : notice.c_str(), x, 629, width, 40, small, !quiet && dirty ? Accent : Muted, DT_LEFT | DT_WORDBREAK);
  } else if (!quiet) {
    text(dc, notice.c_str(), PageLeft, page == OverviewPage ? 604 : 640, PageWidth, 24, small, dirty ? Accent : Muted,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
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
  win::ExitWithSimulator exit_with_simulator(background_start && !preview_ui);
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
          connection = attached ? ui_text(L"Connect requested. Retrying bridge attach.")
                                : ui_text(L"Connect requested. Waiting for Microsoft Flight Simulator 2024.");
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
      DWORD exit_code = 0;
      if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &exit_code))
        win::log_simulator_exit(win::settings_directory(), attached, exit_code);
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
        connection = connection_disconnected.load(std::memory_order_acquire)
                         ? ui_text(L"Disconnected. Choose Connect to enable the cameras again.")
                         : ui_text(L"Simulator closed. Waiting for the next session.");
      }
      PostMessageW(window, StatusMessage, 0, 0);
      // A manual start stays in the tray so a later manual or auto connect can
      // attach without depending on MSFS auto-launch of Taxi Cam. A start from
      // exe.xml quits below once the simulator stays gone.
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
      // The query right lets a crash be told from a normal exit afterwards.
      process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process)
        process = OpenProcess(SYNCHRONIZE, FALSE, pid);
      if (!auto_on && !manual_armed) {
        {
          const std::lock_guard lock(app_mutex);
          connection = ui_text(L"MSFS detected. Auto-connect is off — choose Connect when ready.");
        }
        PostMessageW(window, StatusMessage, 0, 0);
      }
    }
    if (exit_with_simulator.observe(attached != 0, now_ms))
      PostMessageW(window, ExitWithSimulatorMessage, 0, 0);
    const bool want_connect =
        win::should_attempt_connect(auto_on, attempted, command, manual_armed, connection_disconnected.load(std::memory_order_acquire));
    if (attached && want_connect && startup_retry.ready(GetTickCount64())) {
      {
        const std::lock_guard lock(app_mutex);
        if (connection_disconnected.load(std::memory_order_acquire))
          continue;
        if (!connection_requested.load(std::memory_order_acquire) && !win::begin_connection(current)) {
          attempted = true;
          connection = ui_text(L"Restart Taxi Cam to begin a new connection.");
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
            connection = ui_text(L"Could not open the camera control channel.");
            connection.add(retrying ? ui_text(L"Retrying.") : ui_text(L"Choose Disconnect, then Connect to try again."));
          }
        }
        PostMessageW(window, StatusMessage, 0, 0);
      } else {
        exchange_control(mailbox);
        win::LaunchDiagnostics launch_diagnostics;
        const bool allow_load = win::fresh_load_allowed(load_started_this_session);
        if (!preview_ui)
          check_marks_package(attach.path.empty() ? expected_simulator : attach.path);
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
            connection = {};
            connection.add(loaded.message);
            if (!loaded.ok)
              connection.add(ui_text(L"(Windows {0})"), {std::to_wstring(loaded.error)});
            if (retrying && loaded.retry_before_load && startup_retry.retries() == 0)
              connection.add(ui_text(L"Automatic recovery scheduled."));
            else if (retrying)
              connection.add(ui_text(L"Retrying startup preflight."));
            else if (loaded.retry_before_load && !loaded.ok)
              connection.add(ui_text(L"Automatic retries paused; choose Disconnect, then Connect to retry."));
            else if (!loaded.ok)
              connection.add(ui_text(L"Choose Disconnect, then Connect to try again."));
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
            connection = ui_text(L"Bridge status went stale. Retrying attach automatically.");
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
    // Bound cockpit L:vars act within a 50 ms step of this 200 ms status
    // cadence: each step writes the settings and reads the L:vars, and only a
    // changed reading wakes the window (service_lvar_bindings). The door
    // camera's VIDEO key otherwise waited 230-410 ms to turn on (2026-11-05).
    for (int step = 0; step < 4 && running.load(); ++step) {
      Sleep(50);
      win::Status quick;
      if (connection_disconnected.load(std::memory_order_acquire) || !exchange_control(mailbox, &quick))
        continue;
      bool changed = false;
      {
        const std::lock_guard lock(app_mutex);
        if (status.heartbeat && !connection_disconnected.load(std::memory_order_acquire) &&
            (quick.lvar_generation != status.lvar_generation || std::memcmp(&quick.lvars, &status.lvars, sizeof(status.lvars)) != 0)) {
          status.lvar_generation = quick.lvar_generation;
          status.lvars = quick.lvars;
          changed = true;
        }
      }
      if (changed)
        PostMessageW(window, LvarMessage, 0, 0);
    }
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
      notice = tr(L"Checking for updates in the background...");
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
        notice = result.error.empty() ? tr(L"Taxi Cam is up to date.") : tr(result.error.c_str());
        MessageBoxW(window, notice.c_str(), tr(L"Taxi Cam updates"), MB_OK | MB_ICONINFORMATION);
      }
    } else {
      update_prompt = true;
      if (win::simulator_blocks_update()) {
        update_after_simulator = true;
        notice = tr(L"Update downloaded. Close Microsoft Flight Simulator, then choose Check for updates to install.");
        if (result.manual)
          MessageBoxW(window, notice.c_str(), tr(L"Taxi Cam updates"), MB_OK | MB_ICONINFORMATION);
        else
          update_balloon();
      } else {
        const auto prompt =
            tr_format(L"Taxi Cam {0} has been downloaded and verified.\n\nClose Taxi Cam and start the installer now?", {result.tag});
        if (MessageBoxW(window, prompt.c_str(), tr(L"Taxi Cam update ready"), MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES) {
          bool proceed = true;
          if (dirty) {
            const int choice = MessageBoxW(window,
                                           tr(L"Save your unsaved settings before installing?\n\nYes: save and continue.\nNo: discard "
                                              L"changes.\nCancel: keep the app open."),
                                           tr(L"Unsaved settings"), MB_YESNOCANCEL | MB_ICONQUESTION);
            proceed = choice == IDNO || (choice == IDYES && apply());
            // Discarding includes the camera adjustment window, which saves when it closes.
            if (choice == IDNO)
              live_view.changed = false;
            // Cancel keeps the app open, as does a save that failed, so the quit
            // with the simulator stands down.
            stay_after_simulator = stay_after_simulator || choice == IDCANCEL || (choice == IDYES && !proceed);
          }
          if (proceed) {
            std::wstring error;
            if (updater.launch(result, installation, error)) {
              stop_service();
              DestroyWindow(window);
              update_prompt = false;
              return;
            }
            MessageBoxW(window, tr(error.c_str()), tr(L"Taxi Cam updates"), MB_OK | MB_ICONWARNING);
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
// The tray flyout: a small window by the tray icon with what is used in
// flight, without opening Settings. A switch per display's cameras and the
// door camera's (they act as their shortcuts do), TAXI-button control, then
// Open Taxi Cam and Exit. It closes when anything else is clicked.
constexpr wchar_t FlyoutClass[] = L"380TaxiCamera.Flyout";
constexpr int FlyoutWidth = 340, FlyoutHeader = 64, FlyoutRowHeight = 52, FlyoutFooter = 68;
constexpr int FlyoutFollow = 100, FlyoutOpen = 101, FlyoutExit = 102;
struct FlyoutRow {
  std::wstring name, keys;
  int action{};
  bool on{}, enabled = true;
};
struct FlyoutItem {
  RECT bounds{};
  int action{};
};
HWND flyout{};
ULONGLONG flyout_closed{};  // When it last closed by losing the foreground.
std::vector<FlyoutItem> flyout_items;
int flyout_hot = -1;
std::vector<FlyoutRow> flyout_rows() {
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  std::vector<FlyoutRow> rows;
  const auto keys = [](unsigned action) {
    const auto value = static_cast<WORD>(win::hotkey_control_value(hotkey_saved[action]));
    return LOBYTE(value) ? hotkey_label(value) : std::wstring();
  };
  if (profile)
    for (unsigned side = 0; side < profile->sides && side < MaxDisplaySides; ++side) {
      const unsigned action = side == 0 ? win::CameraLeft : side == 1 ? win::CameraRight : win::CameraSd;
      rows.push_back({display_side_title(s, side), keys(action), static_cast<int>(action), (s.manual_mask & (1u << side)) != 0});
    }
  if (s.door_side < MaxDisplaySides)
    rows.push_back({tr(L"Door camera"), keys(win::CameraDoor), static_cast<int>(win::CameraDoor), s.door_on != 0});
  const bool manual = profile && profile->taxi_control == profiles::TaxiControl::manual_only;
  rows.push_back({pmdg_cam_control(s) ? tr(L"CAM button") : tr(L"TAXI buttons"), {}, FlyoutFollow, s.follow_taxi && !manual, !manual});
  return rows;
}
int flyout_height(size_t rows) {
  // The cameras' rows, a rule, the TAXI-button row (the last), the footer.
  return FlyoutHeader + 6 + static_cast<int>(rows) * FlyoutRowHeight + 13 + FlyoutFooter;
}
void paint_flyout(HDC dc, const RECT& client) {
  HBRUSH fill = CreateSolidBrush(Card);
  FillRect(dc, &client, fill);
  DeleteObject(fill);
  const auto rule = [&](int y) {
    RECT line{0, scale(y), client.right, scale(y) + 1};
    HBRUSH b = CreateSolidBrush(Border);
    FillRect(dc, &line, b);
    DeleteObject(b);
  };
  const auto words = [&](const wchar_t* value, int x, int y, int w, int h, HFONT font, COLORREF color, UINT flags = DT_LEFT) {
    RECT r{scale(x), scale(y), scale(x + w), scale(y + h)};
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value, -1, &r, flags | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  };
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  // The aircraft, and the connection in a few words.
  DrawIconEx(dc, scale(16), scale(16), icon, scale(32), scale(32), 0, nullptr, DI_NORMAL);
  const auto s = draft();
  const auto* profile = profiles::find(s.profile);
  words(profile ? profile->name : L"Taxi Cam", 56, 12, FlyoutWidth - 72, 22, heading, Text);
  const auto [state, state_color] = connection_state(sample);
  disc(dc, scale(60), scale(45), scale(4), state_color);
  words(state, 70, 35, FlyoutWidth - 86, 20, small, state_color == Warning ? Warning : Muted);
  rule(FlyoutHeader);
  flyout_items.clear();
  const auto rows = flyout_rows();
  int y = FlyoutHeader + 6;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    if (row.action == FlyoutFollow) {
      rule(y + 6);
      y += 13;
    }
    const RECT bounds{scale(6), scale(y + 2), scale(FlyoutWidth - 6), scale(y + FlyoutRowHeight - 2)};
    if (row.enabled) {
      flyout_items.push_back({bounds, row.action});
      if (flyout_hot == static_cast<int>(flyout_items.size()) - 1)
        round_box(dc, bounds, scale(8), blend(Card, Text, 6));
    }
    const COLORREF ink = row.enabled ? Text : Faint;
    if (row.keys.empty()) {
      words(row.name.c_str(), 16, y, FlyoutWidth - 90, FlyoutRowHeight, normal, ink);
    } else {
      words(row.name.c_str(), 16, y + 6, FlyoutWidth - 90, 22, normal, ink);
      words(row.keys.c_str(), 16, y + 27, FlyoutWidth - 90, 18, mono, Muted);
    }
    const int track = scale(18);
    draw_switch(dc, scale(FlyoutWidth - 16) - switch_width(track), scale(y) + (scale(FlyoutRowHeight) - track) / 2, track, row.on,
                !row.enabled, Card);
    y += FlyoutRowHeight;
  }
  // The footer: the window itself, and leaving.
  RECT footer{0, scale(y), client.right, client.bottom};
  fill = CreateSolidBrush(blend(Card, Well, 45));
  FillRect(dc, &footer, fill);
  DeleteObject(fill);
  rule(y);
  const RECT open{scale(12), scale(y + 12), scale(FlyoutWidth - 12 - 84 - 8), scale(y + 12 + 44)};
  const RECT leave{scale(FlyoutWidth - 12 - 84), scale(y + 12), scale(FlyoutWidth - 12), scale(y + 12 + 44)};
  for (const bool exit : {false, true}) {
    const RECT& r = exit ? leave : open;
    flyout_items.push_back({r, exit ? FlyoutExit : FlyoutOpen});
    ButtonLook look;
    look.hot = flyout_hot == static_cast<int>(flyout_items.size()) - 1;
    button_box(dc, r, look, Card);
    RECT label = r;
    SelectObject(dc, normal);
    SetTextColor(dc, exit ? Muted : Text);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, exit ? tr(L"Exit") : tr(L"Open Taxi Cam"), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  HBRUSH edge = CreateSolidBrush(BorderStrong);
  FrameRect(dc, &client, edge);
  DeleteObject(edge);
}
// TAXI-button control from the flyout, as a shortcut would change it: the
// draft is published and saved by autosave, without reading Settings'
// fields (an unfinished value there must not stop it) or rebuilding its page.
void toggle_follow_taxi() {
  sync_aircraft_session();
  auto s = draft();
  const auto* profile = profiles::find(s.profile);
  if (profile && profile->taxi_control == profiles::TaxiControl::manual_only)
    return;
  s.follow_taxi = !s.follow_taxi;
  if (s.follow_taxi) {
    s.manual_mask = 0;
    s.calibration_mask = 0;
  }
  publish(s);
  dirty_notice();
  refresh_side_toggles(s);
  if (GetDlgItem(window, 221))
    SetDlgItemTextW(window, 221, switch_label(pmdg_cam_control(s) ? tr(L"CAM button") : tr(L"TAXI buttons"), s.follow_taxi).c_str());
  InvalidateRect(window, nullptr, FALSE);
}
int flyout_item_at(POINT point) {
  for (size_t i = 0; i < flyout_items.size(); ++i)
    if (PtInRect(&flyout_items[i].bounds, point))
      return static_cast<int>(i);
  return -1;
}
void show();
LRESULT CALLBACK flyout_procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
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
      paint_flyout(memory, client);
      BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      const int hot = flyout_item_at({static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))});
      if (hot != flyout_hot) {
        flyout_hot = hot;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      flyout_hot = -1;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT && flyout_hot >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_LBUTTONUP: {
      const int index = flyout_item_at({static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))});
      if (index < 0)
        return 0;
      const int action = flyout_items[static_cast<size_t>(index)].action;
      if (action == FlyoutOpen) {
        DestroyWindow(hwnd);
        show();
      } else if (action == FlyoutExit) {
        DestroyWindow(hwnd);
        PostMessageW(window, WM_CLOSE, 1, 0);
      } else {
        // As the shortcuts do: neither applies nor rebuilds what Settings shows.
        if (action == FlyoutFollow)
          toggle_follow_taxi();
        else
          toggle_camera_from_hotkey(static_cast<unsigned>(action));
        if (IsWindow(hwnd))
          InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE)
        DestroyWindow(hwnd);
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE) {
        // The click that takes the foreground away may be on the tray icon.
        flyout_closed = GetTickCount64();
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_DESTROY:
      if (flyout == hwnd)
        flyout = nullptr;
      flyout_hot = -1;
      return 0;
  }
  return DefWindowProcW(hwnd, message, w, l);
}
// Opens the flyout by the pointer (the tray icon was just clicked), inside
// the work area of that monitor; a second click on the icon closes it.
void open_flyout() {
  if (flyout) {
    DestroyWindow(flyout);
    return;
  }
  // Pressing the tray icon already closed it (the taskbar took the
  // foreground): that same click must not open it again.
  if (GetTickCount64() - flyout_closed < 400)
    return;
  static const bool registered = [] {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpfnWndProc = flyout_procedure;
    type.lpszClassName = FlyoutClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    return RegisterClassExW(&type) != 0;
  }();
  if (!registered)
    return;
  POINT cursor{};
  GetCursorPos(&cursor);
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
  const auto& work = monitor.rcWork;
  const int width = scale(FlyoutWidth), height = scale(flyout_height(flyout_rows().size())), margin = scale(12);
  const int x = std::clamp<int>(cursor.x - width / 2, work.left + margin, std::max<int>(work.left + margin, work.right - width - margin));
  const int y = cursor.y > (work.top + work.bottom) / 2 ? work.bottom - height - margin : work.top + margin;
  flyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, FlyoutClass, L"Taxi Cam", WS_POPUP, x, y, width, height, window, nullptr,
                           instance, nullptr);
  if (!flyout)
    return;
  DWORD corner = 2;
  DwmSetWindowAttribute(flyout, 33, &corner, sizeof(corner));
  ShowWindow(flyout, SW_SHOW);
  SetForegroundWindow(flyout);
}
// A step pill: its number in a circle, then its name. The current step is
// filled; on Overview, Displays stands out while the aircraft is not set up
// or its displays are to be chosen again.
void draw_step_pill(const DRAWITEMSTRUCT& item, unsigned step) {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  // Picture is the fourth section, after the steps.
  const bool current = step < SetupSteps ? setup_step(page) == static_cast<int>(step) : page == PicturePage;
  const bool attention = page == OverviewPage && step == 0 && (unsupported_aircraft(sample) || displays_to_choose(sample));
  const bool numbered = step < SetupSteps && page != OverviewPage && built_guided;
  const bool down = (item.itemState & ODS_SELECTED) != 0;
  const auto& r = item.rcItem;
  HBRUSH surround = CreateSolidBrush(surface_under(item.hwndItem));
  FillRect(item.hDC, &r, surround);
  DeleteObject(surround);
  const bool hot = hot_button == item.hwndItem;
  const int height = r.bottom - r.top;
  round_box(item.hDC, r, height / 2,
            current ? Accent
            : down  ? Well
            : hot   ? RaisedHot
                    : Raised,
            current || attention ? Accent
            : hot                ? BorderStrong
                                 : blend(Raised, Text, 7),
            scale(1));
  SetBkMode(item.hDC, TRANSPARENT);
  wchar_t label[96]{};
  GetWindowTextW(item.hwndItem, label, 96);
  RECT label_box{r.left + scale(10), r.top, r.right - scale(10), r.bottom};
  if (numbered) {
    const int d = scale(20), cx = r.left + scale(8), cy = r.top + (height - d) / 2;
    disc(item.hDC, cx + d / 2, cy + d / 2, d / 2, current ? Well : blend(Raised, Text, 14));
    wchar_t number[4];
    std::swprintf(number, 4, L"%u", step + 1);
    RECT number_box{cx, cy, cx + d, cy + d};
    SelectObject(item.hDC, small);
    SetTextColor(item.hDC, Text);
    DrawTextW(item.hDC, number, -1, &number_box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    label_box.left = cx + d + scale(8);
  }
  SelectObject(item.hDC, normal);
  SetTextColor(item.hDC, current ? Well : Text);
  RECT extent{};
  DrawTextW(item.hDC, label, -1, &extent, DT_CALCRECT | DT_SINGLELINE);
  if (extent.right > label_box.right - label_box.left)
    SelectObject(item.hDC, small);
  DrawTextW(item.hDC, label, -1, &label_box, (numbered ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
// The page a navigation button stands for: Setup for every step.
bool navigation_selected(int id) {
  return id == 100 + CamerasPage ? setup_step(page) >= 0 : id - 100 == page;
}
// The buttons that are switches: named for a setting, and On or Off.
bool is_switch(int id) {
  switch (id) {
    case AdvancedId:
    case DoorToggleId:
    case 221:
    case 223:
    case 224:
    case 225:
    case 226:
    case 227:
    case 229:
    case 230:
    case 232:
    case 233:
    case 240:
    case NotificationsId:
      return true;
    default:
      return id >= SetupTestId && id < SetupTestId + 3;
  }
}
bool is_on(int id, const win::Settings& s) {
  switch (id) {
    case AdvancedId:
      return advanced;
    case DoorToggleId:
      return s.door_on;
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
    case 233:
      return s.calibration_mask & 4;
    case 232:
      return s.manual_mask & 4;
    case 229:
      return s.scene_test;
    case NotificationsId:
      return s.notifications != 0;
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
      // Windows 11 colours the caption and border to match; Windows 10 ignores these.
      COLORREF caption = Well, edge = Border;
      DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
      DwmSetWindowAttribute(hwnd, 34, &edge, sizeof(edge));
      icon = make_icon();
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
      taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
      register_camera_hotkeys();
      update_camera_button_input();
      if (hotkey_registration.conflicts())
        notice = tr(L"Some shortcuts are unavailable. Check Aircraft > Taxi cameras.");
      else if (win::any_camera_button(button_saved) && !preview_ui && !button_input.enabled())
        notice = tr(L"Controller buttons are unavailable. Check Aircraft > Taxi cameras.");
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
      retry_unsaved_setup();
      autosave();
      return 0;
    case WM_HOTKEY: {
      const int action = hotkey_registration.action(w, l);
      const auto focus = GetFocus();
      const auto focused_id = focus && GetParent(focus) == hwnd ? GetDlgCtrlID(focus) : 0;
      if (action >= 0 && !preview_ui && !(focused_id >= 620 && focused_id < 620 + static_cast<int>(win::CameraHotkeyNames.size())))
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
      // The layout is one fixed size; the frame goes around its client area.
      RECT frame{0, 0, scale(ClientWidth), scale(ClientHeight)};
      AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
      info->ptMinTrackSize = info->ptMaxTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
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
    case WM_MOUSEMOVE:
      if (setup.dragging) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        move_drag_ghost();
        // The display under the cursor lights up.
        if (const int side = drop_side_at({static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))}); side != setup.drop_side) {
          setup.drop_side = side;
          // The slots' buttons too: their corners show the slot's colour.
          RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      if (setup.dragging && reinterpret_cast<HWND>(l) != hwnd) {
        setup.dragging = false;
        setup.drag_slot = -1;
        setup.drop_side = -1;
        hide_drag_ghost();
        refresh_setup_picking();
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
      }
      break;
    case WM_LBUTTONUP: {
      const POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
      if (setup.dragging) {
        end_card_drag(point);
        return 0;
      }
      // A picture's power and look icons come before the picture under it.
      if (const auto* power = power_area_at(point)) {
        if (power->id >= LookGrainId)
          toggle_camera_look(power->id);
        else
          toggle_camera_from_hotkey(power->id == DoorPowerId ? win::CameraDoor : win::CameraBoth);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (const auto* area = hit_area_at(point)) {
        if (area->side >= 0)
          show_area_picker(static_cast<unsigned>(area->side));
        else if (area->id)
          show_display_snapshot(area->id);
        else
          open_step(0);
        return 0;
      }
      break;
    }
    case WM_SETCURSOR:
      if (reinterpret_cast<HWND>(w) == hwnd && LOWORD(l) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (hit_area_at(point) || power_area_at(point)) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      if (button_cursor(hwnd, w, l))
        return TRUE;
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
        InvalidateRect(reinterpret_cast<HWND>(l), nullptr, FALSE);  // The fill up to the thumb.
        InvalidateRect(hwnd, nullptr, FALSE);
        dirty_notice();
        return 0;
      }
      for (int i = 0; i < 2; ++i)
        if (l && reinterpret_cast<HWND>(l) == GetDlgItem(hwnd, BrightnessSliderId + i)) {
          // A brightness slider writes its value field, which is what is read
          // and saved (its change marks the page unsaved).
          wchar_t value[16];
          std::swprintf(value, 16, L"%+.2f", (static_cast<int>(SendMessageW(reinterpret_cast<HWND>(l), TBM_GETPOS, 0, 0)) - 16) / 4.);
          SetDlgItemTextW(hwnd, 201 + i, value);
          InvalidateRect(reinterpret_cast<HWND>(l), nullptr, FALSE);
          return 0;
        }
      break;
    case WM_NOTIFY: {
      // Sliders are drawn in the window's colours: a rounded channel, filled
      // up to a round thumb.
      const auto* header = reinterpret_cast<const NMHDR*>(l);
      wchar_t type[32]{};
      if (header->code != static_cast<UINT>(NM_CUSTOMDRAW) || !GetClassNameW(header->hwndFrom, type, 32) ||
          lstrcmpiW(type, TRACKBAR_CLASSW) != 0)
        break;
      const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(l);
      if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
      if (draw->dwDrawStage != CDDS_ITEMPREPAINT)
        break;
      const bool enabled = IsWindowEnabled(header->hwndFrom) != 0;
      if (draw->dwItemSpec == TBCD_CHANNEL) {
        RECT bounds{}, thumb{};
        GetClientRect(header->hwndFrom, &bounds);
        SendMessageW(header->hwndFrom, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const int middle = (bounds.top + bounds.bottom) / 2, half = std::max(1, scale(2)), inset = scale(8);
        const RECT channel{bounds.left + inset, middle - half, bounds.right - inset, middle + half};
        round_box(draw->hdc, channel, half, enabled ? Raised : blend(Card, Text, 6));
        if (enabled)
          round_box(draw->hdc, {channel.left, channel.top, (thumb.left + thumb.right) / 2, channel.bottom}, half, Accent);
      } else if (draw->dwItemSpec == TBCD_THUMB) {
        const auto& r = draw->rc;
        disc(draw->hdc, (r.left + r.right) / 2, (r.top + r.bottom) / 2, scale(7), enabled ? Text : Faint);
      }
      return CDRF_SKIPDEFAULT;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      auto dc = reinterpret_cast<HDC>(w);
      // Fields sit in a framed well (field_frame).
      if (message == WM_CTLCOLOREDIT) {
        SetTextColor(dc, Text);
        SetBkColor(dc, Background);
        return reinterpret_cast<LRESULT>(background_brush);
      }
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
      if (id >= StepPillId && id < StepPillId + static_cast<int>(SetupSteps)) {
        draw_step_pill(*item, static_cast<unsigned>(id - StepPillId));
        return TRUE;
      }
      if (id == 100 + PicturePage) {
        draw_step_pill(*item, SetupSteps);
        return TRUE;
      }
      if (id == SetupSwapId) {
        draw_swap_button(*item);
        return TRUE;
      }
      if (draw_page_button(*item))
        return TRUE;
      if (id >= 680 && id < 680 + static_cast<int>(win::CameraHotkeyNames.size())) {
        // The controller button's field: select it, then press the button.
        const unsigned action = static_cast<unsigned>(id - 680);
        const bool capturing = button_capture == static_cast<int>(action);
        draw_field_frame(item->hDC, item->rcItem, capturing || (item->itemState & ODS_FOCUS));
        wchar_t label[160]{};
        GetWindowTextW(item->hwndItem, label, 160);
        RECT r = item->rcItem;
        InflateRect(&r, -scale(8), 0);
        const auto old = SelectObject(item->hDC, small);
        SetTextColor(item->hDC, capturing ? Accent : button_draft[action].button ? Text : Muted);
        SetBkMode(item->hDC, TRANSPARENT);
        DrawTextW(item->hDC, label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item->hDC, old);
        return TRUE;
      }
      const bool disabled = (item->itemState & ODS_DISABLED) != 0;
      const bool selected = !disabled && ((id >= 100 && id < 106 && navigation_selected(id)) || is_on(id, draft()) ||
                                          page_button_selected(id) || (id >= 420 && id < 428 && gallery_button_routed(id)) ||
                                          (id >= SetupTestId && id < SetupTestId + 3 && ((setup.testing >> (id - SetupTestId)) & 1)));
      const bool hot = !disabled && hot_button == item->hwndItem;
      const bool down = !disabled && (item->itemState & ODS_SELECTED) != 0;
      const bool focus = (item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT);
      const COLORREF surface = surface_under(item->hwndItem);
      HBRUSH surround = CreateSolidBrush(surface);
      FillRect(item->hDC, &item->rcItem, surround);
      DeleteObject(surround);
      SetBkMode(item->hDC, TRANSPARENT);
      wchar_t label[160]{};
      GetWindowTextW(item->hwndItem, label, 160);
      RECT r = item->rcItem;
      if (id >= 512 && id <= 515 && page == OverviewPage) {
        // The footer's links: quiet until the pointer is over them.
        const int saved = SaveDC(item->hDC);
        SelectObject(item->hDC, hot ? version_font : small);
        SetTextColor(item->hDC, disabled ? Faint : hot || down ? Text : Muted);
        DrawTextW(item->hDC, label, -1, &r, (id >= 514 ? DT_LEFT : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE);
        RestoreDC(item->hDC, saved);
        return TRUE;
      }
      if (id == 100 + OverviewPage || id == 100 + CamerasPage || id == 100 + PreferencesPage || id == 100 + DiagnosticsPage) {
        // A place in the header: its name, and under the one that is open
        // the accent.
        const bool open = id == 100 + OverviewPage      ? page == OverviewPage
                          : id == 100 + CamerasPage     ? aircraft_page(page)
                          : id == 100 + PreferencesPage ? page == PreferencesPage
                                                        : page == DiagnosticsPage;
        if (hot || down || focus)
          round_box(item->hDC, r, scale(8), blend(surface, Text, down ? 4 : hot ? 7 : 0), focus && !hot ? BorderStrong : NoColor, scale(1));
        SelectObject(item->hDC, normal);
        SetTextColor(item->hDC, open || hot ? Text : Muted);
        RECT extent{};
        DrawTextW(item->hDC, label, -1, &extent, DT_CALCRECT | DT_SINGLELINE);
        if (extent.right > r.right - r.left - scale(8))
          SelectObject(item->hDC, small);
        DrawTextW(item->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (open) {
          const int middle = (r.left + r.right) / 2, half = scale(14);
          round_box(item->hDC, {middle - half, r.bottom - scale(4), middle + half, r.bottom - scale(1)}, scale(2), Accent);
        }
        return TRUE;
      }
      if (id == 602) {
        // The header's menu: three dots.
        ButtonLook look;
        look.hot = hot;
        look.down = down;
        look.focus = focus;
        button_box(item->hDC, r, look, surface);
        const float cx = (r.left + r.right) / 2.f, cy = (r.top + r.bottom) / 2.f, u = scale(16) / 16.f;
        for (const int dot : {-1, 0, 1})
          disc(item->hDC, static_cast<int>(cx + dot * 5 * u), static_cast<int>(cy), std::max(1, scale(3) / 2), hot ? Text : Muted);
        return TRUE;
      }
      const bool primary =
          id == 500 || id == NextId || id == HomeActionId || (id >= SetupPlaceId && id < SetupPlaceId + static_cast<int>(MaxDisplaySides));
      const bool toggle = is_switch(id);
      ButtonLook look;
      look.primary = primary;
      look.chosen = selected && !toggle;
      look.hot = hot;
      look.down = down;
      look.disabled = disabled;
      look.focus = focus;
      button_box(item->hDC, r, look, surface);
      // Clear, for a shortcut or a controller button, is a ×.
      if ((id >= 630 && id < 630 + static_cast<int>(win::CameraHotkeyNames.size())) ||
          (id >= 690 && id < 690 + static_cast<int>(win::CameraHotkeyNames.size())))
        std::wcscpy(label, L"×");
      SelectObject(item->hDC, normal);
      SetTextColor(item->hDC, button_text(look));
      const int track = scale(18), track_width = switch_width(track);
      const bool bare = page == OverviewPage && (id == 224 || id == 225 || id == 232);
      const bool row = (page == PreferencesPage && (id == 230 || id == 240 || id == NotificationsId)) ||
                       (page == OverviewPage && (id == 221 || id == DoorToggleId));
      if (toggle && (bare || row)) {
        // A switch by itself beside its display, or a row of Options: its name
        // across the row and its switch at the right, on the card.
        HBRUSH under = CreateSolidBrush(surface);
        FillRect(item->hDC, &item->rcItem, under);
        DeleteObject(under);
        if (hot || focus)
          round_box(item->hDC, item->rcItem, scale(8), blend(surface, Text, hot ? 5 : 0), focus ? BorderStrong : NoColor, scale(1));
        const int switch_x = bare ? (r.left + r.right - track_width) / 2 : r.right - scale(8) - track_width;
        draw_switch(item->hDC, switch_x, (r.top + r.bottom - track) / 2, track, selected, disabled, surface);
        if (row) {
          for (const bool state : {true, false}) {
            const auto suffix = state ? tr_format(L"{0}: On", {L""}) : tr_format(L"{0}: Off", {L""});
            const auto length = std::wcslen(label);
            if (!suffix.empty() && length > suffix.size() && !std::wcscmp(label + length - suffix.size(), suffix.c_str())) {
              label[length - suffix.size()] = 0;
              break;
            }
          }
          RECT words{r.left + scale(8), r.top, switch_x - scale(8), r.bottom};
          SetTextColor(item->hDC, disabled ? Faint : Text);
          DrawTextW(item->hDC, label, -1, &words, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
          // What it does, in a few words, after its name.
          const wchar_t* does = id == 240               ? tr(L"Connect when the simulator starts.")
                                : id == 230             ? tr(L"Switch to the aircraft you load.")
                                : id == NotificationsId ? tr(L"Tell me when the cameras need attention.")
                                : id == 221             ? tr(L"The cockpit's buttons turn the cameras on.")
                                                        : nullptr;
          RECT extent{};
          DrawTextW(item->hDC, label, -1, &extent, DT_CALCRECT | DT_SINGLELINE);
          words.left += extent.right + scale(14);
          if (does && words.right - words.left > scale(80)) {
            SelectObject(item->hDC, small);
            SetTextColor(item->hDC, disabled ? Faint : Muted);
            DrawTextW(item->hDC, does, -1, &words, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
          }
        }
        return TRUE;
      }
      if (toggle) {
        // A switch: its name at the left, the switch itself at the right. The
        // window text still says On or Off; the name is that text without it.
        for (const bool state : {true, false}) {
          const auto suffix = state ? tr_format(L"{0}: On", {L""}) : tr_format(L"{0}: Off", {L""});
          const auto length = std::wcslen(label);
          if (!suffix.empty() && length > suffix.size() && !std::wcscmp(label + length - suffix.size(), suffix.c_str())) {
            label[length - suffix.size()] = 0;
            break;
          }
        }
        const int width = track_width;
        draw_switch(item->hDC, r.right - scale(10) - width, (r.top + r.bottom - track) / 2, track, selected, disabled, Raised);
        RECT words{r.left + scale(12), r.top, r.right - scale(18) - width, r.bottom};
        RECT extent{};
        DrawTextW(item->hDC, label, -1, &extent, DT_CALCRECT | DT_SINGLELINE);
        if (extent.right > words.right - words.left) {
          SelectObject(item->hDC, small);
          words.left -= scale(4);
          words.right += scale(4);
        }
        DrawTextW(item->hDC, label, -1, &words, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return TRUE;
      }
      InflateRect(&r, -scale(10), 0);
      // A translation can outgrow a button sized for English: step down to the
      // small font with less padding, and shorten with an ellipsis only after that.
      RECT extent{};
      DrawTextW(item->hDC, label, -1, &extent, DT_CALCRECT | DT_SINGLELINE);
      if (extent.right > r.right - r.left) {
        SelectObject(item->hDC, small);
        InflateRect(&r, scale(6), 0);
      }
      DrawTextW(item->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      return TRUE;
    }
    case AreaPickerMessage:
      if (page == SetupPage)
        show_area_picker(static_cast<unsigned>(w));
      return 0;
    case LvarMessage:
      service_lvar_bindings();
      return 0;
    case StatusMessage:
      refresh_connection_button();
      sync_aircraft_session();
      auto_profile();
      convert_legacy_displays();
      prompt_choose_displays();
      service_marks_package_notice();
      service_package_inactive_alert();
      service_lvar_bindings();
      show_notifications();
      update_gallery_buttons();
      update_setup_buttons();
      service_thumbnails();
      if (shortcut_window == window)
        refresh_lvar_status(window);
      {
        // Home shows the displays once the simulator does, and a step its
        // Back and Next while the aircraft is being set up: either changing
        // rebuilds the page, without touching fields being edited elsewhere.
        win::Status latest;
        {
          const std::lock_guard lock(app_mutex);
          latest = status;
        }
        if ((page == OverviewPage && (latest.graphics_ready != 0) != built_with_displays) ||
            (setup_step(page) >= 0 && guided_setup(latest) != built_guided && !dirty))
          build_controls();
      }
      if (flyout) {
        // Its rows follow the aircraft: keep its height to them, growing
        // away from the edge of the screen it opened at.
        RECT bounds{};
        GetWindowRect(flyout, &bounds);
        const int height = scale(flyout_height(flyout_rows().size()));
        if (height != bounds.bottom - bounds.top) {
          MONITORINFO monitor{};
          monitor.cbSize = sizeof(monitor);
          GetMonitorInfoW(MonitorFromWindow(flyout, MONITOR_DEFAULTTONEAREST), &monitor);
          const bool low = bounds.top > (monitor.rcWork.top + monitor.rcWork.bottom) / 2;
          SetWindowPos(flyout, nullptr, bounds.left, low ? bounds.bottom - height : bounds.top, bounds.right - bounds.left, height,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(flyout, nullptr, FALSE);
      }
      if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        InvalidateRect(hwnd, nullptr, FALSE);
        invalidate_camera_mode_cards();
      }
      return 0;
    case ExitWithSimulatorMessage:
      // The tray Quit path: unsaved edits are discarded, as by tray Quit, since
      // the draft also holds previews and calibration takeovers nobody chose to
      // keep. It never waits for Settings to close: a companion left running
      // keeps Steam showing the simulator as running. The worker asks again on
      // every loop, so the quit waits only for a modal dialog (it disables the
      // window) or an update check. An update that downloaded while the
      // simulator ran is offered first; a manual check reuses that download.
      if (stay_after_simulator)
        return 0;
      if (update_after_simulator && !update_prompt && !updater.busy()) {
        update_after_simulator = false;
        check_updates(true);
        poll_updates();
        return 0;
      }
      if (update_prompt || updater.busy() || !IsWindowEnabled(hwnd))
        return 0;
      // The camera adjustment window saves the whole draft when it closes;
      // here it is discarded with the rest.
      live_view.changed = false;
      PostMessageW(hwnd, WM_CLOSE, 1, 0);
      return 0;
    case WhatsNewMessage:
      show_whats_new();
      return 0;
    case WM_CONTEXTMENU:
      PostMessageW(hwnd, TrayMessage, 1, WM_CONTEXTMENU);
      return 0;
    case TrayMessage:
      if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP) {
        HMENU menu = CreatePopupMenu();
        if (w != 1)  // From the window's own Menu button, the window is already open.
          AppendMenuW(menu, MF_STRING, 600, tr(L"Open Taxi Cam"));
        AppendMenuW(menu, MF_STRING | (preview_ui ? MF_GRAYED : 0), 604,
                    tr(win::connection_button_label(connection_requested.load(std::memory_order_acquire))));
        AppendMenuW(menu, MF_STRING | (preview_ui || updater.busy() || update_prompt ? MF_GRAYED : 0), 603,
                    updater.busy() ? tr(L"Checking for updates...") : tr(L"Check for updates"));
        AppendMenuW(menu, MF_STRING | (draft().notifications ? MF_CHECKED : MF_UNCHECKED), 605, tr(L"Show notifications"));
        // Native language names are not translated. Not while a dialog is open
        // over Settings: its controls would lose their fonts.
        HMENU languages = language_menu();
        AppendMenuW(menu, MF_POPUP | (IsWindowEnabled(hwnd) ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(languages), tr(L"Language"));
        AppendMenuW(menu, MF_STRING, 512, tr(L"Report a bug"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 601, tr(L"Exit"));
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
          toggle_notifications();
        }
        if (selected >= LanguageMenuId && selected <= LanguageMenuId + std::size(win::UiLanguages))
          choose_language(selected == LanguageMenuId ? L"" : win::UiLanguages[selected - LanguageMenuId - 1].tag);
        if (selected == 512)
          report_bug();
        if (selected == 601) {
          stop_service();
          DestroyWindow(hwnd);
        }
        PostMessageW(hwnd, WM_NULL, 0, 0);
      } else if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) {
        // A click opens the flyout; a double click, or a notification's, Settings.
        open_flyout();
      } else if (LOWORD(l) == NIN_BALLOONUSERCLICK || LOWORD(l) == WM_LBUTTONDBLCLK) {
        if (flyout)
          DestroyWindow(flyout);
        show();
      }
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(w);
      if (refreshing)
        return 0;
      if (id >= StepPillId && id < StepPillId + static_cast<int>(SetupSteps)) {
        open_step(static_cast<unsigned>(id - StepPillId));
        return 0;
      }
      if (id == BackId) {
        back_step();
        return 0;
      }
      if (id == NextId) {
        next_step();
        return 0;
      }
      if (id == AdvancedId) {
        // A step's Advanced page closes with Advanced.
        const int base = page == RoutingPage ? SetupPage : page == GuidesPage ? CamerasPage : page;
        if (base != page && !leave_page(base))
          return 0;
        advanced = !advanced;
        page = advanced ? page : base;
        build_controls();
        return 0;
      }
      if (id == DoorToggleId) {
        toggle_camera_from_hotkey(win::CameraDoor);
        build_controls();
        return 0;
      }
      if (id == DoorChooseId) {
        // A pilot pair places its lower display first, in Displays.
        const auto* base = profiles::base_profile(draft().profile);
        const auto* profile = profiles::find(draft().profile);
        open_step(base && profile && profile->sides < MaxDisplaySides && profiles::pilot_pair(*base) ? 0 : 2);
        return 0;
      }
      if (handle_controls_command(id, HIWORD(w)))
        return 0;
      if (id == 646) {
        show_saved_list();
        return 0;
      }
      if (id >= SetupCardButtonId && id < SetupCardButtonId + static_cast<int>(GalleryColumns)) {
        // A click picks the panel for Place here (again: unpicks it); a
        // double click enlarges its picture.
        const auto slot = static_cast<unsigned>(id - SetupCardButtonId);
        if (HIWORD(w) == BN_DOUBLECLICKED) {
          setup.picked = setup.card_names[slot];
          setup.picked_texture = setup.ids[slot];
          if (setup.ids[slot])
            show_display_snapshot(setup.ids[slot]);
        } else if (HIWORD(w) == BN_CLICKED) {
          const bool again = setup.picked == setup.card_names[slot];
          setup.picked = again ? std::string() : setup.card_names[slot];
          setup.picked_texture = again ? 0 : setup.ids[slot];
        }
        refresh_setup_picking();
        return 0;
      }
      if (id >= SetupPlaceId && id < SetupPlaceId + static_cast<int>(MaxDisplaySides)) {
        place_setup_panel(static_cast<unsigned>(id - SetupPlaceId));
        refresh_setup_picking();
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
      if (id >= SetupTestId && id < SetupTestId + 3) {
        setup.testing ^= 1u << (id - SetupTestId);
        apply_setup_trial();
        build_controls();
        return 0;
      }
      if (id == SetupSwapId) {
        swap_pilot_displays();
        return 0;
      }
      if (id == SetupRefreshId) {
        thumbnails.fresh_after = GetTickCount64();
        service_thumbnails();
        InvalidateRect(hwnd, nullptr, FALSE);
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
          notice = tr(L"Your choices are cleared. Named and detected displays apply again.");
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
        // A typed brightness moves its slider.
        if (HIWORD(w) == EN_CHANGE && (id == 201 || id == 202) && GetDlgItem(hwnd, BrightnessSliderId + id - 201)) {
          wchar_t value[40]{};
          GetDlgItemTextW(hwnd, id, value, 40);
          wchar_t* end{};
          const double typed = std::wcstod(value, &end);
          if (end != value && std::isfinite(typed)) {
            SendDlgItemMessageW(hwnd, BrightnessSliderId + id - 201, TBM_SETPOS, TRUE,
                                std::clamp(static_cast<int>(std::lround(typed * 4)) + 16, 0, 24));
            InvalidateRect(GetDlgItem(hwnd, BrightnessSliderId + id - 201), nullptr, FALSE);
          }
        }
        dirty_notice();
        return 0;
      }
      if (id == NotificationsId) {
        // Only its own label changes: rebuilding the page would drop a
        // brightness or frame-rate edit that has not been saved yet.
        toggle_notifications();
        return 0;
      }
      if (id == LanguageId) {
        // The languages drop from the button; not while a dialog is open over
        // the window (its controls would lose their fonts).
        if (!IsWindowEnabled(hwnd))
          return 0;
        RECT r{};
        GetWindowRect(GetDlgItem(hwnd, LanguageId), &r);
        HMENU languages = language_menu();
        const UINT selected = TrackPopupMenu(languages, TPM_RETURNCMD | TPM_LEFTBUTTON, r.left, r.bottom, 0, hwnd, nullptr);
        DestroyMenu(languages);
        if (selected >= LanguageMenuId && selected <= LanguageMenuId + std::size(win::UiLanguages))
          choose_language(selected == LanguageMenuId ? L"" : win::UiLanguages[selected - LanguageMenuId - 1].tag);
        return 0;
      }
      if (id == HomeActionId) {
        open_step(0);
        return 0;
      }
      if (id == UpdatesId) {
        check_updates(true);
        return 0;
      }
      if (id >= 100 && id <= 100 + PreferencesPage) {
        if (id == 100 + CamerasPage)
          open_step(0);
        else
          show_page(id - 100);
        return 0;
      }
      if (id == 602) {
        PostMessageW(hwnd, TrayMessage, 1, WM_CONTEXTMENU);
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
          MessageBoxW(hwnd, tr(L"Could not open your browser. Visit https://github.com/rthoms334/taxi-cam."), tr(L"Taxi Cam on GitHub"),
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
          notice = tr(L"Could not load that aircraft profile.");
          return 0;
        }
        next.enabled = draft().enabled;
        if (!win::prepare_profile_selection(next, draft(), false) || !win::save_settings(next)) {
          notice = tr(L"Could not apply that aircraft profile.");
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        publish(next);
        dirty = false;
        notice = tr(L"Aircraft profile selected. Reconnecting its cameras and displays.");
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
          notice = tr(L"Could not save the Auto-connect preference.");
        else
          notice = next ? tr(L"Auto-connect on. Taxi Cam will connect when the simulator is available.")
                        : tr(L"Auto-connect off. Use Connect whenever you are ready, including in a loaded flight.");
        SetDlgItemTextW(hwnd, 240, switch_label(tr(L"Auto-connect"), next).c_str());
        InvalidateRect(hwnd, nullptr, FALSE);
        if (next)
          request_connection(win::ConnectCommand::connect);
        // Options keeps its fields: only this switch's label changed, and a
        // rebuild would drop an edit there that has not been saved yet.
        if (page != PreferencesPage)
          build_controls();
        return 0;
      }
      if (id == 241) {
        toggle_connection();
        return 0;
      }
      if (id == 221 || (id >= 223 && id <= 227) || id == 229 || id == 232 || id == 233) {
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
        if (id == 229)
          s.scene_test = !s.scene_test;
        publish(s);
        dirty_notice();
        build_controls();
        return 0;
      }
      if (id >= LiveFeedId && id < LiveFeedId + static_cast<int>(profiles::CameraViews)) {
        if (apply(false))
          open_live_view(static_cast<unsigned>(id - LiveFeedId));
        return 0;
      }
      if (id == DisplayAreaId) {
        if (apply(false))
          open_live_view(0, true);
        return 0;
      }
      if (id == DoorAreaId) {
        if (apply(false))
          open_live_view(profiles::DoorFeed, true, draft().door_side);
        return 0;
      }
      if (id >= DefaultsId && id < DefaultsId + static_cast<int>(profiles::CameraViews)) {
        if (!apply(false))
          return 0;
        auto s = draft();
        if (const auto* profile = profiles::find(s.profile))
          s.mounts[static_cast<unsigned>(id - DefaultsId)] = profile->mounts[static_cast<unsigned>(id - DefaultsId)];
        publish(s);
        dirty_notice();
        build_controls();
        return 0;
      }
      if (id == RoutingOpenId || id == GuidesOpenId) {
        show_page(id == RoutingOpenId ? RoutingPage : GuidesPage);
        return 0;
      }
      if ((id >= DoorWhereId && id <= DoorWhereId + static_cast<int>(MaxDisplaySides)) || id == DoorSnapshotId || id == DoorLiveId) {
        if (!apply(false))
          return 0;
        auto s = draft();
        if (id == DoorSnapshotId || id == DoorLiveId) {
          s.door_live = id == DoorLiveId ? 1u : 0u;
          // Picked while the live view holds the door camera: the choice is
          // what it gives back and what saves keep.
          if (door_hold_aircraft(s))
            live_view.door_live_before = s.door_live;
        } else {
          s.door_side = id == DoorWhereId ? win::DoorSideNone : static_cast<std::uint32_t>(id - DoorWhereId - 1);
          if (s.door_side == win::DoorSideNone)
            s.door_on = 0;
        }
        if (win::valid_settings(s)) {
          publish(s);
          dirty_notice();
          build_controls();
        }
        return 0;
      }
      if (id == 370) {
        const auto* guide_profile = profiles::find(draft().profile);
        if (guide_profile && !guide_profile->reference_guides)
          return 0;
        apply();
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
          notice = tr(L"Profile guide positions and colour restored.");
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
            notice = tr(L"Waiting for cockpit displays to be drawn. Restart Flight only if the list stays empty.");
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
          notice = tr(L"Left and right swapped for this flight. Clear choices goes back to automatic selection.");
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
  // Before any wording is shown, and before the fonts are made for its script.
  language_preference = win::load_language(win::settings_directory());
  win::select_language(win::resolve_language(language_preference, win::windows_display_languages()));
  notice = tr(L"Changes are saved for this aircraft.");
  win::migrate_saved_camera_rate();
  if (!win::load_settings(current, installation))
    notice = tr(L"Saved settings were invalid; profile defaults loaded.");
  if (current.profile == profiles::DiscoveredProfileId)
    profiles::publish_discovered(current.discovered);
  profiles::publish_override(profiles::base_profile(current.profile), current.display_override, current.door_side);
  whats_new = win::whats_new_pending(win::settings_directory(), InstalledVersion);
  // enabled is runtime connection state, not saved: it starts off until
  // Connect or Auto-connect enables it.
  current.enabled = 0;
  auto_connect.store(win::load_auto_connect(win::settings_directory()), std::memory_order_release);
  if (!win::load_camera_hotkeys(hotkey_saved, win::settings_directory()))
    notice = tr(L"Saved shortcuts were invalid and disabled. Configure them in Aircraft > Taxi cameras.");
  hotkey_draft = hotkey_saved;
  if (!win::load_camera_buttons(button_saved, win::settings_directory()))
    notice = tr(L"Saved controller buttons were invalid and disabled. Configure them in Aircraft > Taxi cameras.");
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
  HWND main = CreateWindowExW(WS_EX_APPWINDOW, WindowClass, L"Taxi Cam",
                              (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                              scale(ClientWidth), scale(ClientHeight), nullptr, nullptr, instance, nullptr);
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
      notice = tr(L"Could not remember the first launch. Settings may reopen next time.");
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
