#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace taxi_camera {

// Bridge state changes the worker tracks for the pilot. Only the ones that
// pass sim_event_toasts() are published through the IPC status (never from a
// render thread) and shown by the companion as Windows tray notifications when
// its "Show notifications" setting is on; the rest stay in the status line,
// tray tooltip and log. SimConnect_Text is deprecated in MSFS 2020/2024 and
// renders nothing.
enum class SimEvent : unsigned {
  bridge_connected,
  cameras_ready,
  connection_stopped,
  simulator_unsupported,
  presentation_stalled,  // Watchdog trip, posted by the watchdog thread itself.
  cameras_disarmed,      // Degraded path applied by the bridge worker.
  presentation_resumed,
  aircraft_mismatch,
  camera_startup_failed,
  capture_paused,
  hook_storm,  // Registration failures exceeded the safe rate; disarmed for the session.
  count
};

struct SimMessage {
  const char* text = "";
  float seconds = 6.0f;
  bool alert = false;  // Warning icon on the notification.
};

inline constexpr SimMessage sim_message_for(SimEvent event) noexcept {
  switch (event) {
    case SimEvent::bridge_connected:
      return {"Taxi Cam connected.", 5.0f, false};
    case SimEvent::cameras_ready:
      return {"Taxi Cam: cameras ready.", 5.0f, false};
    case SimEvent::connection_stopped:
      return {"Taxi Cam disconnected; camera output closed.", 6.0f, false};
    case SimEvent::simulator_unsupported:
      return {"Taxi Cam: this simulator build is not supported yet; cameras stay off.", 10.0f, true};
    case SimEvent::presentation_stalled:
      return {"Taxi Cam: simulator frames stalled; releasing Taxi Cam hooks so the simulator can continue.", 12.0f, true};
    case SimEvent::cameras_disarmed:
      return {"Taxi Cam degraded: cameras disarmed and hooks released; they re-arm when frames resume.", 12.0f, true};
    case SimEvent::presentation_resumed:
      return {"Taxi Cam: simulator frames resumed; cameras re-armed.", 6.0f, false};
    case SimEvent::aircraft_mismatch:
      return {"Taxi Cam: the loaded aircraft does not match the selected profile.", 8.0f, false};
    case SimEvent::camera_startup_failed:
      return {"Taxi Cam: camera startup stopped; see the companion Diagnostics.", 8.0f, true};
    case SimEvent::capture_paused:
      return {"Taxi Cam: camera capture paused; waiting for verified GPU state.", 6.0f, false};
    case SimEvent::hook_storm:
      return {"Taxi Cam: native hook failures exceeded the safe rate; cameras disarmed for this session.", 12.0f, true};
    default:
      return {};
  }
}

// Toast policy: a desktop notification only for what the pilot must act on or
// would otherwise not notice while the simulator is full-screen, i.e. cameras
// off for the rest of the session or until they change something. Routine
// transitions (connect/disconnect, cameras up, aircraft/profile
// mismatch, transient capture pauses, watchdog recovery) and the worker's
// echo of a watchdog or storm disarm never toast.
inline constexpr bool sim_event_toasts(SimEvent event) noexcept {
  switch (event) {
    case SimEvent::simulator_unsupported:  // Cameras stay off; no recovery without a build change.
    case SimEvent::presentation_stalled:   // Watchdog tripped; hooks released.
    case SimEvent::camera_startup_failed:  // Cameras stopped and will not retry.
    case SimEvent::hook_storm:             // Disarmed for the process; needs a simulator restart.
      return true;
    default:
      return false;
  }
}

inline constexpr const char* sim_event_name(SimEvent event) noexcept {
  constexpr const char* names[]{
      "bridge_connected", "cameras_ready",        "connection_stopped", "simulator_unsupported", "presentation_stalled",
      "cameras_disarmed", "presentation_resumed", "aircraft_mismatch",  "camera_startup_failed", "capture_paused",
      "hook_storm"};
  return static_cast<unsigned>(event) < static_cast<unsigned>(SimEvent::count) ? names[static_cast<unsigned>(event)] : "invalid_event";
}

// Per-event repeat interval plus a global budget so a flapping state cannot
// spam the desktop. Watchdog events keep a shorter interval: each trip matters.
class SimMessageLimiter {
 public:
  static constexpr std::uint64_t WindowMs = 20000;
  static constexpr unsigned WindowBudget = 4;
  static constexpr std::uint64_t interval_ms(SimEvent event) noexcept {
    switch (event) {
      case SimEvent::presentation_stalled:
      case SimEvent::cameras_disarmed:
      case SimEvent::presentation_resumed:
        return 15000;
      case SimEvent::aircraft_mismatch:
      case SimEvent::capture_paused:
        return 60000;
      default:
        return 30000;
    }
  }
  bool admit(SimEvent event, std::uint64_t now_ms) noexcept {
    const auto index = static_cast<unsigned>(event);
    if (index >= static_cast<unsigned>(SimEvent::count))
      return false;
    if (now_ms < window_start_ || now_ms - window_start_ >= WindowMs) {
      window_start_ = now_ms;
      window_count_ = 0;
    }
    auto& last = last_ms_[index];
    if (last && now_ms >= last && now_ms - last < interval_ms(event)) {
      ++suppressed_;
      return false;
    }
    if (window_count_ >= WindowBudget) {
      ++suppressed_;
      return false;
    }
    last = now_ms ? now_ms : 1;
    ++window_count_;
    ++admitted_;
    return true;
  }
  std::uint64_t admitted() const noexcept { return admitted_; }
  std::uint64_t suppressed() const noexcept { return suppressed_; }

 private:
  std::array<std::uint64_t, static_cast<unsigned>(SimEvent::count)> last_ms_{};
  std::uint64_t window_start_ = 0;
  unsigned window_count_ = 0;
  std::uint64_t admitted_ = 0, suppressed_ = 0;
};

// Bridge-side admission for one tracked event: the toast policy first, so a
// routine event neither reaches the desktop nor spends the budget meant for
// the notices that matter, then the repeat limiter.
inline bool admit_toast(SimMessageLimiter& limiter, SimEvent event, std::uint64_t now_ms) noexcept {
  return sim_event_toasts(event) && limiter.admit(event, now_ms);
}

// Edge detection from bridge state to events. Level inputs only raise their
// event when they become true; a new connection resets the per-connection ones.
struct SimEventInputs {
  bool connected = false;
  bool cameras_ready = false;
  bool connection_stopped = false;  // Pulse from the connection session.
  bool simulator_unsupported = false;
  bool degraded = false;  // Watchdog gate closed.
  bool aircraft_mismatch = false;
  bool camera_startup_failed = false;
  bool capture_paused = false;
  bool hook_storm = false;
};
class SimEventTracker {
 public:
  // Writes up to count events; returns how many were written.
  std::size_t observe(const SimEventInputs& in, SimEvent* events, std::size_t capacity) noexcept {
    std::size_t count = 0;
    const auto emit = [&](SimEvent event) {
      if (count < capacity)
        events[count++] = event;
    };
    if (in.connection_stopped)
      emit(SimEvent::connection_stopped);
    if (in.connected && !previous_.connected)
      emit(SimEvent::bridge_connected);
    // Independent of the connection, announced once for the process.
    if (in.hook_storm && !storm_announced_) {
      storm_announced_ = true;
      emit(SimEvent::hook_storm);
    }
    // The degraded gate is independent of the connection: it is applied and
    // lifted by the watchdog and must be announced exactly once per transition.
    if (in.degraded && !degraded_announced_) {
      degraded_announced_ = true;
      emit(SimEvent::cameras_disarmed);
    }
    if (!in.degraded && degraded_announced_) {
      degraded_announced_ = false;
      emit(SimEvent::presentation_resumed);
    }
    if (!in.connected) {
      // Per-connection notices repeat on the next connection.
      previous_ = {};
      previous_.degraded = in.degraded;
      return count;
    }
    if (in.cameras_ready && !previous_.cameras_ready)
      emit(SimEvent::cameras_ready);
    if (in.simulator_unsupported && !previous_.simulator_unsupported)
      emit(SimEvent::simulator_unsupported);
    if (in.aircraft_mismatch && !previous_.aircraft_mismatch)
      emit(SimEvent::aircraft_mismatch);
    if (in.camera_startup_failed && !previous_.camera_startup_failed)
      emit(SimEvent::camera_startup_failed);
    if (in.capture_paused && !previous_.capture_paused)
      emit(SimEvent::capture_paused);
    previous_ = in;
    return count;
  }

 private:
  SimEventInputs previous_{};
  bool degraded_announced_ = false;
  bool storm_announced_ = false;
};

// IPC wire form of one admitted event. Every slot is self-describing so the
// bridge can copy the log slot by slot into Status and the companion can order
// them by serial; an empty slot has serial 0.
struct NotificationSlot {
  std::uint64_t serial{}, posted_ms{};  // posted_ms: GetTickCount64 (system-wide clock).
  std::uint32_t event{}, reserved{};
};
constexpr std::size_t NotificationSlots = 16;
using NotificationLog = std::array<NotificationSlot, NotificationSlots>;

// Bridge side. Publishers are the bridge worker and the watchdog thread; the
// worker snapshots the log into the IPC status. Atomics only, no lock: a
// writer reserves a serial, blanks the slot, fills it and publishes the serial
// last; the reader rejects a slot whose serial changed while it was read.
class SimEventLog {
 public:
  void publish(SimEvent event, std::uint64_t now_ms) noexcept {
    if (static_cast<unsigned>(event) >= static_cast<unsigned>(SimEvent::count))
      return;
    const auto serial = serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto& slot = slots_[serial % NotificationSlots];
    slot.serial.store(0, std::memory_order_release);
    slot.event.store(static_cast<std::uint32_t>(event), std::memory_order_relaxed);
    slot.posted_ms.store(now_ms ? now_ms : 1, std::memory_order_relaxed);
    slot.serial.store(serial, std::memory_order_release);
  }
  void snapshot(NotificationLog& out) const noexcept {
    for (std::size_t i = 0; i < NotificationSlots; ++i) {
      const auto& slot = slots_[i];
      const auto before = slot.serial.load(std::memory_order_acquire);
      NotificationSlot copy{before, slot.posted_ms.load(std::memory_order_relaxed), slot.event.load(std::memory_order_relaxed), 0};
      const auto after = slot.serial.load(std::memory_order_acquire);
      out[i] = before && before == after ? copy : NotificationSlot{};
    }
  }
  std::uint64_t published() const noexcept { return serial_.load(std::memory_order_relaxed); }

 private:
  struct Slot {
    std::atomic<std::uint64_t> serial{}, posted_ms{};
    std::atomic<std::uint32_t> event{};
  };
  std::array<Slot, NotificationSlots> slots_{};
  std::atomic<std::uint64_t> serial_{};
};

// Companion side. Yields the events it has not seen, oldest first, and skips
// stale ones so a companion restart or a late poll does not replay history.
// A new simulator process restarts the serial: reset() when the bridge changes.
class NotificationReader {
 public:
  static constexpr std::uint64_t FreshMs = 30000;
  std::size_t take(const NotificationLog& log, std::uint64_t now_ms, SimEvent* events, std::size_t capacity) noexcept {
    std::size_t count = 0;
    for (;;) {
      const NotificationSlot* next = nullptr;
      for (const auto& slot : log)
        if (slot.serial > last_serial_ && (!next || slot.serial < next->serial))
          next = &slot;
      if (!next)
        return count;
      const bool fresh =
          now_ms >= next->posted_ms && now_ms - next->posted_ms <= FreshMs && next->event < static_cast<std::uint32_t>(SimEvent::count);
      if (fresh && count == capacity)
        return count;  // Left for the next poll.
      last_serial_ = next->serial;
      if (fresh)
        events[count++] = static_cast<SimEvent>(next->event);
    }
  }
  void reset() noexcept { last_serial_ = 0; }
  std::uint64_t last_serial() const noexcept { return last_serial_; }

 private:
  std::uint64_t last_serial_ = 0;
};

}  // namespace taxi_camera
