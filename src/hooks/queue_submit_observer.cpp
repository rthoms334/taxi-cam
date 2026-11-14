#include "queue_submit_observer.hpp"
#include "../shared/hook_timing.hpp"

#include <array>
#include <atomic>
#include <limits>

#ifdef TAXI_QUEUE_SUBMIT_VALIDATION
extern "C" BOOL taxi_queue_test_virtual_protect(void*, SIZE_T, DWORD, PDWORD) noexcept;
#endif

namespace taxi_camera::engine_hook::queue_submit {
namespace {

using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

struct QueueState {
  SRWLOCK submit_lock = SRWLOCK_INIT;
  Callbacks callbacks;
  std::atomic<ID3D12CommandQueue*> queue{nullptr};
  std::atomic<bool> enabled{false};
  std::atomic<std::uint64_t> submissions{0}, receipts{0}, refusals{0}, calls{0}, contended{0};
};

SRWLOCK control_lock = SRWLOCK_INIT;
std::array<QueueState, kMaximumQueues> queues;
std::atomic<Execute> original{nullptr};
std::atomic<UnknownQueueObserver> discover_queue{nullptr};
// The queue whose first submit this thread is offering to the discovery
// callback: a call that reached this wrapper for that queue.
thread_local ID3D12CommandQueue* discovering_queue = nullptr;
void** saved_slot = nullptr;
void** pending_slot = nullptr;
DWORD pending_protection = 0;
bool consumed = false;
bool removed = false;
thread_local bool inside_wrapper = false;
thread_local bool nested_submission = false;

struct ExclusiveLock {
  SRWLOCK& lock;
  bool owned = false;
  explicit ExclusiveLock(SRWLOCK& value, bool try_only = false) noexcept : lock(value) {
    if (try_only)
      owned = TryAcquireSRWLockExclusive(&lock) != FALSE;
    else {
      AcquireSRWLockExclusive(&lock);
      owned = true;
    }
  }
  ~ExclusiveLock() { release(); }
  void release() noexcept {
    if (!owned)
      return;
    ReleaseSRWLockExclusive(&lock);
    owned = false;
  }
  ExclusiveLock(const ExclusiveLock&) = delete;
  ExclusiveLock& operator=(const ExclusiveLock&) = delete;
};

QueueState* find_queue(ID3D12CommandQueue* queue) noexcept {
  if (queue)
    for (auto& entry : queues)
      if (entry.queue.load(std::memory_order_acquire) == queue)
        return &entry;
  return nullptr;
}

bool readable(DWORD protection) noexcept {
  if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  switch (protection & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

bool region_for(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& region) noexcept {
  if (!address || VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
      !readable(region.Protect))
    return false;
  const auto first = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
  const auto requested = reinterpret_cast<std::uintptr_t>(address);
  return region.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - first && requested >= first &&
         requested <= first + region.RegionSize && size <= first + region.RegionSize - requested;
}

bool read_pointer(const void* address, void*& output) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (reinterpret_cast<std::uintptr_t>(address) % alignof(void*) != 0 || !region_for(address, sizeof(void*), region))
    return false;
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), address, &output, sizeof(output), &copied) != FALSE && copied == sizeof(output);
}

bool slot_memory(void** slot) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (!region_for(slot, sizeof(void*), region))
    return false;
  const auto access = region.Protect & 0xff;
  // No executable-page exception is needed for a documented native COM vtable.
  // Do not reuse the private engine's separate main-image exception here.
  return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY;
}

bool pin_code(const void* code) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (!region_for(code, 1, region) || region.Type != MEM_IMAGE)
    return false;
  const auto access = region.Protect & 0xff;
  if (access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
    return false;
  HMODULE pinned = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(code),
                            &pinned) != FALSE;
}

BOOL protect(void* address, SIZE_T size, DWORD access, DWORD* prior) noexcept {
#ifdef TAXI_QUEUE_SUBMIT_VALIDATION
  return taxi_queue_test_virtual_protect(address, size, access, prior);
#else
  return VirtualProtect(address, size, access, prior);
#endif
}

bool repair_protection(DWORD& error) noexcept {
  if (!pending_slot)
    return true;
  DWORD discarded = 0;
  if (!protect(pending_slot, sizeof(void*), pending_protection, &discarded)) {
    error = GetLastError();
    return false;
  }
  pending_slot = nullptr;
  pending_protection = 0;
  return true;
}

Result result(Status status, DWORD error = ERROR_SUCCESS, bool retained = false) noexcept {
  return {status, error, consumed && !removed, retained, pending_slot == nullptr};
}

void refuse(QueueState& entry, ID3D12CommandQueue* queue, Refusal reason) noexcept {
  entry.refusals.fetch_add(1, std::memory_order_relaxed);
  entry.callbacks.refused(entry.callbacks.context, queue, reason);
}

bool valid_insertions(const std::array<Insertion, kMaximumInsertions>& insertions,
                      UINT inserted,
                      UINT count,
                      ID3D12CommandList* const* lists) noexcept {
  if (inserted > insertions.size())
    return false;
  for (UINT i = 0; i < inserted; ++i) {
    const auto& item = insertions[i];
    if (!item.list || item.after_list >= count)
      return false;
    const auto position = [](const Insertion& value) { return 2 * value.after_list + (value.before ? 0u : 1u); };
    if (i && position(item) < position(insertions[i - 1]))
      return false;
    for (UINT j = 0; j < count; ++j)
      if (!lists[j] || item.list == lists[j])
        return false;
    for (UINT j = 0; j < i; ++j)
      if (item.list == insertions[j].list)
        return false;
  }
  return true;
}

void STDMETHODCALLTYPE submit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  const auto forward = original.load(std::memory_order_acquire);
  const hook_timing::Scope timing(hook_timing::execute);
  // Published before the slot exchange and immutable after a successful install.
  // A valid call through this wrapper always has an original to forward to.
  auto* entry = find_queue(queue);
  if (!entry && !inside_wrapper && !discovering_queue) {
    if (const auto discover = discover_queue.load(std::memory_order_acquire)) {
      discovering_queue = queue;
      discover(queue);
      discovering_queue = nullptr;
      entry = find_queue(queue);
    }
  }
  if (inside_wrapper) {
    nested_submission = true;
    hook_timing::forward(forward, queue, count, lists);
    return;
  }
  if (entry)
    entry->calls.fetch_add(1, std::memory_order_relaxed);
  if (!entry || !entry->enabled.load(std::memory_order_acquire)) {
    hook_timing::forward(forward, queue, count, lists);
    return;
  }
  ExclusiveLock lock(entry->submit_lock, true);
  if (!lock.owned) {
    // Frame-generation and Present helpers share this queue. Waiting here
    // deadlocks DXGI when the owner is already inside original Execute.
    entry->contended.fetch_add(1, std::memory_order_relaxed);
    const auto notify = entry->callbacks.contended;
    const bool notified = notify && entry->enabled.load(std::memory_order_acquire);
    std::uint64_t token = 0;
    if (notified) {
      entry->refusals.fetch_add(1, std::memory_order_relaxed);
      token = notify(entry->callbacks.context, queue, count, lists);
    }
    hook_timing::forward(forward, queue, count, lists);
    if (notified && entry->callbacks.contended_completed)
      entry->callbacks.contended_completed(entry->callbacks.context, queue, token);
    if (!notify && entry->enabled.load(std::memory_order_acquire))
      refuse(*entry, queue, Refusal::contended_submission);
    return;
  }
  if (!entry->enabled.load(std::memory_order_acquire)) {
    hook_timing::forward(forward, queue, count, lists);
    return;
  }
  struct ReentryGuard {
    ReentryGuard() noexcept {
      inside_wrapper = true;
      nested_submission = false;
    }
    ~ReentryGuard() { inside_wrapper = false; }
  } guard;
  entry->submissions.fetch_add(1, std::memory_order_relaxed);
  const bool oversized = count > kMaximumCommandLists;
  const bool invalid = count == 0 || lists == nullptr;
  std::uint64_t receipt = 0;
  if (!oversized && !invalid)
    receipt = entry->callbacks.before(entry->callbacks.context, queue, count, lists);
  // Unrelated work retains its exact original arguments and fast forwarding.
  if (!receipt && !oversized && !invalid) {
    lock.release();
    hook_timing::forward(forward, queue, count, lists);
    if (entry->callbacks.forwarded_unordered)
      entry->callbacks.forwarded_unordered(entry->callbacks.context, queue);
    return;
  }
  std::array<Insertion, kMaximumInsertions> insertions{};
  std::array<ID3D12CommandList*, kMaximumCommandLists + kMaximumInsertions> augmented{};
  UINT inserted = 0;
  if (receipt && !nested_submission && entry->callbacks.augment) {
    inserted = entry->callbacks.augment(entry->callbacks.context, queue, receipt, count, lists, insertions.data(), kMaximumInsertions);
    if (nested_submission || !valid_insertions(insertions, inserted, count, lists))
      inserted = 0;
    if (entry->callbacks.augmentation_result)
      entry->callbacks.augmentation_result(entry->callbacks.context, queue, receipt, inserted);
    if (nested_submission)
      inserted = 0;
  }
  if (inserted) {
    UINT output = 0, next = 0;
    for (UINT i = 0; i < count; ++i) {
      while (next < inserted && insertions[next].after_list == i && insertions[next].before)
        augmented[output++] = insertions[next++].list;
      augmented[output++] = lists[i];
      while (next < inserted && insertions[next].after_list == i)
        augmented[output++] = insertions[next++].list;
    }
    hook_timing::forward(forward, queue, output, augmented.data());
  } else {
    hook_timing::forward(forward, queue, count, lists);
  }
  if (nested_submission)
    refuse(*entry, queue, Refusal::reentrant_submission);
  else if (oversized || invalid)
    refuse(*entry, queue, oversized ? Refusal::oversized_batch : Refusal::invalid_batch);
  else if (receipt != 0) {
    entry->callbacks.after(entry->callbacks.context, queue, receipt);
    entry->receipts.fetch_add(1, std::memory_order_relaxed);
  }
}

Result exchange(void** slot, void* expected, void* replacement, bool installing) noexcept {
  DWORD prior = 0;
  if (!protect(slot, sizeof(void*), PAGE_READWRITE, &prior))
    return result(Status::protection_change_failed, GetLastError());
  pending_slot = slot;
  pending_protection = prior;
  const auto observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), replacement, expected);
  const bool changed = observed == expected;
  if (changed) {
    if (installing)
      consumed = true;
    else
      removed = true;
  }
  DWORD error = ERROR_SUCCESS;
  if (!repair_protection(error))
    return result(Status::protection_restore_failed, error);
  return result(changed ? (installing ? Status::registered : Status::removed) : Status::slot_changed);
}

bool same_callbacks(const Callbacks& left, const Callbacks& right) noexcept {
  return left.context == right.context && left.before == right.before && left.after == right.after && left.refused == right.refused &&
         left.contended == right.contended && left.contended_completed == right.contended_completed && left.augment == right.augment &&
         left.augmentation_result == right.augmentation_result && left.forwarded_unordered == right.forwarded_unordered;
}

}  // namespace

bool set_unknown_queue_observer(UnknownQueueObserver observer) noexcept {
  if (!observer)
    return false;
  auto expected = static_cast<UnknownQueueObserver>(nullptr);
  return discover_queue.compare_exchange_strong(expected, observer) || expected == observer;
}

Result register_queue(ID3D12CommandQueue* queue, const Callbacks& callbacks) noexcept {
  if (inside_wrapper)
    return {Status::reentrant_operation};
  const ExclusiveLock lock(control_lock);
  const auto respond = [&](Status status, DWORD error = ERROR_SUCCESS) { return result(status, error, find_queue(queue) != nullptr); };
  DWORD error = ERROR_SUCCESS;
  if (!repair_protection(error))
    return respond(Status::protection_restore_failed, error);
  if (!queue || !callbacks.before || !callbacks.after || !callbacks.refused)
    return respond(Status::invalid_argument);
  if (removed)
    return respond(Status::installation_consumed);
  void* table = nullptr;
  if (!read_pointer(queue, table) || !table || reinterpret_cast<std::uintptr_t>(table) % alignof(void*) != 0 ||
      reinterpret_cast<std::uintptr_t>(table) > std::numeric_limits<std::uintptr_t>::max() - 10 * sizeof(void*))
    return respond(Status::invalid_queue_memory);
  auto* slot = reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(table) + 10 * sizeof(void*));
  if (!slot_memory(slot))
    return respond(Status::invalid_slot_memory);
  if (saved_slot && slot != saved_slot)
    return respond(Status::different_vtable);
  auto* available = find_queue(queue);
  const bool existed = available != nullptr;
  if (existed && !same_callbacks(available->callbacks, callbacks))
    return respond(Status::callback_mismatch);
  if (!available)
    for (auto& entry : queues)
      if (entry.queue.load(std::memory_order_acquire) == nullptr) {
        available = &entry;
        break;
      }
  if (!available)
    return respond(Status::queue_limit);
  void* current = nullptr;
  if (!read_pointer(slot, current))
    return respond(Status::invalid_slot_memory);
  if (consumed) {
    // Another tool hooked this slot after us. A queue first seen once that
    // has happened can still be registered from its own discovery: that call
    // reached this wrapper through the other hook, exactly as the queues
    // registered earlier are reached now. The slot is not written again.
    // Without that evidence the slot's holder is not known to forward here.
    if (current != reinterpret_cast<void*>(&submit) && discovering_queue != queue)
      return respond(Status::slot_changed);
  } else {
    if (!current || current == reinterpret_cast<void*>(&submit))
      return respond(Status::invalid_original);
    if (saved_slot) {
      if (current != reinterpret_cast<void*>(original.load(std::memory_order_acquire)))
        return respond(Status::slot_changed);
    } else {
      if (!pin_code(current))
        return respond(Status::invalid_original, GetLastError());
      if (!pin_code(reinterpret_cast<void*>(&submit)))
        return respond(Status::pin_failed, GetLastError());
      MEMORY_BASIC_INFORMATION region{};
      if (!region_for(slot, sizeof(void*), region))
        return respond(Status::invalid_slot_memory);
      if (region.Type == MEM_IMAGE) {
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(slot),
                                &pinned))
          return respond(Status::pin_failed, GetLastError());
      }
      saved_slot = slot;
      original.store(reinterpret_cast<Execute>(current), std::memory_order_release);
    }
  }
  // Retain before touching the slot. Even failed CAS/protection restoration
  // keeps its recovery address and disabled callback context alive. The same
  // explicit registration may be retried; no additional reference is acquired.
  if (!existed) {
    queue->AddRef();
    available->callbacks = callbacks;
    available->queue.store(queue, std::memory_order_release);
  }
  if (!consumed) {
    auto installed = exchange(slot, current, reinterpret_cast<void*>(&submit), true);
    if (installed.status != Status::registered) {
      installed.queue_retained = true;
      return installed;
    }
  }
  available->enabled.store(true, std::memory_order_release);
  return respond(existed ? Status::already_registered : Status::registered);
}

Result disable_queue(ID3D12CommandQueue* queue) noexcept {
  if (inside_wrapper)
    return {Status::reentrant_operation};
  const ExclusiveLock control(control_lock);
  auto* entry = find_queue(queue);
  if (!entry)
    return result(Status::not_registered);
  entry->enabled.store(false, std::memory_order_release);
  const ExclusiveLock lock(entry->submit_lock);
  return result(Status::disabled, ERROR_SUCCESS, true);
}

Result remove() noexcept {
  if (inside_wrapper)
    return {Status::reentrant_operation};
  const ExclusiveLock lock(control_lock);
  for (auto& entry : queues)
    entry.enabled.store(false, std::memory_order_release);
  DWORD error = ERROR_SUCCESS;
  if (!repair_protection(error))
    return result(Status::protection_restore_failed, error);
  if (!consumed || removed)
    return result(Status::not_installed);
  if (!slot_memory(saved_slot))
    return result(Status::invalid_slot_memory);
  return exchange(saved_slot, reinterpret_cast<void*>(&submit), reinterpret_cast<void*>(original.load(std::memory_order_acquire)), false);
}

Result restore_protection() noexcept {
  if (inside_wrapper)
    return {Status::reentrant_operation};
  const ExclusiveLock lock(control_lock);
  DWORD error = ERROR_SUCCESS;
  if (!repair_protection(error))
    return result(Status::protection_restore_failed, error);
  return result(Status::protection_restored);
}

Statistics statistics(ID3D12CommandQueue* queue) noexcept {
  if (auto* entry = find_queue(queue))
    return {entry->submissions.load(std::memory_order_relaxed), entry->receipts.load(std::memory_order_relaxed),
            entry->refusals.load(std::memory_order_relaxed), entry->calls.load(std::memory_order_relaxed),
            entry->contended.load(std::memory_order_relaxed)};
  return {};
}

Statistics total_statistics() noexcept {
  Statistics total;
  for (auto& entry : queues) {
    if (!entry.queue.load(std::memory_order_acquire))
      continue;
    total.submissions += entry.submissions.load(std::memory_order_relaxed);
    total.receipts += entry.receipts.load(std::memory_order_relaxed);
    total.refusals += entry.refusals.load(std::memory_order_relaxed);
    total.calls += entry.calls.load(std::memory_order_relaxed);
    total.contended += entry.contended.load(std::memory_order_relaxed);
  }
  return total;
}

const char* status_name(Status status) noexcept {
  switch (status) {
#define TAXI_QUEUE_STATUS(value) \
  case Status::value:            \
    return #value
    TAXI_QUEUE_STATUS(registered);
    TAXI_QUEUE_STATUS(already_registered);
    TAXI_QUEUE_STATUS(disabled);
    TAXI_QUEUE_STATUS(not_registered);
    TAXI_QUEUE_STATUS(removed);
    TAXI_QUEUE_STATUS(not_installed);
    TAXI_QUEUE_STATUS(invalid_argument);
    TAXI_QUEUE_STATUS(invalid_queue_memory);
    TAXI_QUEUE_STATUS(invalid_slot_memory);
    TAXI_QUEUE_STATUS(invalid_original);
    TAXI_QUEUE_STATUS(different_vtable);
    TAXI_QUEUE_STATUS(queue_limit);
    TAXI_QUEUE_STATUS(callback_mismatch);
    TAXI_QUEUE_STATUS(slot_changed);
    TAXI_QUEUE_STATUS(installation_consumed);
    TAXI_QUEUE_STATUS(pin_failed);
    TAXI_QUEUE_STATUS(protection_change_failed);
    TAXI_QUEUE_STATUS(protection_restore_failed);
    TAXI_QUEUE_STATUS(protection_restored);
    TAXI_QUEUE_STATUS(reentrant_operation);
#undef TAXI_QUEUE_STATUS
  }
  return "unknown";
}

}  // namespace taxi_camera::engine_hook::queue_submit
