#pragma once
#include <windows.h>

namespace taxi_camera::lock_holder_priority {

// Simulator threads wait on the registry and capture-manager locks with
// budgets of 100 us to 5 ms. When the bridge worker is preempted while holding
// one, every waiter stalls or skips its work for a scheduling quantum: in the
// 0.9.53 logs, worker holds averaging 1-200 us sometimes lasted 2-8 ms.
// A marked worker thread runs at THREAD_PRIORITY_HIGHEST while it holds such a
// lock and returns to its own priority when the outermost hold ends.
// Unmarked threads, including every simulator thread, are never changed.
inline thread_local bool worker = false;
inline thread_local unsigned depth = 0;
inline thread_local int restore = THREAD_PRIORITY_ERROR_RETURN;

inline void mark_worker_thread() noexcept {
  worker = true;
}
inline void enter() noexcept {
  if (!worker || depth++)
    return;
  const auto thread = GetCurrentThread();
  const int current = GetThreadPriority(thread);
  restore =
      current != THREAD_PRIORITY_ERROR_RETURN && current < THREAD_PRIORITY_HIGHEST && SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST)
          ? current
          : THREAD_PRIORITY_ERROR_RETURN;
}
inline void leave() noexcept {
  if (!worker || !depth || --depth)
    return;
  if (restore != THREAD_PRIORITY_ERROR_RETURN)
    SetThreadPriority(GetCurrentThread(), restore);
  restore = THREAD_PRIORITY_ERROR_RETURN;
}

}  // namespace taxi_camera::lock_holder_priority
