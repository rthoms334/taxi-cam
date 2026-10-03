#pragma once

#include <array>
#include <cstdint>

namespace taxi_camera {

// Presentation watchdog logic. The host thread samples lock-free counters and
// applies the decision: on trip it closes the graphics gate and disarms the
// cameras; on recovery it re-arms. The class itself never waits or locks.
//
// frame_pulse counts hooked ExecuteCommandLists and Close calls on simulator
// threads; it advances whenever the simulator presents, including in menus.
// sim_frames counts SIMCONNECT_PERIOD_SIM_FRAME telemetry packets; it stops in
// menus and while paused, so on its own it can only substitute for the pulse
// while no direct queue has been observed yet.
//
// A stall that returns each time the cameras re-arm would cycle for as long as
// they stay requested (issue 97: one trip about every 8 s on an A350, every
// 22-74 s on a PMDG 777). Only trips while cameras are requested count. After
// HoldTrips of them within HoldWindowMs, resumed frames hold the gate closed
// instead of re-arming while cameras are still requested, until the host calls
// release() for a fresh request. Trips with nothing requested (flight loading)
// neither count nor hold.
class FreezeWatchdog {
 public:
  static constexpr std::uint64_t StallMs = 3000;
  static constexpr std::uint64_t RecoveryMs = 5000;
  static constexpr unsigned HoldTrips = 3;
  static constexpr std::uint64_t HoldWindowMs = 300000;
  struct Sample {
    std::uint64_t now_ms = 0;
    std::uint64_t frame_pulse = 0;
    bool pulse_available = false;  // A direct queue or Close has been observed.
    std::uint64_t sim_frames = 0;
    bool sim_frames_expected = false;  // Flight session ready: SIM_FRAME should flow.
    bool armed = false;                // Bridge connected with hooks installed.
    bool worker_alive = false;         // Bridge worker heartbeat is recent.
    // Cameras requested before the watchdog gate. A whole-simulator stall
    // makes flight readiness lapse within a second, which clears the request,
    // so a trip uses the value from the last sample that saw progress.
    bool cameras_wanted = false;
  };
  struct Decision {
    bool trip = false;
    bool recover = false;
    // Frames resumed after repeated trips; the gate stays closed until release().
    bool hold = false;
    // SIM_FRAME stopped while presentation continued: pause or menu, logged once.
    bool telemetry_stall_noted = false;
    // Hooked lists and queues went quiet while SIM_FRAME continued: flight
    // reload or a renderer path we do not hook, logged once, never a trip.
    bool presentation_stall_noted = false;
    bool worker_alive = false;
    const char* reason = "";
    std::uint64_t stalled_ms = 0;
  };

  Decision observe(const Sample& sample) noexcept {
    Decision decision;
    decision.worker_alive = sample.worker_alive;
    if (!sample.armed) {
      // Nothing to guard; forget stall timers so re-arming starts a fresh window.
      pulse_since_ = sim_since_ = 0;
      recover_since_ = 0;
      telemetry_noted_ = presentation_noted_ = false;
      return decision;
    }
    const bool pulse_advanced = sample.pulse_available && (pulse_since_ == 0 || sample.frame_pulse != last_pulse_);
    if (pulse_advanced || !sample.pulse_available) {
      last_pulse_ = sample.frame_pulse;
      pulse_since_ = sample.now_ms;
      presentation_noted_ = false;
    }
    const bool sim_advanced = sim_since_ == 0 || sample.sim_frames != last_sim_;
    if (sim_advanced) {
      last_sim_ = sample.sim_frames;
      sim_since_ = sample.now_ms;
      telemetry_noted_ = false;
    }
    if (pulse_advanced || (!sample.pulse_available && sim_advanced))
      wanted_at_progress_ = sample.cameras_wanted;
    const auto pulse_stalled_ms = sample.pulse_available && sample.now_ms >= pulse_since_ ? sample.now_ms - pulse_since_ : 0;
    const auto sim_stalled_ms = sample.now_ms >= sim_since_ ? sample.now_ms - sim_since_ : 0;
    const bool pulse_stalled = sample.pulse_available && pulse_stalled_ms >= StallMs;
    // SIM_FRAME advancing proves the simulator is alive even when our hooked
    // lists and queues see nothing (flight reload). A soft freeze stops both
    // within half a second (issue 53), so requiring both loses no real trip.
    // Telemetry that has never flowed cannot veto a presentation stall.
    const bool sim_known = sample.sim_frames != 0;
    const bool sim_stalled = sim_stalled_ms >= StallMs;
    const bool telemetry_stalled = sample.sim_frames_expected && sim_stalled;
    if (tripped_) {
      const bool progressing = sample.pulse_available ? pulse_advanced : sim_advanced;
      if (held_ || !progressing) {
        recover_since_ = 0;
        return decision;
      }
      if (!recover_since_)
        recover_since_ = sample.now_ms;
      else if (sample.now_ms - recover_since_ >= RecoveryMs) {
        recover_since_ = 0;
        if (sample.cameras_wanted && recent_trips(sample.now_ms) >= HoldTrips) {
          held_ = true;
          decision.hold = true;
          decision.reason = "repeated_stalls";
          return decision;
        }
        tripped_ = false;
        decision.recover = true;
        decision.reason = "presentation_resumed";
      }
      return decision;
    }
    if ((pulse_stalled && (sim_stalled || !sim_known)) || (!sample.pulse_available && telemetry_stalled)) {
      tripped_ = true;
      if (wanted_at_progress_)
        trip_ms_[counted_++ % HoldTrips] = sample.now_ms ? sample.now_ms : 1;
      ++trips_;
      recover_since_ = 0;
      decision.trip = true;
      decision.reason = pulse_stalled ? (sim_known ? "presentation_and_sim_frame_stalled" : "presentation_stalled_no_telemetry")
                                      : "sim_frame_stalled_before_first_submit";
      decision.stalled_ms = pulse_stalled ? pulse_stalled_ms : sim_stalled_ms;
      return decision;
    }
    if (pulse_stalled && !presentation_noted_) {
      presentation_noted_ = true;
      decision.presentation_stall_noted = true;
      decision.reason = "presentation_stalled_sim_frame_alive";
      decision.stalled_ms = pulse_stalled_ms;
    }
    if (telemetry_stalled && !telemetry_noted_) {
      telemetry_noted_ = true;
      decision.telemetry_stall_noted = true;
      decision.reason = "sim_frame_stalled_presentation_alive";
      decision.stalled_ms = sim_stalled_ms;
    }
    return decision;
  }

  // A fresh pilot request after a hold. Frames have already run for the
  // recovery window, so the gate re-arms at once. Trip history is kept: one
  // more stall inside the window holds again after that stall's recovery.
  Decision release() noexcept {
    Decision decision;
    if (!held_)
      return decision;
    held_ = tripped_ = false;
    recover_since_ = 0;
    decision.recover = true;
    decision.reason = "released_after_hold";
    return decision;
  }

  // Trips while cameras were requested, inside the hold window.
  unsigned recent_trips(std::uint64_t now_ms) const noexcept {
    unsigned count = 0;
    for (const auto at : trip_ms_)
      count += at && now_ms >= at && now_ms - at < HoldWindowMs;
    return count;
  }
  bool tripped() const noexcept { return tripped_; }
  bool held() const noexcept { return held_; }
  unsigned trips() const noexcept { return trips_; }

 private:
  std::uint64_t last_pulse_ = 0, pulse_since_ = 0;
  std::uint64_t last_sim_ = 0, sim_since_ = 0;
  std::uint64_t recover_since_ = 0;
  std::array<std::uint64_t, HoldTrips> trip_ms_{};  // Latest counted trip times; 0 is empty.
  unsigned trips_ = 0, counted_ = 0;
  bool tripped_ = false;
  bool held_ = false;
  bool wanted_at_progress_ = false;
  bool telemetry_noted_ = false;
  bool presentation_noted_ = false;
};

}  // namespace taxi_camera
