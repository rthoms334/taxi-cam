#include "../../src/shared/lock_holder_priority.hpp"
#include "../../src/shared/lock_hold_stats.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
int priority() {
  return GetThreadPriority(GetCurrentThread());
}
}  // namespace

int main() {
  namespace boost = taxi_camera::lock_holder_priority;
  bool ok = true;
  // Each case runs on its own thread: the marker and depth are thread-local.
  std::thread([&] {
    ok &= require(priority() == THREAD_PRIORITY_NORMAL, "Test thread starts at normal priority");
    boost::enter();
    ok &= require(priority() == THREAD_PRIORITY_NORMAL, "An unmarked thread is never raised");
    boost::leave();
    boost::mark_worker_thread();
    boost::enter();
    ok &= require(priority() == THREAD_PRIORITY_HIGHEST, "A worker holding a lock runs at highest");
    boost::enter();
    boost::leave();
    ok &= require(priority() == THREAD_PRIORITY_HIGHEST, "A nested release keeps the outer hold raised");
    boost::leave();
    ok &= require(priority() == THREAD_PRIORITY_NORMAL, "The outermost release restores the worker's priority");
    boost::leave();
    ok &= require(priority() == THREAD_PRIORITY_NORMAL && boost::depth == 0, "An unmatched release changes nothing");
  }).join();
  std::thread([&] {
    boost::mark_worker_thread();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    boost::enter();
    boost::leave();
    ok &= require(priority() == THREAD_PRIORITY_BELOW_NORMAL, "The worker returns to its own priority, not to normal");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    boost::enter();
    ok &= require(priority() == THREAD_PRIORITY_TIME_CRITICAL, "A higher worker priority is never lowered");
    boost::leave();
    ok &= require(priority() == THREAD_PRIORITY_TIME_CRITICAL, "Releasing leaves a higher priority untouched");
  }).join();
  std::thread([&] {
    // TimedMutex holds from a worker are raised, including try_lock, and a
    // failed try_lock leaves the priority as it was.
    taxi_camera::LockHoldStats stats;
    taxi_camera::TimedMutex<std::mutex> mutex(stats);
    boost::mark_worker_thread();
    {
      const std::lock_guard lock(mutex);
      ok &= require(priority() == THREAD_PRIORITY_HIGHEST, "A worker holding the timed mutex runs at highest");
    }
    ok &= require(priority() == THREAD_PRIORITY_NORMAL, "Unlocking the timed mutex restores the worker");
    if (mutex.try_lock()) {
      ok &= require(priority() == THREAD_PRIORITY_HIGHEST, "A worker try_lock hold runs at highest");
      std::thread([&] {
        ok &= require(!mutex.try_lock() && priority() == THREAD_PRIORITY_NORMAL, "A simulator-side failed try_lock is not raised");
      }).join();
      mutex.unlock();
    }
    ok &= require(priority() == THREAD_PRIORITY_NORMAL && boost::depth == 0, "try_lock release restores the worker");
    std::thread holder;
    std::atomic<bool> held{false}, release{false};
    holder = std::thread([&] {
      mutex.lock();
      held = true;
      while (!release)
        std::this_thread::yield();
      mutex.unlock();
    });
    while (!held)
      std::this_thread::yield();
    ok &= require(!mutex.try_lock() && priority() == THREAD_PRIORITY_NORMAL && boost::depth == 0,
                  "A worker's failed try_lock restores its priority");
    release = true;
    holder.join();
  }).join();
  if (!ok)
    return 1;
  std::printf("PASS lock holder priority: %u checks; worker-only raise, nesting, own-priority restore, timed mutex.\n", checks);
  return 0;
}
