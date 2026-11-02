#include "../../src/shared/door_camera.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../src/camera/render_schedule.hpp"
#include "../../src/profiles/door_templates.hpp"
#include "../../src/shared/manual_camera_intent.hpp"

using namespace taxi_camera;
using namespace taxi_camera::standalone;

namespace {
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
profiles::DiscoveredLayout fenix_layout() {
  profiles::DiscoveredLayout l{};
  std::strcpy(l.key, "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_CFM_SL");
  l.sides = 2;
  l.shapes[0] = l.shapes[1] = {768, 768, 5};
  std::strcpy(l.textures[0], "$A320_PFD_Captain");
  std::strcpy(l.textures[1], "$A320_PFD_FO");
  const char* anchors[]{"$A320_ECAM_Upper", "$A320_ND_Captain", "$A320_ECAM_Lower", "$A320_ND_FO"};
  for (unsigned i = 0; i < 4; ++i) {
    std::strcpy(l.anchors[i].name, anchors[i]);
    l.anchors[i].width = l.anchors[i].height = 768;
  }
  return l;
}
}  // namespace

int main() {
  // A snapshot renders from each turn-on until three pages are composed from
  // it, then stops; live keeps rendering.
  DoorSnapshot snapshot;
  require(!snapshot.observe(false, false, 0), "Off never renders");
  require(snapshot.observe(true, false, 4) && snapshot.observe(true, false, 6), "A turn-on renders");
  require(!snapshot.observe(true, false, 7) && !snapshot.observe(true, false, 7), "Three pages later it stops and stays stopped");
  require(!snapshot.observe(false, false, 7) && snapshot.observe(true, false, 7), "Each turn-on takes a new snapshot");
  require(snapshot.observe(true, true, 30) && snapshot.observe(true, true, 31), "Live keeps rendering");
  require(snapshot.observe(true, false, 31) && snapshot.observe(true, false, 33) && !snapshot.observe(true, false, 34),
          "Switching from live takes a snapshot");
  DoorSnapshot reset;
  require(reset.observe(true, false, 10) && !reset.observe(true, false, 13), "Pages counted from the turn-on");
  require(reset.observe(true, false, 0) && !reset.observe(true, false, 3), "A restarted page count starts a new snapshot");
  require(door_state(false, true, true) == DoorState::off && door_state(true, false, false) == DoorState::waiting &&
              door_state(true, true, true) == DoorState::live && door_state(true, false, true) == DoorState::snapshot,
          "Door states");

  // Two-camera aircraft have a door camera; the PMDG 777 does not.
  Settings settings{};
  settings.profile = profiles::A380.id;
  require(!door_camera_set_up(settings) && !toggle_door_camera(settings) && !settings.door_on, "No display, no door camera");
  settings.door_side = 1;
  require(valid_settings(settings) && door_camera_set_up(settings), "A pilot display can show the door camera");
  require(toggle_door_camera(settings) && settings.door_on && camera_action_requested(settings, Status{}, CameraDoor),
          "The door action turns it on");
  require(toggle_door_camera(settings) && !settings.door_on, "and off");
  settings.door_side = 2;
  require(!valid_settings(settings), "A side the aircraft does not have is refused");
  settings.door_side = 1;
  settings.door_live = 2;
  require(!valid_settings(settings), "Live is 0 or 1");
  settings = {};
  settings.profile = profiles::Pmdg777300ER.id;
  settings.door_side = 2;
  require(!valid_settings(settings), "The 777 has no door camera yet");
  settings.door_on = 1;
  fit_door_camera(settings);
  require(settings.door_side == DoorSideNone && !settings.door_on && valid_settings(settings), "fit_door_camera removes it");
  settings = {};
  settings.profile = profiles::AerosoftA346.id;
  settings.door_side = 2;
  fit_door_camera(settings);
  require(settings.door_side == 2 && valid_settings(settings), "A lower ECAM keeps it");
  require(camera_action_sides(profiles::A380, CameraDoor) == 0, "The door action is not a side toggle");

  // The Fenix template: the lower ECAM becomes the lower display.
  const auto* door = profiles::door_template("SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_321_IAE_WF_SC");
  require(door && profiles::door_template("simobjects/airplanes/fnx_32x/presets/fnx/FNX_320_CFM_SL") == door,
          "Every Fenix preset finds the template");
  require(!profiles::door_template("SimObjects/Airplanes/FNX_32X_copy/presets/x/y") && !profiles::door_template(""),
          "Other aircraft do not");
  auto layout = fenix_layout();
  require(profiles::add_door_display(layout, *door) && layout.sides == 3 && std::strcmp(layout.textures[2], "$A320_ECAM_Lower") == 0 &&
              layout.shapes[2].width == 768 && layout.shapes[2].mips == 5,
          "The lower ECAM is added");
  require(std::strcmp(layout.anchors[2].name, "$A320_ND_FO") == 0 && !layout.anchors[3].name[0] && !layout.anchors[3].width &&
              profiles::valid_layout(layout),
          "and stops being an anchor");
  auto chosen = fenix_layout();
  chosen.sides = 3;
  std::strcpy(chosen.textures[2], "$A320_ECAM_Upper");
  chosen.shapes[2] = {768, 768, 5};
  std::strcpy(chosen.anchors[0].name, "$A320_Standby");
  chosen.anchors[0].width = chosen.anchors[0].height = 256;
  const auto before = chosen;
  require(profiles::add_door_display(chosen, *door) && profiles::same_layout(chosen, before), "A lower display already chosen is kept");
  auto broken = fenix_layout();
  broken.key[0] = 0;
  const auto broken_before = broken;
  require(!profiles::add_door_display(broken, *door) && profiles::same_layout(broken, broken_before), "An invalid result changes nothing");

  // Only the wanted feeds render: the taxi cameras, the door camera, or both.
  native_camera::RenderSchedule schedule;
  schedule.configure(10, 3, 1, 1u << profiles::DoorFeed);
  require(schedule.feeds() == 3 && schedule.rendered() == 1, "A set of three renders only the door camera");
  for (std::uint64_t t = 1000; t < 1100; t += 33) {
    const auto active = schedule.tick(t);
    require(!active[0] && !active[1] && active[2], "Every frame opens the door camera alone");
  }
  schedule.configure(10, 3, 1, 3);
  unsigned opened[3]{};
  for (std::uint64_t t = 2000; t < 2000 + 33 * 10; t += 33) {
    const auto active = schedule.tick(t);
    for (unsigned i = 0; i < 3; ++i)
      opened[i] += active[i];
  }
  require(opened[0] == 5 && opened[1] == 5 && !opened[2], "The taxi cameras take turns without the door camera");
  schedule.configure(10, 3, 0, 0);
  require(schedule.rendered() == 3 && schedule.wanted() == 7, "No mask renders every feed");
  schedule.configure(10, 2, 0, 1u << profiles::DoorFeed);
  require(schedule.rendered() == 2, "A feed the set does not have is ignored");
  std::printf("PASS: %u door camera checks.\n", checks);
  return 0;
}
