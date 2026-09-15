#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>
#include "launcher.hpp"
#include "../shared/protocol.hpp"
#include "settings_store.hpp"
#include "bug_report.hpp"
#include "updater.hpp"

namespace {
using namespace taxi_camera;
namespace win = standalone;
constexpr UINT TrayMessage = WM_APP + 1, StatusMessage = WM_APP + 2;
constexpr wchar_t WindowClass[] = L"380TaxiCamera.Settings";
constexpr COLORREF Background = RGB(17, 21, 28), Sidebar = RGB(12, 16, 22), Card = RGB(26, 32, 41), Border = RGB(44, 54, 67),
                   Text = RGB(232, 238, 246), Muted = RGB(154, 170, 188), Accent = RGB(66, 219, 184);
HINSTANCE instance{};
HWND window{}, report_tooltip{};
HFONT normal{}, small{}, title_font{}, heading{};
HBRUSH background_brush{}, card_brush{};
HICON icon{};
UINT dpi = 96, taskbar_created{};
int page = 0;
std::vector<HWND> controls;
std::vector<HWND> navigation;
std::vector<std::uint64_t> combo_ids;
std::wstring installation, expected_simulator, notice = L"Changes are saved for this aircraft.";
std::mutex app_mutex;
win::Settings current;
win::Status status;
bool received_bridge_status{};  // Guarded by app_mutex; retained across simulator sessions.
std::wstring connection = L"Waiting for Microsoft Flight Simulator 2024";
std::atomic<bool> running{true};
std::atomic<DWORD> simulator_pid{};
HANDLE worker{}, show_event{}, singleton{};
bool dirty = false, refreshing = false, background_start = false, preview_ui = false;
win::Updater updater;
ULONGLONG next_update_check{};
bool update_prompt{};
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
  std::swprintf(buffer, 64, id == 201 || id == 202 ? L"%.4g" : L"%.10g", value);
  auto h = child(L"EDIT", buffer, id, x, y, w, 30, ES_AUTOHSCROLL | ES_LEFT | WS_BORDER);
  SendMessageW(h, EM_SETLIMITTEXT, 32, 0);
}
void toggle(const wchar_t* label, int id, bool enabled, int x, int y, int width = 125) {
  const auto text = std::wstring(label) + (enabled ? L": On" : L": Off");
  button(text.c_str(), id, x, y, width);
}
void make_fonts() {
  for (HFONT f : {normal, small, title_font, heading})
    if (f)
      DeleteObject(f);
  auto make = [](int size, int weight) {
    return CreateFontW(-scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable");
  };
  normal = make(15, FW_NORMAL);
  small = make(13, FW_NORMAL);
  title_font = make(30, FW_SEMIBOLD);
  heading = make(18, FW_SEMIBOLD);
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
  current = value;
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
bool read_fields(win::Settings& settings) {
  bool ok = true;
  const double rate = number(200, settings.camera_rate, ok);
  if (rate < 15 || rate > 60 || std::floor(rate) != rate)
    ok = false;
  if (ok)
    settings.camera_rate = static_cast<UINT>(rate);
  const double budget = number(203, settings.calibration_budget, ok);
  if (budget < 64 || budget > 16384 || std::floor(budget) != budget)
    ok = false;
  if (ok)
    settings.calibration_budget = static_cast<UINT>(budget);
  settings.exposure = static_cast<float>(number(201, settings.exposure, ok));
  settings.night_boost = static_cast<float>(number(202, settings.night_boost, ok));
  for (unsigned i = 0; i < 2; ++i)
    for (unsigned j = 0; j < 6; ++j)
      settings.mounts[i][j] = number(300 + static_cast<int>(i * 10 + j), settings.mounts[i][j], ok);
  if (page == 3) {
    for (unsigned i = 0; i < 2; ++i) {
      const LRESULT selected = SendDlgItemMessageW(window, 400 + i, CB_GETCURSEL, 0, 0);
      if (selected == 0) {
        if (i)
          settings.right_id = 0;
        else
          settings.left_id = 0;
      }
      if (selected > 0 && static_cast<size_t>(selected - 1) < combo_ids.size()) {
        const auto id = combo_ids[selected - 1];
        if (i)
          settings.right_id = id;
        else
          settings.left_id = id;
      }
    }
    if (settings.left_id && settings.right_id && settings.left_id != settings.right_id)
      ++settings.route_request;
  }
  return ok && win::valid_settings(settings);
}
void build_controls();
bool apply(bool save = true) {
  auto settings = draft();
  if (!read_fields(settings)) {
    notice = L"Check the values: rate 15–60, EV −16 to +4, lens 0.05–1.55.";
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
  InvalidateRect(window, nullptr, FALSE);
  return true;
}
void target_combos(const win::Settings& s) {
  win::Status sample;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
  }
  combo_ids.clear();
  for (UINT i = 0; i < std::min(sample.candidate_count, 16u); ++i)
    combo_ids.push_back(sample.candidates[i].id);
  std::sort(combo_ids.begin(), combo_ids.end());
  for (unsigned side = 0; side < 2; ++side) {
    HWND combo = child(L"COMBOBOX", L"", 400 + side, 260 + static_cast<int>(side) * 375, 237, 315, 240, CBS_DROPDOWNLIST | WS_VSCROLL);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic assignment"));
    const auto selected = side ? (s.right_id ? s.right_id : sample.right_id) : (s.left_id ? s.left_id : sample.left_id);
    int selection = 0;
    for (size_t i = 0; i < combo_ids.size(); ++i) {
      wchar_t name[80];
      std::swprintf(name, 80, L"Texture #%llu", static_cast<unsigned long long>(combo_ids[i]));
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
      if (combo_ids[i] == selected)
        selection = static_cast<int>(i + 1);
    }
    SendMessageW(combo, CB_SETCURSEL, selection, 0);
  }
}
void build_controls() {
  refreshing = true;
  if (report_tooltip) {
    DestroyWindow(report_tooltip);
    report_tooltip = nullptr;
  }
  for (HWND h : controls)
    DestroyWindow(h);
  controls.clear();
  navigation.clear();
  const auto s = draft();
  const wchar_t* names[]{L"Overview", L"Camera views", L"Display", L"PFD routing", L"Diagnostics"};
  for (int i = 0; i < 5; ++i)
    navigation.push_back(button(names[i], 100 + i, 20, 156 + i * 49, 166, 40));
  const auto report_button = button(L"Report a bug", 512, 24, 638, 40, 40);
  report_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
  if (report_tooltip) {
    TOOLINFOW tip{};
    tip.cbSize = sizeof(tip);
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window;
    tip.uId = reinterpret_cast<UINT_PTR>(report_button);
    tip.lpszText = const_cast<wchar_t*>(L"Report a bug");
    SendMessageW(report_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
  }
  button(L"Menu", 602, 930, 37, 80, 34);
  button(L"Save changes", 500, 835, 686, 175, 42);
  button(L"Hide to tray", 501, 650, 686, 165, 42);
  if (page == 0) {
    HWND combo = child(L"COMBOBOX", L"", 210, 260, 312, 430, 220, CBS_DROPDOWNLIST | WS_VSCROLL);
    for (const auto* profile : profiles::Catalog)
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile->name));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    toggle(L"Service", 220, s.enabled, 830, 304, 150);
    toggle(L"TAXI buttons", 221, s.follow_taxi, 800, 412, 180);
    edit(s.camera_rate, 200, 855, 528, 100);
  } else if (page == 1) {
    for (int i = 0; i < 2; ++i) {
      const int x = 260 + i * 375;
      for (int j = 0; j < 6; ++j)
        edit(s.mounts[i][j], 300 + i * 10 + j, x + (j % 3) * 106, 245 + (j / 3) * 108, 88);
      button(L"Lower 0.25 m", 330 + i * 10, x, 439, 146);
      button(L"Raise 0.25 m", 331 + i * 10, x + 160, 439, 146);
      button(L"Aft 1 m", 332 + i * 10, x, 488, 146);
      button(L"Forward 1 m", 333 + i * 10, x + 160, 488, 146);
    }
    button(L"Reset camera mounts", 350, 260, 594, 240);
  } else if (page == 2) {
    edit(s.exposure, 201, 840, 210, 120);
    toggle(L"Auto exposure", 222, s.automatic_exposure, 785, 318, 190);
    edit(s.night_boost, 202, 840, 430, 120);
    edit(s.camera_rate, 200, 840, 547, 120);
  } else if (page == 3) {
    toggle(L"Auto detect", 223, s.auto_detect, 795, 126, 180);
    target_combos(s);
    button(L"Refresh textures", 402, 260, 294, 190);
    button(L"Swap left / right", 403, 475, 294, 190);
    toggle(L"Left preview", 224, (s.manual_mask & 1) != 0, 260, 429, 200);
    toggle(L"Right preview", 225, (s.manual_mask & 2) != 0, 505, 429, 200);
    toggle(L"Calibrate left", 226, (s.calibration_mask & 1) != 0, 260, 540, 200);
    toggle(L"Calibrate right", 227, (s.calibration_mask & 2) != 0, 505, 540, 200);
  } else if (page == 4) {
    toggle(L"Scene test", 229, s.scene_test, 260, 449, 200);
    toggle(L"First camera only", 228, s.single_camera, 737, 449, 235);
    edit(s.calibration_budget, 203, 840, 537, 120);
    button(L"Open log folder", 510, 260, 579, 210);
    button(L"Stop camera tests", 511, 500, 579, 210);
  }
  refreshing = false;
  InvalidateRect(window, nullptr, TRUE);
}
HICON make_icon() {
  constexpr int n = 32;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = n;
  info.bmiHeader.biHeight = -n;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits{};
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap)
    return LoadIconW(nullptr, IDI_APPLICATION);
  auto* pixels = static_cast<DWORD*>(bits);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      DWORD c = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 240 ? 0xff42dbb8 : 0;
      if (x >= 7 && x < 25 && y >= 11 && y < 23)
        c = 0xff11151c;
      if (x >= 10 && x < 16 && y >= 8 && y < 12)
        c = 0xff11151c;
      if ((x - 16) * (x - 16) + (y - 17) * (y - 17) < 16)
        c = 0xffe8eef6;
      pixels[y * n + x] = c;
    }
  HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = bitmap;
  ii.hbmMask = mask;
  HICON result = CreateIconIndirect(&ii);
  DeleteObject(bitmap);
  DeleteObject(mask);
  return result;
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
void update_balloon() {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = 1;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
  wcscpy_s(data.szInfoTitle, L"Taxi Cam update downloaded");
  wcscpy_s(data.szInfo, L"Close Microsoft Flight Simulator, then use Check for updates in the tray menu to install.");
  Shell_NotifyIconW(NIM_MODIFY, &data);
}
void show() {
  ShowWindow(window, SW_SHOW);
  ShowWindow(window, SW_RESTORE);
  SetForegroundWindow(window);
}
void draw_page(HDC dc) {
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, background_brush);
  auto left = rectangle(0, 0, 210, 780);
  HBRUSH b = CreateSolidBrush(Sidebar);
  FillRect(dc, &left, b);
  DeleteObject(b);
  DrawIconEx(dc, scale(24), scale(35), icon, scale(32), scale(32), 0, nullptr, DI_NORMAL);
  text(dc, L"TAXI CAM", 68, 33, 134, 22, heading);
  text(dc, L"Native camera service", 24, 84, 176, 22, small, Muted);
  text(dc, L"WINDOWS COMPANION", 24, 120, 182, 22, small, Muted);
  text(dc, L"v" TAXI_CAM_VERSION_WIDE, 24, 692, 155, 22, small, Muted);
  const wchar_t* titles[]{L"Taxi camera", L"Camera views", L"Display", L"PFD routing", L"Diagnostics"};
  const wchar_t* subtitles[]{L"Your taxi cameras, controlled from the flight deck.",
                             L"Fine-tune each camera independently. Changes stay with this aircraft.",
                             L"Balance visibility, colour and camera update rate.", L"Connect each TAXI button to the correct display.",
                             L"Live status and the controls used during camera testing."};
  text(dc, titles[page], 244, 30, 740, 48, title_font);
  text(dc, subtitles[page], 247, 84, 758, 30, normal, Muted);
  win::Status sample;
  std::wstring live;
  {
    const std::lock_guard lock(app_mutex);
    sample = status;
    live = connection;
  }
  if (page == 0) {
    panel(dc, 244, 137, 766, 125);
    text(dc, sample.graphics_ready ? L"Native bridge connected" : L"Waiting for the simulator", 266, 153, 705, 30, heading,
         sample.graphics_ready ? Accent : Text);
    const auto line = sample.heartbeat ? widen(sample.message) : live;
    text(dc, line.c_str(), 266, 195, 715, 47, normal, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 281, 766, 93);
    text(dc, L"Aircraft profile", 260, 284, 350, 26, small, Muted);
    panel(dc, 244, 395, 766, 96);
    text(dc, L"Flight-deck control", 264, 406, 450, 30, heading);
    text(dc, L"Left and right EFIS TAXI buttons activate their own PFD.", 264, 446, 530, 24, small, Muted);
    panel(dc, 244, 511, 766, 102);
    text(dc, L"Camera frame rate", 264, 525, 460, 30, heading);
    text(dc, L"15–60 fps per camera. Lower rates leave more time for the sim.", 264, 564, 540, 24, small, Muted);
    text(dc, L"Cameras and TAXI buttons turn off above 60 knots.", 250, 630, 730, 24, small, Muted);
  } else if (page == 1) {
    constexpr const wchar_t* labels[]{L"Right (m)", L"Up (m)", L"Forward (m)", L"Pitch (deg)", L"Yaw (deg)", L"Lens (rad)"};
    for (int i = 0; i < 2; ++i) {
      const int x = 244 + i * 375;
      panel(dc, x, 138, 354, 424);
      text(dc, i ? L"Tail camera" : L"Nose-wheel camera", x + 16, 154, 324, 30, heading);
      text(dc, i ? L"LOWER VIEW · 768 × 504" : L"UPPER VIEW · 768 × 255", x + 16, 190, 324, 22, small, Muted);
      for (int j = 0; j < 6; ++j)
        text(dc, labels[j], x + 16 + (j % 3) * 106, 215 + (j / 3) * 108, 98, 24, small, Muted);
      text(dc, L"Position relative to the aircraft datum", x + 16, 397, 324, 22, small, Muted);
    }
    text(dc, L"Positive pitch looks up. Positive yaw looks right.", 530, 594, 462, 45, small, Muted, DT_LEFT | DT_WORDBREAK);
  } else if (page == 2) {
    const int ys[]{144, 267, 390, 510};
    const wchar_t* names[]{L"Daytime exposure", L"Automatic night exposure", L"Maximum night boost", L"Camera frame rate"};
    const wchar_t* descriptions[]{
        L"Exposure compensation in EV. Your calibrated baseline is −8.8.", L"Gradually brighten the camera display as ambient light drops.",
        L"Additional exposure at night, from 0 to +8 EV.", L"Activation limit for each camera, from 15 to 60 fps."};
    for (int i = 0; i < 4; ++i) {
      panel(dc, 244, ys[i], 766, 105);
      text(dc, names[i], 264, ys[i] + 12, 515, 29, heading);
      text(dc, descriptions[i], 264, ys[i] + 49, 525, 41, small, Muted, DT_LEFT | DT_WORDBREAK);
    }
    wchar_t value[96];
    std::swprintf(value, 96, L"Currently applied exposure: %.2f EV", sample.exposure);
    text(dc, sample.heartbeat ? value : L"Applied exposure appears when the camera bridge connects.", 251, 630, 680, 24, small, Muted);
  } else if (page == 3) {
    panel(dc, 244, 119, 766, 226);
    text(dc, L"PFD assignment", 262, 127, 420, 30, heading);
    text(dc, L"Target identities apply to this simulator session.", 262, 166, 715, 25, small, Muted);
    text(dc, L"LEFT PFD", 260, 207, 315, 25, small, Muted);
    text(dc, L"RIGHT PFD", 635, 207, 315, 25, small, Muted);
    panel(dc, 244, 368, 766, 112);
    text(dc, L"Manual camera preview", 260, 381, 705, 29, heading);
    text(dc, L"Using a preview turns off automatic TAXI-button control.", 260, 410, 705, 22, small, Muted);
    panel(dc, 244, 500, 766, 112);
    text(dc, L"Target calibration", 260, 507, 705, 29, heading);
    text(dc, L"Animated bars identify each screen before enabling a live feed.", 260, 581, 705, 23, small, Muted);
    text(dc, L"Enable flight-deck control on Overview to return to normal use.", 250, 630, 745, 24, small, Muted);
  } else if (page == 4) {
    panel(dc, 244, 138, 766, 277);
    wchar_t data[1024];
    std::swprintf(data, 1024,
                  L"Bridge                 %s\nCamera pair         %s\nLeft / right PFD    %llu / %llu\nCaptured frames  "
                  L"%llu\nCompositions       %llu\nPFD writes            %llu\nHook failures        %llu",
                  sample.graphics_ready ? L"Connected" : L"Waiting", sample.scene_ready ? L"Ready" : L"Waiting",
                  static_cast<unsigned long long>(sample.left_id), static_cast<unsigned long long>(sample.right_id),
                  static_cast<unsigned long long>(sample.captures), static_cast<unsigned long long>(sample.composed),
                  static_cast<unsigned long long>(sample.stamps), static_cast<unsigned long long>(sample.hook_failures));
    text(dc, data, 266, 153, 355, 242, normal, Text, DT_LEFT | DT_WORDBREAK);
    std::swprintf(data, 1024,
                  L"Probe CPU: %.2f ms (max %.2f)\nManager %.3f   Pool %.3f\nLifecycle %.3f   Entries %.3f\nView 1 %.3f   View 2 "
                  L"%.3f\nHandoff %.3f   Pose %.3f\nActivation %.3f   Publish %.3f\n\nExcludes engine rendering and GPU time.",
                  sample.probe_cpu_ms, sample.probe_max_ms, sample.stage_ms[0], sample.stage_ms[1], sample.stage_ms[2], sample.stage_ms[3],
                  sample.stage_ms[4], sample.stage_ms[5], sample.stage_ms[6], sample.stage_ms[7], sample.stage_ms[8], sample.stage_ms[9]);
    text(dc, data, 637, 156, 350, 236, small, Muted, DT_LEFT | DT_WORDBREAK);
    panel(dc, 244, 436, 766, 86);
    text(dc, L"The scene test runs independently of PFD assignment. PFD delivery needs both views.", 260, 488, 700, 24, small, Muted);
    const auto line = sample.heartbeat ? widen(sample.message) : live;
    text(dc, L"Calibration batches per 50 ms (64–16384)", 260, 534, 550, 27, small, Muted);
    text(dc, line.c_str(), 260, 625, 730, 38, small, Muted, DT_LEFT | DT_WORDBREAK);
  }
  text(dc, notice.c_str(), 248, 687, 382, 43, small, dirty ? Accent : Muted, DT_LEFT | DT_WORDBREAK);
}

DWORD WINAPI connection_worker(void*) {
  win::Mailbox mailbox;
  DWORD attached{};
  bool attempted = false;
  win::LaunchRetry startup_retry;
  HANDLE process{};
  while (running.load()) {
    if (preview_ui) {
      Sleep(100);
      continue;
    }
    const DWORD pid = win::find_simulator(expected_simulator);
    if (attached && (pid != attached || (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0))) {
      mailbox.close();
      if (process)
        CloseHandle(process);
      process = nullptr;
      attached = 0;
      attempted = false;
      simulator_pid = 0;
      {
        const std::lock_guard lock(app_mutex);
        status = {};
        connection = L"Simulator closed. Waiting for the next session.";
      }
      PostMessageW(window, StatusMessage, 0, 0);
      if (background_start) {
        PostMessageW(window, WM_CLOSE, 1, 0);
        break;
      }
    }
    if (pid && pid != attached) {
      attached = pid;
      simulator_pid = pid;
      attempted = false;
      startup_retry = {};
      process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    }
    if (attached && !attempted && startup_retry.ready(GetTickCount64())) {
      attempted = true;
      if (!mailbox.open(attached, true)) {
        const std::lock_guard lock(app_mutex);
        connection = L"Could not open the camera control channel.";
      } else {
        const auto settings = draft();
        if (mailbox.lock(100)) {
          mailbox.data()->owner_pid = GetCurrentProcessId();
          mailbox.data()->owner_heartbeat = GetTickCount64();
          mailbox.data()->settings = settings;
          mailbox.unlock();
        }
        const auto loaded = win::load_bridge(attached, expected_simulator, installation + L"\\taxi-camera-bridge.dll", &running);
        const bool retrying = startup_retry.schedule(loaded, GetTickCount64());
        attempted = !retrying;
        {
          const std::lock_guard lock(app_mutex);
          connection = loaded.message;
          if (retrying)
            connection += L" Retrying startup preflight.";
          else if (loaded.retry_before_load && !loaded.ok)
            connection += L" Startup retries exhausted; restart Taxi Cam to retry.";
        }
        PostMessageW(window, StatusMessage, 0, 0);
      }
    }
    const auto settings = draft();
    if (mailbox.data() && running.load() && mailbox.lock()) {
      mailbox.data()->settings = settings;
      mailbox.data()->owner_pid = GetCurrentProcessId();
      mailbox.data()->owner_heartbeat = GetTickCount64();
      auto sample = mailbox.data()->status;
      mailbox.unlock();
      {
        const std::lock_guard lock(app_mutex);
        status = sample;
        received_bridge_status = received_bridge_status || sample.heartbeat != 0;
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
        const auto prompt = L"Taxi Cam " + result.tag +
                            L" has been downloaded and verified.\n\nClose Taxi Cam and start the installer now?";
        if (MessageBoxW(window, prompt.c_str(), L"Taxi Cam update ready", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES) {
          bool proceed = true;
          if (dirty) {
            const int choice = MessageBoxW(window, L"Save your unsaved settings before installing?\n\nYes: save and continue.\nNo: discard changes.\nCancel: keep the app open.",
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
    case 220:
      return s.enabled;
    case 221:
      return s.follow_taxi;
    case 222:
      return s.automatic_exposure;
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
      build_controls();
      tray(true);
      SetTimer(hwnd, 1, 250, nullptr);
      return 0;
    }
    case WM_TIMER:
      if (show_event && WaitForSingleObject(show_event, 0) == WAIT_OBJECT_0)
        show();
      poll_updates();
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
      const bool selected = (id >= 100 && id < 105 && id - 100 == page) || is_on(id, draft());
      HBRUSH surround = CreateSolidBrush((id >= 100 && id < 105) || id == 512 ? Sidebar : Background);
      FillRect(item->hDC, &item->rcItem, surround);
      DeleteObject(surround);
      const bool primary = id == 500;
      const bool down = (item->itemState & ODS_SELECTED) != 0;
      const COLORREF fill = primary ? Accent : selected ? RGB(30, 64, 63) : down ? Border : Card;
      HBRUSH brush = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, scale(1), selected || primary ? Accent : Border);
      const auto oldb = SelectObject(item->hDC, brush), oldp = SelectObject(item->hDC, pen);
      RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, scale(9), scale(9));
      SelectObject(item->hDC, oldb);
      SelectObject(item->hDC, oldp);
      DeleteObject(brush);
      DeleteObject(pen);
      RECT r = item->rcItem;
      if (id == 512) {
        draw_bug_icon(item->hDC, r, Accent);
        InflateRect(&r, -scale(4), -scale(4));
      } else {
        wchar_t label[160];
        GetWindowTextW(item->hwndItem, label, 160);
        SelectObject(item->hDC, normal);
        SetTextColor(item->hDC, primary ? Background : selected ? Accent : Text);
        SetBkMode(item->hDC, TRANSPARENT);
        InflateRect(&r, -scale(10), 0);
        DrawTextW(item->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      if (item->itemState & ODS_FOCUS) {
        if (id != 512)
          InflateRect(&r, -2, -4);
        DrawFocusRect(item->hDC, &r);
      }
      return TRUE;
    }
    case StatusMessage:
      if (IsWindowVisible(hwnd))
        InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_CONTEXTMENU:
      PostMessageW(hwnd, TrayMessage, 0, WM_CONTEXTMENU);
      return 0;
    case TrayMessage:
      if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 600, L"Settings");
        AppendMenuW(menu, MF_STRING | (preview_ui || updater.busy() || update_prompt ? MF_GRAYED : 0), 603,
                    updater.busy() ? L"Checking for updates..." : L"Check for updates");
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
        if (selected == 603)
          check_updates(true);
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
      if (HIWORD(w) == EN_CHANGE || HIWORD(w) == CBN_SELCHANGE) {
        dirty_notice();
        return 0;
      }
      if (id >= 100 && id < 105) {
        auto s = draft();
        if (!read_fields(s)) {
          notice = L"Finish the current values before changing pages.";
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
      if (id == 500) {
        apply();
        return 0;
      }
      if (id == 501) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      if (id >= 220 && id <= 229) {
        if (!apply(false))
          return 0;
        auto s = draft();
        if (id == 220)
          s.enabled = !s.enabled;
        if (id == 221) {
          s.follow_taxi = !s.follow_taxi;
          if (s.follow_taxi) {
            s.manual_mask = 0;
            s.calibration_mask = 0;
          }
        }
        if (id == 222)
          s.automatic_exposure = !s.automatic_exposure;
        if (id == 223)
          s.auto_detect = !s.auto_detect;
        if (id == 224 || id == 225) {
          s.follow_taxi = 0;
          s.calibration_mask = 0;
          s.manual_mask ^= id == 224 ? 1u : 2u;
        }
        if (id == 226 || id == 227) {
          s.follow_taxi = 0;
          s.manual_mask = 0;
          s.calibration_mask ^= id == 226 ? 1u : 2u;
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
      if ((id >= 330 && id <= 333) || (id >= 340 && id <= 343)) {
        if (!apply(false))
          return 0;
        auto s = draft();
        const unsigned side = id >= 340 ? 1u : 0u;
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
      if (id == 350) {
        auto s = draft();
        s.mounts = profiles::active().mounts;
        publish(s);
        dirty_notice();
        build_controls();
        return 0;
      }
      if (id == 402) {
        apply(false);
        build_controls();
        return 0;
      }
      if (id == 403) {
        if (!apply(false))
          return 0;
        auto s = draft();
        std::swap(s.left_id, s.right_id);
        ++s.route_request;
        publish(s);
        dirty_notice();
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
      if (report_tooltip) {
        DestroyWindow(report_tooltip);
        report_tooltip = nullptr;
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
  instance = app;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int argc{};
  auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
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
  if (!win::load_settings(current, installation))
    notice = L"Saved settings were invalid; profile defaults loaded.";
  INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
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
  if (!background_start)
    ShowWindow(main, SW_SHOW);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(main, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  running = false;
  updater.stop();
  WaitForSingleObject(worker, 1500);
  CloseHandle(worker);
  CloseHandle(show_event);
  CloseHandle(singleton);
  return 0;
}
