#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <cstdint>

namespace taxi_camera::engine_hook::queue_submit {

inline constexpr unsigned kMaximumQueues = 8;
inline constexpr unsigned kMaximumCommandLists = 256;
// Per display side (captain, first officer, lower display): a shadow restore
// before the instrument's draws and the camera copy after them, plus one
// display snapshot.
inline constexpr unsigned kMaximumInsertions = 7;

struct Insertion {
  // Original list index. before=false preserves the original after-list API.
  UINT after_list = 0;
  ID3D12CommandList* list = nullptr;
  bool before = false;
};

enum class Refusal { oversized_batch, invalid_batch, reentrant_submission, contended_submission };

struct Callbacks {
  void* context = nullptr;
  // Snapshot recording generations and retain their capture state here. Return
  // an opaque nonzero receipt for owned capture work, or zero for no such work.
  std::uint64_t (*before)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept = nullptr;
  // Called only AFTER the exact original ExecuteCommandLists returns. It may
  // enqueue Queue::Signal and publish that receipt's pending fence value. A
  // successful queue Signal is submission evidence, not GPU completion.
  // It may submit its own private compositor lists: nested Execute calls forward
  // directly without observation. A private tail capture may be submitted only
  // after attaching its ownership to this outer receipt; the outer Signal must
  // cover it. Nested submissions receive no independent capture receipt.
  void (*after)(void*, ID3D12CommandQueue*, std::uint64_t receipt) noexcept = nullptr;
  // No completion is reported on a refused observation. Invalidate outstanding
  // capture publication for this queue, retaining referenced GPU resources.
  void (*refused)(void*, ID3D12CommandQueue*, Refusal) noexcept = nullptr;
  // Optional nonblocking notification BEFORE a contended batch is forwarded.
  // Exact live list arguments permit scoped invalidation before Reset/reuse can
  // retire its recording. Replaces refused for this path only; must not wait.
  std::uint64_t (*contended)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept = nullptr;
  // Paired even for a zero token, after the exact original batch returns.
  void (*contended_completed)(void*, ID3D12CommandQueue*, std::uint64_t token) noexcept = nullptr;
  // Optional owned work inside the SAME native Execute call, after a nonzero
  // before receipt. Return at most capacity insertions, ordered by original
  // index and then before/after: 2*after_list + (before ? 0 : 1).
  // Every inserted list must be closed, DIRECT, on this device, distinct from
  // every original/inserted list, and retained by this receipt BEFORE returning.
  // The owner proves exact target state/lifetime and a legal whole-batch pass
  // boundary. This callback must neither submit work nor wait. A malformed or
  // reentrant plan is ignored in full; normal receipt retirement still applies.
  UINT (*augment)(void*, ID3D12CommandQueue*, std::uint64_t receipt, UINT, ID3D12CommandList* const*, Insertion*, UINT capacity) noexcept =
      nullptr;
  // Optional metadata notification before native forwarding, paired with each
  // augment call. Zero means its plan was not used. Retain planned resources
  // conservatively through the outer receipt even when insertion is refused.
  // Must not submit work, change the plan's lifetime or wait.
  void (*augmentation_result)(void*, ID3D12CommandQueue*, std::uint64_t receipt, UINT inserted) noexcept = nullptr;
  // Optional notification after a batch whose before returned zero has been
  // forwarded. Lets before publish post-forward invalidation for a batch it
  // could not order within its wait budget. Must not wait or submit.
  void (*forwarded_unordered)(void*, ID3D12CommandQueue*) noexcept = nullptr;
};

enum class Status {
  registered,
  already_registered,
  disabled,
  not_registered,
  removed,
  not_installed,
  invalid_argument,
  invalid_queue_memory,
  invalid_slot_memory,
  invalid_original,
  different_vtable,
  queue_limit,
  callback_mismatch,
  slot_changed,
  installation_consumed,
  pin_failed,
  protection_change_failed,
  protection_restore_failed,
  protection_restored,
  reentrant_operation,
};

struct Result {
  Status status = Status::not_installed;
  DWORD windows_error = ERROR_SUCCESS;
  bool hook_installed = false;
  bool queue_retained = false;
  bool protection_restored = true;
};

struct Statistics {
  std::uint64_t submissions = 0;
  std::uint64_t receipts = 0;
  std::uint64_t refusals = 0;
  // Every wrapper entry for a registered queue, including disabled, contended
  // and unrelated forwards: a presentation pulse independent of capture state.
  std::uint64_t calls = 0;
  // Another thread owned this queue's submit lock: Present/frame-generation
  // helpers or other injectors submitting while an observed batch was in flight.
  std::uint64_t contended = 0;
};

// Current-process public COM objects supplied by the caller only. Installs one
// shared native ID3D12CommandQueue vtable slot10, without copying a vtable or
// editing executable instructions. At most8 queue identities on that same
// vtable can be registered. Unknown queues sharing it forward unchanged.
//
// The queue MUST be a live, valid native COM interface, not an unverified interface wrapper.
// Registration keeps one AddRef permanently and pins this module, the original
// function's image and the vtable's image when applicable. Once queue_retained
// is true, including a partial failure, the callback/context association is
// immutable; the caller must retain its context until process exit. There is
// no live unloading, queue-reference release or executable-code allocation.
// Failed registration retains no queue reference unless queue_retained is true.
//
// Observed before/original/after stay serialized on that queue so a Wait queued
// in before always has a later Signal. A thread that cannot take the submit lock
// immediately forwards the original batch without waiting: Present and
// frame-generation helpers must not block on Taxi Cam. That contended path
// pairs contended/contended_completed when supplied, or reports the legacy
// Refusal::contended_submission after forwarding. Callbacks must not wait;
// source invalidation belongs after forwarding, lifetime marks may precede it.
// Unrelated observed batches (before returns zero) release the lock before the
// original ExecuteCommandLists. Normally the wrapper forwards the original
// queue/count/list-array exactly once, including every refused path. A validated
// augment plan uses one bounded extended array for that SAME single call; every
// original list remains once at its original relative position. Calls are never
// split, and the application's array is never modified.
// Notifications must not throw, call registration/removal APIs or take locks
// that can be held by their callers. Only after may submit private compositor
// or explicitly owned tail work as described above; a nested before/original
// submission reports reentrant_submission. The original public COM method uses
// its documented Win64 ABI. This is not an SEH fault barrier or a guarantee
// that invalid application submissions execute.
Result register_queue(ID3D12CommandQueue* queue, const Callbacks& callbacks) noexcept;

// Optional native bootstrap discovery. Called outside the submission/registry locks,
// before observation begins, with the application's live queue argument. The callback
// may register a DIRECT queue. No default means the original fail-closed behavior.
// Registering that same queue from the callback also succeeds while another
// tool's hook holds the slot: the call being discovered came through it.
using UnknownQueueObserver = void (*)(ID3D12CommandQueue*) noexcept;
bool set_unknown_queue_observer(UnknownQueueObserver) noexcept;

// Disables notifications and waits only for an observed wrapper that still holds
// this queue's submit lock. Contended and unrelated forwards do not hold it.
// Does not wait for the GPU or release anything. Repeating registration with
// the identical callbacks may enable this retained queue.
Result disable_queue(ID3D12CommandQueue* queue) noexcept;

// Stops notifications and exchanges only our slot value for the saved original.
// A successful removal permanently consumes this installation. This does not
// prove quiescence: a thread may already have fetched the wrapper address.
// All retained modules, contexts and native queue references remain alive.
Result remove() noexcept;
Result restore_protection() noexcept;
Statistics statistics(ID3D12CommandQueue* queue) noexcept;
// Sum over every registered queue. Lock-free; safe from any thread.
Statistics total_statistics() noexcept;
const char* status_name(Status status) noexcept;

}  // namespace taxi_camera::engine_hook::queue_submit
