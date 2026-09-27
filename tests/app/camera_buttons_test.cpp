#include "../../src/app/camera_buttons.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}
constexpr CameraButton Throttle{0x044f, 0x0405, 5}, ButtonBox{0x1234, 0xbead, 12};

void format_checks() {
  for (const auto binding : {Throttle, ButtonBox, CameraButton{}, CameraButton{0, 0, 1}, CameraButton{0xffff, 0xffff, 0xffff}}) {
    CameraButton parsed{9, 9, 9};
    require(parse_camera_button(format_camera_button(binding).c_str(), parsed) && parsed == binding,
            "Saved controller buttons roundtrip, including disabled actions");
  }
  require(format_camera_button(Throttle) == L"044f:0405:5", "Saved form is vendor:product:button");
  for (const auto invalid : {L"044f:0405:0", L"044f:0405", L"044f:0405:5:1", L"10000:0405:5", L"044f:0405:65536", L"-1:0405:5",
                             L" 044f:0405:5", L"044f:0405:x", L"0x44f:0405:5", L"044f::5", L"044f:0405:+5"}) {
    CameraButton parsed{9, 9, 9};
    require(!parse_camera_button(invalid, parsed) && parsed == CameraButton{}, "Malformed saved buttons are rejected and cleared");
  }
  require(describe_camera_button({}, L"") == L"No button", "Disabled buttons are described as such");
  require(describe_camera_button(Throttle, L"TCA Quadrant") == L"Button 5 · TCA Quadrant", "Connected controllers show their name");
  require(describe_camera_button(Throttle, L"") == L"Button 5 · controller 044F:0405", "Disconnected controllers show their IDs");
}

void validation_checks() {
  require(valid_camera_buttons({}) && !any_camera_button({}), "No buttons is the valid default");
  CameraButtons bindings{Throttle, {}, ButtonBox, {}};
  require(valid_camera_buttons(bindings) && any_camera_button(bindings), "Distinct buttons and disabled actions are valid");
  std::wstring error;
  bindings[3] = Throttle;
  require(!valid_camera_buttons(bindings, &error) && error.find(L"different controller button") != std::wstring::npos,
          "One button cannot drive two actions");
  bindings[3] = {Throttle.vendor, Throttle.product, 6};
  require(valid_camera_buttons(bindings), "Other buttons on the same controller are distinct");
  bindings[3] = {Throttle.vendor, 0x0406, Throttle.button};
  require(valid_camera_buttons(bindings), "The same button number on another controller is distinct");
  bindings[1] = {Throttle.vendor, Throttle.product, 0};
  require(!valid_camera_buttons(bindings, &error) && error.find(L"incomplete") != std::wstring::npos,
          "A controller without a button is rejected");
  const CameraButtons actions{Throttle, ButtonBox, {0, 0, 1}, {}};
  require(camera_button_action(actions, Throttle) == 0 && camera_button_action(actions, ButtonBox) == 1 &&
              camera_button_action(actions, {0, 0, 1}) == 2,
          "Each bound button selects its action");
  require(camera_button_action(actions, {Throttle.vendor, Throttle.product, 6}) == -1 && camera_button_action(actions, {}) == -1,
          "Unbound buttons and disabled actions select nothing");
}

void persistence_checks(const std::wstring& directory) {
  CameraButtons loaded{Throttle};
  require(load_camera_buttons(loaded, directory) && loaded == CameraButtons{}, "First run has no controller buttons");
  require(!load_camera_buttons(loaded, L"") && loaded == CameraButtons{}, "No settings folder loads no buttons");
  const CameraButtons saved{Throttle, {}, ButtonBox, {0x0001, 0x0002, 3}};
  require(save_camera_buttons(saved, directory) && load_camera_buttons(loaded, directory) && loaded == saved,
          "Controller buttons and disabled actions persist across restart");
  const auto path = directory + L"\\buttons.ini";
  require(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "Saving leaves no temporary file");
  auto duplicate = saved;
  duplicate[1] = duplicate[0];
  require(!save_camera_buttons(duplicate, directory) && load_camera_buttons(loaded, directory) && loaded == saved,
          "Duplicate buttons are refused without damaging the saved file");
  require(WritePrivateProfileStringW(L"buttons", L"sd", nullptr, path.c_str()) && load_camera_buttons(loaded, directory) &&
              loaded[0] == saved[0] && loaded[2] == saved[2] && !loaded[3].button,
          "An absent action stays disabled");
  require(WritePrivateProfileStringW(L"buttons", L"right", L"044f:0405", path.c_str()), "Write malformed fixture");
  require(!load_camera_buttons(loaded, directory) && loaded == CameraButtons{}, "Malformed saved buttons disable every button");
  require(WritePrivateProfileStringW(L"buttons", L"right", L"044f:0405:5", path.c_str()), "Write duplicate fixture");
  require(!load_camera_buttons(loaded, directory) && loaded == CameraButtons{}, "A duplicated saved button disables every button");
  require(save_camera_buttons(saved, directory) && load_camera_buttons(loaded, directory) && loaded == saved,
          "Valid buttons recover after malformed input");
  CameraHotkeys chords;
  require(load_camera_hotkeys(chords, directory) && chords == DefaultCameraHotkeys,
          "Controller buttons do not create or change keyboard shortcuts");
  DeleteFileW(path.c_str());
}

void edge_checks() {
  using Buttons = std::vector<std::uint16_t>;
  ButtonReportState state;
  Buttons down, up;
  state.update(0, {3, 1}, down, up);
  require(down == Buttons{1, 3} && up.empty(), "Outside the baseline, buttons down in a first report are presses");
  down.clear();
  state.update(0, {1, 3}, down, up);
  require(down.empty() && up.empty(), "Held buttons do not repeat while axes keep reporting");
  state.update(0, {3, 3, 4}, down, up);
  require(down == Buttons{4} && up == Buttons{1}, "Changes are reported once each, in both directions");
  down.clear();
  up.clear();
  state.update(0, {}, down, up);
  state.update(0, {1}, down, up);
  require(down == Buttons{1} && up == Buttons{3, 4}, "Release then press is a new press");
  down.clear();
  up.clear();
  // Buttons 1-32 in report 1 and 33-64 in report 2.
  ButtonReportState split;
  split.update(1, {2}, down, up);
  split.update(2, {40}, down, up);
  down.clear();
  split.update(2, {40}, down, up);
  split.update(1, {2}, down, up);
  require(down.empty() && up.empty(), "A report that does not carry a button is not its release");
  split.update(1, {}, down, up);
  split.update(2, {40, 41}, down, up);
  require(down == Buttons{41} && up == Buttons{2}, "Each report ID keeps its own held buttons");
  // Recorded idle WinWing URSA MINOR throttle: switch positions stream as held.
  const Buttons throttle{2, 4, 8, 15, 21, 27, 30, 35, 38};
  ButtonReportState winwing;
  down.clear();
  up.clear();
  winwing.update(1, throttle, down, up, true);
  winwing.update(1, throttle, down, up, true);
  winwing.update(1, throttle, down, up);
  require(down.empty() && up.empty(), "Held switch positions at the start are not presses");
  auto engine_on = throttle;
  engine_on.erase(engine_on.begin());
  engine_on.push_back(1);
  winwing.update(1, engine_on, down, up);
  require(down == Buttons{1} && up == Buttons{2}, "Moving a held switch reports its new position as a press");
  down.clear();
  up.clear();
  ButtonReportState later;
  later.update(1, {}, down, up, true);
  later.update(1, {6}, down, up, true);
  require(down == Buttons{6}, "The baseline only records a report ID's first report");
}

void repeat_checks() {
  CameraButtonRepeatGuard guard;
  require(guard.accept(0, 1000) && guard.accept(1, 1000), "Actions accept their first press independently");
  require(!guard.accept(0, 1000 + CameraButtonRepeatMs - 1), "Switch bounce inside the repeat window is ignored");
  require(guard.accept(0, 1000 + CameraButtonRepeatMs), "A deliberate press after the window toggles again");
  require(guard.accept(2, 0), "A press at tick zero is accepted");
  require(guard.accept(1, 999), "A clock reset never blocks a press");
  require(!guard.accept(4, 5000), "Unknown actions are refused");
}

struct FakeRawInput {
  inline static std::vector<RAWINPUTDEVICE> calls;
  inline static bool fail{};
  static BOOL WINAPI registration(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    require(size == sizeof(RAWINPUTDEVICE) && count == 3, "Raw Input registers the three controller collections together");
    if (fail) {
      SetLastError(ERROR_ACCESS_DENIED);
      return FALSE;
    }
    calls.insert(calls.end(), devices, devices + count);
    return TRUE;
  }
};

void registration_checks() {
  const auto fixture = reinterpret_cast<HWND>(static_cast<INT_PTR>(1));
  FakeRawInput::calls.clear();
  {
    CameraButtonInput input(FakeRawInput::registration);
    require(!input.enabled() && input.read(nullptr).empty(), "Input starts off and reads nothing");
    require(input.enable(fixture) && input.enabled() && FakeRawInput::calls.size() == 3, "Enabling registers controller input");
    for (const auto& device : FakeRawInput::calls)
      require(device.usUsagePage == HID_USAGE_PAGE_GENERIC && device.hwndTarget == fixture &&
                  device.dwFlags == (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY),
              "Controller input is received in the background with device notifications");
    require(FakeRawInput::calls[0].usUsage == HID_USAGE_GENERIC_JOYSTICK && FakeRawInput::calls[1].usUsage == HID_USAGE_GENERIC_GAMEPAD &&
                FakeRawInput::calls[2].usUsage == 0x08,
            "Joysticks, gamepads and multi-axis controllers are registered");
    require(input.enable(fixture) && FakeRawInput::calls.size() == 3, "Enabling again for the same window is a no-op");
    input.disable();
    require(!input.enabled() && FakeRawInput::calls.size() == 6, "Disabling removes the registration");
    for (size_t i = 3; i < 6; ++i)
      require(FakeRawInput::calls[i].dwFlags == RIDEV_REMOVE && !FakeRawInput::calls[i].hwndTarget,
              "Removal uses RIDEV_REMOVE without a target window");
    input.disable();
    require(FakeRawInput::calls.size() == 6, "Disabling twice removes once");
    FakeRawInput::fail = true;
    require(!input.enable(fixture) && !input.enabled() && input.error() == ERROR_ACCESS_DENIED,
            "A refused registration reports the Windows error");
    FakeRawInput::fail = false;
    require(!input.enable(nullptr) && !input.enabled() && FakeRawInput::calls.size() == 6, "No window registers nothing");
    require(input.enable(fixture) && input.error() == 0 && FakeRawInput::calls.size() == 9, "A later registration clears the error");
  }
  require(FakeRawInput::calls.size() == 12 && FakeRawInput::calls.back().dwFlags == RIDEV_REMOVE,
          "Destruction removes the remaining registration");
  require(CameraButtonInput::device_name(0xfffe, 0xfffe).empty(), "An absent controller has no name");
}
}  // namespace

int main() {
  try {
    wchar_t repository[32768]{};
    require(GetCurrentDirectoryW(32768, repository), "Get test artifact directory");
    const auto directory = std::wstring(repository) + L"\\build\\buttons-test-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated ignored settings fixture");
    format_checks();
    validation_checks();
    persistence_checks(directory);
    edge_checks();
    repeat_checks();
    registration_checks();
    RemoveDirectoryW(directory.c_str());
    std::printf("PASS controller buttons: %u binding, persistence, press-edge and registration checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL controller buttons: %s\n", error.what());
    return 1;
  }
}
