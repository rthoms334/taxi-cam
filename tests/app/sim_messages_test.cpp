#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "../../src/shared/sim_messages.hpp"

namespace {
using taxi_camera::NotificationLog;
using taxi_camera::NotificationReader;
using taxi_camera::NotificationSlot;
using taxi_camera::SimEvent;
using taxi_camera::SimEventInputs;
using taxi_camera::SimEventLog;
using taxi_camera::SimEventTracker;
using taxi_camera::SimMessageLimiter;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void mapping() {
  for (unsigned i = 0; i < static_cast<unsigned>(SimEvent::count); ++i) {
    const auto event = static_cast<SimEvent>(i);
    const auto message = taxi_camera::sim_message_for(event);
    require(message.text && std::strlen(message.text) >= 12 && std::strlen(message.text) < 200, "Event without display text");
    require(std::strncmp(message.text, "Taxi Cam", 8) == 0, "Message does not identify Taxi Cam");
    require(message.seconds >= 3 && message.seconds <= 15, "Display duration out of range");
    require(std::strcmp(taxi_camera::sim_event_name(event), "invalid_event") != 0, "Event without a log name");
  }
  require(taxi_camera::sim_message_for(SimEvent::count).text[0] == 0, "Invalid event produced text");
  require(taxi_camera::sim_message_for(SimEvent::presentation_stalled).alert &&
              taxi_camera::sim_message_for(SimEvent::cameras_disarmed).alert &&
              taxi_camera::sim_message_for(SimEvent::simulator_unsupported).alert,
          "Hang, degraded and unsupported notices are not alerts");
  require(!taxi_camera::sim_message_for(SimEvent::bridge_connected).alert, "Routine connect notice is an alert");
  require(std::strstr(taxi_camera::sim_message_for(SimEvent::cameras_disarmed).text, "disarmed") &&
              std::strstr(taxi_camera::sim_message_for(SimEvent::cameras_disarmed).text, "hooks"),
          "Degraded notice does not say what happened");
}

// Which events reach the desktop. One row per SimEvent so a new event has to be
// classified here; anything a pilot does not have to act on stays off the tray.
void toast_policy() {
  struct Row {
    SimEvent event;
    bool toasts;
  };
  constexpr Row rows[]{
      {SimEvent::bridge_connected, false},     {SimEvent::cameras_ready, false},       {SimEvent::connection_stopped, false},
      {SimEvent::simulator_unsupported, true}, {SimEvent::presentation_stalled, true}, {SimEvent::cameras_disarmed, false},
      {SimEvent::presentation_resumed, false}, {SimEvent::aircraft_mismatch, false},   {SimEvent::camera_startup_failed, true},
      {SimEvent::capture_paused, false},       {SimEvent::hook_storm, true},
  };
  static_assert(sizeof(rows) / sizeof(rows[0]) == static_cast<unsigned>(SimEvent::count), "Classify every SimEvent in the toast table");
  unsigned toasting = 0;
  for (unsigned i = 0; i < static_cast<unsigned>(SimEvent::count); ++i) {
    require(static_cast<unsigned>(rows[i].event) == i, "Toast table out of order");
    require(taxi_camera::sim_event_toasts(rows[i].event) == rows[i].toasts, taxi_camera::sim_event_name(rows[i].event));
    if (rows[i].toasts) {
      ++toasting;
      require(taxi_camera::sim_message_for(rows[i].event).alert, "A toasting event is not a warning");
    }
  }
  require(toasting == 4, "Toast count changed; update the docs and this table");
  require(!taxi_camera::sim_event_toasts(SimEvent::count), "Invalid event toasts");
  // The worker's echo of a disarm never toasts on its own: the watchdog posts
  // presentation_stalled and the storm posts hook_storm, one notice each.
  require(!taxi_camera::sim_event_toasts(SimEvent::cameras_disarmed) && !taxi_camera::sim_event_toasts(SimEvent::presentation_resumed),
          "Watchdog echo toasts");
  // Bridge admission: routine events are refused before the limiter and do not
  // spend the window budget; toast events still go through the repeat limiter.
  SimMessageLimiter limit;
  for (unsigned second = 0; second < 30; ++second) {
    require(!taxi_camera::admit_toast(limit, SimEvent::bridge_connected, 1000 + second * 1000), "Routine event admitted");
    require(!taxi_camera::admit_toast(limit, SimEvent::capture_paused, 1000 + second * 1000), "Routine event admitted");
    require(!taxi_camera::admit_toast(limit, SimEvent::cameras_ready, 1000 + second * 1000), "Routine event admitted");
  }
  require(limit.admitted() == 0 && limit.suppressed() == 0, "Routine events touched the limiter");
  require(taxi_camera::admit_toast(limit, SimEvent::hook_storm, 40000), "Toast event refused after routine traffic");
  require(!taxi_camera::admit_toast(limit, SimEvent::hook_storm, 41000), "Toast event repeated inside its interval");
  require(taxi_camera::admit_toast(limit, SimEvent::presentation_stalled, 41000) &&
              taxi_camera::admit_toast(limit, SimEvent::camera_startup_failed, 41000) &&
              taxi_camera::admit_toast(limit, SimEvent::simulator_unsupported, 41000),
          "Distinct toast events blocked");
  require(!taxi_camera::admit_toast(limit, SimEvent::presentation_stalled,
                                    41000 + SimMessageLimiter::interval_ms(SimEvent::presentation_stalled)),
          "Window budget not applied to toast events");
}

void limiter() {
  SimMessageLimiter limit;
  std::uint64_t now = 5000;
  require(limit.admit(SimEvent::bridge_connected, now), "First notice refused");
  require(!limit.admit(SimEvent::bridge_connected, now + 1000), "Repeated notice admitted inside its interval");
  require(limit.admit(SimEvent::cameras_ready, now + 1000), "Different event blocked by another event's interval");
  require(limit.admit(SimEvent::bridge_connected, now + SimMessageLimiter::interval_ms(SimEvent::bridge_connected)),
          "Notice refused after its interval");
  // Flapping: connect/stop every second must be capped by the window budget.
  SimMessageLimiter flap;
  unsigned admitted = 0;
  for (unsigned second = 0; second < 20; ++second) {
    admitted += flap.admit(second % 2 ? SimEvent::connection_stopped : SimEvent::bridge_connected, 100000 + second * 1000);
    admitted += flap.admit(SimEvent::cameras_ready, 100000 + second * 1000);
    admitted += flap.admit(SimEvent::capture_paused, 100000 + second * 1000);
  }
  require(admitted <= SimMessageLimiter::WindowBudget, "Flapping state exceeded the window budget");
  require(flap.suppressed() > 0 && flap.admitted() == admitted, "Limiter statistics disagree with its decisions");
  // The next window admits again.
  require(flap.admit(SimEvent::aircraft_mismatch, 100000 + SimMessageLimiter::WindowMs + 61000), "Fresh window refused a notice");
  // Watchdog events keep a shorter interval than routine ones and are never starved by them.
  SimMessageLimiter dog;
  require(dog.admit(SimEvent::presentation_stalled, 1000) && !dog.admit(SimEvent::presentation_stalled, 5000) &&
              dog.admit(SimEvent::presentation_stalled, 1000 + SimMessageLimiter::interval_ms(SimEvent::presentation_stalled)),
          "Watchdog notice interval not enforced");
  require(SimMessageLimiter::interval_ms(SimEvent::presentation_stalled) < SimMessageLimiter::interval_ms(SimEvent::capture_paused),
          "Watchdog notices are rate-limited harder than routine ones");
  require(!dog.admit(SimEvent::count, 1), "Invalid event admitted");
}

std::array<SimEvent, 8> events{};
std::size_t observe(SimEventTracker& tracker, const SimEventInputs& in) {
  events = {};
  return tracker.observe(in, events.data(), events.size());
}
bool has(std::size_t count, SimEvent event) {
  for (std::size_t i = 0; i < count; ++i)
    if (events[i] == event)
      return true;
  return false;
}

void tracker() {
  SimEventTracker track;
  SimEventInputs in;
  require(observe(track, in) == 0, "Idle bridge produced events");
  in.connected = true;
  auto count = observe(track, in);
  require(count == 1 && has(count, SimEvent::bridge_connected), "Connect edge not announced once");
  require(observe(track, in) == 0, "Steady connection repeated its notice");
  in.cameras_ready = true;
  in.capture_paused = true;
  count = observe(track, in);
  require(count == 2 && has(count, SimEvent::cameras_ready) && has(count, SimEvent::capture_paused), "Level edges not announced");
  require(observe(track, in) == 0, "Level inputs repeated their notices");
  in.capture_paused = false;
  require(observe(track, in) == 0, "Clearing a level produced a notice");
  in.capture_paused = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::capture_paused), "Re-asserted level not announced");
  // Watchdog degraded path: announced once while connected, resumed once when lifted.
  in.degraded = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::cameras_disarmed), "Degraded edge not announced");
  require(observe(track, in) == 0, "Degraded level repeated");
  in.degraded = false;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::presentation_resumed), "Recovery not announced");
  // Disconnect: stop pulse announced; per-connection notices repeat on reconnect.
  in.connected = false;
  in.connection_stopped = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::connection_stopped), "Connection stop not announced");
  in.connection_stopped = false;
  require(observe(track, in) == 0, "Disconnected idle produced events");
  // Degraded while disconnected still announces: the gate is independent of the companion.
  in.degraded = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::cameras_disarmed), "Degraded while disconnected not announced");
  in.degraded = false;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::presentation_resumed), "Recovery while disconnected not announced");
  in.connected = true;
  count = observe(track, in);
  require(
      count == 3 && has(count, SimEvent::bridge_connected) && has(count, SimEvent::cameras_ready) && has(count, SimEvent::capture_paused),
      "Reconnect did not repeat per-connection notices");
  in.simulator_unsupported = true;
  in.camera_startup_failed = true;
  in.aircraft_mismatch = true;
  count = observe(track, in);
  require(count == 3 && has(count, SimEvent::simulator_unsupported) && has(count, SimEvent::camera_startup_failed) &&
              has(count, SimEvent::aircraft_mismatch),
          "Fault levels not announced");
  // A failure storm is announced once for the process, even across reconnects.
  in.hook_storm = true;
  count = observe(track, in);
  require(count == 1 && has(count, SimEvent::hook_storm), "Hook storm not announced");
  require(observe(track, in) == 0, "Hook storm repeated");
  in.connected = false;
  observe(track, in);
  in.connected = true;
  count = observe(track, in);
  require(!has(count, SimEvent::hook_storm) && has(count, SimEvent::bridge_connected), "Hook storm repeated after reconnect");
  // Bounded output: a small buffer truncates instead of overflowing.
  SimEventTracker small;
  SimEventInputs burst;
  burst.connected = burst.cameras_ready = burst.capture_paused = burst.aircraft_mismatch = true;
  std::array<SimEvent, 2> two{};
  require(small.observe(burst, two.data(), two.size()) == 2, "Tracker overflowed its output buffer");
}

// Bridge log -> IPC status -> companion reader: order, no replay, freshness.
void notifications() {
  static_assert(sizeof(NotificationSlot) == 24 && sizeof(NotificationLog) == 24 * taxi_camera::NotificationSlots,
                "Notification wire layout changed; bump the IPC protocol");
  SimEventLog log;
  NotificationLog wire{};
  NotificationReader reader;
  std::array<SimEvent, 8> out{};
  log.snapshot(wire);
  require(reader.take(wire, 1000, out.data(), out.size()) == 0 && reader.last_serial() == 0, "Empty log produced events");
  log.publish(SimEvent::bridge_connected, 1000);
  log.publish(SimEvent::cameras_ready, 1200);
  log.publish(SimEvent::aircraft_mismatch, 1300);
  require(log.published() == 3, "Publish count");
  log.snapshot(wire);
  auto count = reader.take(wire, 1500, out.data(), out.size());
  require(count == 3 && out[0] == SimEvent::bridge_connected && out[1] == SimEvent::cameras_ready && out[2] == SimEvent::aircraft_mismatch,
          "Events not delivered oldest first");
  require(reader.take(wire, 1600, out.data(), out.size()) == 0, "Unchanged log replayed its events");
  // The slot layout does not matter to the reader; only serials do.
  NotificationLog shuffled{};
  std::size_t next = 0;
  for (std::size_t i = wire.size(); i-- > 0;)
    shuffled[next++] = wire[i];
  require(reader.take(shuffled, 1600, out.data(), out.size()) == 0, "Reordered copy of seen events replayed");
  // A companion (re)start sees history: only recent events are shown, but all are marked seen.
  NotificationReader restarted;
  log.publish(SimEvent::presentation_stalled, 5000);
  log.snapshot(wire);
  count = restarted.take(wire, 5000 + NotificationReader::FreshMs, out.data(), out.size());
  require(count == 1 && out[0] == SimEvent::presentation_stalled && restarted.last_serial() == 4, "Stale events shown after restart");
  require(reader.take(wire, 5100, out.data(), out.size()) == 1 && out[0] == SimEvent::presentation_stalled,
          "Live reader missed the fourth event");
  // Ring wrap: more events than slots since the last poll keeps the newest, in order.
  constexpr auto slots = taxi_camera::NotificationSlots;
  for (unsigned i = 0; i < slots + 4; ++i)
    log.publish(static_cast<SimEvent>(i % static_cast<unsigned>(SimEvent::count)), 6000 + i);
  log.snapshot(wire);
  std::array<SimEvent, slots + 8> many{};
  count = reader.take(wire, 7000, many.data(), many.size());
  require(count == slots && reader.last_serial() == 4 + slots + 4, "Wrapped log not consumed to its newest serial");
  for (std::size_t i = 1; i < count; ++i)
    require(static_cast<unsigned>(many[i]) == (static_cast<unsigned>(many[i - 1]) + 1) % static_cast<unsigned>(SimEvent::count),
            "Wrapped events out of order");
  // Output capacity: unread fresh events stay for the next poll.
  NotificationReader slow;
  std::array<SimEvent, 3> three{};
  count = slow.take(wire, 7000, three.data(), three.size());
  require(count == 3 && slow.last_serial() == 4 + 3 + 4, "Capacity-limited take advanced past unread events");
  count = slow.take(wire, 7000, many.data(), many.size());
  require(count == slots - 3 && slow.last_serial() == reader.last_serial(), "Second take did not resume where the first stopped");
  // Invalid events on the wire are skipped, not shown.
  NotificationLog bad{};
  bad[0] = {1, 100, static_cast<std::uint32_t>(SimEvent::count), 0};
  bad[1] = {2, 100, static_cast<std::uint32_t>(SimEvent::cameras_ready), 0};
  NotificationReader strict;
  count = strict.take(bad, 200, out.data(), out.size());
  require(count == 1 && out[0] == SimEvent::cameras_ready && strict.last_serial() == 2, "Invalid wire event delivered");
  require(!static_cast<bool>(SimEventLog{}.published()), "Fresh log has a serial");
  // A new simulator process starts at serial 1 again.
  reader.reset();
  SimEventLog fresh_process;
  fresh_process.publish(SimEvent::bridge_connected, 9000);
  fresh_process.snapshot(wire);
  require(reader.take(wire, 9001, out.data(), out.size()) == 1 && out[0] == SimEvent::bridge_connected,
          "Reset reader ignored a new process's first event");
}
}  // namespace

int main() {
  try {
    mapping();
    toast_policy();
    limiter();
    tracker();
    notifications();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "sim messages: %s\n", error.what());
    return 1;
  }
  std::printf("sim messages: %u checks passed\n", checks);
  return 0;
}
