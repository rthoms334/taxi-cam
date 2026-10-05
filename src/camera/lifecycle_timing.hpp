#pragma once

#include "../shared/bounded_lock.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace taxi_camera::native_camera {

// Lifecycle sub-timers of one camera-manager update (simulator main thread).
// Each native_* kind times the engine calls named after it wherever the
// observer makes them; proofs is the rest of the lifecycle stage: memory
// proofs and rereads, the AA and clip writes, and the native erase.
enum class LifecycleTimer : std::size_t {
  native_initialize,         // initialize_descriptor
  native_create,             // create_entry
  native_activate,           // activate_entry, opening or closing a gate
  native_pose,               // apply_pose: the four setters and update_view, one call
  native_resize_projection,  // the closed-gate resize's update_view
  native_output,             // the closed-gate resize's output routine
  native_mount,              // attach_child and detach_node, each a call
  proofs,
  count
};
inline constexpr std::size_t kLifecycleTimerCount = static_cast<std::size_t>(LifecycleTimer::count);
inline constexpr std::array<const char*, kLifecycleTimerCount> kLifecycleTimerNames{
    "native_initialize",        "native_create", "native_activate", "native_pose",
    "native_resize_projection", "native_output", "native_mount",    "proofs"};

// QPC ticks, calls and the slowest call per kind, reset at the start of every
// update. Observer thread only: plain adds, no lock, allocation or formatting.
struct LifecycleTimers {
  std::array<std::int64_t, kLifecycleTimerCount> ticks{}, max_ticks{};
  std::array<std::uint32_t, kLifecycleTimerCount> calls{};
  // Every native_* tick so far in this update. A lifecycle stage section
  // subtracts what was recorded inside it to leave its proofs.
  std::int64_t native_ticks = 0;

  void record(LifecycleTimer timer, std::int64_t elapsed) noexcept {
    const auto index = static_cast<std::size_t>(timer);
    if (elapsed < 0 || index >= kLifecycleTimerCount)
      return;
    ticks[index] += elapsed;
    max_ticks[index] = std::max(max_ticks[index], elapsed);
    ++calls[index];
    if (timer != LifecycleTimer::proofs)
      native_ticks += elapsed;
  }
  // One lifecycle stage section of elapsed ticks; native_before is
  // native_ticks when it began.
  void record_lifecycle(std::int64_t elapsed, std::int64_t native_before) noexcept {
    record(LifecycleTimer::proofs, std::max<std::int64_t>(0, elapsed - (native_ticks - native_before)));
  }
};

// Stages of ProbePerformance (ProbeStage::count, asserted in probe.hpp).
inline constexpr std::size_t kLifecycleStageCount = 12;
// Updates longer than this, in total, are recorded.
inline constexpr double kLifecycleEventMs = 2.0;
inline constexpr unsigned kLifecycleEventCapacity = 16;

// One camera-manager update over kLifecycleEventMs, as plain values. Stage and
// memory-call totals are those of a serviced update (zero otherwise).
struct LifecycleEvent {
  std::uint64_t update = 0;
  std::uint64_t tick_ms = 0;
  double total_ms = 0, pre_ms = 0;
  bool serviced = false;
  unsigned feeds = 0;
  std::array<double, kLifecycleStageCount> stage_ms{};
  std::array<double, kLifecycleTimerCount> timer_ms{}, timer_max_ms{};
  std::array<std::uint32_t, kLifecycleTimerCount> timer_calls{};
  double query_ms = 0, read_ms = 0;
  std::uint64_t queries = 0, reads = 0;
};

inline void copy_lifecycle_timers(const LifecycleTimers& timers, double milliseconds_per_tick, LifecycleEvent& event) noexcept {
  for (std::size_t i = 0; i < kLifecycleTimerCount; ++i) {
    event.timer_ms[i] = static_cast<double>(timers.ticks[i]) * milliseconds_per_tick;
    event.timer_max_ms[i] = static_cast<double>(timers.max_ticks[i]) * milliseconds_per_tick;
    event.timer_calls[i] = timers.calls[i];
  }
}

// Fixed ring between the observer, its only producer, and the bridge worker,
// its only consumer. No lock: DeferredRing publishes each slot with a release
// store of its ready ticket that pop() acquires, and only the consumer commits
// (frees) a slot, so with one consumer neither side waits for the other. A full
// ring counts a drop instead of allocating.
class LifecycleEventLog {
 public:
  void push(const LifecycleEvent& event) noexcept {
    if (!ring_.push(event))
      drops_.fetch_add(1, std::memory_order_relaxed);
  }
  // Consumer only: the queued events, oldest first, and the drops since the
  // previous take.
  std::size_t take(std::array<LifecycleEvent, kLifecycleEventCapacity>& events, std::uint64_t& dropped) noexcept {
    std::size_t count = 0;
    while (count < events.size() && ring_.pop(events[count]))
      ++count;
    ring_.take_overflow();
    dropped = drops_.exchange(0, std::memory_order_relaxed);
    return count;
  }

 private:
  DeferredRing<LifecycleEvent, kLifecycleEventCapacity> ring_;
  std::atomic<std::uint64_t> drops_{0};
};

// One bridge.log line: 'Lifecycle event: update=N tick=ms total_ms= pre_ms=
// serviced= feeds=', each stage as name=ms, each sub-timer as
// name=ms/calls/slowest-call-ms, then the update's memory-call totals.
// Truncates rather than overflowing out.
inline void format_lifecycle_event(char* out,
                                   std::size_t size,
                                   const LifecycleEvent& event,
                                   const std::array<const char*, kLifecycleStageCount>& stage_names) noexcept {
  if (!out || !size)
    return;
  std::size_t used = 0;
  const auto advance = [&](int written) {
    used = written < 0 || static_cast<std::size_t>(written) >= size - used ? size : used + static_cast<std::size_t>(written);
  };
  advance(std::snprintf(out, size, "Lifecycle event: update=%llu tick=%llu total_ms=%.2f pre_ms=%.2f serviced=%u feeds=%u",
                        static_cast<unsigned long long>(event.update), static_cast<unsigned long long>(event.tick_ms), event.total_ms,
                        event.pre_ms, event.serviced ? 1u : 0u, event.feeds));
  for (std::size_t i = 0; i < kLifecycleStageCount && used < size; ++i)
    advance(std::snprintf(out + used, size - used, " %s=%.2f", stage_names[i], event.stage_ms[i]));
  for (std::size_t i = 0; i < kLifecycleTimerCount && used < size; ++i)
    advance(std::snprintf(out + used, size - used, " %s=%.3f/%u/%.3f", kLifecycleTimerNames[i], event.timer_ms[i],
                          static_cast<unsigned>(event.timer_calls[i]), event.timer_max_ms[i]));
  if (used < size)
    advance(std::snprintf(out + used, size - used, " query_ms=%.2f read_ms=%.2f queries=%llu reads=%llu", event.query_ms, event.read_ms,
                          static_cast<unsigned long long>(event.queries), static_cast<unsigned long long>(event.reads)));
}

}  // namespace taxi_camera::native_camera
