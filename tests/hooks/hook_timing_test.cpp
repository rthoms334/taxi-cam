#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "../../src/shared/bounded_lock.hpp"
#include "../../src/shared/hook_timing.hpp"

namespace {
namespace ht = taxi_camera::hook_timing;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void spin_us(std::uint64_t duration) {
  const auto until = ht::now_us() + duration;
  while (ht::now_us() < until)
    YieldProcessor();
}
// Converts ticks to microseconds against QPC over a short calibration spin.
double ticks_per_us() {
  const auto t0 = ht::ticks();
  const auto u0 = ht::now_us();
  spin_us(20000);
  return static_cast<double>(ht::ticks() - t0) / static_cast<double>(ht::now_us() - u0);
}
struct Delta {
  std::uint64_t calls[ht::SiteCount], self[ht::SiteCount];
};
Delta snapshot() {
  auto& slot = ht::own_slot();
  Delta value{};
  for (unsigned s = 0; s < ht::SiteCount; ++s) {
    value.calls[s] = slot.calls[s].load();
    value.self[s] = slot.self[s].load();
  }
  return value;
}

void forwarded_time_is_excluded(double per_us) {
  const auto before = snapshot();
  {
    const ht::Scope scope(ht::execute);
    ht::forward([] { spin_us(3000); });
  }
  const auto after = snapshot();
  require(after.calls[ht::execute] - before.calls[ht::execute] == 1, "An execute Scope did not record one call");
  require((after.self[ht::execute] - before.self[ht::execute]) / per_us < 1000, "Forwarded runtime time was counted as self time");
}

void bridge_work_is_self_time(double per_us) {
  const auto before = snapshot();
  {
    const ht::Scope scope(ht::close);
    spin_us(2000);
    ht::forward([] {});
  }
  const auto after = snapshot();
  require((after.self[ht::close] - before.self[ht::close]) / per_us >= 1900, "Bridge work inside a Scope was not counted as self time");
}

void nested_hooks_record_once(double per_us) {
  const auto before = snapshot();
  {
    const ht::Scope outer(ht::reset);
    // The outer forward reaches another bridge hook: only the inner forward,
    // which reaches the runtime directly, is excluded.
    ht::forward([] {
      const ht::Scope inner(ht::state);
      spin_us(1500);
      ht::forward([] { spin_us(3000); });
    });
  }
  const auto after = snapshot();
  require(after.calls[ht::reset] - before.calls[ht::reset] == 1, "Outer hook did not record once");
  require(after.calls[ht::state] == before.calls[ht::state], "Nested bridge hook recorded separately");
  const auto self = (after.self[ht::reset] - before.self[ht::reset]) / per_us;
  require(self >= 1400 && self < 4000, "Nested hook self time did not include inner work and exclude the inner forward");
}

// High-volume sites time one call in SampleRate and scale it: calls stay exact
// and the self-time estimate stays close to the true total. Forwarded time is
// still excluded, and an untimed outer call keeps nested hooks untimed.
void sampled_sites_estimate_totals(double per_us) {
  static_assert(ht::sampled(ht::draw) && ht::sampled(ht::state) && ht::sampled(ht::barrier) && ht::sampled(ht::device));
  static_assert(!ht::sampled(ht::reset) && !ht::sampled(ht::close) && !ht::sampled(ht::execute) && !ht::sampled(ht::camera_manager));
  const auto before = snapshot();
  constexpr unsigned Calls = 3200;
  for (unsigned i = 0; i < Calls; ++i) {
    const ht::Scope scope(ht::draw);
    spin_us(10);
    ht::forward([] { spin_us(10); });
  }
  const auto after = snapshot();
  require(after.calls[ht::draw] - before.calls[ht::draw] == Calls, "Sampled site did not count every call");
  const auto estimate = (after.self[ht::draw] - before.self[ht::draw]) / per_us;
  // About 200 sampled calls: the estimate is within about 7% at one standard
  // deviation, so 60-140% of 32 ms is a wide, stable bound.
  require(estimate > Calls * 10 * 0.6 && estimate < Calls * 10 * 1.4, "Sampled self time did not estimate the bridge work");
}

void lock_waits_are_classified(double per_us) {
  auto& slot = ht::own_slot();
  const auto waits = slot.waits[ht::lifecycle_wait].load(), expired = slot.expired[ht::recording_wait].load();
  const auto ticks = slot.wait_ticks[ht::lifecycle_wait].load();
  std::mutex mutex;
  std::atomic<bool> held{false};
  std::thread owner([&] {
    const std::lock_guard guard(mutex);
    held.store(true, std::memory_order_release);
    spin_us(1500);
  });
  while (!held.load(std::memory_order_acquire))
    SwitchToThread();
  {
    const taxi_camera::BoundedLock lock(mutex, taxi_camera::wait_budget::lifecycle_us);
    require(lock.owns_lock(), "5 ms bounded lock did not acquire after a 1.5 ms hold");
  }
  owner.join();
  require(slot.waits[ht::lifecycle_wait].load() == waits + 1, "Successful bounded wait was not recorded");
  require((slot.wait_ticks[ht::lifecycle_wait].load() - ticks) / per_us >= 500, "Successful bounded wait time was not recorded");

  held = false;
  std::atomic<bool> release{false};
  std::thread blocker([&] {
    const std::lock_guard guard(mutex);
    held.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire))
      SwitchToThread();
  });
  while (!held.load(std::memory_order_acquire))
    SwitchToThread();
  {
    const taxi_camera::BoundedLock lock(mutex, taxi_camera::wait_budget::recording_us);
    require(!lock.owns_lock(), "100 us bounded lock acquired a held mutex");
  }
  release = true;
  blocker.join();
  require(slot.expired[ht::recording_wait].load() == expired + 1, "Expired bounded wait was not recorded");

  const auto uncontended = slot.waits[ht::recording_wait].load();
  {
    const taxi_camera::BoundedLock lock(mutex, taxi_camera::wait_budget::recording_us);
    require(lock.owns_lock(), "Uncontended bounded lock did not acquire");
  }
  require(slot.waits[ht::recording_wait].load() == uncontended, "Uncontended acquisition was recorded as a wait");
}

// The same waits are also counted against the lock they named: an expired
// submit-budget wait on submission_mutex_ and a successful lifecycle wait on
// the registry land in their own cells, an untagged one in "other".
void lock_waits_name_their_lock() {
  const auto cell = [](ht::Wait wait, ht::WaitLock lock) -> ht::LockWaits& { return ht::lock_waits[ht::lock_wait_cell(wait, lock)]; };
  static_assert(ht::lock_wait_cell(ht::submit_wait, ht::manager_lock) != ht::lock_wait_cell(ht::submit_wait, ht::submission_lock));
  static_assert(ht::lock_wait_cell(ht::other_wait, ht::WaitLockCount) == ht::LockWaitCells - 1);
  auto& submission = cell(ht::submit_wait, ht::submission_lock);
  auto& manager = cell(ht::submit_wait, ht::manager_lock);
  auto& registry = cell(ht::lifecycle_wait, ht::registry_lock);
  auto& other = cell(ht::lifecycle_wait, ht::other_lock);
  const auto submission_waits = submission.waits.load(), submission_expired = submission.expired.load();
  const auto manager_waits = manager.waits.load(), registry_waits = registry.waits.load(), registry_expired = registry.expired.load();
  const auto other_waits = other.waits.load();
  std::mutex mutex;
  std::atomic<bool> held{false}, release{false};
  std::thread blocker([&] {
    const std::lock_guard guard(mutex);
    held.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire))
      SwitchToThread();
  });
  while (!held.load(std::memory_order_acquire))
    SwitchToThread();
  {
    const taxi_camera::BoundedLock lock(mutex, taxi_camera::wait_budget::submit_us, nullptr, ht::submission_lock);
    require(!lock.owns_lock(), "1 ms bounded lock acquired a held mutex");
  }
  release = true;
  blocker.join();
  require(submission.waits.load() == submission_waits + 1 && submission.expired.load() == submission_expired + 1,
          "An expired submit wait was not counted against submission_mutex_");
  require(manager.waits.load() == manager_waits, "A submission_mutex_ wait was counted against mutex_");
  require(submission.max_wait.load() > 0, "An expired wait's length was not recorded for its lock");

  held = false;
  std::thread owner([&] {
    const std::lock_guard guard(mutex);
    held.store(true, std::memory_order_release);
    spin_us(1500);
  });
  while (!held.load(std::memory_order_acquire))
    SwitchToThread();
  {
    const taxi_camera::BoundedLock lock(mutex, taxi_camera::wait_budget::lifecycle_us, nullptr, ht::registry_lock);
    require(lock.owns_lock(), "5 ms bounded lock did not acquire after a 1.5 ms hold");
  }
  owner.join();
  require(registry.waits.load() == registry_waits + 1 && registry.expired.load() == registry_expired,
          "A successful lifecycle wait was not counted against the registry");
  require(other.waits.load() == other_waits, "A registry wait was counted as an untagged one");

  held = false;
  std::thread untagged([&] {
    const std::lock_guard guard(mutex);
    held.store(true, std::memory_order_release);
    spin_us(1500);
  });
  while (!held.load(std::memory_order_acquire))
    SwitchToThread();
  {
    const taxi_camera::BoundedLock lock(mutex, taxi_camera::wait_budget::lifecycle_us);
    require(lock.owns_lock(), "Untagged 5 ms bounded lock did not acquire after a 1.5 ms hold");
  }
  untagged.join();
  require(other.waits.load() == other_waits + 1 && registry.waits.load() == registry_waits + 1,
          "An untagged wait was not counted as other");
}

// The site line lists the interval's nonzero wait cells after " | by_lock".
void report_lists_lock_waits() {
  ht::Report report;
  char sites[1400], threads[1400];
  report.sample(sites, sizeof(sites), threads, sizeof(threads));
  // Tens of millions of TSC ticks: whole microseconds at any TSC rate.
  ht::record_wait(taxi_camera::wait_budget::submit_us, 40000000, false, ht::manager_lock);
  ht::record_wait(taxi_camera::wait_budget::submit_us, 20000000, true, ht::manager_lock);
  ht::record_wait(taxi_camera::wait_budget::recording_us, 1000, true, ht::index_lock);
  require(report.sample(sites, sizeof(sites), threads, sizeof(threads)), "Report sample was not produced");
  const char* section = std::strstr(sites, " | by_lock");
  const char* classes = std::strstr(sites, " wait_other=");
  require(section && classes && classes < section, "Lock waits did not follow the wait classes");
  unsigned long long waits = 0, expired = 0;
  double total_us = -1, max_us = -1;
  const char* manager = std::strstr(section, " wait1000_manager=");
  require(manager && std::sscanf(manager, " wait1000_manager=%llu/%lf/%lf/%llu", &waits, &total_us, &max_us, &expired) == 4,
          "The submit wait on mutex_ was not listed");
  require(waits == 2 && expired == 1 && max_us > 0 && max_us < total_us, "The mutex_ submit waits were miscounted");
  require(std::strstr(section, " wait100_index=1/") != nullptr, "The index-shard wait was not listed");
  require(std::strstr(section, "_submission=") == nullptr && std::strstr(section, "_registry=") == nullptr,
          "A lock without waits this interval was listed");
  require(report.sample(sites, sizeof(sites), threads, sizeof(threads)), "Idle report sample was not produced");
  section = std::strstr(sites, " | by_lock");
  require(section && !std::strcmp(section, " | by_lock"), "An idle interval listed lock waits or kept a maximum");
}

void report_names_threads() {
  ht::Report report;
  char sites[1024], threads[1024];
  require(!report.sample(sites, sizeof(sites), threads, sizeof(threads)), "First report sample was not a baseline");
  // Names are read while the thread is alive, as for simulator threads.
  std::atomic<bool> recorded{false}, sampled{false};
  std::thread worker([&] {
    SetThreadDescription(GetCurrentThread(), L"Timing Worker");
    for (int i = 0; i < 50; ++i) {
      const ht::Scope scope(ht::execute);
      spin_us(100);
    }
    recorded.store(true, std::memory_order_release);
    while (!sampled.load(std::memory_order_acquire))
      SwitchToThread();
  });
  while (!recorded.load(std::memory_order_acquire))
    SwitchToThread();
  {
    const ht::Scope scope(ht::camera_manager);
  }
  const bool produced = report.sample(sites, sizeof(sites), threads, sizeof(threads));
  sampled.store(true, std::memory_order_release);
  worker.join();
  require(produced, "Second report sample was not produced");
  require(std::strstr(sites, "Hook timing: interval_ms=") == sites, "Site line prefix changed");
  require(std::strstr(sites, " execute=50/") != nullptr, "Site line did not report the interval's execute calls");
  require(std::strstr(sites, " updates=1 ") != nullptr, "Camera-manager updates were not reported");
  require(std::strstr(sites, " wait5000=") != nullptr, "Wait classes were not reported");
  require(std::strstr(threads, "Hook timing threads:") == threads, "Thread line prefix changed");
  require(std::strstr(threads, ":Timing_Worker=") != nullptr, "Thread description was not reported");
  require(std::strstr(threads, "[execute:50/") != nullptr, "Thread's dominant site was not reported");
  std::printf("%s\n%s\n", sites, threads);
  require(report.sample(sites, sizeof(sites), threads, sizeof(threads)), "Idle report sample was not produced");
  require(std::strstr(sites, " execute=0/0/0") != nullptr, "Idle interval retained previous calls or maxima");
}
// Empty high-volume hooks must not report the timestamp reads as bridge work.
void report_removes_measurement_floor() {
  ht::Report report;
  char sites[1024], threads[1400];
  report.sample(sites, sizeof(sites), threads, sizeof(threads));
  constexpr unsigned Calls = 1u << 20;
  // Preemption on a shared runner only adds measured time, so the best of
  // three intervals is the measurement; each still counts every call.
  double best_us = -1;
  for (unsigned attempt = 0; attempt < 3 && !(best_us >= 0 && best_us < Calls * 0.003); ++attempt) {
    for (unsigned i = 0; i < Calls; ++i) {
      const ht::Scope scope(ht::device);
      ht::forward([] {});
    }
    require(report.sample(sites, sizeof(sites), threads, sizeof(threads)), "Report sample was not produced");
    const char* field = std::strstr(sites, " device=");
    unsigned long long calls = 0;
    double self_us = -1;
    require(field && std::sscanf(field, " device=%llu/%lf", &calls, &self_us) == 2, "Device site was not reported");
    require(calls == Calls, "Device calls were not counted exactly");
    require(self_us >= 0, "Corrected self time was negative");
    best_us = best_us < 0 ? self_us : std::min(best_us, self_us);
  }
  // Uncorrected, this is about 15 ns per call (15 ms); allow 3 ns of residue.
  require(best_us >= 0 && best_us < Calls * 0.003, "Measurement floor was reported as bridge self time");
}

// Twelve busier threads fill the list; a named main thread is still reported.
void report_keeps_main_thread() {
  ht::Report report;
  char sites[1024], threads[1400];
  report.sample(sites, sizeof(sites), threads, sizeof(threads));
  std::atomic<unsigned> recorded{0};
  std::atomic<bool> sampled{false};
  const auto work = [&](const wchar_t* name, unsigned calls) {
    SetThreadDescription(GetCurrentThread(), name);
    for (unsigned i = 0; i < calls; ++i) {
      const ht::Scope scope(ht::close);
      spin_us(50);
    }
    recorded.fetch_add(1, std::memory_order_release);
    while (!sampled.load(std::memory_order_acquire))
      SwitchToThread();
  };
  std::thread busy[ht::Report::TopThreads];
  for (auto& thread : busy)
    thread = std::thread(work, L"Busy Worker", 40u);
  std::thread main_thread(work, L"Sim WinMain", 1u);
  while (recorded.load(std::memory_order_acquire) < ht::Report::TopThreads + 1)
    SwitchToThread();
  const bool produced = report.sample(sites, sizeof(sites), threads, sizeof(threads));
  sampled.store(true, std::memory_order_release);
  for (auto& thread : busy)
    thread.join();
  main_thread.join();
  require(produced, "Report sample was not produced");
  require(std::strstr(threads, ":Sim_WinMain=") != nullptr, "Main thread outside the top threads was not reported");
}
}  // namespace

int main() {
  try {
    const auto per_us = ticks_per_us();
    require(per_us > 0, "TSC did not advance");
    forwarded_time_is_excluded(per_us);
    bridge_work_is_self_time(per_us);
    nested_hooks_record_once(per_us);
    sampled_sites_estimate_totals(per_us);
    lock_waits_are_classified(per_us);
    lock_waits_name_their_lock();
    report_lists_lock_waits();
    report_names_threads();
    report_keeps_main_thread();
    report_removes_measurement_floor();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "hook timing: %s\n", error.what());
    return 1;
  }
  std::printf("hook timing: %u checks passed\n", checks);
  return 0;
}
