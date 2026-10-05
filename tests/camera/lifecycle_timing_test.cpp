#include "../../src/camera/lifecycle_timing.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
namespace nc = taxi_camera::native_camera;
using nc::LifecycleTimer;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
constexpr std::size_t index(LifecycleTimer timer) {
  return static_cast<std::size_t>(timer);
}
constexpr std::array<const char*, nc::kLifecycleStageCount> StageNames{
    "manager", "pool", "lifecycle", "entries", "view1", "view2", "handoff", "pose", "activation", "publication", "aa", "placement"};

void timers() {
  nc::LifecycleTimers timers;
  timers.record(LifecycleTimer::native_create, 40);
  timers.record(LifecycleTimer::native_create, 10);
  timers.record(LifecycleTimer::native_create, 25);
  timers.record(LifecycleTimer::native_activate, -5);  // A clock step back is not a call.
  require(timers.ticks[index(LifecycleTimer::native_create)] == 75 && timers.max_ticks[index(LifecycleTimer::native_create)] == 40 &&
              timers.calls[index(LifecycleTimer::native_create)] == 3,
          "Native create calls were not summed with their slowest call");
  require(timers.calls[index(LifecycleTimer::native_activate)] == 0 && timers.native_ticks == 75, "A negative interval was recorded");

  // A lifecycle section of 125 ticks that began before all 105 native ticks
  // leaves 20 ticks of proofs; natives before a section are not taken off it.
  const auto before = timers.native_ticks;
  timers.record(LifecycleTimer::native_output, 30);
  timers.record_lifecycle(125, 0);
  require(timers.ticks[index(LifecycleTimer::proofs)] == 20 && timers.calls[index(LifecycleTimer::proofs)] == 1,
          "Proofs did not exclude the section's native calls");
  timers.record_lifecycle(10, before);  // A section shorter than the natives it contains clamps at zero.
  require(timers.ticks[index(LifecycleTimer::proofs)] == 20 && timers.calls[index(LifecycleTimer::proofs)] == 2,
          "A short lifecycle section produced negative proofs");
  timers.record(LifecycleTimer::native_mount, 7);
  timers.record_lifecycle(50, timers.native_ticks);
  require(timers.ticks[index(LifecycleTimer::proofs)] == 70 && timers.native_ticks == 112, "Proofs counted as native time");

  nc::LifecycleEvent event;
  nc::copy_lifecycle_timers(timers, 0.5, event);
  require(event.timer_ms[index(LifecycleTimer::native_create)] == 37.5 && event.timer_max_ms[index(LifecycleTimer::native_create)] == 20 &&
              event.timer_calls[index(LifecycleTimer::native_create)] == 3 && event.timer_ms[index(LifecycleTimer::proofs)] == 35,
          "Timer ticks were not converted to milliseconds");
  require(timers.untimed_ticks == 70 && event.untimed_ms == 35, "Sections without memory calls were not wholly untimed");
  timers = {};
  require(timers.native_ticks == 0 && timers.untimed_ticks == 0 && timers.calls == std::array<std::uint32_t, nc::kLifecycleTimerCount>{},
          "Per-update reset kept values");
}

// Untimed is each section's proofs less the query, read and write ticks
// recorded inside it, never below zero and never taking native time twice.
void untimed() {
  nc::LifecycleTimers timers;
  timers.record(LifecycleTimer::native_resize_projection, 5);
  timers.record_lifecycle(100, 0, 30);  // 95 proofs, 30 of them timed memory calls.
  require(timers.ticks[index(LifecycleTimer::proofs)] == 95 && timers.untimed_ticks == 65, "Timed memory calls were not taken off proofs");
  timers.record_lifecycle(40, timers.native_ticks, 60);  // More memory than proofs clamps at zero.
  require(timers.ticks[index(LifecycleTimer::proofs)] == 135 && timers.untimed_ticks == 65, "Untimed ticks went negative");
  timers.record_lifecycle(20, timers.native_ticks, -8);  // A negative memory delta counts as none.
  require(timers.untimed_ticks == 85, "A negative memory delta added untimed ticks");
  nc::LifecycleEvent event;
  nc::copy_lifecycle_timers(timers, 0.1, event);
  require(event.untimed_ms > 8.49 && event.untimed_ms < 8.51 && event.timer_ms[index(LifecycleTimer::proofs)] > 15.49,
          "Untimed ticks were not converted to milliseconds");
}

// cpu_ms is wall time scaled by cycles over time-stamp ticks; unusable
// samples are negative, so the line leaves cpu_ms out.
void thread_cpu() {
  const nc::ThreadCpuSample start{1000, 5000};
  require(nc::thread_cpu_ms(start, {1500, 6000}, 8.0) == 4.0, "Half the cycles of the interval were not half its wall time");
  require(nc::thread_cpu_ms(start, {2000, 6000}, 8.0) == 8.0, "A fully running thread did not match its wall time");
  require(nc::thread_cpu_ms({0, 5000}, {1500, 6000}, 8.0) < 0 && nc::thread_cpu_ms(start, {0, 6000}, 8.0) < 0 &&
              nc::thread_cpu_ms(start, {900, 6000}, 8.0) < 0 && nc::thread_cpu_ms(start, {1500, 5000}, 8.0) < 0 &&
              nc::thread_cpu_ms(start, {1500, 6000}, 0) < 0,
          "An unusable CPU sample produced a CPU time");
  // This thread, measured: a busy interval runs most of its wall time and a
  // sleeping one almost none of it.
  const auto measure = [](bool busy) {
    LARGE_INTEGER frequency{}, begin{}, end{};
    QueryPerformanceFrequency(&frequency);
    const auto first = nc::sample_thread_cpu();
    QueryPerformanceCounter(&begin);
    if (busy) {
      volatile std::uint64_t sink = 0;
      do {
        for (unsigned i = 0; i < 1000; ++i)
          sink = sink + i;
        QueryPerformanceCounter(&end);
      } while ((end.QuadPart - begin.QuadPart) * 1000 < frequency.QuadPart * 30);
    } else {
      Sleep(40);
      QueryPerformanceCounter(&end);
    }
    const auto last = nc::sample_thread_cpu();
    const double wall = static_cast<double>(end.QuadPart - begin.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    return std::array<double, 2>{nc::thread_cpu_ms(first, last, wall), wall};
  };
  const auto busy = measure(true);
  require(busy[0] > busy[1] * 0.3 && busy[0] < busy[1] * 1.1, "A busy interval's CPU time was far from its wall time");
  const auto idle = measure(false);
  require(idle[0] >= 0 && idle[0] < idle[1] * 0.5, "A sleeping interval reported most of its wall time as CPU");
}

nc::LifecycleEvent numbered(std::uint64_t update) {
  nc::LifecycleEvent event;
  event.update = update;
  event.total_ms = static_cast<double>(update);
  for (std::size_t i = 0; i < event.stage_ms.size(); ++i)
    event.stage_ms[i] = static_cast<double>(update + i);
  for (std::size_t i = 0; i < event.timer_ms.size(); ++i)
    event.timer_ms[i] = static_cast<double>(update * 2 + i);
  event.reads = update * 3;
  return event;
}
bool intact(const nc::LifecycleEvent& event) {
  bool ok = event.total_ms == static_cast<double>(event.update) && event.reads == event.update * 3;
  for (std::size_t i = 0; i < event.stage_ms.size(); ++i)
    ok = ok && event.stage_ms[i] == static_cast<double>(event.update + i);
  for (std::size_t i = 0; i < event.timer_ms.size(); ++i)
    ok = ok && event.timer_ms[i] == static_cast<double>(event.update * 2 + i);
  return ok;
}

void event_log() {
  static nc::LifecycleEventLog log;
  static std::array<nc::LifecycleEvent, nc::kLifecycleEventCapacity> taken;
  std::uint64_t dropped = 99;
  require(log.take(taken, dropped) == 0 && dropped == 0, "An empty ring returned events");
  for (std::uint64_t update = 1; update <= 20; ++update)
    log.push(numbered(update));
  auto count = log.take(taken, dropped);
  require(count == nc::kLifecycleEventCapacity && dropped == 4, "A full ring did not keep 16 events and count four drops");
  for (std::size_t i = 0; i < count; ++i)
    require(taken[i].update == i + 1 && intact(taken[i]), "The ring did not keep the oldest events in order");
  require(log.take(taken, dropped) == 0 && dropped == 0, "A drained ring repeated events or drops");
  // Reuse across the wrap: batches of ten, drained between, lose nothing.
  std::uint64_t next = 100, expected = 100;
  for (unsigned batch = 0; batch < 5; ++batch) {
    for (unsigned i = 0; i < 10; ++i)
      log.push(numbered(next++));
    count = log.take(taken, dropped);
    require(count == 10 && dropped == 0, "A part-filled ring dropped events");
    for (std::size_t i = 0; i < count; ++i)
      require(taken[i].update == expected++ && intact(taken[i]), "Wrapped ring events out of order or torn");
  }
}

// The observer pushes while the worker takes: every taken event is whole and
// in order, and taken plus dropped accounts for every push. No lock is used.
void concurrent_event_log() {
  static nc::LifecycleEventLog log;
  constexpr std::uint64_t Pushes = 200000;
  std::atomic<bool> done{false};
  std::thread producer([&] {
    for (std::uint64_t update = 1; update <= Pushes; ++update)
      log.push(numbered(update));
    done.store(true, std::memory_order_release);
  });
  static std::array<nc::LifecycleEvent, nc::kLifecycleEventCapacity> taken;
  std::uint64_t received = 0, drops = 0, last = 0;
  bool ordered = true, whole = true;
  for (;;) {
    const bool finished = done.load(std::memory_order_acquire);
    std::uint64_t dropped = 0;
    const auto count = log.take(taken, dropped);
    drops += dropped;
    for (std::size_t i = 0; i < count; ++i) {
      ordered = ordered && taken[i].update > last;
      whole = whole && intact(taken[i]);
      last = taken[i].update;
    }
    received += count;
    if (finished && !count && !dropped)
      break;
  }
  producer.join();
  require(ordered && whole, "A concurrently taken event was torn or out of order");
  require(received + drops == Pushes && received > 0, "Concurrent pushes were neither taken nor counted as drops");
}

void formatting() {
  nc::LifecycleEvent event;
  event.update = 7;
  event.tick_ms = 258885781;
  event.total_ms = 14.68;
  event.pre_ms = 0.01;
  event.serviced = true;
  event.feeds = 3;
  event.stage_ms[2] = 13.63;
  event.timer_ms[index(LifecycleTimer::native_create)] = 9.5;
  event.timer_calls[index(LifecycleTimer::native_create)] = 3;
  event.timer_max_ms[index(LifecycleTimer::native_create)] = 4.25;
  event.timer_ms[index(LifecycleTimer::proofs)] = 1.3;
  event.timer_calls[index(LifecycleTimer::proofs)] = 1;
  event.timer_max_ms[index(LifecycleTimer::proofs)] = 1.3;
  event.queries = 351;
  event.reads = 1360;
  event.query_ms = 1.47;
  char line[1024];
  nc::format_lifecycle_event(line, sizeof(line), event, StageNames);
  const char* header = "Lifecycle event: update=7 tick=258885781 total_ms=14.68 pre_ms=0.01 serviced=1 feeds=3 manager=0.00 ";
  require(std::strncmp(line, header, std::strlen(header)) == 0, "Lifecycle event header");
  require(std::strstr(line, " lifecycle=13.63 ") && std::strstr(line, " native_create=9.500/3/4.250 ") &&
              std::strstr(line, " native_initialize=0.000/0/0.000 ") && std::strstr(line, " proofs=1.300/1/1.300 ") &&
              std::strstr(line, " placement=0.00 native_initialize="),
          "Lifecycle event stages or sub-timers");
  // Not measured: no cpu_ms.
  const char* tail = " query_ms=1.47 read_ms=0.00 queries=351 reads=1360 write_ms=0.00 writes=0 untimed_ms=0.00";
  const auto length = std::strlen(line), tail_length = std::strlen(tail);
  require(length < 700 && length > tail_length && std::strcmp(line + length - tail_length, tail) == 0, "Lifecycle event memory totals");
  event.write_ms = 0.42;
  event.writes = 4;
  event.untimed_ms = 7.35;
  event.cpu_ms = 14.5;
  nc::format_lifecycle_event(line, sizeof(line), event, StageNames);
  const char* measured = " reads=1360 write_ms=0.42 writes=4 untimed_ms=7.35 cpu_ms=14.50";
  require(std::strlen(line) < 700 && std::strlen(line) > std::strlen(measured) &&
              std::strcmp(line + std::strlen(line) - std::strlen(measured), measured) == 0,
          "Lifecycle event writes, untimed or CPU time");
  event.serviced = false;  // An unserviced update has no CPU time.
  nc::format_lifecycle_event(line, sizeof(line), event, StageNames);
  require(!std::strstr(line, "cpu_ms") && std::strstr(line, " untimed_ms=7.35"), "An unserviced update reported CPU time");
  event.serviced = true;
  // A short buffer truncates in place and stays terminated.
  for (const std::size_t size : {1u, 20u, 120u, 300u}) {
    std::array<char, 400> buffer;
    buffer.fill('#');
    nc::format_lifecycle_event(buffer.data(), size, event, StageNames);
    require(buffer[size] == '#' && std::strlen(buffer.data()) == size - 1, "Truncated line overran or was not terminated");
  }
}
}  // namespace

int main() {
  try {
    timers();
    untimed();
    thread_cpu();
    event_log();
    concurrent_event_log();
    formatting();
    std::printf("PASS: %u lifecycle timing checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
