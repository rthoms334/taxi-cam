#include "render_boundary_observer.hpp"
#include "../graphics/native_device_identity.hpp"
#include "../shared/hook_timing.hpp"
#include <array>
#include <atomic>
#include <unordered_map>
#ifdef TAXI_RENDER_BOUNDARY_VALIDATION
extern "C" BOOL taxi_boundary_test_virtual_protect(void*, SIZE_T, DWORD, PDWORD) noexcept;
extern "C" SIZE_T taxi_boundary_test_virtual_query(const void*, MEMORY_BASIC_INFORMATION*, SIZE_T) noexcept;
extern "C" BOOL taxi_boundary_test_read_process_memory(HANDLE, const void*, void*, SIZE_T, SIZE_T*) noexcept;
#endif

namespace taxi_camera::engine_hook::render_boundary {
namespace {
using Legacy = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using Begin = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,
                                       UINT,
                                       const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                       const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                       D3D12_RENDER_PASS_FLAGS);
using End = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
using Enhanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT, const D3D12_BARRIER_GROUP*);
using Draw = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using CopyResource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using CopyTexture = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                             const D3D12_TEXTURE_COPY_LOCATION*,
                                             UINT,
                                             UINT,
                                             UINT,
                                             const D3D12_TEXTURE_COPY_LOCATION*,
                                             const D3D12_BOX*);
struct Slot {
  UINT index;
  void** address = nullptr;
  void* forward = nullptr;
  void* wrapper = nullptr;
  bool installed = false;
};
struct Identity {
  std::uint64_t generation = 0;
  bool active = false, suspended = false, invalid = false, prior_work = false, pass_closed = false;
};
// D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_DEPTH (0x8) and
// D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_STENCIL (0x10) are legal Agility SDK
// flags. ReShade forwards them unchanged. Bits outside this mask still
// invalidate the recording. PRESERVE_LOCAL access types stay invalid.
constexpr UINT known_render_pass_flags = static_cast<UINT>(D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES) |
                                         static_cast<UINT>(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS) |
                                         static_cast<UINT>(D3D12_RENDER_PASS_FLAG_RESUMING_PASS) | 0x8u | 0x10u;
SRWLOCK control_lock = SRWLOCK_INIT;
// Identities are split by list pointer so recording threads, which each record
// different lists, do not share one lock word on every draw. A key always maps
// to the same shard; whole-registry changes take every shard in index order.
struct alignas(64) Shard {
  SRWLOCK lock = SRWLOCK_INIT;
  std::unordered_map<ID3D12GraphicsCommandList*, Identity> identities;
  // Advanced before every exclusive release of this shard, so a reader that
  // sees the same value before and after a call knows none of its identities
  // changed in between. Its own line: readers poll it on every hook call.
  // Starts at 1; 0 means "not read from this shard".
  alignas(64) std::atomic<std::uint64_t> changes{1};
};
std::array<Shard, 16> shards;
constexpr std::size_t kMaximumIdentities = 8192;
std::atomic<std::size_t> identity_count{0};
Shard& shard_for(const ID3D12GraphicsCommandList* key) noexcept {
  const auto hash = (reinterpret_cast<std::uintptr_t>(key) >> 4) * 0x9e3779b97f4a7c15ull;
  return shards[(hash >> 60) & 15];
}
Callbacks callbacks;
// Read on every hook call. Kept off the lines of the counters below, which
// every thread's barrier and copy hooks write.
alignas(64) std::atomic<bool> enabled{false};
std::atomic<Legacy> original_legacy{nullptr};
std::atomic<Begin> original_begin{nullptr};
std::atomic<End> original_end{nullptr};
std::atomic<Enhanced> original_enhanced{nullptr};
std::atomic<Draw> original_draw{nullptr};
std::atomic<DrawIndexed> original_draw_indexed{nullptr};
std::atomic<CopyResource> original_copy_resource{nullptr};
std::atomic<CopyTexture> original_copy_texture{nullptr};
// Diagnostic counters written from every recording thread, on lines of their
// own so the writes do not evict the read-mostly state above.
struct alignas(64) Counters {
  std::atomic<std::uint64_t> legacy_calls{0}, enhanced_calls{0}, legacy_candidates{0}, enhanced_candidates{0}, pass_refusals{0},
      batch_refusals{0};
  std::atomic<std::uint64_t> copy_resource_calls{0}, copy_texture_calls{0}, metadata_truncated_calls{0}, maximum_legacy_batch{0};
};
Counters counters;
auto& legacy_calls = counters.legacy_calls;
auto& enhanced_calls = counters.enhanced_calls;
auto& legacy_candidates = counters.legacy_candidates;
auto& enhanced_candidates = counters.enhanced_candidates;
auto& pass_refusals = counters.pass_refusals;
auto& batch_refusals = counters.batch_refusals;
auto& copy_resource_calls = counters.copy_resource_calls;
auto& copy_texture_calls = counters.copy_texture_calls;
auto& metadata_truncated_calls = counters.metadata_truncated_calls;
auto& maximum_legacy_batch = counters.maximum_legacy_batch;
std::array<Slot, 8> slots{{{26}, {68}, {69}, {80}, {12}, {13}, {16}, {17}}};
struct ActiveEndSlot {
  void* table = nullptr;
  Slot slot{69};
};
std::array<ActiveEndSlot, 8> active_end_slots{};
std::array<std::atomic<End>, 8> original_active_ends{};
std::size_t active_end_count = 0;
void* saved_table = nullptr;
void** pending_slot = nullptr;
DWORD pending_access = 0;
bool configured = false, removed = false;
thread_local bool inside = false;
struct Lock {
  SRWLOCK& lock;
  bool exclusive;
  Lock(SRWLOCK& value, bool write) : lock(value), exclusive(write) {
    if (write)
      AcquireSRWLockExclusive(&lock);
    else
      AcquireSRWLockShared(&lock);
  }
  ~Lock() {
    if (exclusive)
      ReleaseSRWLockExclusive(&lock);
    else
      ReleaseSRWLockShared(&lock);
  }
};
// One identity shard. Every exclusive release advances the shard's changes first.
struct ShardLock {
  Shard& shard;
  bool exclusive;
  ShardLock(Shard& value, bool write) : shard(value), exclusive(write) {
    if (write)
      AcquireSRWLockExclusive(&shard.lock);
    else
      AcquireSRWLockShared(&shard.lock);
  }
  ~ShardLock() {
    if (exclusive) {
      shard.changes.fetch_add(1, std::memory_order_release);
      ReleaseSRWLockExclusive(&shard.lock);
    } else {
      ReleaseSRWLockShared(&shard.lock);
    }
  }
};
// Exclusive over every shard, acquired in index order.
struct AllShardsLock {
  AllShardsLock() {
    for (auto& shard : shards)
      AcquireSRWLockExclusive(&shard.lock);
  }
  ~AllShardsLock() {
    for (auto& shard : shards)
      shard.changes.fetch_add(1, std::memory_order_release);
    for (auto it = shards.rbegin(); it != shards.rend(); ++it)
      ReleaseSRWLockExclusive(&it->lock);
  }
};
// Runs f(identities, iterator) with key's shard locked; iterator may be end().
template <class F>
auto with_identity(ID3D12GraphicsCommandList* key, bool exclusive, F&& f) {
  auto& shard = shard_for(key);
  const ShardLock lock(shard, exclusive);
  return f(shard.identities, shard.identities.find(key));
}
// ReShade's command-list proxy and the native list are different pointers.
// IID_UnwrappedObject returns the native object and AddRefs it. The test fake
// returns the same pointer without AddRef, so that result is not released.
// The pointer is a map key only; no method is called on it after Release.
// Callers must pass a live COM object. Registry entry points use registry_key.
ID3D12GraphicsCommandList* identity_key(ID3D12GraphicsCommandList* list) noexcept {
  if (!list)
    return nullptr;
  IUnknown* unwrapped = nullptr;
  const HRESULT hr = list->QueryInterface(taxi_camera::UnwrappedObjectId, reinterpret_cast<void**>(&unwrapped));
  if (FAILED(hr) || !unwrapped || unwrapped == static_cast<IUnknown*>(list)) {
    if (unwrapped && unwrapped != static_cast<IUnknown*>(list))
      unwrapped->Release();
    return list;
  }
  auto* native = static_cast<ID3D12GraphicsCommandList*>(unwrapped);
  unwrapped->Release();
  return native;
}
Identity find_identity(ID3D12GraphicsCommandList* key, bool& found) noexcept {
  return with_identity(key, false, [&](auto& identities, auto it) {
    found = it != identities.end();
    return found ? it->second : Identity{};
  });
}
// Identities this thread read directly under its own list pointer, with the
// shard's change count at that read. A copy is current exactly while that count
// is unchanged: every identity write happens under the exclusive shard lock and
// advances the count before release, and the count read under the shared lock
// cannot move until that lock is released. Only direct hits are kept; a proxy
// argument's identity lives under another key and takes the checked path.
struct CachedIdentity {
  ID3D12GraphicsCommandList* list = nullptr;
  std::uint64_t changes = 0;
  Identity value{};
};
thread_local std::array<CachedIdentity, 4> cached_identities{};
thread_local unsigned next_cached_identity = 0;
// Registered keys are unwrapped native lists, and hooks receive native lists,
// so an entry under the argument itself needs no unwrap. Only a miss pays for
// QueryInterface, which finds the native list behind a proxy argument.
// changes receives the list's shard count for a direct hit, else 0.
Identity lookup(ID3D12GraphicsCommandList* list, std::uint64_t& changes) noexcept {
  auto& shard = shard_for(list);
  const auto now = shard.changes.load(std::memory_order_acquire);
  for (const auto& entry : cached_identities)
    if (entry.list == list && entry.changes == now) {
      changes = now;
      return entry.value;
    }
  changes = 0;
  bool found = false;
  Identity direct{};
  std::uint64_t observed = 0;
  {
    const ShardLock lock(shard, false);
    const auto it = shard.identities.find(list);
    found = it != shard.identities.end();
    if (found) {
      direct = it->second;
      observed = shard.changes.load(std::memory_order_relaxed);
    }
  }
  if (found) {
    cached_identities[next_cached_identity++ % cached_identities.size()] = {list, observed, direct};
    changes = observed;
    return direct;
  }
  const auto key = identity_key(list);
  return key == list ? Identity{} : find_identity(key, found);
}
Identity lookup(ID3D12GraphicsCommandList* list) noexcept {
  std::uint64_t changes = 0;
  return lookup(list, changes);
}
void disable_capture() noexcept {
  enabled.store(false, std::memory_order_release);
  // A failed/changed hook may have missed pass calls. Never revive the previous
  // recording's empty-pass assumption when a later registration repairs slots.
  const AllShardsLock lock;
  for (auto& shard : shards)
    for (auto& entry : shard.identities)
      entry.second.invalid = true;
}
bool same_safe(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  const auto current = lookup(list);
  return enabled.load(std::memory_order_acquire) && generation && current.generation == generation && !current.active &&
         !current.suspended && !current.invalid && current.prior_work;
}
struct Guard {
  Guard() { inside = true; }
  ~Guard() { inside = false; }
};
bool same_identity(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  return generation && lookup(list).generation == generation;
}
std::uint32_t scope_flags(const Identity& value) noexcept {
  return (enabled.load(std::memory_order_acquire) ? ScopeEnabled : 0u) | (value.active ? ScopeActivePass : 0u) |
         (value.suspended ? ScopeSuspendedPass : 0u) | (value.invalid ? ScopeInvalidRecording : 0u) |
         (value.prior_work ? ScopePriorGpuWork : 0u);
}
std::uint32_t scope_invalidation(const Identity& value) noexcept {
  return (!enabled.load(std::memory_order_acquire) ? InvalidationObserverDisabled : 0u) |
         ((value.suspended || value.invalid) ? InvalidationPassState : 0u);
}
void notify_invalidation(ID3D12GraphicsCommandList* list, std::uint64_t generation, std::uint32_t reasons) noexcept {
  if (reasons && callbacks.recording_invalidated && same_identity(list, generation))
    callbacks.recording_invalidated(callbacks.context, list, generation, reasons);
}
// Keep optional client cache scopes balanced even when identity changes during
// metadata callbacks. The guard is destroyed before any injection or native call.
struct MetadataScope {
  ID3D12GraphicsCommandList* list;
  std::uint64_t generation;
  void* context;
  void (*end)(void*, ID3D12GraphicsCommandList*, std::uint64_t) noexcept;
  MetadataScope(ID3D12GraphicsCommandList* value, std::uint64_t id) noexcept
      : list(value),
        generation(id),
        context(callbacks.context),
        end(callbacks.metadata_begin && callbacks.metadata_end ? callbacks.metadata_end : nullptr) {
    if (end)
      callbacks.metadata_begin(context, list, generation);
  }
  ~MetadataScope() {
    if (end)
      end(context, list, generation);
  }
};
bool global_uncertainty(std::uint32_t reasons) noexcept {
  return (reasons & ~(InvalidationPassBegin | InvalidationSplitBarrier | InvalidationAliasOrDiscard)) != 0;
}
bool allowed(const Identity& value) noexcept {
  if (!enabled.load(std::memory_order_acquire))
    return false;
  if (!value.generation)
    return false;
  if (value.active || value.suspended || value.invalid || !value.prior_work) {
    ++pass_refusals;
    return false;
  }
  return true;
}
void observe_work(ID3D12GraphicsCommandList* list) noexcept {
  if (!enabled.load(std::memory_order_acquire))
    return;
  const auto identity = lookup(list);
  if (!identity.generation || identity.prior_work || identity.active || identity.suspended || identity.invalid)
    return;
  const auto key = identity_key(list);
  with_identity(key, true, [&](auto& identities, auto it) {
    if (it != identities.end() && it->second.generation == identity.generation && !it->second.active && !it->second.suspended &&
        !it->second.invalid)
      it->second.prior_work = true;
  });
}
// A direct identity read before the draw is still current unless its shard
// changed while the draw ran. That saves a second lookup per draw.
void after_draw_work(ID3D12GraphicsCommandList* list, const Identity& before, std::uint64_t changes) noexcept {
  const auto generation = before.generation;
  const auto current = changes && shard_for(list).changes.load(std::memory_order_acquire) == changes ? before : lookup(list);
  if (!generation || current.generation != generation)
    return;
  if (!current.prior_work)
    observe_work(list);
  const auto reasons = scope_invalidation(current);
  if (reasons) {
    notify_invalidation(list, generation, reasons);
    if (!same_identity(list, generation))
      return;
  }
  if (callbacks.after_draw)
    callbacks.after_draw(callbacks.context, list, generation,
                         enabled.load(std::memory_order_acquire) && !current.active && !current.suspended && !current.invalid);
}
void STDMETHODCALLTYPE
draw(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) noexcept {
  const auto original = original_draw.load(std::memory_order_acquire);
  if (inside) {
    original(list, vertices, instances, first_vertex, first_instance);
    return;
  }
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::draw);
  std::uint64_t changes = 0;
  const auto identity = lookup(list, changes);
  hook_timing::forward(original, list, vertices, instances, first_vertex, first_instance);
  if (vertices && instances)
    after_draw_work(list, identity, changes);
}
void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* list,
                                    UINT indices,
                                    UINT instances,
                                    UINT first_index,
                                    INT vertex_offset,
                                    UINT first_instance) noexcept {
  const auto original = original_draw_indexed.load(std::memory_order_acquire);
  if (inside) {
    original(list, indices, instances, first_index, vertex_offset, first_instance);
    return;
  }
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::draw);
  std::uint64_t changes = 0;
  const auto identity = lookup(list, changes);
  hook_timing::forward(original, list, indices, instances, first_index, vertex_offset, first_instance);
  if (indices && instances && identity.generation && callbacks.draw_indexed_repeats && enabled.load(std::memory_order_acquire) &&
      !identity.invalid) {
    const UINT repeats = (std::min)(callbacks.draw_indexed_repeats(callbacks.context, list, identity.generation, indices, instances,
                                                                   first_index, vertex_offset, first_instance),
                                    63u);
    for (UINT n = 0; n < repeats; ++n)
      original(list, indices, instances, first_index, vertex_offset, first_instance);
  }
  if (indices && instances)
    after_draw_work(list, identity, changes);
}
bool previous_legacy_change(UINT index, const D3D12_RESOURCE_BARRIER* barriers, ID3D12Resource* resource) noexcept {
  for (UINT n = 0; n < index; ++n) {
    if (barriers[n].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
      return true;
    if (barriers[n].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barriers[n].Transition.pResource == resource)
      return true;
  }
  return false;
}
void STDMETHODCALLTYPE copy_resource(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source) noexcept {
  const auto original = original_copy_resource.load(std::memory_order_acquire);
  if (inside) {
    original(list, destination, source);
    return;
  }
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::copy);
  const auto identity = lookup(list);
  notify_invalidation(list, identity.generation, scope_invalidation(identity));
  hook_timing::forward(original, list, destination, source);
  if (!same_identity(list, identity.generation))
    return;
  ++copy_resource_calls;
  observe_work(list);
  if (callbacks.after_copy_resource)
    callbacks.after_copy_resource(callbacks.context, list, identity.generation, destination, source, same_safe(list, identity.generation));
}
void STDMETHODCALLTYPE copy_texture(ID3D12GraphicsCommandList* list,
                                    const D3D12_TEXTURE_COPY_LOCATION* destination,
                                    UINT x,
                                    UINT y,
                                    UINT z,
                                    const D3D12_TEXTURE_COPY_LOCATION* source,
                                    const D3D12_BOX* box) noexcept {
  const auto original = original_copy_texture.load(std::memory_order_acquire);
  if (inside) {
    original(list, destination, x, y, z, source, box);
    return;
  }
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::copy);
  const auto identity = lookup(list);
  notify_invalidation(list, identity.generation, scope_invalidation(identity));
  hook_timing::forward(original, list, destination, x, y, z, source, box);
  if (!same_identity(list, identity.generation))
    return;
  ++copy_texture_calls;
  observe_work(list);
  if (callbacks.after_copy_texture)
    callbacks.after_copy_texture(callbacks.context, list, identity.generation, destination, x, y, z, source, box,
                                 same_safe(list, identity.generation));
}
// Only called after a complete bounded metadata scan excludes global uncertainty.
// This adds O(2*N) comparisons and never uses the generic quadratic prefix scan.
void selected_legacy(ID3D12GraphicsCommandList* list,
                     std::uint64_t generation,
                     UINT count,
                     const D3D12_RESOURCE_BARRIER* barriers) noexcept {
  if (!callbacks.selected_legacy_targets || !same_safe(list, generation)) {
    ++batch_refusals;
    return;
  }
  std::array<ID3D12Resource*, 2> targets{};
  const UINT selected = callbacks.selected_legacy_targets(callbacks.context, list, generation, targets.data(), 2);
  if (!selected || selected > targets.size() || !targets[0] || (selected == 2 && (!targets[1] || targets[0] == targets[1])) ||
      !same_safe(list, generation)) {
    ++batch_refusals;
    return;
  }
  std::array<bool, 2> seen{};
  UINT remaining = selected;
  for (UINT n = 0; n < count && remaining; ++n) {
    const auto& b = barriers[n];
    // Preserve the generic prefix guard: even another resource's alias has
    // unknown heap overlap. NULL aliases likewise refuse the remaining targets.
    if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
      ++batch_refusals;
      break;
    }
    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
      continue;
    for (UINT target = 0; target < selected; ++target) {
      if (seen[target] || b.Transition.pResource != targets[target])
        continue;
      seen[target] = true;
      --remaining;
      // Every first transition consumes this target, including splits or other
      // states/subresources; a later RT exit cannot assume the pre-batch state.
      if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE || b.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
          b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET ||
          (b.Transition.Subresource != 0 && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
        continue;
      if (!same_safe(list, generation))
        return;
      ++legacy_candidates;
      callbacks.before_legacy(callbacks.context, list, generation, b.Transition);
    }
  }
}
void STDMETHODCALLTYPE legacy(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) noexcept {
  const auto original = original_legacy.load(std::memory_order_acquire);
  if (inside) {
    original(list, count, barriers);
    return;
  }
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::barrier);
  const auto identity = lookup(list);
  if (identity.generation) {
    ++legacy_calls;
    auto maximum = maximum_legacy_batch.load(std::memory_order_relaxed);
    while (count > maximum && !maximum_legacy_batch.compare_exchange_weak(maximum, count, std::memory_order_relaxed)) {
    }
  }
  auto uncertainty = scope_invalidation(identity);
  const bool metadata_complete =
      !count || (barriers && count <= maximum_legacy_metadata_barriers &&
                 reinterpret_cast<std::uintptr_t>(barriers) <= UINTPTR_MAX - std::uint64_t(count) * sizeof(*barriers));
  if (!metadata_complete) {
    uncertainty |= InvalidationBarrierBatch;
    if (identity.generation)
      ++metadata_truncated_calls;
  }
  if (identity.generation && metadata_complete && barriers) {
    const auto count_observed = count;
    for (UINT n = 0; n < count_observed; ++n) {
      const auto& b = barriers[n];
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
        uncertainty |= InvalidationAliasOrDiscard;
      else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        if (b.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY || b.Flags == D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)
          uncertainty |= InvalidationSplitBarrier;
        else if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
          uncertainty |= InvalidationBarrierBatch;
        if (!b.Transition.pResource)
          uncertainty |= InvalidationBarrierBatch;
      } else if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_UAV)
        uncertainty |= InvalidationBarrierBatch;
      if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
        uncertainty |= InvalidationBarrierBatch;
    }
  }
  if (identity.generation && metadata_complete && callbacks.observe_legacy && barriers) {
    const MetadataScope metadata(list, identity.generation);
    const auto count_observed = count;
    const auto scope = scope_flags(identity) | (global_uncertainty(uncertainty) ? ScopeInvalidRecording : 0u);
    for (UINT n = 0; n < count_observed; ++n) {
      if (!same_identity(list, identity.generation))
        break;
      callbacks.observe_legacy(callbacks.context, list, identity.generation, barriers[n], scope);
    }
  }
  // Invalidate after metadata delivery so a partial/uncertain batch cannot
  // leave a seemingly complete state from its last observed prefix element.
  notify_invalidation(list, identity.generation, uncertainty);
  if (allowed(identity)) {
    if (!metadata_complete || global_uncertainty(uncertainty) || !barriers || !count)
      ++batch_refusals;
    else if (count > 256)
      selected_legacy(list, identity.generation, count, barriers);
    else
      for (UINT n = 0; n < count; ++n) {
        const auto& b = barriers[n];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE || !b.Transition.pResource ||
            (b.Transition.Subresource != 0 && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
          continue;
        const bool target_exit =
            b.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && b.Transition.StateAfter != D3D12_RESOURCE_STATE_RENDER_TARGET;
        const bool uav_exit = callbacks.before_legacy_uav && b.Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
                              b.Transition.StateAfter != D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!target_exit && !uav_exit)
          continue;
        if (previous_legacy_change(n, barriers, b.Transition.pResource)) {
          ++batch_refusals;
          continue;
        }
        if (!same_safe(list, identity.generation))
          break;
        ++legacy_candidates;
        if (target_exit)
          callbacks.before_legacy(callbacks.context, list, identity.generation, b.Transition);
        else
          callbacks.before_legacy_uav(callbacks.context, list, identity.generation, b.Transition);
      }
  }
  // Preserve the exact count, pointer, order and flags; no replay or splitting.
  hook_timing::forward(original, list, count, barriers);
}
bool install_active_end(ID3D12GraphicsCommandList4* list) noexcept;
void STDMETHODCALLTYPE begin(ID3D12GraphicsCommandList4* list,
                             UINT count,
                             const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                             const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,
                             D3D12_RENDER_PASS_FLAGS flags) noexcept {
  const auto original = original_begin.load(std::memory_order_acquire);
  if (inside) {
    original(list, count, targets, depth, flags);
    return;
  }
  const auto key = identity_key(list);
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::render_pass);
  const auto identity = lookup(key);
  bool ordinary_access = false;
  if (identity.generation) {
    auto& shard = shard_for(key);
    const ShardLock lock(shard, true);
    auto& identities = shard.identities;
    const auto it = identities.find(key);
    if (it != identities.end() && it->second.generation == identity.generation) {
      auto& value = it->second;
      if (value.active)
        value.invalid = true;
      if ((static_cast<UINT>(flags) & ~known_render_pass_flags) != 0)
        value.invalid = true;
      // PRESERVE_LOCAL passes permit state setup between passes, but no added
      // GPU operations. Refuse their entire recording rather than inferring a
      // resource's local-access lifetime from later barrier state alone.
      ordinary_access = count <= 8 && (!count || targets);
      if (ordinary_access)
        for (UINT n = 0; n < count; ++n)
          if (static_cast<UINT>(targets[n].BeginningAccess.Type) > 3 || static_cast<UINT>(targets[n].EndingAccess.Type) > 3)
            ordinary_access = false;
      if (depth && (static_cast<UINT>(depth->DepthBeginningAccess.Type) > 3 || static_cast<UINT>(depth->DepthEndingAccess.Type) > 3 ||
                    static_cast<UINT>(depth->StencilBeginningAccess.Type) > 3 || static_cast<UINT>(depth->StencilEndingAccess.Type) > 3))
        ordinary_access = false;
      if (!ordinary_access)
        value.invalid = true;
      // A resuming pass can continue another command list. Only its completed,
      // nonsuspending End permits capture again; no intervening copy is added.
      if (value.suspended && (flags & D3D12_RENDER_PASS_FLAG_RESUMING_PASS) == 0)
        value.invalid = true;
      value.active = true;
      value.pass_closed = false;
      value.suspended = (flags & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS) != 0;
    }
  }
  if (identity.generation && callbacks.pass_began)
    callbacks.pass_began(callbacks.context, key, identity.generation, flags, ordinary_access);
  if (identity.generation && callbacks.pass_targets)
    callbacks.pass_targets(callbacks.context, key, identity.generation, count, targets, depth);
  const auto after_begin_state = lookup(key);
  notify_invalidation(key, identity.generation,
                      InvalidationPassBegin | scope_invalidation(identity) |
                          ((after_begin_state.invalid || after_begin_state.suspended || (flags & D3D12_RENDER_PASS_FLAG_RESUMING_PASS) != 0)
                               ? InvalidationPassState
                               : 0u));
  hook_timing::forward(original, list, count, targets, depth, flags);
  // The native runtime can replace this object's vtable for the active pass.
  // Observe and patch only its actual End slot before returning to the caller.
  // Different active tables keep distinct immutable originals and trampolines.
  if (identity.generation && !install_active_end(list))
    disable_capture();
}
void end_impl(ID3D12GraphicsCommandList4* list, End original) noexcept {
  if (inside) {
    original(list);
    return;
  }
  const hook_timing::Scope timing(hook_timing::render_pass);
  const auto key = identity_key(list);
  const Guard guard;
  const auto identity = lookup(key);
  // ReShade's proxy End and the active-pass vtable End are separate calls.
  // A repeated End of an ordinary pass that already closed is not PassState.
  // An End with no Begin still invalidates: pass_closed is false.
  if (identity.generation && identity.pass_closed && !identity.active && !identity.suspended && !identity.invalid &&
      enabled.load(std::memory_order_acquire)) {
    hook_timing::forward(original, list);
    return;
  }
  if (!identity.active || identity.suspended || identity.invalid || !enabled.load(std::memory_order_acquire))
    notify_invalidation(key, identity.generation, InvalidationPassState | scope_invalidation(identity));
  hook_timing::forward(original, list);
  if (identity.generation && callbacks.pass_ended)
    callbacks.pass_ended(callbacks.context, key, identity.generation);
  // End access can discard/resolve; no image is copied here and no RTV binding
  // is presumed to survive. Only later actual transitions prove layout/state.
  if (identity.generation) {
    auto& shard = shard_for(key);
    const ShardLock lock(shard, true);
    auto& identities = shard.identities;
    const auto it = identities.find(key);
    if (it != identities.end() && it->second.generation == identity.generation) {
      if (!it->second.active)
        it->second.invalid = true;
      it->second.active = false;
      it->second.pass_closed = true;
      if (!it->second.invalid && !it->second.suspended)
        it->second.prior_work = true;
    }
  }
}
void STDMETHODCALLTYPE end(ID3D12GraphicsCommandList4* list) noexcept {
  end_impl(list, original_end.load(std::memory_order_acquire));
}
template <std::size_t Index>
void STDMETHODCALLTYPE active_end(ID3D12GraphicsCommandList4* list) noexcept {
  end_impl(list, original_active_ends[Index].load(std::memory_order_acquire));
}
bool texture_batches(UINT count, const D3D12_BARRIER_GROUP* groups, UINT max_groups = 16, UINT max_barriers = 256) noexcept {
  if (!count || count > max_groups || !groups)
    return false;
  UINT total = 0;
  for (UINT n = 0; n < count; ++n) {
    if (groups[n].NumBarriers > max_barriers - total)
      return false;
    total += groups[n].NumBarriers;
    switch (groups[n].Type) {
      case D3D12_BARRIER_TYPE_TEXTURE:
        if (groups[n].NumBarriers && !groups[n].pTextureBarriers)
          return false;
        break;
      case D3D12_BARRIER_TYPE_BUFFER:
        if (groups[n].NumBarriers && !groups[n].pBufferBarriers)
          return false;
        break;
      case D3D12_BARRIER_TYPE_GLOBAL:
        if (groups[n].NumBarriers && !groups[n].pGlobalBarriers)
          return false;
        break;
      default:
        return false;
    }
  }
  return true;
}
bool previous_enhanced_change(UINT group_index, UINT index, const D3D12_BARRIER_GROUP* groups, ID3D12Resource* resource) noexcept {
  for (UINT group = 0; group <= group_index; ++group) {
    if (groups[group].Type != D3D12_BARRIER_TYPE_TEXTURE)
      continue;
    const UINT limit = group == group_index ? index : groups[group].NumBarriers;
    for (UINT n = 0; n < limit; ++n) {
      const auto& b = groups[group].pTextureBarriers[n];
      // Enhanced alias activation uses discard. Its heap overlap is not known,
      // so even a discard of another resource prevents assuming our pre-batch state.
      if (b.pResource == resource || (b.Flags & D3D12_TEXTURE_BARRIER_FLAG_DISCARD) != 0)
        return true;
    }
  }
  return false;
}
bool sole_subresource(const D3D12_BARRIER_SUBRESOURCE_RANGE& range) noexcept {
  // Native NumMipLevels==0 is the index form; all other members are ignored.
  if (!range.NumMipLevels)
    return range.IndexOrFirstMipLevel == 0 || range.IndexOrFirstMipLevel == UINT_MAX;
  return range.IndexOrFirstMipLevel == 0 && range.NumMipLevels == 1 && range.FirstArraySlice == 0 && range.NumArraySlices == 1 &&
         range.FirstPlane == 0 && range.NumPlanes == 1;
}
void STDMETHODCALLTYPE enhanced(ID3D12GraphicsCommandList7* list, UINT count, const D3D12_BARRIER_GROUP* groups) noexcept {
  const auto original = original_enhanced.load(std::memory_order_acquire);
  if (inside) {
    original(list, count, groups);
    return;
  }
  const Guard guard;
  const hook_timing::Scope timing(hook_timing::barrier);
  const auto identity = lookup(list);
  if (identity.generation && callbacks.enhanced_call)
    callbacks.enhanced_call(callbacks.context, list, identity.generation);
  if (identity.generation)
    ++enhanced_calls;
  auto uncertainty = scope_invalidation(identity);
  const bool metadata_complete = texture_batches(count, groups, 64, 4096);
  if (count && !metadata_complete)
    uncertainty |= InvalidationBarrierBatch;
  if (identity.generation && metadata_complete) {
    for (UINT g = 0; g < count; ++g) {
      if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE)
        continue;
      for (UINT n = 0; n < groups[g].NumBarriers; ++n) {
        const auto& b = groups[g].pTextureBarriers[n];
        if ((b.SyncBefore & D3D12_BARRIER_SYNC_SPLIT) != 0 || (b.SyncAfter & D3D12_BARRIER_SYNC_SPLIT) != 0)
          uncertainty |= InvalidationSplitBarrier;
        if (b.Flags == D3D12_TEXTURE_BARRIER_FLAG_DISCARD)
          uncertainty |= InvalidationAliasOrDiscard;
        else if (b.Flags != D3D12_TEXTURE_BARRIER_FLAG_NONE)
          uncertainty |= InvalidationBarrierBatch;
        if (!b.pResource)
          uncertainty |= InvalidationBarrierBatch;
      }
    }
  }
  if (identity.generation && callbacks.observe_enhanced) {
    if (!metadata_complete)
      ++metadata_truncated_calls;
    else {
      const MetadataScope metadata(list, identity.generation);
      const auto scope = scope_flags(identity) | (global_uncertainty(uncertainty) ? ScopeInvalidRecording : 0u);
      for (UINT g = 0; g < count; ++g) {
        if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE)
          continue;
        for (UINT n = 0; n < groups[g].NumBarriers; ++n) {
          if (!same_identity(list, identity.generation))
            break;
          callbacks.observe_enhanced(callbacks.context, list, identity.generation, groups[g].pTextureBarriers[n], scope);
        }
      }
    }
  }
  notify_invalidation(list, identity.generation, uncertainty);
  if (allowed(identity)) {
    if (!metadata_complete || global_uncertainty(uncertainty) || !texture_batches(count, groups))
      ++batch_refusals;
    else
      for (UINT group = 0; group < count; ++group) {
        if (groups[group].Type != D3D12_BARRIER_TYPE_TEXTURE)
          continue;
        for (UINT n = 0; n < groups[group].NumBarriers; ++n) {
          const auto& b = groups[group].pTextureBarriers[n];
          if (!b.pResource || b.Flags != D3D12_TEXTURE_BARRIER_FLAG_NONE || b.LayoutBefore != D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
              b.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET || b.AccessBefore != D3D12_BARRIER_ACCESS_RENDER_TARGET ||
              (b.SyncBefore & D3D12_BARRIER_SYNC_SPLIT) != 0 || (b.SyncAfter & D3D12_BARRIER_SYNC_SPLIT) != 0 ||
              !sole_subresource(b.Subresources))
            continue;
          if (previous_enhanced_change(group, n, groups, b.pResource)) {
            ++batch_refusals;
            continue;
          }
          if (!same_safe(list, identity.generation))
            break;
          ++enhanced_candidates;
          callbacks.before_enhanced(callbacks.context, list, identity.generation, b);
        }
      }
  }
  hook_timing::forward(original, list, count, groups);
}
bool region(const void* address, std::size_t size, MEMORY_BASIC_INFORMATION& info) noexcept {
  if (!address ||
#ifdef TAXI_RENDER_BOUNDARY_VALIDATION
      taxi_boundary_test_virtual_query(address, &info, sizeof(info)) != sizeof(info) ||
#else
      VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
#endif
      info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    return false;
  const auto access = info.Protect & 255;
  if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
      access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
    return false;
  const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress), point = reinterpret_cast<std::uintptr_t>(address);
  return info.RegionSize <= UINTPTR_MAX - base && point >= base && point <= base + info.RegionSize &&
         size <= base + info.RegionSize - point;
}
bool pointer_exact(const void* address, void*& value) noexcept {
  const auto location = reinterpret_cast<std::uintptr_t>(address);
  if (!location || location % 8 != 0 || location > UINTPTR_MAX - 8)
    return false;
  SIZE_T bytes = 0;
#ifdef TAXI_RENDER_BOUNDARY_VALIDATION
  return taxi_boundary_test_read_process_memory(GetCurrentProcess(), address, &value, 8, &bytes) && bytes == 8;
#else
  return ReadProcessMemory(GetCurrentProcess(), address, &value, 8, &bytes) && bytes == 8;
#endif
}
bool pointer(const void* address, void*& value) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  return region(address, 8, info) && pointer_exact(address, value);
}
// Metadata fixtures pass integer registry keys. Those are not COM objects, so
// the hook-path unwrap must not run on them. A known active-pass table also
// cannot spend a VirtualQuery here: its fast path is two slot reads.
ID3D12GraphicsCommandList* registry_key(ID3D12GraphicsCommandList* list) noexcept {
  void* table = nullptr;
  if (!pointer(list, table) || !table)
    return list;
  return identity_key(list);
}
bool data_slot(void** address) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  if (!region(address, 8, info))
    return false;
  const auto access = info.Protect & 255;
  return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY;
}
bool pin(void* code) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  HMODULE module = nullptr;
  if (!region(code, 1, info) || info.Type != MEM_IMAGE)
    return false;
  const auto access = info.Protect & 255;
  return (access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY) &&
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(code),
                            &module);
}
BOOL protect(void* address, SIZE_T bytes, DWORD access, DWORD* previous) noexcept {
#ifdef TAXI_RENDER_BOUNDARY_VALIDATION
  return taxi_boundary_test_virtual_protect(address, bytes, access, previous);
#else
  return VirtualProtect(address, bytes, access, previous);
#endif
}
bool repair(DWORD& error) noexcept {
  if (!pending_slot)
    return true;
  DWORD discarded = 0;
  if (!protect(pending_slot, 8, pending_access, &discarded)) {
    error = GetLastError();
    return false;
  }
  pending_slot = nullptr;
  pending_access = 0;
  return true;
}
Result result(const char* status, DWORD error = 0, bool ready = false) noexcept {
  return {ready, pending_slot == nullptr, error, status};
}
bool exchange(Slot& slot, bool install, DWORD& error) noexcept {
  DWORD previous = 0;
  if (!data_slot(slot.address) || !protect(slot.address, 8, PAGE_READWRITE, &previous)) {
    error = GetLastError();
    return false;
  }
  pending_slot = slot.address;
  pending_access = previous;
  void* expected = install ? slot.forward : slot.wrapper;
  void* replacement = install ? slot.wrapper : slot.forward;
  const bool changed =
      InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot.address), replacement, expected) == expected;
  if (changed)
    slot.installed = install;
  else
    error = ERROR_INVALID_STATE;
  if (!repair(error))
    return false;
  return changed;
}
bool same_callbacks(const Callbacks& a, const Callbacks& b) noexcept {
  return a.context == b.context && a.before_legacy == b.before_legacy && a.before_enhanced == b.before_enhanced &&
         a.observe_legacy == b.observe_legacy && a.observe_enhanced == b.observe_enhanced &&
         a.after_copy_resource == b.after_copy_resource && a.after_copy_texture == b.after_copy_texture && a.after_draw == b.after_draw &&
         a.recording_invalidated == b.recording_invalidated && a.pass_targets == b.pass_targets && a.pass_ended == b.pass_ended &&
         a.selected_legacy_targets == b.selected_legacy_targets && a.metadata_begin == b.metadata_begin &&
         a.metadata_end == b.metadata_end && a.pass_began == b.pass_began && a.enhanced_call == b.enhanced_call &&
         a.before_legacy_uav == b.before_legacy_uav && a.draw_indexed_repeats == b.draw_indexed_repeats;
}
bool install_active_end(ID3D12GraphicsCommandList4* list) noexcept {
  {
    // Known, already-installed tables need two exact reads and no mutation.
    // RPM checks current readability itself; this actual live COM argument is
    // not an engine-read borrowed address. Failed reads/mismatches refuse.
    const Lock read_lock(control_lock, false);
    if (removed || !enabled.load(std::memory_order_acquire))
      return false;
    if (!pending_slot) {
      void* table = nullptr;
      if (!pointer_exact(list, table) || !table || reinterpret_cast<std::uintptr_t>(table) > UINTPTR_MAX - 70 * 8)
        return false;
      if (table == saved_table && slots[2].installed) {
        void* current = nullptr;
        return pointer_exact(slots[2].address, current) && current == slots[2].wrapper;
      }
      for (std::size_t n = 0; n < active_end_count; ++n) {
        const auto& known = active_end_slots[n];
        if (known.table == table && known.slot.installed) {
          void* current = nullptr;
          return pointer_exact(known.slot.address, current) && current == known.slot.wrapper;
        }
      }
    }
  }
  // SRW locks cannot be upgraded: release shared ownership, then acquire
  // exclusive ownership and repeat all state/identity/protection checks.
  const Lock lock(control_lock, true);
  DWORD error = 0;
  if (removed || !enabled.load(std::memory_order_acquire) || !repair(error))
    return false;
  void* table = nullptr;
  if (!pointer(list, table) || !table || reinterpret_cast<std::uintptr_t>(table) > UINTPTR_MAX - 70 * 8)
    return false;
  if (table == saved_table) {
    void* current = nullptr;
    return pointer(slots[2].address, current) && current == slots[2].wrapper;
  }
  for (std::size_t n = 0; n < active_end_count; ++n) {
    auto& known = active_end_slots[n];
    if (known.table != table)
      continue;
    void* current = nullptr;
    if (!pointer(known.slot.address, current) || current != (known.slot.installed ? known.slot.wrapper : known.slot.forward))
      return false;
    return known.slot.installed || exchange(known.slot, true, error);
  }
  if (active_end_count == active_end_slots.size())
    return false;
  auto& entry = active_end_slots[active_end_count];
  constexpr std::array<End, 8> wrappers{&active_end<0>, &active_end<1>, &active_end<2>, &active_end<3>,
                                        &active_end<4>, &active_end<5>, &active_end<6>, &active_end<7>};
  entry.table = table;
  entry.slot.address = static_cast<void**>(table) + 69;
  entry.slot.wrapper = reinterpret_cast<void*>(wrappers[active_end_count]);
  if (!data_slot(entry.slot.address) || !pointer(entry.slot.address, entry.slot.forward) || !entry.slot.forward ||
      entry.slot.forward == entry.slot.wrapper || !pin(entry.slot.forward) || !pin(entry.slot.wrapper))
    return false;
  original_active_ends[active_end_count].store(reinterpret_cast<End>(entry.slot.forward), std::memory_order_release);
  // Retain the original even when installation/restoring protection fails.
  // Any already-installed trampoline must always be able to forward safely.
  ++active_end_count;
  return exchange(entry.slot, true, error);
}
}  // namespace

Result register_list(ID3D12GraphicsCommandList* list, std::uint64_t generation, const Callbacks& supplied) noexcept {
  if (inside)
    return result("reentrant_registration");
  const Lock control(control_lock, true);
  DWORD error = 0;
  if (!repair(error))
    return result("protection_restore_failed", error);
  if (removed || !list || !generation || !supplied.before_legacy || !supplied.before_enhanced)
    return result("invalid_registration");
  if (configured && !same_callbacks(callbacks, supplied))
    return result("callback_mismatch");
  ID3D12GraphicsCommandList7* extended = nullptr;
  if (FAILED(list->QueryInterface(IID_PPV_ARGS(&extended))) || !extended)
    return result("native_interface7_unavailable");
  const bool same = static_cast<ID3D12GraphicsCommandList*>(extended) == list;
  extended->Release();
  if (!same)
    return result("different_interface_pointer");
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return result("not_direct");
  void* table = nullptr;
  if (!pointer(list, table) || !table || reinterpret_cast<std::uintptr_t>(table) > UINTPTR_MAX - 81 * 8)
    return result("invalid_object");
  if (configured && table != saved_table)
    return result("different_vtable");
  if (!configured) {
    const std::array<void*, 8> wrappers{reinterpret_cast<void*>(&legacy),       reinterpret_cast<void*>(&begin),
                                        reinterpret_cast<void*>(&end),          reinterpret_cast<void*>(&enhanced),
                                        reinterpret_cast<void*>(&draw),         reinterpret_cast<void*>(&draw_indexed),
                                        reinterpret_cast<void*>(&copy_texture), reinterpret_cast<void*>(&copy_resource)};
    for (UINT n = 0; n < slots.size(); ++n) {
      auto& slot = slots[n];
      slot.address = static_cast<void**>(table) + slot.index;
      slot.wrapper = wrappers[n];
      if (!data_slot(slot.address) || !pointer(slot.address, slot.forward) || !slot.forward || slot.forward == slot.wrapper ||
          !pin(slot.forward) || !pin(slot.wrapper))
        return result("invalid_slot_or_pin", GetLastError());
    }
    callbacks = supplied;
    saved_table = table;
    original_legacy.store(reinterpret_cast<Legacy>(slots[0].forward), std::memory_order_release);
    original_begin.store(reinterpret_cast<Begin>(slots[1].forward), std::memory_order_release);
    original_end.store(reinterpret_cast<End>(slots[2].forward), std::memory_order_release);
    original_enhanced.store(reinterpret_cast<Enhanced>(slots[3].forward), std::memory_order_release);
    original_draw.store(reinterpret_cast<Draw>(slots[4].forward), std::memory_order_release);
    original_draw_indexed.store(reinterpret_cast<DrawIndexed>(slots[5].forward), std::memory_order_release);
    original_copy_texture.store(reinterpret_cast<CopyTexture>(slots[6].forward), std::memory_order_release);
    original_copy_resource.store(reinterpret_cast<CopyResource>(slots[7].forward), std::memory_order_release);
    configured = true;
  }
  for (auto& slot : slots) {
    void* current = nullptr;
    if (!pointer(slot.address, current) || current != (slot.installed ? slot.wrapper : slot.forward)) {
      disable_capture();
      return result("slot_changed");
    }
    if (!slot.installed && !exchange(slot, true, error)) {
      disable_capture();
      return result("installation_incomplete", error);
    }
  }
  for (std::size_t n = 0; n < active_end_count; ++n) {
    auto& slot = active_end_slots[n].slot;
    void* current = nullptr;
    if (!pointer(slot.address, current) || current != (slot.installed ? slot.wrapper : slot.forward) ||
        (!slot.installed && !exchange(slot, true, error))) {
      disable_capture();
      return result("active_end_repair_failed", error);
    }
  }
  {
    const auto key = registry_key(list);
    auto& shard = shard_for(key);
    const ShardLock lock(shard, true);
    auto& identities = shard.identities;
    const auto found = identities.find(key);
    if (found != identities.end() && found->second.generation != generation)
      return result("identity_not_retired");
    if (found == identities.end()) {
      if (identity_count.fetch_add(1, std::memory_order_relaxed) >= kMaximumIdentities) {
        identity_count.fetch_sub(1, std::memory_order_relaxed);
        return result("identity_limit");
      }
      try {
        identities.emplace(key, Identity{generation});
      } catch (...) {
        identity_count.fetch_sub(1, std::memory_order_relaxed);
        return result("allocation_failed");
      }
    }
  }
  enabled.store(true, std::memory_order_release);
  return result("registered", 0, true);
}
bool erase_identity(ID3D12GraphicsCommandList* key, std::uint64_t generation) noexcept {
  auto& shard = shard_for(key);
  const ShardLock lock(shard, true);
  auto& identities = shard.identities;
  const auto it = identities.find(key);
  if (it == identities.end() || it->second.generation != generation)
    return false;
  identities.erase(it);
  identity_count.fetch_sub(1, std::memory_order_relaxed);
  return true;
}
// Runs from the list's private-data release while the runtime destroys it, so
// the list is not called when its own pointer is the key: hooks register the
// native list they receive, which is its own key. Only a registration under
// another key (a proxy argument) pays registry_key's unwrap. Under PIX's GPU
// capture layer, QueryInterface on a list in destruction raised an exception
// through this noexcept path and aborted the simulator (2026-09-27).
void unregister_list(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  if (erase_identity(list, generation) || !identity_count.load(std::memory_order_relaxed))
    return;
  if (const auto key = registry_key(list); key != list)
    erase_identity(key, generation);
}
// Hooks receive the native list, which is normally its own registry key. An
// entry under that exact pointer with the caller's generation belongs to this
// list incarnation, so Reset and invalidation skip registry_key's readability
// probe (VirtualQuery and ReadProcessMemory) and unwrap. Anything else, such as
// a proxy argument, takes the checked path unchanged.
Identity* same_key_identity(Shard& shard, ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  const auto it = shard.identities.find(list);
  return it != shard.identities.end() && it->second.generation == generation ? &it->second : nullptr;
}
void successful_reset(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  {
    auto& shard = shard_for(list);
    const ShardLock lock(shard, true);
    if (auto* identity = same_key_identity(shard, list, generation)) {
      *identity = Identity{generation, false, false, !enabled.load(std::memory_order_acquire)};
      return;
    }
  }
  const auto key = registry_key(list);
  if (key == list)
    return;
  auto& shard = shard_for(key);
  const ShardLock lock(shard, true);
  auto& identities = shard.identities;
  const auto it = identities.find(key);
  if (it != identities.end() && it->second.generation == generation)
    it->second = Identity{generation, false, false, !enabled.load(std::memory_order_acquire)};
}
void reset_failed(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  invalidate_recording(list, generation, InvalidationResetFailed);
}
void invalidate_recording(ID3D12GraphicsCommandList* list, std::uint64_t generation, std::uint32_t reasons) noexcept {
  auto key = list;
  bool found = false;
  {
    auto& shard = shard_for(list);
    const ShardLock lock(shard, true);
    if (auto* identity = same_key_identity(shard, list, generation)) {
      identity->invalid = true;
      found = true;
    }
  }
  if (!found) {
    key = registry_key(list);
    if (key == list)
      return;
    auto& shard = shard_for(key);
    const ShardLock lock(shard, true);
    auto& identities = shard.identities;
    const auto it = identities.find(key);
    if (it == identities.end() || it->second.generation != generation)
      return;
    it->second.invalid = true;
  }
  const ScopedBypass bypass;
  notify_invalidation(key, generation, reasons ? reasons : InvalidationUnobservedWork);
}
Result remove() noexcept {
  if (inside)
    return result("reentrant_removal");
  const Lock lock(control_lock, true);
  enabled = false;
  removed = true;
  DWORD error = 0;
  if (!repair(error))
    return result("protection_restore_failed", error);
  for (auto& slot : slots)
    if (slot.installed && !exchange(slot, false, error))
      return result("removal_incomplete", error);
  for (std::size_t n = 0; n < active_end_count; ++n) {
    auto& slot = active_end_slots[n].slot;
    if (slot.installed && !exchange(slot, false, error))
      return result("active_end_removal_incomplete", error);
  }
  {
    const AllShardsLock registry;
    for (auto& shard : shards)
      shard.identities.clear();
    identity_count.store(0, std::memory_order_relaxed);
  }
  return result("removed");
}
Result repair_protection() noexcept {
  const Lock lock(control_lock, true);
  DWORD error = 0;
  return repair(error) ? result("protection_restored") : result("protection_restore_failed", error);
}
bool recording_allows_injection(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  return list && generation && same_safe(list, generation);
}
bool operational() noexcept {
  return enabled.load(std::memory_order_acquire);
}
Statistics statistics() noexcept {
  return {legacy_calls.load(),        enhanced_calls.load(),     legacy_candidates.load(),
          enhanced_candidates.load(), pass_refusals.load(),      batch_refusals.load(),
          copy_resource_calls.load(), copy_texture_calls.load(), metadata_truncated_calls.load(),
          maximum_legacy_batch.load()};
}
#ifdef TAXI_RENDER_BOUNDARY_STATE_VALIDATION
std::uint32_t validation_state(ID3D12GraphicsCommandList* list, std::uint64_t generation) noexcept {
  const auto value = lookup(list);
  return (value.generation == generation ? 1u : 0u) | (value.active ? 2u : 0u) | (value.suspended ? 4u : 0u) | (value.invalid ? 8u : 0u) |
         (value.prior_work ? 16u : 0u) | (enabled.load() ? 32u : 0u);
}
#endif
ScopedBypass::ScopedBypass() noexcept : previous_(inside) {
  inside = true;
}
ScopedBypass::~ScopedBypass() {
  inside = previous_;
}
}  // namespace taxi_camera::engine_hook::render_boundary
