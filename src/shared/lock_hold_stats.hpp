#pragma once
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstdint>
#include "lock_holder_priority.hpp"

namespace taxi_camera {

// How long each call site held a lock, for the bridge log. record() is
// lock-free and runs after the lock is released; the worker drains one
// interval at a time. A site is a source line or an acquiring return address.
// Sites beyond Slots share the overflow slot (site 1).
class LockHoldStats {
 public:
  static constexpr unsigned Slots = 64;
  static constexpr std::uintptr_t OverflowSite = 1;
  struct Entry {
    std::uintptr_t site = 0;
    std::uint64_t count = 0, ticks = 0, max_ticks = 0;
  };

  void record(std::uintptr_t site, std::uint64_t ticks) noexcept {
    if (site <= OverflowSite)
      site = OverflowSite;
    auto index = static_cast<unsigned>((site * 0x9e3779b97f4a7c15ull) >> 58) % Slots;
    for (unsigned probe = 0; probe < Slots; ++probe, index = (index + 1) % Slots) {
      auto& slot = slots_[index];
      auto seen = slot.site.load(std::memory_order_relaxed);
      if (!seen && slot.site.compare_exchange_strong(seen, site, std::memory_order_relaxed))
        seen = site;
      if (seen == site) {
        add(slot, ticks);
        return;
      }
    }
    add(overflow_, ticks);
  }
  // Worker only. Copies the interval's nonzero sites and clears their totals.
  unsigned drain(Entry* out, unsigned capacity) noexcept {
    unsigned n = 0;
    const auto take = [&](Slot& slot, std::uintptr_t site) {
      const auto count = slot.count.exchange(0, std::memory_order_relaxed);
      const auto ticks = slot.ticks.exchange(0, std::memory_order_relaxed);
      const auto max_ticks = slot.max_ticks.exchange(0, std::memory_order_relaxed);
      if (count && n < capacity)
        out[n++] = {site, count, ticks, max_ticks};
    };
    for (auto& slot : slots_)
      if (const auto site = slot.site.load(std::memory_order_relaxed))
        take(slot, site);
    take(overflow_, OverflowSite);
    return n;
  }

 private:
  struct alignas(64) Slot {
    std::atomic<std::uintptr_t> site{};
    std::atomic<std::uint64_t> count{}, ticks{}, max_ticks{};
  };
  static void add(Slot& slot, std::uint64_t ticks) noexcept {
    slot.count.fetch_add(1, std::memory_order_relaxed);
    slot.ticks.fetch_add(ticks, std::memory_order_relaxed);
    auto seen = slot.max_ticks.load(std::memory_order_relaxed);
    while (ticks > seen && !slot.max_ticks.compare_exchange_weak(seen, ticks, std::memory_order_relaxed)) {
    }
  }
  std::array<Slot, Slots> slots_{};
  Slot overflow_{};
};

// Records one hold of a lock the caller already owns. Declare it after the
// lock so it is destroyed, and records, just before the lock is released.
class HoldTimer {
 public:
  HoldTimer(LockHoldStats& stats, std::uintptr_t site, bool held) noexcept : stats_(stats), site_(site), held_(held), start_(__rdtsc()) {}
  ~HoldTimer() {
    if (held_)
      stats_.record(site_, __rdtsc() - start_);
  }
  HoldTimer(const HoldTimer&) = delete;
  HoldTimer& operator=(const HoldTimer&) = delete;

 private:
  LockHoldStats& stats_;
  std::uintptr_t site_;
  bool held_;
  std::uint64_t start_;
};

// A mutex that reports each hold to LockHoldStats, keyed by the address that
// acquired it (or a site the holder sets afterwards). Hold state belongs to
// the owner, so it needs no synchronization of its own. Not recursive.
// A bridge worker thread holding it runs above normal (lock_holder_priority).
template <class Mutex>
class TimedMutex {
 public:
  explicit TimedMutex(LockHoldStats& stats) noexcept : stats_(stats) {}
  TimedMutex(const TimedMutex&) = delete;
  TimedMutex& operator=(const TimedMutex&) = delete;
  __attribute__((noinline)) void lock() {
    lock_holder_priority::enter();
    mutex_.lock();
    acquired(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  // Raised only once acquired: a bounded wait spins on try_lock, and a failed
  // attempt must not cost a priority system call.
  __attribute__((noinline)) bool try_lock() {
    if (!mutex_.try_lock())
      return false;
    lock_holder_priority::enter();
    acquired(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    return true;
  }
  void unlock() {
    const auto held = __rdtsc() - start_;
    const auto site = site_;
    mutex_.unlock();
    lock_holder_priority::leave();
    stats_.record(site, held);
  }
  // Owner only: attribute the current hold to a more useful site.
  void attribute(std::uintptr_t site) noexcept { site_ = site; }

 private:
  void acquired(std::uintptr_t site) noexcept {
    site_ = site;
    start_ = __rdtsc();
  }
  Mutex mutex_;
  LockHoldStats& stats_;
  std::uint64_t start_ = 0;
  std::uintptr_t site_ = 0;
};

}  // namespace taxi_camera
