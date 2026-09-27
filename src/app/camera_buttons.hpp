#pragma once
#include <windows.h>
#include <hidsdi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "camera_hotkeys.hpp"

namespace taxi_camera::standalone {
// Game-controller buttons for the camera actions, saved beside the keyboard
// shortcuts. A binding names the controller by USB vendor and product ID and
// the button by its HID Button-page usage, the number Windows shows for it.
// Identical controllers therefore share their bindings.
struct CameraButton {
  std::uint16_t vendor{}, product{}, button{};  // button 0 disables the action.
  bool operator==(const CameraButton&) const = default;
};
// Same action order as CameraHotkeys. There are no default buttons.
using CameraButtons = std::array<CameraButton, std::tuple_size_v<CameraHotkeys>>;
inline bool valid_camera_button(CameraButton binding) noexcept {
  return binding.button || (!binding.vendor && !binding.product);
}
inline bool valid_camera_buttons(const CameraButtons& bindings, std::wstring* error = nullptr) {
  for (size_t i = 0; i < bindings.size(); ++i) {
    if (!valid_camera_button(bindings[i])) {
      if (error)
        *error = L"A controller button binding is incomplete. Clear it and set the button again.";
      return false;
    }
    for (size_t j = 0; j < i; ++j)
      if (bindings[i].button && bindings[i] == bindings[j]) {
        if (error)
          *error = L"Each camera action needs a different controller button. Clear any button you do not need.";
        return false;
      }
  }
  return true;
}
inline bool any_camera_button(const CameraButtons& bindings) noexcept {
  return std::any_of(bindings.begin(), bindings.end(), [](CameraButton binding) { return binding.button != 0; });
}
// Saved as vendor:product:button, with hexadecimal IDs; empty when disabled.
inline std::wstring format_camera_button(CameraButton binding) {
  if (!binding.button)
    return {};
  wchar_t value[32];
  std::swprintf(value, 32, L"%04x:%04x:%u", binding.vendor, binding.product, binding.button);
  return value;
}
inline bool parse_camera_button(const wchar_t* text, CameraButton& binding) {
  binding = {};
  if (!*text)
    return true;
  unsigned fields[3]{};
  for (int i = 0; i < 3; ++i) {
    const unsigned base = i < 2 ? 16 : 10;
    const auto* start = text;
    for (; *text && *text != L':'; ++text) {
      const unsigned digit = *text >= L'0' && *text <= L'9' ? unsigned(*text - L'0')
                             : base == 16 && std::towlower(*text) >= L'a' && std::towlower(*text) <= L'f'
                                 ? unsigned(std::towlower(*text) - L'a' + 10)
                                 : base;
      if (digit >= base || (fields[i] = fields[i] * base + digit) > 0xffff)
        return false;
    }
    if (text == start || *text != (i < 2 ? L':' : L'\0'))
      return false;
    text += i < 2;
  }
  if (!fields[2])
    return false;
  binding = {static_cast<std::uint16_t>(fields[0]), static_cast<std::uint16_t>(fields[1]), static_cast<std::uint16_t>(fields[2])};
  return true;
}
// A missing file means no buttons. Absent actions stay disabled, so actions
// added later never take a button that is already used in the simulator.
inline bool load_camera_buttons(CameraButtons& bindings, const std::wstring& directory) {
  bindings = {};
  if (directory.empty())
    return false;
  const auto path = directory + L"\\buttons.ini";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
  }
  CameraButtons loaded{};
  for (size_t i = 0; i < loaded.size(); ++i) {
    wchar_t value[64]{};
    const auto length = GetPrivateProfileStringW(L"buttons", CameraHotkeyKeys[i], L"", value, 64, path.c_str());
    if (length >= 63 || !parse_camera_button(value, loaded[i]))
      return false;
  }
  if (!valid_camera_buttons(loaded))
    return false;
  bindings = loaded;
  return true;
}
inline bool save_camera_buttons(const CameraButtons& bindings, const std::wstring& directory) {
  if (directory.empty() || !valid_camera_buttons(bindings))
    return false;
  std::wstring value = L"[buttons]\r\n";
  for (size_t i = 0; i < bindings.size(); ++i)
    value += std::wstring(CameraHotkeyKeys[i]) + L"=" + format_camera_button(bindings[i]) + L"\r\n";
  return write_preferences_file(directory + L"\\buttons.ini", value.c_str(), static_cast<int>(value.size()));
}
inline int camera_button_action(const CameraButtons& bindings, CameraButton press) noexcept {
  for (size_t i = 0; i < bindings.size(); ++i)
    if (press.button && bindings[i] == press)
      return static_cast<int>(i);
  return -1;
}
inline std::wstring describe_camera_button(CameraButton binding, const std::wstring& device) {
  if (!binding.button)
    return L"No button";
  wchar_t value[192];
  if (device.empty())
    std::swprintf(value, 192, L"Button %u · controller %04X:%04X", binding.button, binding.vendor, binding.product);
  else
    std::swprintf(value, 192, L"Button %u · %ls", binding.button, device.c_str());
  return value;
}

// Pressed buttons per HID input report. A controller that splits its buttons
// across report IDs keeps a separate set for each, so a report that does not
// carry a button is never read as its release.
class ButtonReportState {
 public:
  // Records the report's pressed buttons, appending those that went down and
  // those that came up. A baseline report only records what is already held:
  // switch positions such as the WinWing throttle's engine masters report as
  // permanently held buttons, and are not presses.
  void update(std::uint8_t report_id,
              std::vector<std::uint16_t> pressed,
              std::vector<std::uint16_t>& down,
              std::vector<std::uint16_t>& up,
              bool baseline = false) {
    std::sort(pressed.begin(), pressed.end());
    pressed.erase(std::unique(pressed.begin(), pressed.end()), pressed.end());
    auto held = std::find_if(held_.begin(), held_.end(), [&](const auto& entry) { return entry.first == report_id; });
    if (held == held_.end()) {
      held = held_.insert(held_.end(), {report_id, {}});
      if (baseline) {
        held->second = std::move(pressed);
        return;
      }
    }
    std::set_difference(pressed.begin(), pressed.end(), held->second.begin(), held->second.end(), std::back_inserter(down));
    std::set_difference(held->second.begin(), held->second.end(), pressed.begin(), pressed.end(), std::back_inserter(up));
    held->second = std::move(pressed);
  }

 private:
  std::vector<std::pair<std::uint8_t, std::vector<std::uint16_t>>> held_;
};
// Reports that first arrive this soon after listening starts, or after a
// controller arrives, only record held buttons. Controllers with axes stream
// reports continuously, so they settle well inside it; a controller that
// reports only on change has sent nothing and its first press still counts.
inline constexpr std::uint64_t CameraButtonBaselineMs = 500;
struct CameraButtonEvent {
  CameraButton button;
  bool pressed{};  // False when the button was released.
};
// Switch bounce would otherwise turn a display straight back off.
inline constexpr std::uint64_t CameraButtonRepeatMs = 250;
class CameraButtonRepeatGuard {
 public:
  bool accept(size_t action, std::uint64_t now) noexcept {
    if (action >= last_.size() || (seen_[action] && now >= last_[action] && now - last_[action] < CameraButtonRepeatMs))
      return false;
    seen_[action] = true;
    last_[action] = now;
    return true;
  }

 private:
  std::array<std::uint64_t, std::tuple_size_v<CameraButtons>> last_{};
  std::array<bool, std::tuple_size_v<CameraButtons>> seen_{};
};

// Raw Input from HID game controllers (joystick, gamepad and multi-axis
// controller collections). RIDEV_INPUTSINK keeps presses arriving while MSFS
// has focus and Taxi Cam is hidden. The simulator still receives every button,
// so a button bound here should be left unassigned in the simulator.
class CameraButtonInput {
 public:
  using Register = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
  explicit CameraButtonInput(Register registration = RegisterRawInputDevices) : register_(registration) {}
  ~CameraButtonInput() { disable(); }
  CameraButtonInput(const CameraButtonInput&) = delete;
  CameraButtonInput& operator=(const CameraButtonInput&) = delete;
  bool enable(HWND window) {
    if (window && window == window_)
      return true;
    disable();
    auto devices = collections(RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window);
    if (!window || !register_(devices.data(), static_cast<UINT>(devices.size()), sizeof(RAWINPUTDEVICE))) {
      error_ = window ? GetLastError() : ERROR_INVALID_WINDOW_HANDLE;
      if (!error_)
        error_ = ERROR_GEN_FAILURE;
      return false;
    }
    window_ = window;
    error_ = 0;
    baseline_until_ = GetTickCount64() + CameraButtonBaselineMs;
    return true;
  }
  void disable() noexcept {
    if (window_) {
      auto devices = collections(RIDEV_REMOVE, nullptr);
      register_(devices.data(), static_cast<UINT>(devices.size()), sizeof(RAWINPUTDEVICE));
    }
    window_ = nullptr;
    devices_.clear();
  }
  bool enabled() const noexcept { return window_ != nullptr; }
  DWORD error() const noexcept { return error_; }
  // Buttons that went down or came up in one WM_INPUT message.
  std::vector<CameraButtonEvent> read(HRAWINPUT handle) {
    std::vector<CameraButtonEvent> presses;
    UINT size{};
    if (!window_ || GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size < sizeof(RAWINPUTHEADER))
      return presses;
    buffer_.assign((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    if (GetRawInputData(handle, RID_INPUT, buffer_.data(), &size, sizeof(RAWINPUTHEADER)) != size)
      return presses;
    const auto* input = reinterpret_cast<const RAWINPUT*>(buffer_.data());
    if (input->header.dwType != RIM_TYPEHID)
      return presses;
    auto& device = find(input->header.hDevice);
    const auto& hid = input->data.hid;
    const auto offset = static_cast<size_t>(reinterpret_cast<const BYTE*>(hid.bRawData) - reinterpret_cast<const BYTE*>(input));
    if (device.preparsed.empty() || !hid.dwSizeHid || size < offset ||
        std::uint64_t(hid.dwSizeHid) * hid.dwCount > std::uint64_t(size - offset))
      return presses;
    const auto preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(device.preparsed.data());
    const bool baseline = GetTickCount64() < device.baseline_until;
    for (DWORD r = 0; r < hid.dwCount; ++r) {
      auto* report = reinterpret_cast<CHAR*>(const_cast<BYTE*>(hid.bRawData) + std::size_t(r) * hid.dwSizeHid);
      auto count = static_cast<ULONG>(device.usages.size());
      // Reports without Button-page usages, or for another report ID, change nothing.
      if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, device.usages.data(), &count, preparsed, report, hid.dwSizeHid) !=
          HIDP_STATUS_SUCCESS)
        continue;
      down_.clear();
      up_.clear();
      device.state.update(device.report_ids ? static_cast<std::uint8_t>(report[0]) : 0,
                          std::vector<std::uint16_t>(device.usages.begin(), device.usages.begin() + count), down_, up_, baseline);
      for (const auto button : down_)
        presses.push_back({{device.vendor, device.product, button}, true});
      for (const auto button : up_)
        presses.push_back({{device.vendor, device.product, button}, false});
    }
    return presses;
  }
  // A controller plugged in while listening settles like one present at start.
  // Windows also announces every present controller after registration.
  void arrived(HANDLE handle) {
    if (!window_)
      return;
    devices_.erase(handle);
    find(handle).baseline_until = GetTickCount64() + CameraButtonBaselineMs;
  }
  // Raw Input handles can be reused after a controller is unplugged.
  void remove(HANDLE device) { devices_.erase(device); }
  // Product name of a connected controller, "Game controller" when it has
  // none, or empty when no matching controller is connected.
  static std::wstring device_name(std::uint16_t vendor, std::uint16_t product) {
    UINT count{};
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || !count)
      return {};
    std::vector<RAWINPUTDEVICELIST> list(count);
    const auto listed = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (listed == static_cast<UINT>(-1))
      return {};
    for (UINT i = 0; i < listed; ++i) {
      RID_DEVICE_INFO info{};
      if (list[i].dwType != RIM_TYPEHID || !controller(list[i].hDevice, info) || info.hid.dwVendorId != vendor ||
          info.hid.dwProductId != product)
        continue;
      UINT length{};
      if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &length) != 0 || !length)
        return L"Game controller";
      std::wstring path(length, L'\0');
      if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path.data(), &length) == static_cast<UINT>(-1))
        return L"Game controller";
      const HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      wchar_t name[128]{};
      const bool named = file != INVALID_HANDLE_VALUE && HidD_GetProductString(file, name, sizeof(name) - sizeof(wchar_t)) && name[0];
      if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
      return named ? std::wstring(name) : L"Game controller";
    }
    return {};
  }

 private:
  struct Device {
    std::uint16_t vendor{}, product{};
    std::vector<std::uint64_t> preparsed;  // Empty for devices without buttons.
    std::vector<USAGE> usages;
    ButtonReportState state;
    bool report_ids{};
    std::uint64_t baseline_until{};
  };
  static std::array<RAWINPUTDEVICE, 3> collections(DWORD flags, HWND window) {
    constexpr USHORT MultiAxisController = 0x08;
    return {{{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK, flags, window},
             {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD, flags, window},
             {HID_USAGE_PAGE_GENERIC, MultiAxisController, flags, window}}};
  }
  static bool controller(HANDLE handle, RID_DEVICE_INFO& info) {
    info = {};
    info.cbSize = sizeof(info);
    UINT size = sizeof(info);
    return GetRawInputDeviceInfoW(handle, RIDI_DEVICEINFO, &info, &size) == sizeof(info) && info.dwType == RIM_TYPEHID &&
           info.hid.usUsagePage == HID_USAGE_PAGE_GENERIC &&
           (info.hid.usUsage == HID_USAGE_GENERIC_JOYSTICK || info.hid.usUsage == HID_USAGE_GENERIC_GAMEPAD || info.hid.usUsage == 0x08);
  }
  // Queried once per device handle; controllers report continuously while axes move.
  Device& find(HANDLE handle) {
    const auto existing = devices_.find(handle);
    if (existing != devices_.end())
      return existing->second;
    auto& device = devices_[handle];
    device.baseline_until = baseline_until_;
    RID_DEVICE_INFO info{};
    UINT size{};
    if (!controller(handle, info) || GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr, &size) != 0 || !size)
      return device;
    std::vector<std::uint64_t> preparsed((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    if (GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, preparsed.data(), &size) == static_cast<UINT>(-1))
      return device;
    const auto data = reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data());
    HIDP_CAPS caps{};
    if (HidP_GetCaps(data, &caps) != HIDP_STATUS_SUCCESS || !caps.NumberInputButtonCaps)
      return device;
    std::vector<HIDP_BUTTON_CAPS> buttons(caps.NumberInputButtonCaps);
    USHORT count = caps.NumberInputButtonCaps;
    if (HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, data) != HIDP_STATUS_SUCCESS)
      return device;
    buttons.resize(count);
    const auto usages = HidP_MaxUsageListLength(HidP_Input, HID_USAGE_PAGE_BUTTON, data);
    if (!usages || std::none_of(buttons.begin(), buttons.end(), [](const auto& cap) { return cap.UsagePage == HID_USAGE_PAGE_BUTTON; }))
      return device;
    device.vendor = static_cast<std::uint16_t>(info.hid.dwVendorId);
    device.product = static_cast<std::uint16_t>(info.hid.dwProductId);
    device.report_ids = std::any_of(buttons.begin(), buttons.end(), [](const auto& cap) { return cap.ReportID != 0; });
    device.usages.resize(usages);
    device.preparsed = std::move(preparsed);
    return device;
  }
  Register register_;
  HWND window_{};
  DWORD error_{};
  std::uint64_t baseline_until_{};
  std::unordered_map<HANDLE, Device> devices_;
  std::vector<std::uint64_t> buffer_;
  std::vector<std::uint16_t> down_, up_;
};
}  // namespace taxi_camera::standalone
