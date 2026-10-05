#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "../../src/bridge/freeze_watchdog.hpp"
#include "../../src/shared/bounded_lock.hpp"

namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void bounded_lock() {
  using taxi_camera::BoundedLock;
  std::mutex mutex;
  std::atomic<std::uint64_t> contended{0};
  {
    const BoundedLock lock(mutex, taxi_camera::wait_budget::recording_us, &contended);
    require(lock.owns_lock() && contended == 0, "Uncontended bounded lock did not acquire");
    require(!mutex.try_lock(), "Bounded lock did not hold the mutex");
  }
  require(mutex.try_lock(), "Bounded lock destructor did not release the mutex");
  mutex.unlock();

  // Another thread holds the lock past the budget: the waiter must give up
  // within a bounded time and report contention, never block until release.
  std::atomic<bool> release{false}, held{false};
  std::thread owner([&] {
    const std::lock_guard guard(mutex);
    held.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire))
      SwitchToThread();
  });
  while (!held.load(std::memory_order_acquire))
    SwitchToThread();
  const auto started = taxi_camera::bounded_lock_now_us();
  {
    const BoundedLock lock(mutex, 2000, &contended);
    const auto waited = taxi_camera::bounded_lock_now_us() - started;
    require(!lock.owns_lock(), "Bounded lock acquired a mutex another thread holds");
    require(waited >= 1500 && waited < 200000, "Bounded lock wait did not respect its budget");
    require(contended == 1, "Expired bounded wait was not counted");
  }
  {
    const BoundedLock immediate(mutex, 0, &contended);
    require(!immediate.owns_lock() && contended == 2, "Zero-budget bounded lock waited or was miscounted");
  }
  release.store(true, std::memory_order_release);
  owner.join();
  {
    BoundedLock lock(mutex, 100, &contended);
    require(lock.owns_lock(), "Released mutex was not reacquired");
    require(lock.release(), "release() did not report ownership");
    require(!lock.owns_lock(), "release() kept ownership");
  }
  require(!mutex.try_lock(), "release() unlocked the mutex it handed over");
  mutex.unlock();

  std::recursive_mutex recursive;
  const std::lock_guard outer(recursive);
  const BoundedLock<std::recursive_mutex> inner(recursive, 0, &contended);
  require(inner.owns_lock() && contended == 2, "Owning thread failed to re-enter a recursive bounded lock");
}

void deferred_ring() {
  taxi_camera::DeferredRing<unsigned, 4> ring;
  unsigned value = 0;
  require(ring.empty() && !ring.pop(value), "Empty ring produced a value");
  for (unsigned i = 1; i <= 4; ++i)
    require(ring.push(i), "Ring refused a value within capacity");
  require(!ring.push(5) && ring.take_overflow() && !ring.take_overflow(), "Ring overflow was not reported exactly once");
  for (unsigned i = 1; i <= 4; ++i) {
    require(ring.pop(value) && value == i, "Ring lost order or a value");
  }
  require(!ring.pop(value) && ring.empty(), "Drained ring still produced values");
  // Wraparound after a full drain keeps working and stays ordered.
  for (unsigned round = 0; round < 3; ++round) {
    require(ring.push(10 + round) && ring.push(20 + round), "Wrapped ring refused a value");
    require(ring.pop(value) && value == 10 + round && ring.pop(value) && value == 20 + round, "Wrapped ring lost order");
  }
  // Concurrent producers never block and never lose an accepted entry.
  taxi_camera::DeferredRing<unsigned, 64> shared;
  std::atomic<unsigned> accepted{0};
  std::thread producers[4];
  for (auto& producer : producers)
    producer = std::thread([&] {
      for (unsigned i = 0; i < 64; ++i)
        if (shared.push(i))
          accepted.fetch_add(1, std::memory_order_relaxed);
    });
  for (auto& producer : producers)
    producer.join();
  unsigned drained = 0;
  while (shared.pop(value))
    ++drained;
  require(drained == accepted && drained <= 64 && shared.empty(), "Concurrent ring lost or duplicated accepted entries");

  // peek leaves its entry queued, and the ring not idle, until commit: a lock-free
  // reader that sees the ring idle has seen everything done before the commit.
  taxi_camera::DeferredRing<unsigned, 4> staged;
  require(staged.idle() && !staged.overflowed() && !staged.peek(value), "An empty ring was not idle");
  require(staged.push(7) && !staged.idle() && !staged.empty(), "A queued entry left the ring idle");
  require(staged.peek(value) && value == 7 && staged.peek(value) && value == 7 && !staged.idle(), "peek consumed its entry");
  staged.commit();
  require(staged.idle() && staged.empty() && !staged.peek(value), "commit did not consume the peeked entry");
  // A lost entry keeps the ring busy after the drain until a consumer takes it.
  for (unsigned i = 1; i <= 4; ++i)
    require(staged.push(i), "Staged ring refused a value within capacity");
  require(!staged.push(5) && staged.overflowed() && !staged.idle(), "An overflowed ring was idle");
  for (unsigned i = 1; i <= 4; ++i) {
    require(staged.peek(value) && value == i && !staged.idle(), "Staged ring lost order or went idle before its commit");
    staged.commit();
  }
  require(staged.empty() && staged.overflowed() && !staged.idle(), "A drained ring hid its lost entry");
  require(staged.take_overflow() && !staged.overflowed() && staged.idle() && !staged.take_overflow(),
          "Taking the overflow did not leave the ring idle exactly once");
  // Wraparound through peek/commit, and pop after a peek returns the same entry.
  for (unsigned round = 0; round < 6; ++round) {
    require(staged.push(30 + round) && staged.peek(value) && value == 30 + round, "Wrapped staged ring lost a value");
    require(staged.pop(value) && value == 30 + round && staged.idle(), "pop after peek did not consume the peeked entry");
  }
  // A consumer committing while producers push: every accepted entry exactly once.
  taxi_camera::DeferredRing<unsigned, 16> streamed;
  std::atomic<unsigned> streamed_accepted{0}, accepted_sum{0};
  std::atomic<bool> producing{true};
  unsigned streamed_drained = 0, streamed_sum = 0;
  std::thread consumer([&] {
    unsigned entry = 0;
    for (;;) {
      const bool done = !producing.load(std::memory_order_acquire);
      while (streamed.peek(entry)) {
        streamed_sum += entry;
        ++streamed_drained;
        streamed.commit();
      }
      if (done && streamed.empty())
        break;
      SwitchToThread();
    }
  });
  std::thread streamers[3];
  for (unsigned n = 0; n < 3; ++n)
    streamers[n] = std::thread([&, n] {
      for (unsigned i = 1; i <= 200; ++i)
        if (streamed.push(n * 1000 + i)) {
          streamed_accepted.fetch_add(1, std::memory_order_relaxed);
          accepted_sum.fetch_add(n * 1000 + i, std::memory_order_relaxed);
        }
    });
  for (auto& streamer : streamers)
    streamer.join();
  producing.store(false, std::memory_order_release);
  consumer.join();
  require(streamed_drained == streamed_accepted && streamed_sum == accepted_sum && streamed.empty(),
          "A committing consumer lost or duplicated accepted entries");
  // Every refused push reported itself; taking it leaves the ring idle.
  require(streamed.overflowed() == (streamed_accepted < 600), "Concurrent overflow was not reported");
  streamed.take_overflow();
  require(streamed.idle(), "A drained ring with its overflow taken was not idle");
}

taxi_camera::FreezeWatchdog::Sample sample(std::uint64_t now, std::uint64_t pulse, std::uint64_t frames, bool armed = true) {
  taxi_camera::FreezeWatchdog::Sample value;
  value.now_ms = now;
  value.frame_pulse = pulse;
  value.pulse_available = pulse != 0;
  value.sim_frames = frames;
  value.sim_frames_expected = true;
  value.armed = armed;
  value.worker_alive = true;
  return value;
}

void watchdog() {
  using taxi_camera::FreezeWatchdog;
  FreezeWatchdog dog;
  std::uint64_t now = 1000, pulse = 10, frames = 100;
  // Healthy presentation: pulses advance every sample, never trips.
  for (unsigned i = 0; i < 40; ++i) {
    now += 250;
    const auto decision = dog.observe(sample(now, ++pulse, ++frames));
    require(!decision.trip && !decision.recover && !decision.telemetry_stall_noted, "Healthy presentation tripped the watchdog");
  }
  // SIM_FRAME stops while the pulse continues: paused or in a menu. Noted once, never a trip.
  unsigned notes = 0;
  for (unsigned i = 0; i < 40; ++i) {
    now += 250;
    const auto decision = dog.observe(sample(now, ++pulse, frames));
    require(!decision.trip, "Telemetry pause alone tripped the watchdog");
    notes += decision.telemetry_stall_noted;
  }
  require(notes == 1, "Telemetry stall was not noted exactly once");
  now += 250;
  require(!dog.observe(sample(now, ++pulse, ++frames)).telemetry_stall_noted, "Resumed telemetry was noted again");
  // Presentation stalls: trips after StallMs, exactly once, with the stall duration.
  const auto stall_start = now;
  bool tripped = false;
  for (unsigned i = 0; i < 20; ++i) {
    now += 250;
    const auto decision = dog.observe(sample(now, pulse, frames));
    if (decision.trip) {
      require(!tripped, "Watchdog tripped twice for one stall");
      tripped = true;
      require(now - stall_start >= FreezeWatchdog::StallMs && now - stall_start < FreezeWatchdog::StallMs + 500,
              "Trip did not happen at the stall bound");
      require(decision.stalled_ms >= FreezeWatchdog::StallMs && decision.worker_alive, "Trip did not carry stall duration");
      require(decision.reason[0] != 0, "Trip reason missing");
    }
  }
  require(tripped && dog.tripped() && dog.trips() == 1, "Presentation stall did not trip the watchdog");
  // Frames resume: recovery only after RecoveryMs of continuous progress.
  bool recovered = false;
  const auto resume_start = now;
  for (unsigned i = 0; i < 30; ++i) {
    now += 250;
    const auto decision = dog.observe(sample(now, ++pulse, ++frames));
    require(!decision.trip, "Re-tripped while already tripped");
    if (decision.recover) {
      require(!recovered, "Recovered twice");
      recovered = true;
      require(now - resume_start >= FreezeWatchdog::RecoveryMs && now - resume_start < FreezeWatchdog::RecoveryMs + 500,
              "Recovery did not wait for the recovery window");
    }
  }
  require(recovered && !dog.tripped(), "Resumed presentation did not recover the watchdog");
  // A stall that resumes briefly then stalls again must not recover.
  for (unsigned i = 0; i < 14; ++i) {
    now += 250;
    dog.observe(sample(now, pulse, frames));
  }
  require(dog.tripped() && dog.trips() == 2, "Second stall did not trip");
  now += 250;
  dog.observe(sample(now, ++pulse, frames));
  for (unsigned i = 0; i < 8; ++i) {
    now += 250;
    require(!dog.observe(sample(now, pulse, frames)).recover, "Interrupted progress recovered the watchdog");
  }
  // Hooked presentation goes quiet while SIM_FRAME keeps flowing (flight reload,
  // or a renderer path we do not hook): noted once, never a trip.
  FreezeWatchdog reload;
  now = 50000;
  std::uint64_t reload_pulse = 100, reload_frames = 1000;
  for (unsigned i = 0; i < 8; ++i) {
    now += 250;
    reload.observe(sample(now, ++reload_pulse, ++reload_frames));
  }
  unsigned quiet_notes = 0;
  for (unsigned i = 0; i < 80; ++i) {
    now += 250;
    const auto decision = reload.observe(sample(now, reload_pulse, ++reload_frames));
    require(!decision.trip, "Quiet hooks with live SIM_FRAME tripped the watchdog (flight-reload false positive)");
    quiet_notes += decision.presentation_stall_noted;
  }
  require(quiet_notes == 1 && !reload.tripped(), "Quiet presentation was not noted exactly once");
  // When SIM_FRAME then stops too, the trip follows within the stall bound.
  bool reload_trip = false;
  for (unsigned i = 0; i < 14; ++i) {
    now += 250;
    reload_trip |= reload.observe(sample(now, reload_pulse, reload_frames)).trip;
  }
  require(reload_trip, "Both counters stalled without a trip");
  // Telemetry that never flowed cannot veto a presentation stall.
  FreezeWatchdog untelemetered;
  now = 70000;
  bool no_telemetry_trip = false;
  for (unsigned i = 0; i < 16; ++i) {
    now += 250;
    auto value = sample(now, 9, 0);
    value.sim_frames_expected = false;
    no_telemetry_trip |= untelemetered.observe(value).trip;
  }
  require(no_telemetry_trip, "A presentation stall without any telemetry did not trip");
  // Disarmed bridge: stalls are ignored and timers restart on re-arm.
  FreezeWatchdog idle;
  for (unsigned i = 0; i < 40; ++i) {
    now += 250;
    require(!idle.observe(sample(now, 5, 5, false)).trip, "Disarmed watchdog tripped");
  }
  for (unsigned i = 0; i < 10; ++i) {
    now += 250;
    require(!idle.observe(sample(now, 5, 5, true)).trip, "Re-armed watchdog tripped before a full stall window");
  }
  now += 1000;
  require(idle.observe(sample(now, 5, 5, true)).trip, "Re-armed watchdog did not trip after a full stall window");
  // Before the first submit is seen, stalled SIM_FRAME telemetry substitutes for the pulse.
  FreezeWatchdog early;
  now = 100000;
  for (unsigned i = 0; i < 4; ++i) {
    now += 250;
    require(!early.observe(sample(now, 0, 7 + i)).trip, "Advancing telemetry tripped without a pulse");
  }
  bool early_trip = false;
  for (unsigned i = 0; i < 16; ++i) {
    now += 250;
    early_trip |= early.observe(sample(now, 0, 10)).trip;
  }
  require(early_trip, "Stalled telemetry did not substitute for a missing presentation pulse");
}
}  // namespace

int main() {
  try {
    bounded_lock();
    deferred_ring();
    watchdog();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "freeze guards: %s\n", error.what());
    return 1;
  }
  std::printf("freeze guards: %u checks passed\n", checks);
  return 0;
}
