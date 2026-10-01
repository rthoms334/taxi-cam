#include "../../src/camera/pose_lead.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using taxi_camera::native_camera::BodyPose;
using taxi_camera::native_camera::PoseLead;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
BodyPose at(double x, double y, double z) {
  BodyPose pose;
  pose.origin = {x, y, z};
  return pose;
}
bool near(double a, double b) {
  return std::abs(a - b) < 1e-9;
}
}  // namespace

int main() {
  bool ok = true;
  constexpr std::uint64_t Epoch = 4, Resets = 1;
  PoseLead lead;
  // First read: nothing to measure against, no lead.
  auto pose = lead.lead(at(100, 0, 0), 10, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 100) && lead.step_metres() == 0 && lead.lead_metres() == 0, "The first read was led");
  // 3 updates later the model moved 9 m: 3 m per frame, one frame of lead.
  pose = lead.lead(at(109, 0, 0), 13, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 112) && near(lead.step_metres(), 3) && near(lead.lead_metres(), 3),
                "The pose was not led by one frame of the model's movement");
  // Orientation is never changed.
  ok &= require(pose.forward == at(0, 0, 0).forward && pose.up == at(0, 0, 0).up, "The lead changed the orientation");
  // A second read in the same update reuses that step.
  pose = lead.lead(at(109, 0, 0), 13, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 112) && near(lead.step_metres(), 3), "A repeated read in one update lost the step");
  // Two frames of lead; zero frames (an aircraft without a lead) only measures.
  pose = lead.lead(at(112, 0, 0), 14, Epoch, Resets, 2);
  ok &= require(near(pose.origin[0], 118) && near(lead.lead_metres(), 6), "Two lead frames were not applied");
  pose = lead.lead(at(115, 0, 0), 15, Epoch, Resets, 0);
  ok &= require(near(pose.origin[0], 115) && near(lead.step_metres(), 3) && lead.lead_metres() == 0,
                "An aircraft without a lead was moved, or its step was not measured");
  // Invalid lead settings are treated as none.
  pose = lead.lead(at(118, 0, 0), 16, Epoch, Resets, std::numeric_limits<double>::quiet_NaN());
  ok &= require(near(pose.origin[0], 118), "A NaN lead moved the cameras");
  pose = lead.lead(at(121, 0, 0), 17, Epoch, Resets, 5);
  ok &= require(near(pose.origin[0], 121), "A lead beyond the maximum was applied");
  // Stationary: no lead, no drift.
  pose = lead.lead(at(121, 0, 0), 18, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 121) && lead.step_metres() == 0, "A stationary aircraft was led");
  // A slew or teleport (over 40 m per frame) is not led.
  pose = lead.lead(at(5000, 0, 0), 19, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 5000) && lead.step_metres() == 0, "A teleport was led");
  // Reads too far apart, a new aircraft session or a session reset measure afresh.
  lead.lead(at(5003, 0, 0), 20, Epoch, Resets, 1);
  pose = lead.lead(at(5200, 0, 0), 20 + PoseLead::kMaximumUpdates + 1, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 5200) && lead.step_metres() == 0, "Reads too far apart estimated a step");
  pose = lead.lead(at(5203, 0, 0), 52, Epoch + 1, Resets, 1);
  ok &= require(near(pose.origin[0], 5203) && lead.step_metres() == 0, "A new aircraft session kept the old reference");
  pose = lead.lead(at(5206, 0, 0), 53, Epoch + 1, Resets + 1, 1);
  ok &= require(near(pose.origin[0], 5206) && lead.step_metres() == 0, "A session reset kept the old reference");
  // An earlier update count (counter reset) does not produce a step.
  pose = lead.lead(at(5209, 0, 0), 40, Epoch + 1, Resets + 1, 1);
  ok &= require(near(pose.origin[0], 5209) && lead.step_metres() == 0, "A backwards update count produced a step");
  // Movement in three dimensions keeps its direction.
  PoseLead diagonal;
  diagonal.lead(at(0, 0, 0), 1, Epoch, Resets, 1);
  pose = diagonal.lead(at(2, -1, 2), 2, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 4) && near(pose.origin[1], -2) && near(pose.origin[2], 4) && near(diagonal.step_metres(), 3),
                "A diagonal step was not led along its direction");
  // Epoch zero (no readiness yet) never estimates a step.
  PoseLead unready;
  unready.lead(at(0, 0, 0), 1, 0, Resets, 1);
  pose = unready.lead(at(3, 0, 0), 2, 0, Resets, 1);
  ok &= require(near(pose.origin[0], 3), "A read before session readiness was led");
  if (!ok)
    return 1;
  std::printf("PASS pose lead: %u checks; per-frame step from update counts, session/teleport/gap guards, orientation kept.\n", checks);
  return 0;
}
