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
  constexpr double Speed = 100;  // m/s, about 194 kt.
  PoseLead lead;
  // First read: no velocity yet, no lead.
  auto pose = lead.lead(at(0), 10.0, 0.03, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 0) && lead.speed() == 0 && lead.lead_metres() == 0, "The first read was led");
  // 0.1 s later the model moved 10 m: 100 m/s. A 30 ms frame leads 3 m.
  pose = lead.lead(at(10), 10.1, 0.03, Epoch, Resets, 1);
  ok &= require(near(lead.speed(), Speed) && near(pose.origin[0], 13) && near(lead.step_metres(), 3) && near(lead.lead_metres(), 3),
                "A 30 ms frame at 100 m/s was not led 3 m");
  // The lead follows the duration of the frame being drawn, not an average:
  // a 20 ms frame leads 2 m and a 45 ms frame 4.5 m at the same speed.
  pose = lead.lead(at(20), 10.2, 0.02, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 22) && near(lead.lead_metres(), 2), "A short frame was not led by its own duration");
  pose = lead.lead(at(30), 10.3, 0.045, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 34.5) && near(lead.lead_metres(), 4.5), "A long frame was not led by its own duration");
  // Orientation is never changed.
  ok &= require(pose.forward == at(0).forward && pose.up == at(0).up && pose.right == at(0).right, "The lead changed the orientation");
  // A noisy read (the transform advanced unevenly) is smoothed: one read 20%
  // fast moves the velocity only part of the way.
  pose = lead.lead(at(42), 10.4, 0.03, Epoch, Resets, 1);
  ok &= require(lead.speed() > Speed && lead.speed() < 120 && near(lead.speed(), Speed + PoseLead::kSmoothing * 20),
                "A noisy read was not smoothed");
  // A repeated read at the same clock keeps the velocity.
  const double before = lead.speed();
  lead.lead(at(42), 10.4, 0.03, Epoch, Resets, 1);
  ok &= require(near(lead.speed(), before), "A repeated read changed the velocity");
  // Lead frames: 2 doubles it, 0 (no lead) only measures, invalid is none.
  PoseLead frames;
  frames.lead(at(0), 1.0, 0.03, Epoch, Resets, 2);
  pose = frames.lead(at(10), 1.1, 0.03, Epoch, Resets, 2);
  ok &= require(near(pose.origin[0], 16) && near(frames.lead_metres(), 6), "Two lead frames were not applied");
  pose = frames.lead(at(20), 1.2, 0.03, Epoch, Resets, 0);
  ok &= require(near(pose.origin[0], 20) && near(frames.step_metres(), 3) && frames.lead_metres() == 0,
                "A zero lead moved the cameras or did not measure");
  pose = frames.lead(at(30), 1.3, 0.03, Epoch, Resets, std::numeric_limits<double>::quiet_NaN());
  ok &= require(near(pose.origin[0], 30), "A NaN lead moved the cameras");
  pose = frames.lead(at(40), 1.4, 0.03, Epoch, Resets, 5);
  ok &= require(near(pose.origin[0], 40), "A lead beyond the maximum was applied");
  // A hitch longer than 50 ms is capped.
  pose = frames.lead(at(50), 1.5, 0.5, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 55), "A long hitch was not capped at 50 ms");
  // A zero or invalid frame duration gives no lead.
  pose = frames.lead(at(60), 1.6, 0, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 60), "A zero frame duration was led");
  // Stopped: velocity decays towards zero with the smoothing.
  PoseLead stopped;
  stopped.lead(at(5), 1.0, 0.03, Epoch, Resets, 1);
  pose = stopped.lead(at(5), 1.1, 0.03, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 5) && stopped.speed() == 0, "A stationary aircraft was led");
  // A slew or teleport (over 400 m/s) is not led, and the next read measures afresh.
  pose = stopped.lead(at(500), 1.2, 0.03, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 500) && stopped.speed() == 0, "A teleport was led");
  pose = stopped.lead(at(510), 1.3, 0.03, Epoch, Resets, 1);
  ok &= require(near(stopped.speed(), Speed) && near(pose.origin[0], 513), "After a teleport the velocity was not measured afresh");
  // Reads too far apart, a new aircraft session or a session reset measure afresh.
  pose = stopped.lead(at(600), 2.0, 0.03, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 600) && stopped.speed() == 0, "Reads too far apart measured a velocity");
  stopped.lead(at(610), 2.1, 0.03, Epoch, Resets, 1);
  pose = stopped.lead(at(620), 2.2, 0.03, Epoch + 1, Resets, 1);
  ok &= require(near(pose.origin[0], 620) && stopped.speed() == 0, "A new aircraft session kept the old velocity");
  stopped.lead(at(630), 2.3, 0.03, Epoch + 1, Resets, 1);
  pose = stopped.lead(at(640), 2.4, 0.03, Epoch + 1, Resets + 1, 1);
  ok &= require(near(pose.origin[0], 640) && stopped.speed() == 0, "A session reset kept the old velocity");
  // A clock running backwards does not measure a velocity.
  PoseLead backwards;
  backwards.lead(at(0), 5.0, 0.03, Epoch, Resets, 1);
  pose = backwards.lead(at(10), 4.9, 0.03, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 10) && backwards.speed() == 0, "A backwards clock produced a velocity");
  // Movement in three dimensions keeps its direction.
  PoseLead diagonal;
  diagonal.lead(at(0, 0, 0), 1.0, 0.03, Epoch, Resets, 1);
  pose = diagonal.lead(at(6, -3, 6), 1.1, 0.03, Epoch, Resets, 1);
  ok &= require(near(pose.origin[0], 7.8) && near(pose.origin[1], -3.9) && near(pose.origin[2], 7.8) && near(diagonal.speed(), 90),
                "A diagonal movement was not led along its direction");
  // Epoch zero (no readiness yet) never measures.
  PoseLead unready;
  unready.lead(at(0), 1.0, 0.03, 0, Resets, 1);
  pose = unready.lead(at(10), 1.1, 0.03, 0, Resets, 1);
  ok &= require(near(pose.origin[0], 10), "A read before session readiness was led");
  if (!ok)
    return 1;
  std::printf("PASS pose lead: %u checks; velocity x frame duration, smoothing, hitch cap, session/teleport/gap guards.\n", checks);
  return 0;
}
