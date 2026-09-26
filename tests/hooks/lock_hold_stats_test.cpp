#include "../../src/shared/lock_hold_stats.hpp"

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
const taxi_camera::LockHoldStats::Entry* find(const std::vector<taxi_camera::LockHoldStats::Entry>& entries, std::uintptr_t site) {
  for (const auto& e : entries)
    if (e.site == site)
      return &e;
  return nullptr;
}
std::vector<taxi_camera::LockHoldStats::Entry> drain(taxi_camera::LockHoldStats& stats) {
  std::vector<taxi_camera::LockHoldStats::Entry> entries(taxi_camera::LockHoldStats::Slots + 1);
  entries.resize(stats.drain(entries.data(), static_cast<unsigned>(entries.size())));
  return entries;
}
}  // namespace

int main() {
  using taxi_camera::LockHoldStats;
  bool ok = true;
  LockHoldStats stats;
  stats.record(1058, 100);
  stats.record(1058, 300);
  stats.record(2643, 50);
  auto entries = drain(stats);
  const auto* a = find(entries, 1058);
  const auto* b = find(entries, 2643);
  ok &= require(entries.size() == 2 && a && b, "Two sites drained");
  ok &= require(a && a->count == 2 && a->ticks == 400 && a->max_ticks == 300, "Count, total and longest per site");
  ok &= require(b && b->count == 1 && b->ticks == 50 && b->max_ticks == 50, "Second site kept separately");
  ok &= require(drain(stats).empty(), "Drain clears the interval but keeps no stale totals");
  stats.record(1058, 7);
  entries = drain(stats);
  ok &= require(entries.size() == 1 && entries[0].site == 1058 && entries[0].max_ticks == 7, "A drained site is reused");

  // More sites than slots share the overflow site; nothing is lost.
  LockHoldStats full;
  for (std::uintptr_t site = 100; site < 100 + LockHoldStats::Slots + 10; ++site)
    full.record(site, 1);
  entries = drain(full);
  std::uint64_t total = 0;
  for (const auto& e : entries)
    total += e.count;
  ok &= require(total == LockHoldStats::Slots + 10 && find(entries, LockHoldStats::OverflowSite), "Overflow keeps every hold");

  // Concurrent recorders: exact totals and the longest hold.
  LockHoldStats shared;
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < 8; ++t)
    threads.emplace_back([&, t] {
      for (unsigned i = 0; i < 100000; ++i)
        shared.record(10 + (i % 4), t == 3 && i == 777 ? 5000 : 2);
    });
  for (auto& thread : threads)
    thread.join();
  entries = drain(shared);
  std::uint64_t count = 0, max_ticks = 0;
  for (const auto& e : entries) {
    count += e.count;
    max_ticks = e.max_ticks > max_ticks ? e.max_ticks : max_ticks;
  }
  ok &= require(entries.size() == 4 && count == 800000 && max_ticks == 5000, "Concurrent holds are counted exactly");

  // TimedMutex reports each hold, attributed to the site the owner chose.
  LockHoldStats timed_stats;
  taxi_camera::TimedMutex<std::mutex> mutex(timed_stats);
  {
    const std::lock_guard lock(mutex);
    mutex.attribute(42);
  }
  if (mutex.try_lock()) {
    mutex.attribute(43);
    mutex.unlock();
  }
  entries = drain(timed_stats);
  ok &= require(find(entries, 42) && find(entries, 43) && find(entries, 42)->count == 1, "Timed mutex records attributed holds");
  {
    LockHoldStats timer_stats;
    {
      const taxi_camera::HoldTimer held(timer_stats, 7, true);
      const taxi_camera::HoldTimer skipped(timer_stats, 8, false);
    }
    entries = drain(timer_stats);
    ok &= require(entries.size() == 1 && entries[0].site == 7, "Hold timer records only a held lock");
  }
  if (!ok)
    return 1;
  std::printf("PASS lock hold stats: %u checks; per-site count/total/longest, overflow, concurrent recorders, timed mutex.\n", checks);
  return 0;
}
