#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include "../../src/shared/manual_camera_intent.hpp"

using namespace taxi_camera::standalone;
namespace profiles = taxi_camera::profiles;

namespace {
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
bool parsed(const wchar_t* text, const char* expected) {
  char name[LvarNameSize];
  return parse_lvar_name(text, name) && std::strcmp(name, expected) == 0;
}
LvarBindingStates read_states(float right, std::uint32_t rises, std::uint32_t falls) {
  LvarBindingStates states{};
  states[CameraRight] = {right, rises, falls, 1};
  return states;
}
}  // namespace

int main() {
  // Names: an L: prefix and spaces are dropped; empty unbinds; anything that
  // could carry a unit or a second variable is refused.
  require(parsed(L"S_PED_COCKPIT_DOOR_VIDEO", "S_PED_COCKPIT_DOOR_VIDEO") && parsed(L"  L:S_CCTV  ", "S_CCTV") &&
              parsed(L"l:A32NX.TAXI_2", "A32NX.TAXI_2") && parsed(L"", "") && parsed(L"   ", "") && parsed(L"L:", ""),
          "Typed names are cleaned up");
  for (const wchar_t* bad : {L"L:NAME, bool", L"NAME;OTHER", L"A B", L"L:L:NAME", L"NAMEé", L"A:LIGHT TAXI", L"NAME:1"}) {
    char name[LvarNameSize] = "previous";
    require(!parse_lvar_name(bad, name) && !name[0], "Unsafe names are refused and leave nothing");
  }
  std::wstring longest(LvarNameSize - 1, L'A'), too_long(LvarNameSize, L'A');
  char name[LvarNameSize];
  require(parse_lvar_name(longest.c_str(), name) && std::strlen(name) == LvarNameSize - 1 && !parse_lvar_name(too_long.c_str(), name),
          "Names fit the 63-character field");
  LvarBindings bindings{};
  require(valid_lvar_bindings(bindings), "No bindings is valid");
  std::strcpy(bindings[CameraRight].name, "S_PED_COCKPIT_DOOR_VIDEO");
  bindings[CameraRight].trigger = static_cast<std::uint32_t>(LvarTrigger::latched);
  require(valid_lvar_bindings(bindings), "A bound action is valid");
  auto bad = bindings;
  bad[CameraLeft].trigger = 2;
  require(!valid_lvar_bindings(bad), "Unknown triggers are refused");
  bad = bindings;
  std::memset(bad[CameraLeft].name, 'A', LvarNameSize);
  require(!valid_lvar_bindings(bad), "An unterminated name is refused");
  bad = bindings;
  std::strcpy(bad[CameraLeft].name, "A B");
  require(!valid_lvar_bindings(bad), "An unsafe saved name is refused");
  require(lvar_on(1) && lvar_on(1.00275) && lvar_on(100) && lvar_on(-1) && !lvar_on(0) && !lvar_on(0.003) &&
              !lvar_on(std::numeric_limits<double>::quiet_NaN()),
          "On is a magnitude of at least one half");

  // Momentary: each press toggles once; the first reading and a new stream
  // only record the counters.
  bindings = {};
  std::strcpy(bindings[CameraRight].name, "S_PED_COCKPIT_DOOR_VIDEO");
  LvarBindingTracker tracker;
  std::array<LvarCommand, LvarBindingCount> commands{};
  require(!tracker.update(bindings, 0, {}, commands), "Nothing acts without a stream");
  require(!tracker.update(bindings, 7, read_states(0, 4, 4), commands), "The first reading only records the counters");
  require(!tracker.update(bindings, 7, read_states(0, 4, 4), commands), "Unchanged counters do nothing");
  require(tracker.update(bindings, 7, read_states(1, 5, 4), commands) == 1 && commands[0].action == CameraRight &&
              commands[0].kind == LvarCommandKind::toggle,
          "A press toggles the action");
  require(!tracker.update(bindings, 7, read_states(0, 5, 5), commands), "Its release does nothing");
  require(!tracker.update(bindings, 7, read_states(0, 7, 7), commands), "Two presses between readings cancel out");
  require(tracker.update(bindings, 7, read_states(0, 10, 10), commands) == 1, "Three presses toggle once");
  require(!tracker.update(bindings, 8, read_states(1, 1, 0), commands), "A new stream starts again without acting");
  require(tracker.update(bindings, 8, read_states(0, 2, 1), commands) == 1, "Presses on the new stream act");
  auto unread = read_states(0, 3, 2);
  unread[CameraRight].read = 0;
  require(!tracker.update(bindings, 8, unread, commands), "An unread binding does nothing");
  require(!tracker.update(bindings, 8, read_states(0, 4, 3), commands), "Reading again starts again");
  auto renamed = bindings;
  std::strcpy(renamed[CameraRight].name, "S_CCTV");
  require(!tracker.update(renamed, 8, read_states(0, 5, 4), commands), "A new name starts again on the same stream");
  require(!tracker.update({}, 8, read_states(0, 6, 5), commands), "An unbound action never acts");
  std::uint32_t wrap = std::numeric_limits<std::uint32_t>::max();
  LvarBindingTracker wrapping;
  wrapping.update(bindings, 3, read_states(0, wrap, wrap), commands);
  require(wrapping.update(bindings, 3, read_states(1, 0, wrap), commands) == 1, "Counters wrap");

  // Latched: a change reports the value's state; the companion acts only when
  // the action's displays differ.
  bindings[CameraRight].trigger = static_cast<std::uint32_t>(LvarTrigger::latched);
  LvarBindingTracker latched;
  require(!latched.update(bindings, 1, read_states(1, 0, 0), commands), "A switch already on does nothing at first");
  require(latched.update(bindings, 1, read_states(0, 0, 1), commands) == 1 && commands[0].kind == LvarCommandKind::off, "Off turns off");
  require(latched.update(bindings, 1, read_states(1, 1, 1), commands) == 1 && commands[0].kind == LvarCommandKind::on, "On turns on");
  require(latched.update(bindings, 1, read_states(1, 2, 2), commands) == 1 && commands[0].kind == LvarCommandKind::on,
          "A quick off and on ends on");
  LvarBindingStates both{};
  both[CameraLeft] = {1, 0, 0, 1};
  both[CameraSd] = {0, 0, 0, 1};
  bindings[CameraLeft] = bindings[CameraRight];
  bindings[CameraSd] = bindings[CameraRight];
  LvarBindingTracker several;
  several.update(bindings, 2, both, commands);
  both[CameraLeft].falls = 1;
  both[CameraLeft].value = 0;
  both[CameraSd].rises = 1;
  both[CameraSd].value = 1;
  require(several.update(bindings, 2, both, commands) == 2 && commands[0].action == CameraLeft &&
              commands[0].kind == LvarCommandKind::off && commands[1].action == CameraSd && commands[1].kind == LvarCommandKind::on,
          "Each binding gives its own command");

  // Whether the action's displays are on is read as its shortcut reads it.
  Settings settings{};
  Status status{};
  settings.profile = profiles::A380.id;
  settings.follow_taxi = 1;
  status.taxi_buttons_mask = 2;
  require(camera_action_requested(settings, status, CameraRight) && !camera_action_requested(settings, status, CameraLeft) &&
              !camera_action_requested(settings, status, CameraBoth),
          "Followed TAXI buttons give the state");
  settings.follow_taxi = 0;
  settings.manual_mask = 3;
  require(camera_action_requested(settings, status, CameraLeft) && camera_action_requested(settings, status, CameraBoth),
          "Otherwise the manual request does");
  require(!camera_action_requested(settings, status, CameraSd), "An aircraft without the SD has no SD state");
  settings.profile = profiles::Pmdg777300ER.id;
  settings.follow_taxi = 1;
  settings.manual_mask = 4;
  status.taxi_buttons_mask = 1;
  require(camera_action_requested(settings, status, CameraSd) && !camera_action_requested(settings, status, CameraLeft),
          "The 777's manual layer, not its CAM pages");
  settings.profile = profiles::IniA380.id;
  settings.manual_mask = 1;
  status.taxi_buttons_mask = 2;
  require(camera_action_requested(settings, status, CameraLeft) && !camera_action_requested(settings, status, CameraRight),
          "Manual-only aircraft use the manual request whatever follow_taxi says");

  // Settings carry the bindings; the protocol refuses unsafe ones.
  settings = {};
  require(valid_settings(settings), "Default settings are valid");
  std::strcpy(settings.lvar_bindings[CameraBoth].name, "A B");
  require(!valid_settings(settings), "Settings with an unsafe name are refused");
  std::printf("PASS: %u L:var binding checks.\n", checks);
  return 0;
}
