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
BodyPose at(double x, double y = 0, double z = 0) {
  BodyPose pose;
  pose.origin = {x, y, z};
  return pose;
}
bool near(double a, double b, double tolerance = 1e-6) {
  return std::abs(a - b) < tolerance;
}
}  // namespace

int main() {
  bool ok = true;
  constexpr std::uint64_t Epoch = 4, Resets = 1;
  constexpr double Frame = 0.025;
  PoseLead lead;
  // First read: no estimate yet, no lead.
  auto pose = lead.lead(at(0), 100, 10.0, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 0) && lead.step_metres() == 0 && lead.lead_metres() == 0, "The first read was led");
  // Three updates later the model moved 7.5 m: 2.5 m per update, 100 m/s. The
  // first estimate is taken as measured; one frame of lead is 2.5 m.
  pose = lead.lead(at(7.5), 103, 10.0 + 3 * Frame, Epoch, Resets, 1);
  ok &= require(near(lead.step_metres(), 2.5) && near(lead.speed(), 100) && near(pose.origin[0], 10) && near(lead.lead_metres(), 2.5),
                "The first step was not led by one update's movement");
  // A double simulation step in one update (5 m) moves the average only by its
  // per-update weight, so the cameras do not jump with it.
  pose = lead.lead(at(12.5), 104, 10.0 + 4 * Frame, Epoch, Resets, 1);
  ok &= require(near(lead.step_metres(), 2.5 + PoseLead::kStepSmoothing * 2.5) && near(pose.origin[0], 12.5 + 2.75),
                "A double step was not smoothed");
  // Reads several updates apart weigh as that many per-update steps.
  PoseLead spaced;
  spaced.lead(at(0), 1, 1.0, Epoch, Resets, 1);
  spaced.lead(at(3), 2, 1.0 + Frame, Epoch, Resets, 1);      // 3 m per update.
  spaced.lead(at(9), 4, 1.0 + 3 * Frame, Epoch, Resets, 1);  // 3 m per update again.
  ok &= require(near(spaced.step_metres(), 3), "A steady step changed across spaced reads");
  spaced.lead(at(9 + 3 * 5), 7, 1.0 + 6 * Frame, Epoch, Resets, 1);  // 5 m per update over 3 updates.
  const double weight = 1 - std::pow(1 - PoseLead::kStepSmoothing, 3);
  ok &= require(near(spaced.step_metres(), 3 + weight * 2), "Spaced reads were not weighted per update");
  // Orientation is never changed.
  ok &= require(pose.forward == at(0).forward && pose.up == at(0).up && pose.right == at(0).right, "The lead changed the orientation");
  // A repeated read in the same update keeps the estimate.
  const double before = lead.step_metres();
  lead.lead(at(12.5), 104, 10.0 + 4 * Frame, Epoch, Resets, 1);
  ok &= require(near(lead.step_metres(), before), "A repeated read changed the estimate");
  // Lead frames: 2 doubles it, 0 (no lead) only measures, invalid is none.
  PoseLead frames;
  frames.lead(at(0), 1, 1.0, Epoch, Resets, 2);
  pose = frames.lead(at(3), 2, 1.0 + Frame, Epoch, Resets, 2);
  ok &= require(near(pose.origin[0], 9) && near(frames.lead_metres(), 6), "Two lead frames were not applied");
  pose = frames.lead(at(6), 3, 1.0 + 2 * Frame, Epoch, Resets, 0);
  ok &= require(near(pose.origin[0], 6) && near(frames.step_metres(), 3) && frames.lead_metres() == 0,
                "A zero lead moved the cameras or did not measure");
  pose = frames.lead(at(9), 4, 1.0 + 3 * Frame, Epoch, Resets, std::numeric_limits<double>::quiet_NaN());
  ok &= require(near(pose.origin[0], 9), "A NaN lead moved the cameras");
  pose = frames.lead(at(12), 5, 1.0 + 4 * Frame, Epoch, Resets, 5);
  ok &= require(near(pose.origin[0], 12), "A lead beyond the maximum was applied");
  // Stationary: no lead.
  PoseLead stopped;
  stopped.lead(at(5), 1, 1.0, Epoch, Resets, 1);
  pose = stopped.lead(at(5), 2, 1.0 + Frame, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 5) && stopped.step_metres() == 0, "A stationary aircraft was led");
  // A slew or teleport (over 40 m per update or 400 m/s) is not led, and the
  // next read measures afresh.
  pose = stopped.lead(at(500), 3, 1.0 + 2 * Frame, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 500) && stopped.step_metres() == 0, "A teleport was led");
  pose = stopped.lead(at(502.5), 4, 1.0 + 3 * Frame, Epoch, Resets, 1);
  ok &= require(near(stopped.step_metres(), 2.5) && near(pose.origin[0], 505), "After a teleport the step was not measured afresh");
  // Reads too many updates or too long apart, a new session or a reset measure afresh.
  pose = stopped.lead(at(600), 4 + PoseLead::kMaximumUpdates + 1, 1.2, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 600) && stopped.step_metres() == 0, "Reads too many updates apart measured a step");
  stopped.lead(at(602), 36, 1.2 + Frame, Epoch, Resets, 1);
  pose = stopped.lead(at(604), 37, 2.0, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 604) && stopped.step_metres() == 0, "Reads too long apart measured a step");
  stopped.lead(at(606), 38, 2.0 + Frame, Epoch, Resets, 1);
  pose = stopped.lead(at(608), 39, 2.0 + 2 * Frame, Epoch + 1, Resets, 1);
  ok &= require(near(pose.origin[0], 608) && stopped.step_metres() == 0, "A new aircraft session kept the old step");
  stopped.lead(at(610), 40, 2.0 + 3 * Frame, Epoch + 1, Resets, 1);
  pose = stopped.lead(at(612), 41, 2.0 + 4 * Frame, Epoch + 1, Resets + 1, 1);
  ok &= require(near(pose.origin[0], 612) && stopped.step_metres() == 0, "A session reset kept the old step");
  // An earlier update count does not produce a step.
  PoseLead backwards;
  backwards.lead(at(0), 10, 1.0, Epoch, Resets, 1);
  pose = backwards.lead(at(3), 9, 1.0 + Frame, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 3) && backwards.step_metres() == 0, "A backwards update count produced a step");
  // Movement in three dimensions keeps its direction.
  PoseLead diagonal;
  diagonal.lead(at(0, 0, 0), 1, 1.0, Epoch, Resets, 1);
  pose = diagonal.lead(at(2, -1, 2), 2, 1.0 + Frame, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 4) && near(pose.origin[1], -2) && near(pose.origin[2], 4) && near(diagonal.step_metres(), 3),
                "A diagonal movement was not led along its direction");
  // Epoch zero (no readiness yet) never measures.
  PoseLead unready;
  unready.lead(at(0), 1, 1.0, 0, Resets, 1);
  pose = unready.lead(at(3), 2, 1.0 + Frame, 0, Resets, 1);
  ok &= require(near(pose.origin[0], 3), "A read before session readiness was led");
  if (!ok)
    return 1;
  std::printf("PASS pose lead: %u checks; smoothed per-update step, double-step damping, session/teleport/gap guards.\n", checks);
  return 0;
}
