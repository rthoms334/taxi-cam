#include "../../src/camera/render_schedule.hpp"
#include "../../src/profiles/catalog.hpp"
#include "../../src/shared/camera_rate_policy.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using taxi_camera::EffectiveCameraRate;
using taxi_camera::native_camera::RenderSchedule;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

struct Run {
  std::array<unsigned, 3> images{};
  unsigned updates = 0, gate_changes = 0, busiest_frame = 0, quietest_frame = 3;
  std::array<unsigned, 4> frames_with{};  // Updates that rendered 0, 1, 2 or 3 cameras.
};

// Drives the schedule for ten seconds at a simulator frame time (optionally
// jittered), checking its contracts on every update. The first second settles
// the frame-time estimate and is left out of the per-frame spread.
Run drive(unsigned rate, unsigned feeds, double frame_ms, double jitter = 0, unsigned per_frame = 0) {
  RenderSchedule schedule;
  schedule.configure(rate, feeds, per_frame);
  Run run;
  std::array<bool, 3> previous{};
  double t = 0;
  for (unsigned tick = 0; t < 10000; ++tick) {
    const auto now = static_cast<std::uint64_t>(t);
    const auto active = schedule.tick(now);
    ++run.updates;
    if (t >= 1000)
      run.gate_changes += active != previous;
    previous = active;
    unsigned open = 0;
    for (unsigned feed = 0; feed < 3; ++feed) {
      require(!active[feed] || feed < feeds, "A disabled feed opened");
      if (active[feed]) {
        ++open;
        ++run.images[feed];
      }
    }
    ++run.frames_with[open];
    if (t >= 1000) {
      run.busiest_frame = std::max(run.busiest_frame, open);
      run.quietest_frame = std::min(run.quietest_frame, open);
    }
    t += frame_ms + (tick % 2 ? jitter : -jitter);
  }
  return run;
}

void cadence() {
  for (unsigned rate : {5u, 10u, 15u, 20u, 30u, 45u, 60u})
    for (unsigned feeds : {1u, 2u, 3u})
      for (double fps : {18.0, 24.0, 30.0, 45.0, 60.0, 90.0, 144.0}) {
        const auto run = drive(rate, feeds, 1000 / fps, fps < 100 ? 2 : 0);
        const double expected = std::min<double>(rate, fps);
        for (unsigned feed = 0; feed < feeds; ++feed) {
          require(run.images[feed] / 10.0 >= expected * 0.93 - 0.3, "A camera did not reach the target or the simulator's rate");
          require(run.images[feed] / 10.0 <= rate * 1.05 + 0.5, "A camera ran above its target");
        }
        // Every frame carries the same number of cameras or one more.
        require(run.busiest_frame <= run.quietest_frame + 1, "The camera renders bunched onto some frames");
        if (fps <= rate)
          require(run.quietest_frame == feeds && run.gate_changes <= 1,
                  "A target at or above the simulator's rate did not render every frame");
      }
}

// 2026-10-07 live, PMDG 777 near 30 fps: with the cameras timed on clocks a
// long frame owed two cameras and a short one none, and frames swung between
// about 25 and 50 ms. Counted per frame, 10 per camera is one camera on every
// frame and 15 is one and two in turn.
void even_frames() {
  const auto performance = drive(10, 3, 1000.0 / 30, 1);
  require(performance.busiest_frame == 1 && performance.quietest_frame == 1, "Performance at 30 fps did not render one camera per frame");
  for (unsigned feed = 0; feed < 3; ++feed)
    require(performance.images[feed] >= 97 && performance.images[feed] <= 102, "Performance at 30 fps was not 10 per camera");
  const auto balanced = drive(15, 3, 1000.0 / 30, 1);
  require(balanced.quietest_frame == 1 && balanced.busiest_frame == 2 && balanced.images[0] >= 145,
          "Balanced at 30 fps did not render one and two cameras in turn");
  const auto smooth_36 = drive(30, 3, 1000.0 / 36, 1);
  require(smooth_36.quietest_frame == 2 && smooth_36.busiest_frame == 3 && smooth_36.images[0] >= 285,
          "Smooth at 36 fps did not reach 30 per camera with two or three cameras per frame");
  const auto smooth_24 = drive(30, 3, 1000.0 / 24, 1);
  require(smooth_24.quietest_frame == 3 && smooth_24.images[0] >= 235, "Smooth at 24 fps did not render every camera every frame");
  // Auto: a fixed count of cameras on every frame, round-robin.
  for (unsigned per_frame : {1u, 2u, 3u}) {
    const auto automatic = drive(10, 3, 1000.0 / 30, 2, per_frame);
    require(automatic.quietest_frame == per_frame && automatic.busiest_frame == per_frame, "Auto did not render its count on every frame");
    require(automatic.images[0] == automatic.images[1] || automatic.images[0] == automatic.images[1] + 1,
            "Auto did not share frames fairly");
  }
  require(drive(10, 2, 1000.0 / 30, 0, 3).quietest_frame == 2, "Auto asked for more cameras than there are");
}

void stalls_suspension_and_changes() {
  using Gates = std::array<bool, 3>;
  RenderSchedule schedule;
  schedule.configure(10, 3);
  // The first frame opens one camera; then 10 × 3 at 30 fps is one per frame.
  require(schedule.tick(1000) == Gates{true, false, false}, "The first frame did not open one camera");
  for (std::uint64_t now = 1033; now < 1400; now += 33)
    schedule.tick(now);
  require(schedule.tick(1400) == Gates{true, false, false} || schedule.tick(1433) != Gates{}, "The rotation stopped");
  // A long stall opens at most one frame's count.
  const auto after_stall = schedule.tick(9000);
  require(static_cast<unsigned>(after_stall[0] + after_stall[1] + after_stall[2]) <= 1, "A stall caused a catch-up burst");
  // Suspension closes every camera.
  for (std::uint64_t now = 9033; now < 12000; now += 33)
    require(schedule.tick(now, true) == Gates{}, "Suspension left a gate open");
  const auto resumed = schedule.tick(12033);
  require(static_cast<unsigned>(resumed[0] + resumed[1] + resumed[2]) == 1, "The cameras did not resume one per frame");
  // A clock reversal closes every camera for that update.
  require(schedule.tick(500) == Gates{}, "A clock reversal left a gate open");
  // A count of every camera holds all of them open.
  schedule.configure(10, 3, 3);
  require(schedule.tick(533) == Gates{true, true, true} && schedule.tick(566) == Gates{true, true, true},
          "Every camera per frame did not hold every gate open");
  // Fewer feeds close the dropped ones at once.
  schedule.configure(10, 1, 3);
  require(schedule.tick(600) == Gates{true, false, false}, "A dropped feed stayed open");
  schedule.reset();
  require(schedule.tick(10)[0], "Reset did not start again");
  RenderSchedule bounds;
  bounds.configure(0, 0, 9);
  require(bounds.rate() == taxi_camera::kMinimumCameraRate && bounds.feeds() == 1 && bounds.per_frame() == 1, "Lower bounds");
  bounds.configure(999, 999);
  require(bounds.rate() == taxi_camera::kMaximumCameraRate && bounds.feeds() == 3 && bounds.per_frame() == 0, "Upper bounds");
  require(bounds.tick(std::numeric_limits<std::uint64_t>::max() - 1)[0], "Large clock value");
}

void frame_time_spreads() {
  using taxi_camera::frame_time_spread;
  std::array<std::uint32_t, 151> counts{};
  require(frame_time_spread(counts).frames == 0 && frame_time_spread(counts).p95 == 0, "An empty window has no spread");
  counts[20] = 200;
  counts[60] = 100;
  const auto bunched = frame_time_spread(counts);
  require(bunched.median == 20 && bunched.p95 == 60 && bunched.slowest == 60 && bunched.frames == 300,
          "A 20, 20, 60 ms rhythm did not show its spread");
  counts = {};
  counts[32] = 290;
  counts[34] = 10;
  const auto even = frame_time_spread(counts);
  require(even.median == 32 && even.p95 == 32 && even.slowest == 34, "Even frames showed a spread");
  counts[150] = 1;
  require(frame_time_spread(counts).slowest == 150, "The longest bucket was not reported");
}

void rates_and_targets() {
  using namespace taxi_camera;
  const auto check = [](EffectiveCameraRate actual, unsigned rate, unsigned useful, unsigned reasons, const char* message) {
    require(actual.rate == rate && actual.useful_maximum == useful && actual.reasons == reasons, message);
  };
  check(effective_camera_rate(10, 0), 10, 60, kRateLimitNone, "An unmeasured aircraft runs the target");
  check(effective_camera_rate(30, 80), 30, 60, kRateLimitNone, "A350 PFD refresh above the maximum is no cap");
  check(effective_camera_rate(30, 16), 16, 16, kRateLimitPfdRefresh, "ini A380 16 Hz refresh caps a 30 target");
  check(effective_camera_rate(15, 16), 15, 16, kRateLimitNone, "15 fits under the ini A380 refresh");
  check(effective_camera_rate(30, 2), 5, 5, kRateLimitPfdRefresh, "The PFD cap never goes below the minimum");
  check(effective_camera_rate(3, 0), 5, 60, kRateLimitNone, "An out-of-range target is clamped, not flagged");
  require(std::string_view(camera_rate_limit_name(kRateLimitNone)) == "target" &&
              std::string_view(camera_rate_limit_name(kRateLimitPfdRefresh)) == "pfd_refresh" &&
              std::string_view(camera_rate_limit_name(kRateLimitSimulator)) == "simulator_fps",
          "Rate limit names");
  require(camera_rate_limit_text(kRateLimitNone)[0] == L'\0' && camera_rate_limit_text(kRateLimitSimulator)[0] != L'\0',
          "Companion rate suffix");
  require(profiles::A380.pfd_refresh_hz == 0 && profiles::IniA380.pfd_refresh_hz == 16 && profiles::A359.pfd_refresh_hz == 80 &&
              profiles::A35K.pfd_refresh_hz == 80 && profiles::Pmdg777.pfd_refresh_hz == 0,
          "Catalog PFD refresh values changed");
  const auto target = [](CameraMode mode, unsigned custom) { return camera_mode_target(static_cast<unsigned>(mode), custom); };
  require(target(CameraMode::performance, 60) == 10 && target(CameraMode::balanced, 60) == 15 && target(CameraMode::smooth, 5) == 30,
          "Preset targets");
  require(target(CameraMode::custom, 42) == 42 && target(CameraMode::custom, 1) == kMinimumCameraRate &&
              target(CameraMode::custom, 999) == kMaximumCameraRate,
          "Custom runs the saved target, clamped");
  require(auto_cameras_per_frame(0, 3) == 1 && auto_cameras_per_frame(1, 3) == 2 && auto_cameras_per_frame(2, 3) == 3 &&
              auto_cameras_per_frame(2, 2) == 2 && auto_cameras_per_frame(9, 3) == 3 && auto_cameras_per_frame(0, 0) == 1,
          "Auto levels are 1, 2 and every camera per frame");
  require(camera_mode_target(99, 30) == kBalancedCameraRate && kDefaultCameraMode == CameraMode::automatic,
          "Unknown modes fall back to Balanced; Auto is the default");
  require(std::isnan(reachable_camera_rate(30, std::numeric_limits<double>::quiet_NaN())) && reachable_camera_rate(30, 24) == 24 &&
              reachable_camera_rate(10, 60) == 10,
          "Reachable rate is the target or the simulator's rate");
  require(kMaximumCaptureSourceRate >= 120, "Capture spacing refuses consecutive frames up to 120 fps");
}

void update_rate_meter() {
  taxi_camera::UpdateRateMeter meter;
  require(std::isnan(meter.update(1000, 0)), "Rate known before a window");
  require(std::isnan(meter.update(2999, 90)), "Rate known before the window closed");
  require(std::abs(meter.update(3000, 90) - 45) < 1e-9, "45 updates per second");
  require(std::abs(meter.update(4000, 140) - 45) < 1e-9, "Rate held inside a window");
  require(std::isnan(meter.update(5000, 91)), "A reset counter kept a stale rate");
  meter.update(7000, 91);
  require(std::isnan(meter.rate()), "A stalled counter counted as a simulator rate");
  require(std::abs(meter.update(9000, 211) - 60) < 1e-9, "Recovered after a stall");
  // 2026-10-07 live: a counter frozen while the cameras were off jumped by
  // thousands on resume and read as 1080 updates per second.
  require(std::isnan(meter.update(11000, 211 + 2160)), "A counter jump read as a frame rate");
  require(std::abs(meter.update(13000, 2371 + 80) - 40) < 1e-9, "The meter did not recover after a counter jump");
}

// Auto: drives the policy one 2 s rate window at a time against a simulator
// whose update rate is its camera-off rate less the cost of each level.
using Costs = std::array<double, taxi_camera::kAutoCameraLevels>;
struct AutoDrive {
  taxi_camera::AutoCameraPolicy policy;
  std::uint64_t now = 0, window = 0;
  unsigned level = 0, step_ups = 0;
  std::array<unsigned, taxi_camera::kAutoCameraLevels> windows_at{};
  // Cameras off for two windows: the first spans the switch and is skipped.
  void off(double fps) {
    step(fps, false);
    step(fps, false);
  }
  unsigned step(double fps, bool active = true, bool helps = true) {
    now += 2000;
    ++window;
    taxi_camera::AutoCameraPolicy::Input in;
    in.now_ms = now;
    in.window = window;
    in.fps = fps;
    in.cameras_active = active;
    in.next_helps = helps;
    const auto next = policy.update(in);
    require(next < taxi_camera::kAutoCameraLevels && (next == level || next + 1 == level || next == level + 1),
            "Auto moved more than one level at once");
    step_ups += next > level;
    level = next;
    ++windows_at[level];
    return level;
  }
  void run(double seconds, double camera_off, const Costs& cost) {
    for (double t = 0; t < seconds; t += 2)
      step(camera_off * (1 - cost[level]));
  }
};

void auto_camera_policy() {
  using taxi_camera::AutoCameraPolicy;
  const Costs costly{0.02, 0.06, 0.15}, cheap{0.01, 0.03, 0.06};
  // 60 fps with headroom: 20 fps costs 12 %, so Auto settles on 15 and retries
  // 20 ever less often instead of every few seconds.
  {
    AutoDrive drive;
    drive.off(60);
    require(drive.policy.base() == 60 && drive.policy.floor() == 54, "Camera-off rate sets the floor 10 % below it");
    drive.run(20, 60, costly);
    require(drive.level == 1, "Auto did not reach 15 with headroom");
    drive.windows_at = {};
    drive.step_ups = 0;
    drive.run(600, 60, costly);
    require(drive.level == 1, "Auto did not settle on its second level when the third costs too much");
    require(drive.step_ups <= 5 && drive.windows_at[2] * 100 <= 300 * 3, "Auto retried a target that does not fit too often");
  }
  // A cheap aircraft at 60 fps: 30 fits and Auto stays there.
  {
    AutoDrive drive;
    drive.off(60);
    drive.run(90, 60, cheap);
    require(drive.level == 2, "Auto did not reach its top level when it fits");
    drive.step_ups = 0;
    drive.run(600, 60, cheap);
    require(drive.level == 2 && drive.step_ups == 0, "Auto left a level that fits");
  }
  // 25 fps: 15 fits within 10 %, 20 does not.
  {
    AutoDrive drive;
    drive.off(25);
    drive.run(600, 25, costly);
    require(drive.level == 1, "A 25 fps simulator did not settle on 15");
  }
  // 21 fps: the 20 fps minimum keeps the first target.
  {
    AutoDrive drive;
    drive.off(21);
    drive.run(600, 21, costly);
    require(drive.level == 0 && drive.policy.floor() == AutoCameraPolicy::kMinimumFps, "Auto pushed a 21 fps simulator below 20");
  }
  // Heavier scenery: Auto backs off at once, then climbs back once the
  // cooldowns pass and the lower rate becomes the reference.
  {
    AutoDrive drive;
    drive.off(60);
    drive.run(90, 60, cheap);
    require(drive.level == 2, "Scenery fixture did not reach the top level");
    drive.run(4, 45, cheap);
    require(drive.level < 2, "Auto kept the top level after the frame rate fell below the floor");
    drive.run(12, 45, cheap);
    require(drive.level == 0, "Auto did not back off to 10");
    drive.run(600, 45, cheap);
    require(drive.level == 2, "Auto did not climb back once the lower rate became the reference");
  }
  // A moment of light scenery raises the reference; at the first target it
  // follows the measured rate down again instead of holding Auto at the bottom.
  {
    AutoDrive drive;
    drive.off(40);
    drive.run(20, 80, cheap);
    drive.run(400, 40, cheap);
    require(drive.level == 2, "A brief high rate held Auto at the first level");
  }
  // Cameras off, unknown rates, a target that does not help, the first window
  // after a change, and reset.
  {
    AutoDrive drive;
    drive.off(60);
    drive.run(90, 60, cheap);
    const double floor_before = drive.policy.floor();
    require(drive.step(10, false) == 2 && drive.policy.floor() == floor_before,
            "The first camera-off window, which spans the switch, set the reference");
    for (int i = 0; i < 20; ++i)
      require(drive.step(30, false) == 2, "Cameras off moved the level");
    require(drive.policy.floor() == 27, "Cameras off did not refresh the camera-off rate");
    for (int i = 0; i < 20; ++i)
      require(drive.step(std::numeric_limits<double>::quiet_NaN()) == 2 && drive.step(0) == 2, "An unknown rate moved the level");
    taxi_camera::AutoCameraPolicy::Input same;
    same.now_ms = drive.now + 1;
    same.window = drive.window;
    same.fps = 1;
    same.cameras_active = true;
    require(drive.policy.update(same) == 2, "One window was judged twice");
    drive.policy.reset();
    drive.level = 0;
    require(drive.policy.level() == 0 && std::isnan(drive.policy.base()), "Reset kept the level or the reference");
    drive.off(60);
    for (int i = 0; i < 30; ++i)
      require(drive.step(59, true, false) == 0, "Auto stepped up to a target that gives the cameras nothing more");
    AutoDrive settle;
    settle.off(60);
    require(settle.step(10) == 0, "The activation window was not discarded");
    require(settle.policy.floor() == 54, "The discarded window changed the reference");
  }
}
}  // namespace

int main() {
  try {
    cadence();
    even_frames();
    stalls_suspension_and_changes();
    frame_time_spreads();
    rates_and_targets();
    update_rate_meter();
    auto_camera_policy();
    std::printf(
        "PASS: %u render-schedule checks; cameras counted per frame round-robin, held open when the simulator is slower, "
        "no catch-up bursts, mode targets, rate caps and Auto.\n",
        checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
