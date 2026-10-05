#pragma once
#include <intrin.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace taxi_camera::hook_timing {

// CPU time the bridge spends on simulator-owned threads. A Scope brackets one
// hook body; only the outermost Scope on a thread records, so chained bridge
// hooks count once under the outer site. A Forward brackets a call to the
// original function: when no bridge hook runs inside it, that interval is the
// runtime's own work and is excluded from the outer Scope's self time. Forwards
// that are not bracketed are counted as self time (an overestimate, never an
// underestimate). Each thread writes only its own slot; the log reads totals.
// High-volume sites time one call in SampleRate, chosen at random per thread,
// and record that call's self time multiplied by SampleRate. Calls are counted
// exactly; maxima are the largest sampled call. Timing a call costs about two
// timestamp reads, which would otherwise dominate these sites' own cost.
enum Site : unsigned { state, draw, barrier, render_pass, copy, close, reset, execute, device, camera_manager, SiteCount };
// Bounded-lock waits, classified by budget. Only waits after a failed first
// try_lock are recorded, including waits that later acquire the lock.
enum Wait : unsigned { recording_wait, close_wait, submit_wait, lifecycle_wait, other_wait, WaitCount };

inline constexpr unsigned SampleRate = 16;
constexpr bool sampled(unsigned site) noexcept {
  return site == state || site == draw || site == barrier || site == device;
}

constexpr const char* site_name(unsigned site) noexcept {
  constexpr const char* names[] = {"state", "draw",  "barrier", "render_pass", "copy",
                                   "close", "reset", "execute", "device",      "camera_manager"};
  return site < SiteCount ? names[site] : "unknown";
}
constexpr const char* wait_name(unsigned wait) noexcept {
  constexpr const char* names[] = {"wait100", "wait500", "wait1000", "wait5000", "wait_other"};
  return wait < WaitCount ? names[wait] : "unknown";
}
constexpr Wait wait_class(std::uint32_t budget_us) noexcept {
  return budget_us == 100    ? recording_wait
         : budget_us == 500  ? close_wait
         : budget_us == 1000 ? submit_wait
         : budget_us == 5000 ? lifecycle_wait
                             : other_wait;
}
// The lock a bounded wait was for. Several locks share each budget: wait1000
// is the capture manager's mutex_ and its submission_mutex_, and the other
// budgets mix the registry, its index shards and observation mutex, the
// manager, the scene runtime and the handoff.
enum WaitLock : unsigned {
  manager_lock,
  submission_lock,
  registry_lock,
  index_lock,
  observation_lock,
  runtime_lock,
  handoff_lock,
  other_lock,
  WaitLockCount
};
constexpr const char* wait_lock_name(unsigned lock) noexcept {
  constexpr const char* names[] = {"manager", "submission", "registry", "index", "observation", "runtime", "handoff", "other"};
  return lock < WaitLockCount ? names[lock] : "unknown";
}

struct alignas(64) ThreadSlot {
  std::atomic<std::uint32_t> tid{};
  std::atomic<std::uint64_t> calls[SiteCount]{}, self[SiteCount]{}, max_self[SiteCount]{};
  std::atomic<std::uint64_t> waits[WaitCount]{}, wait_ticks[WaitCount]{}, max_wait[WaitCount]{}, expired[WaitCount]{};
};
inline constexpr unsigned MaxThreads = 128;
// Slot MaxThreads is shared by threads that find no free slot.
inline std::array<ThreadSlot, MaxThreads + 1> slots;
// Bounded-lock waits by budget class and lock. A wait already spun past a
// failed try_lock, so these are shared relaxed counters rather than per-thread
// ones: a few adds on a rare path. BoundedLock records an acquired wait just
// after taking the lock, so these adds (a few hundred per 5 s with the cameras
// on) lengthen that hold by tens of nanoseconds; accepted as diagnostic cost.
struct alignas(32) LockWaits {
  std::atomic<std::uint64_t> waits{}, ticks{}, max_wait{}, expired{};
};
inline constexpr unsigned LockWaitCells = static_cast<unsigned>(WaitCount) * WaitLockCount;
inline std::array<LockWaits, LockWaitCells> lock_waits;
constexpr unsigned lock_wait_cell(Wait wait, WaitLock lock) noexcept {
  return static_cast<unsigned>(wait) * WaitLockCount + (lock < WaitLockCount ? lock : other_lock);
}

struct ThreadState {
  ThreadSlot* slot;
  unsigned depth;
  unsigned site;
  bool timed;
  std::uint32_t random;
  std::uint64_t start;
  std::uint64_t forwarded;
  std::uint64_t entries;
};
inline thread_local ThreadState current{};

inline std::uint64_t ticks() noexcept {
  return __rdtsc();
}

inline ThreadSlot& own_slot() noexcept {
  auto& state = current;
  if (state.slot)
    return *state.slot;
  const auto tid = static_cast<std::uint32_t>(GetCurrentThreadId());
  for (unsigned i = 0; i < MaxThreads; ++i) {
    std::uint32_t expected = 0;
    if (slots[i].tid.load(std::memory_order_relaxed) == 0 &&
        slots[i].tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
      state.slot = &slots[i];
      return *state.slot;
    }
  }
  state.slot = &slots[MaxThreads];
  return *state.slot;
}
inline void add(ThreadSlot& slot, std::atomic<std::uint64_t>& value, std::uint64_t amount) noexcept {
  if (&slot == &slots[MaxThreads])
    value.fetch_add(amount, std::memory_order_relaxed);
  else
    value.store(value.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}
inline void raise(std::atomic<std::uint64_t>& value, std::uint64_t amount) noexcept {
  auto seen = value.load(std::memory_order_relaxed);
  while (amount > seen && !value.compare_exchange_weak(seen, amount, std::memory_order_relaxed)) {
  }
}
inline void record(unsigned site, std::uint64_t self) noexcept {
  auto& slot = own_slot();
  add(slot, slot.calls[site], 1);
  add(slot, slot.self[site], sampled(site) ? self * SampleRate : self);
  raise(slot.max_self[site], self);
}
inline void count(unsigned site) noexcept {
  auto& slot = own_slot();
  add(slot, slot.calls[site], 1);
}
// xorshift32, seeded from the thread id, so the sampled calls do not follow any
// fixed pattern of calls on the thread.
inline bool take_sample(ThreadState& state) noexcept {
  auto x = state.random ? state.random : (static_cast<std::uint32_t>(GetCurrentThreadId()) * 2654435761u) | 1u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state.random = x;
  return x % SampleRate == 0;
}
inline void record_wait(std::uint32_t budget_us, std::uint64_t waited, bool acquired, WaitLock lock = other_lock) noexcept {
  const auto kind = wait_class(budget_us);
  auto& slot = own_slot();
  add(slot, slot.waits[kind], 1);
  add(slot, slot.wait_ticks[kind], waited);
  raise(slot.max_wait[kind], waited);
  if (!acquired)
    add(slot, slot.expired[kind], 1);
  auto& cell = lock_waits[lock_wait_cell(kind, lock)];
  cell.waits.fetch_add(1, std::memory_order_relaxed);
  cell.ticks.fetch_add(waited, std::memory_order_relaxed);
  raise(cell.max_wait, waited);
  if (!acquired)
    cell.expired.fetch_add(1, std::memory_order_relaxed);
}
// The outermost Scope's exit when it cannot just count in its own slot: a timed
// call, the thread's first call (no slot yet) or a thread on the shared slot.
// Out of line, so the untimed exit inlined into every hook stays short.
__attribute__((noinline, cold)) inline void finish(ThreadState& state) noexcept {
  if (!state.timed) {
    count(state.site);
    return;
  }
  const auto total = ticks() - state.start;
  record(state.site, total > state.forwarded ? total - state.forwarded : 0);
}

class Scope {
 public:
  explicit Scope(Site site) noexcept {
    auto& state = current;
    ++state.entries;
    if (state.depth++ == 0) {
      state.site = site;
      state.timed = !sampled(site) || take_sample(state);
      if (state.timed) {
        state.forwarded = 0;
        state.start = ticks();
      }
    }
  }
  // Inlined untimed exit: the same plain load and store add() uses for an owned
  // slot. own_slot() claimed the slot for this thread alone, so no other thread
  // writes this counter and the worker's relaxed read sees a whole value.
  __attribute__((always_inline)) ~Scope() {
    auto& state = current;
    if (--state.depth)
      return;
    if (!state.timed && state.slot && state.slot != &slots[MaxThreads]) {
      auto& calls = state.slot->calls[state.site];
      calls.store(calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      return;
    }
    finish(state);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

class Forward {
 public:
  Forward() noexcept {
    const auto& state = current;
    active_ = state.depth && state.timed;
    entries_ = state.entries;
    start_ = active_ ? ticks() : 0;
  }
  ~Forward() {
    auto& state = current;
    if (active_ && state.entries == entries_)
      state.forwarded += ticks() - start_;
  }
  Forward(const Forward&) = delete;
  Forward& operator=(const Forward&) = delete;

 private:
  bool active_;
  std::uint64_t entries_;
  std::uint64_t start_;
};

// Calls the original once, excluding its runtime time from the current Scope.
template <class F, class... Args>
inline decltype(auto) forward(F&& function, Args&&... args) {
  const Forward bracket;
  return function(static_cast<Args&&>(args)...);
}

inline std::uint64_t now_us() noexcept {
  static const std::int64_t frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart ? value.QuadPart : 1;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<std::uint64_t>(counter.QuadPart / frequency * 1000000 + counter.QuadPart % frequency * 1000000 / frequency);
}

// Thread description set by the owning process, or empty. Spaces become '_'.
inline void thread_name(std::uint32_t tid, char* out, std::size_t size) noexcept {
  if (!size)
    return;
  out[0] = 0;
  using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto describe =
      reinterpret_cast<Describe>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  if (!describe)
    return;
  const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!thread)
    return;
  PWSTR text = nullptr;
  if (SUCCEEDED(describe(thread, &text)) && text) {
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(size), nullptr, nullptr);
    if (written <= 0)
      out[0] = 0;
    out[size - 1] = 0;
    for (char* c = out; *c; ++c)
      if (*c == ' ' || *c == '|' || *c == ',' || *c == '=')
        *c = '_';
    LocalFree(text);
  }
  CloseHandle(thread);
}

// Interval deltas for the bridge log. Worker thread only; the first sample only
// establishes the baseline. Microseconds are converted from TSC ticks using the
// QPC interval, so no calibration is needed on the hook path.
class Report {
 public:
  // Threads with the most self time, plus any simulator main thread (a name
  // containing "Main") so its cost is reported even when it is small.
  static constexpr unsigned TopThreads = 12;

  bool sample(char* sites, std::size_t sites_size, char* threads, std::size_t threads_size) noexcept {
    const auto tsc = ticks();
    const auto us = now_us();
    auto& current_totals = current_;
    for (unsigned i = 0; i <= MaxThreads; ++i)
      read(slots[i], current_totals[i]);
    std::array<LockTotals, LockWaitCells> locks;
    for (unsigned c = 0; c < locks.size(); ++c)
      read(lock_waits[c], locks[c]);
    if (!started_ || us <= us_ || tsc <= tsc_) {
      started_ = true;
      floor_ = measurement_floor();
      tsc_ = tsc;
      us_ = us;
      for (unsigned i = 0; i <= MaxThreads; ++i)
        previous_[i] = current_totals[i];
      lock_previous_ = locks;
      return false;
    }
    const double per_us = static_cast<double>(tsc - tsc_) / static_cast<double>(us - us_);
    const auto to_us = [per_us](std::uint64_t value) { return static_cast<double>(value) / per_us; };
    const auto interval_ms = (us - us_) / 1000;
    std::uint64_t calls[SiteCount]{}, self[SiteCount]{}, max_self[SiteCount]{};
    std::uint64_t waits[WaitCount]{}, wait_ticks[WaitCount]{}, max_wait[WaitCount]{}, expired[WaitCount]{};
    struct Thread {
      unsigned slot;
      std::uint64_t self, wait;
      unsigned first, second;
    };
    Thread active[MaxThreads + 1]{};
    unsigned active_count = 0;
    for (unsigned i = 0; i <= MaxThreads; ++i) {
      const auto& now = current_totals[i];
      const auto& before = previous_[i];
      Thread thread{i, 0, 0, SiteCount, SiteCount};
      std::uint64_t first = 0, second = 0;
      for (unsigned s = 0; s < SiteCount; ++s) {
        const auto delta = corrected(now.self[s] - before.self[s], now.calls[s] - before.calls[s]);
        calls[s] += now.calls[s] - before.calls[s];
        self[s] += delta;
        if (now.max_self[s] > max_self[s])
          max_self[s] = now.max_self[s];
        thread.self += delta;
        if (delta > first) {
          second = first;
          thread.second = thread.first;
          first = delta;
          thread.first = s;
        } else if (delta > second) {
          second = delta;
          thread.second = s;
        }
      }
      for (unsigned w = 0; w < WaitCount; ++w) {
        waits[w] += now.waits[w] - before.waits[w];
        const auto delta = now.wait_ticks[w] - before.wait_ticks[w];
        wait_ticks[w] += delta;
        thread.wait += delta;
        expired[w] += now.expired[w] - before.expired[w];
        if (now.max_wait[w] > max_wait[w])
          max_wait[w] = now.max_wait[w];
      }
      if (thread.self || thread.wait) {
        unsigned at = active_count++;
        while (at > 0 && active[at - 1].self < thread.self) {
          active[at] = active[at - 1];
          --at;
        }
        active[at] = thread;
      }
    }
    std::uint64_t total_self = 0;
    for (unsigned s = 0; s < SiteCount; ++s)
      total_self += self[s];
    std::size_t used = 0;
    const auto append = [](char* out, std::size_t size, std::size_t& at, auto... values) {
      if (at >= size)
        return;
      const int written = std::snprintf(out + at, size - at, values...);
      at = written > 0 && static_cast<std::size_t>(written) < size - at ? at + static_cast<std::size_t>(written) : size;
    };
    const auto frames = calls[camera_manager];
    append(sites, sites_size, used,
           "Hook timing: interval_ms=%llu tsc_mhz=%.0f floor_ns=%.1f self_ms=%.1f self_us_per_update=%.0f updates=%llu |",
           static_cast<unsigned long long>(interval_ms), per_us, floor_ * 1000.0 / per_us, to_us(total_self) / 1000.0,
           frames ? to_us(total_self) / frames : 0.0, static_cast<unsigned long long>(frames));
    for (unsigned s = 0; s < SiteCount; ++s)
      append(sites, sites_size, used, " %s=%llu/%.0f/%.0f", site_name(s), static_cast<unsigned long long>(calls[s]), to_us(self[s]),
             to_us(max_self[s]));
    append(sites, sites_size, used, "%s", " |");
    for (unsigned w = 0; w < WaitCount; ++w)
      append(sites, sites_size, used, " %s=%llu/%.0f/%.0f/%llu", wait_name(w), static_cast<unsigned long long>(waits[w]),
             to_us(wait_ticks[w]), to_us(max_wait[w]), static_cast<unsigned long long>(expired[w]));
    // The same waits split by lock, as <class>_<lock>=count/total_us/max_us/
    // expired; only the interval's nonzero cells are listed.
    append(sites, sites_size, used, "%s", " | by_lock");
    for (unsigned c = 0; c < locks.size(); ++c) {
      const auto& now = locks[c];
      const auto& before = lock_previous_[c];
      if (now.waits == before.waits && !now.max_wait)
        continue;
      append(sites, sites_size, used, " %s_%s=%llu/%.0f/%.0f/%llu", wait_name(c / WaitLockCount), wait_lock_name(c % WaitLockCount),
             static_cast<unsigned long long>(now.waits - before.waits), to_us(now.ticks - before.ticks), to_us(now.max_wait),
             static_cast<unsigned long long>(now.expired - before.expired));
    }
    lock_previous_ = locks;
    if (sites_size)
      sites[sites_size - 1] = 0;
    used = 0;
    append(threads, threads_size, used, "%s", "Hook timing threads:");
    for (unsigned t = 0; t < active_count; ++t) {
      const auto& thread = active[t];
      const auto tid = current_totals[thread.slot].tid;
      char name[48];
      if (thread.slot == MaxThreads)
        std::snprintf(name, sizeof(name), "shared");
      else
        thread_name(tid, name, sizeof(name));
      if (t >= TopThreads && !std::strstr(name, "Main"))
        continue;
      const auto& now = current_totals[thread.slot];
      const auto& before = previous_[thread.slot];
      append(threads, threads_size, used, " %lu:%s=%.0f/%.0f", static_cast<unsigned long>(tid), name[0] ? name : "unnamed",
             to_us(thread.self), to_us(thread.wait));
      for (const auto s : {thread.first, thread.second})
        if (s < SiteCount)
          append(threads, threads_size, used, "%s%s:%llu/%.0f", s == thread.first ? " [" : ",", site_name(s),
                 static_cast<unsigned long long>(now.calls[s] - before.calls[s]),
                 to_us(corrected(now.self[s] - before.self[s], now.calls[s] - before.calls[s])));
      if (thread.first < SiteCount)
        append(threads, threads_size, used, "%s", "]");
    }
    if (threads_size)
      threads[threads_size - 1] = 0;
    tsc_ = tsc;
    us_ = us;
    for (unsigned i = 0; i <= MaxThreads; ++i)
      previous_[i] = current_totals[i];
    return true;
  }

 private:
  struct Totals {
    std::uint32_t tid;
    std::uint64_t calls[SiteCount], self[SiteCount], max_self[SiteCount];
    std::uint64_t waits[WaitCount], wait_ticks[WaitCount], max_wait[WaitCount], expired[WaitCount];
  };
  // Maxima are per interval: the reader clears them as it reads.
  static void read(ThreadSlot& slot, Totals& out) noexcept {
    out.tid = slot.tid.load(std::memory_order_relaxed);
    for (unsigned s = 0; s < SiteCount; ++s) {
      out.calls[s] = slot.calls[s].load(std::memory_order_relaxed);
      out.self[s] = slot.self[s].load(std::memory_order_relaxed);
      out.max_self[s] = slot.max_self[s].exchange(0, std::memory_order_relaxed);
    }
    for (unsigned w = 0; w < WaitCount; ++w) {
      out.waits[w] = slot.waits[w].load(std::memory_order_relaxed);
      out.wait_ticks[w] = slot.wait_ticks[w].load(std::memory_order_relaxed);
      out.max_wait[w] = slot.max_wait[w].exchange(0, std::memory_order_relaxed);
      out.expired[w] = slot.expired[w].load(std::memory_order_relaxed);
    }
  }
  struct LockTotals {
    std::uint64_t waits, ticks, max_wait, expired;
  };
  static void read(LockWaits& cell, LockTotals& out) noexcept {
    out.waits = cell.waits.load(std::memory_order_relaxed);
    out.ticks = cell.ticks.load(std::memory_order_relaxed);
    out.max_wait = cell.max_wait.exchange(0, std::memory_order_relaxed);
    out.expired = cell.expired.load(std::memory_order_relaxed);
  }
  // Self time a timed call records for an empty hook: the Scope and Forward
  // timestamp reads themselves. Every call's expected recorded time includes
  // it (sampled calls are scaled back to every call), so the report removes
  // calls * floor from each site. Measured on the worker at the baseline.
  static double measurement_floor() noexcept {
    constexpr unsigned Samples = 4096;
    std::uint64_t total = 0;
    for (unsigned i = 0; i < Samples; ++i) {
      const auto start = ticks();
      const auto forward_start = ticks();
      const auto forward_end = ticks();
      const auto end = ticks();
      total += (end - start) - (forward_end - forward_start);
    }
    return static_cast<double>(total) / Samples;
  }
  std::uint64_t corrected(std::uint64_t self, std::uint64_t calls) const noexcept {
    const auto floor = static_cast<std::uint64_t>(floor_ * static_cast<double>(calls));
    return self > floor ? self - floor : 0;
  }
  bool started_ = false;
  double floor_ = 0;
  std::uint64_t tsc_ = 0, us_ = 0;
  Totals current_[MaxThreads + 1]{}, previous_[MaxThreads + 1]{};
  std::array<LockTotals, LockWaitCells> lock_previous_{};
};

}  // namespace taxi_camera::hook_timing
