#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <cstdint>

namespace taxi_camera::engine_hook::render_boundary {
// Linear metadata inspection, without allocations or resource dereferences.
// Generic insertion is capped at 256; larger complete batches admit only the two explicitly selected PFD identities.
inline constexpr UINT maximum_legacy_metadata_barriers = 1u << 20;
enum ScopeFlags : std::uint32_t {
  ScopeEnabled = 1,
  ScopeActivePass = 2,
  ScopeSuspendedPass = 4,
  ScopeInvalidRecording = 8,
  ScopePriorGpuWork = 16,
};
enum InvalidationFlags : std::uint32_t {
  InvalidationPassBegin = 1,
  InvalidationPassState = 2,
  InvalidationBarrierBatch = 4,
  InvalidationSplitBarrier = 8,
  InvalidationAliasOrDiscard = 16,
  InvalidationUnobservedWork = 32,
  InvalidationObserverDisabled = 64,
  InvalidationResetFailed = 128,
};
struct Callbacks {
  void* context = nullptr;
  // Called BEFORE the exact application's whole native barrier call. Only a
  // non-split transition from RENDER_TARGET, outside all active/suspended passes,
  // with no earlier same-resource transition or alias is delivered. The resource
  // is an actual public COM argument, not a dereferenced engine-read address.
  // Subresource must be zero or ALL (enhanced ranges must name sole mip/array/
  // plane zero). Caller must match scene identity and prove that the actual
  // color resource contains exactly one mip, layer, sample and plane.
  // Injection must restore the identical before state/layout before returning.
  // A prior observed native draw outside a pass, or a completed ordinary pass,
  // is required. This excludes transitions before a future first RESUMING pass.
  // Any PRESERVE_LOCAL pass access invalidates the recording until Reset.
  void (*before_legacy)(void*,
                        ID3D12GraphicsCommandList*,
                        std::uint64_t object_generation,
                        const D3D12_RESOURCE_TRANSITION_BARRIER&) noexcept = nullptr;
  void (*before_enhanced)(void*,
                          ID3D12GraphicsCommandList7*,
                          std::uint64_t object_generation,
                          const D3D12_TEXTURE_BARRIER&) noexcept = nullptr;
  // Metadata only, before RT/pass filters; never issue GPU work here. Inspect
  // Legacy batches are inspected completely up to the explicit metadata bound;
  // larger/null/overflowing spans are rejected in full. Enhanced metadata is
  // bounded to 4096 barriers/64 groups. No resource pointer is dereferenced.
  // Legacy delivery includes exact transition, alias and UAV records; NULL
  // alias arguments retain their wildcard meaning. Unknown types invalidate.
  void (*observe_legacy)(void*,
                         ID3D12GraphicsCommandList*,
                         std::uint64_t object_generation,
                         const D3D12_RESOURCE_BARRIER&,
                         std::uint32_t scope_flags) noexcept = nullptr;
  void (*observe_enhanced)(void*,
                           ID3D12GraphicsCommandList7*,
                           std::uint64_t object_generation,
                           const D3D12_TEXTURE_BARRIER&,
                           std::uint32_t scope_flags) noexcept = nullptr;
  // AFTER the exact original native copy is recorded once. Internal snapshot
  // copies bypass reentry. A false capture_allowed permits metadata only.
  // Caller validates whole color texture shape and exact live generations.
  void (*after_copy_resource)(void*,
                              ID3D12GraphicsCommandList*,
                              std::uint64_t object_generation,
                              ID3D12Resource* destination,
                              ID3D12Resource* source,
                              bool capture_allowed) noexcept = nullptr;
  void (*after_copy_texture)(void*,
                             ID3D12GraphicsCommandList*,
                             std::uint64_t object_generation,
                             const D3D12_TEXTURE_COPY_LOCATION* destination,
                             UINT x,
                             UINT y,
                             UINT z,
                             const D3D12_TEXTURE_COPY_LOCATION* source,
                             const D3D12_BOX* source_box,
                             bool capture_allowed) noexcept = nullptr;
  // AFTER one exact nonzero native direct/indexed draw. A false permission
  // consumes/discards the caller's staged source proof, without GPU work.
  // This observes raster work, not a legacy/enhanced resource-state model.
  void (*after_draw)(void*, ID3D12GraphicsCommandList*, std::uint64_t object_generation, bool capture_allowed) noexcept = nullptr;
  // Metadata-only invalidation before uncertain native work. Discard source
  // state/staged queue-tail proof for this recording. No GPU commands here.
  // Registration, descriptor bindings and publication are not state proof.
  void (*recording_invalidated)(void*,
                                ID3D12GraphicsCommandList*,
                                std::uint64_t object_generation,
                                std::uint32_t reason_flags) noexcept = nullptr;
  // Exact native render-pass descriptors, before pass invalidation. Metadata only.
  void (*pass_targets)(void*,
                       ID3D12GraphicsCommandList*,
                       std::uint64_t,
                       UINT,
                       const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                       const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*) noexcept = nullptr;
  void (*pass_ended)(void*, ID3D12GraphicsCommandList*, std::uint64_t) noexcept = nullptr;
  // Optional comparison-only selection for complete legacy batches >256.
  // Called once with capacity two, outside locks, under the reentry guard.
  // Return zero, one, or two distinct nonnull pointers; no resource dereference,
  // lifetime acquisition or GPU commands here. Actual before_legacy arguments
  // still require caller-owned generation/lifetime/shape admission. At most one
  // first eligible transition per selected resource is delivered before the
  // unchanged original batch. All metadata and pass guards remain mandatory.
  UINT (*selected_legacy_targets)(void*,
                                  ID3D12GraphicsCommandList*,
                                  std::uint64_t object_generation,
                                  ID3D12Resource** targets,
                                  UINT capacity) noexcept = nullptr;
  // Optional paired metadata scope, outside observer locks. Begin/end bracket
  // only the complete metadata loop, including early identity refusal. No GPU
  // work or retained native-object ownership is granted. Implementations may
  // cache lifetime-qualified metadata until end; individual guards still apply.
  // Both callbacks must be provided; a partial pair is ignored.
  void (*metadata_begin)(void*, ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept = nullptr;
  void (*metadata_end)(void*, ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept = nullptr;
};
struct Result {
  bool ready = false;
  bool protection_restored = true;
  DWORD error = 0;
  const char* status = "not_installed";
};
struct Statistics {
  std::uint64_t legacy_calls = 0, enhanced_calls = 0, legacy_candidates = 0, enhanced_candidates = 0;
  std::uint64_t pass_refusals = 0, batch_refusals = 0;
  std::uint64_t copy_resource_calls = 0, copy_texture_calls = 0, metadata_truncated_calls = 0;
  std::uint64_t maximum_legacy_batch = 0;
};
// Explicit live native DIRECT object. QI7 must succeed and return this identical
// interface pointer before its extended vtable slots are inspected. Eight slots
// 12/13/16/17/26/68/69/80 are patched in-place, with original/module pins and no clones.
// If native Begin swaps to an active-pass vtable, only its observed End69 is
// additionally patched. At most eight such tables, each with its own original
// and trampoline, are retained. No expanded-interface slots are guessed.
// One shared vtable, <=8192 live object identities; no command-list AddRef held.
// Context/callback code must live for the process. Callbacks run without locks;
// they must reject stale object generations. Unknown lists forward unchanged.
Result register_list(ID3D12GraphicsCommandList*, std::uint64_t object_generation, const Callbacks&) noexcept;
void unregister_list(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
// Wire only from the already-verified native Reset after S_OK. Failed Reset
// must call reset_failed. Synthetic creation/reset events are insufficient.
// A disabled/failed hook invalidates existing recordings. Reset while disabled
// cannot reauthorize them; an actual successful Reset after repair is required.
void successful_reset(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
void reset_failed(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
// Call from existing bundle/indirect/otherwise-unobserved native-work events.
// Retires this recording's capture permission until native successful Reset.
// Does not forward/execute that work, dereference resources or notify unknown
// generations. The callback runs after releasing all observer locks.
void invalidate_recording(ID3D12GraphicsCommandList*,
                          std::uint64_t object_generation,
                          std::uint32_t reason_flags = InvalidationUnobservedWork) noexcept;
Result remove() noexcept;
Result repair_protection() noexcept;
bool operational() noexcept;
// Permission for caller-owned work at an actual native list boundary. This is
// only recording/pass permission; callers must separately prove the bound RTV,
// resource lifetime/state and complete graphics restoration.
bool recording_allows_injection(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
Statistics statistics() noexcept;
#ifdef TAXI_RENDER_BOUNDARY_STATE_VALIDATION
// Test-only scalar state; never part of the production add-on interface.
std::uint32_t validation_state(ID3D12GraphicsCommandList*, std::uint64_t object_generation) noexcept;
#endif
// Internal capture barriers already bypass while a boundary callback executes.
// This explicit guard supports any caller-owned commands outside that callback.
class ScopedBypass {
 public:
  ScopedBypass() noexcept;
  ~ScopedBypass();
  ScopedBypass(const ScopedBypass&) = delete;
  ScopedBypass& operator=(const ScopedBypass&) = delete;

 private:
  bool previous_;
};
}  // namespace taxi_camera::engine_hook::render_boundary
