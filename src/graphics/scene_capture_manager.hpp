#pragma once

#include "../hooks/queue_submit_observer.hpp"
#include "scene_capture_d3d12.hpp"
#include "scene_handoff.hpp"
#include "scene_source_state.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace taxi_camera {

// Process-lifetime manager. Register all native DIRECT queues with callbacks()
// before admitting recorded captures/consumers. A failed queue observation must
// call submission_refused; disabling notifications while owned recordings remain
// executable is unsupported. Callback entry points must never hold a lock needed
// by another submission callback. No methods call the private simulator engine.
class SceneCaptureManager {
 public:
  static constexpr std::size_t MaximumDevices = 4;
  static constexpr std::size_t MaximumLists = 4096;
  static constexpr std::size_t MaximumPackets = 16;
  static constexpr std::uint64_t MaximumBytes = 256ull * 1024 * 1024;
  struct Frame {
    std::uint64_t token = 0;
    std::uint64_t device_key = 0;
    SceneCopyMatch match;
    ID3D12Resource* resource = nullptr;  // Borrowed owned snapshot, COPY_DEST.
  };
  struct Submission {
    std::uint64_t receipt = 0;
    ID3D12Fence* fence = nullptr;  // Process-lifetime borrowed timeline.
    std::uint64_t value = 0;       // Queued only by successful end.
  };
  struct RenderTargetDiagnostic {
    std::uint64_t matched_boundaries = 0, width = 0;
    std::uint32_t height = 0, format = 0, mips = 0;
    const char* last_refusal = "not_observed";
  };
  struct CopyDiagnostic {
    std::uint64_t matched_copies = 0, width = 0;
    std::uint32_t height = 0, format = 0, mips = 0;
    const char* last_refusal = "not_observed";
  };
  struct Statistics {
    std::uint64_t captures = 0, submissions = 0, resets = 0;
    std::uint64_t completed = 0, skipped = 0, quarantined = 0, bytes = 0;
    std::uint64_t render_target_writes = 0, render_target_rewrites = 0;
    std::array<RenderTargetDiagnostic, 2> render_targets{};
    std::uint64_t copy_writes = 0, copy_rewrites = 0;
    std::array<CopyDiagnostic, 2> copies{};
    std::uint64_t source_candidates = 0, source_draws = 0, tail_submissions = 0, tail_captures = 0;
    const char* tail_status = "not_started";
    std::uint64_t unknown_submitted_lists = 0, invalid_source_recordings = 0, scoped_source_invalidations = 0;
    std::uint64_t invalid_draws = 0, source_lease_failures = 0, global_aliases = 0, recording_overflows = 0;
    std::uint32_t last_invalidation_reasons = 0;
  };

  explicit SceneCaptureManager(SceneHandoff& handoff) noexcept;
  ~SceneCaptureManager();
  SceneCaptureManager(const SceneCaptureManager&) = delete;
  SceneCaptureManager& operator=(const SceneCaptureManager&) = delete;

  bool register_device(std::uint64_t device_key, ID3D12Device* native_device) noexcept;
  void destroy_device(std::uint64_t device_key) noexcept;
  bool register_command_list(ID3D12GraphicsCommandList* native_list, std::uint64_t device_key, std::uint64_t object_generation) noexcept;
  // A list discovered at submission has an unobserved existing recording.
  // Only a subsequent, fully observed successful native Reset admits it.
  bool register_unobserved_command_list(ID3D12GraphicsCommandList*, std::uint64_t device_key, std::uint64_t generation) noexcept;
  using UnknownListObserver = void (*)(void*, ID3D12GraphicsCommandList*, std::uint64_t device_key) noexcept;
  bool set_unknown_list_observer(UnknownListObserver, void* context) noexcept;
  void successful_reset(ID3D12GraphicsCommandList* native_list, std::uint64_t object_generation) noexcept;
  void destroy_command_list(ID3D12GraphicsCommandList* native_list, std::uint64_t object_generation) noexcept;

  // Candidate/model observation starts at resource creation, before scene
  // publication. Only begin/stop controls tail capture; neither invents a state.
  void begin_source_tracking() noexcept;
  void stop_source_tracking() noexcept;
  void set_source_rate(std::uint32_t frames_per_second) noexcept;
  bool register_source_candidate(std::uint64_t device_key,
                                 ID3D12Resource*,
                                 std::uint64_t resource_generation,
                                 const D3D12_RESOURCE_DESC&,
                                 source_state::Model initial = source_state::Model::unknown) noexcept;
  void unregister_source_candidate(std::uint64_t device_key, ID3D12Resource*, std::uint64_t resource_generation) noexcept;
  // Stage EVERY public draw, using count0 to clear previous tentative bindings.
  // Only the matching actual native after-draw consumes this thread's stage.
  void stage_source_draw(ID3D12GraphicsCommandList*,
                         std::uint64_t object_generation,
                         UINT count,
                         ID3D12Resource* const* targets,
                         const std::uint64_t* resource_generations) noexcept;
  void after_source_draw(ID3D12GraphicsCommandList*, std::uint64_t object_generation, bool allowed) noexcept;
  void observe_source_legacy(ID3D12GraphicsCommandList*, std::uint64_t object_generation, const D3D12_RESOURCE_BARRIER&) noexcept;
  void observe_source_enhanced(ID3D12GraphicsCommandList*, std::uint64_t object_generation, const D3D12_TEXTURE_BARRIER&) noexcept;
  void invalidate_source_recording(ID3D12GraphicsCommandList*,
                                   std::uint64_t object_generation,
                                   bool affects_all_sources = false,
                                   std::uint32_t reasons = 0) noexcept;
  // Known render-pass/indirect targets affect only their exact resource keys.
  // Missing/truncated target metadata retains the conservative global guard.
  void invalidate_source_targets(ID3D12GraphicsCommandList*,
                                 std::uint64_t object_generation,
                                 UINT count,
                                 ID3D12Resource* const* targets,
                                 const std::uint64_t* resource_generations) noexcept;

  // Call AFTER forwarding exactly one real application copy. whole_texture_copy
  // is the adapter's proof of an exact whole source/destination image operation;
  // both descriptions are also checked here. Partial/boxed/subresource operations
  // must be normalized and proved externally; false records nothing.
  bool record_copy_after_forward(ID3D12GraphicsCommandList* native_list,
                                 ID3D12Resource* source,
                                 ID3D12Resource* destination,
                                 bool whole_texture_copy) noexcept;
  // Native copy observer path: the callback's exact object incarnation is
  // required. Repeated whole copies on this unsubmitted recording overwrite
  // the same retained source's packet; the final copied image wins.
  bool record_copy_after_forward(ID3D12GraphicsCommandList* native_list,
                                 ID3D12Resource* source,
                                 ID3D12Resource* destination,
                                 bool whole_texture_copy,
                                 std::uint64_t object_generation) noexcept;
  bool record_texture_copy_after_forward(ID3D12GraphicsCommandList* native_list,
                                         const D3D12_TEXTURE_COPY_LOCATION* destination,
                                         UINT x,
                                         UINT y,
                                         UINT z,
                                         const D3D12_TEXTURE_COPY_LOCATION* source,
                                         const D3D12_BOX* source_box,
                                         bool capture_allowed,
                                         std::uint64_t object_generation) noexcept;
  // BEFORE forwarding an actual complete native legacy transition from RT:
  // flags NONE, sole subresource zero/ALL, outside a render pass. The adapter
  // bypasses its barrier observer while this method emits a temporary bounce.
  // Repeated matching boundaries overwrite this recording's packet so the
  // final completed image wins; no extra per-draw texture allocation occurs.
  // The observed object generation is required and rechecked before any capture.
  bool record_render_target_before_transition(ID3D12GraphicsCommandList* native_list,
                                              ID3D12Resource* actual_render_target,
                                              bool proven_legacy_render_target,
                                              std::uint64_t object_generation) noexcept;
  bool record_render_target_before_enhanced_transition(ID3D12GraphicsCommandList7* native_list,
                                                       ID3D12Resource* actual_render_target,
                                                       bool proven_enhanced_render_target,
                                                       std::uint64_t object_generation) noexcept;
  // Call when the current application recording reads the one stable output.
  // Keeps every later replay on the same timeline as private output writes.
  bool register_consumer_recording(ID3D12GraphicsCommandList* native_list) noexcept;

  engine_hook::queue_submit::Callbacks callbacks() noexcept;
  // Discovery runs without submission serialization. Only fully observed,
  // unrelated recordings bypass it; all other batches revalidate under both
  // manager and submission locks before retaining leases or queuing a fence.
  std::uint64_t before_submission(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
  void after_submission(ID3D12CommandQueue*, std::uint64_t receipt) noexcept;
  void submission_refused(ID3D12CommandQueue*, engine_hook::queue_submit::Refusal) noexcept;

  // begin/end MUST run on the same thread, with no intervening manager API that
  // begins another submission. The private DIRECT queue must not be registered
  // for ordinary capture notifications. No CPU waits for GPU completion occur.
  Submission begin_private_submission(std::uint64_t device_key, ID3D12CommandQueue*) noexcept;
  bool end_private_submission(std::uint64_t receipt) noexcept;
  void abort_private_submission(std::uint64_t receipt) noexcept;

  // Each returned frame is leased exactly once, after successful native Reset
  // or destruction retires its application recording, all receipts return, and
  // its producer fence completes. Caller supplies fixed bounded storage.
  std::size_t poll_completed_frames(Frame* frames, std::size_t capacity) noexcept;
  bool finish_consumption(std::uint64_t token, ID3D12Fence*, std::uint64_t value) noexcept;
  // Only for a leased frame on which NO private work was recorded/submitted.
  bool discard_frame(std::uint64_t token) noexcept;
  Statistics statistics() const noexcept;

 private:
  struct Device {
    std::uint64_t key = 0;
    ID3D12Device* native = nullptr;
    ID3D12Fence* timeline = nullptr;
    std::uint64_t last_signal = 0;
    bool active = false, failed = false;
    source_state::Tracker source_states;
  };
  struct SourceLease {
    source_state::Key key;
    ID3D12Resource* native = nullptr;
  };
  struct List {
    ID3D12GraphicsCommandList* native = nullptr;
    std::uint64_t device_key = 0, object_generation = 0, recording = 1;
    std::uint16_t packets = 0;
    unsigned feeds = 0;
    bool consumer = false;
    source_state::Recording source_effects;
    bool source_touched = false;
    bool awaiting_native_reset = false;
    std::array<SourceLease, source_state::Tracker::capacity> source_leases{};
    std::size_t source_lease_count = 0;
  };
  struct Packet {
    SceneCaptureD3D12 gpu;
    D3D12_RESOURCE_DESC description{};
    SceneCopyMatch match;
    std::uint64_t token = 0, device_key = 0, submitted = 0;
    ID3D12CommandQueue* producer = nullptr;  // Registered queue retained by hook.
    unsigned in_flight = 0;
    bool assigned = false, retired = false, quarantined = false, leased = false;
    ID3D12CommandAllocator* tail_allocator = nullptr;
    ID3D12GraphicsCommandList* tail_list = nullptr;
    ID3D12GraphicsCommandList7* tail_list7 = nullptr;
    std::uint64_t tail_device_key = 0;
  };
  struct Transaction {
    std::uint64_t id = 0;
    Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    std::uint64_t value = 0;
    std::uint16_t packets = 0;
    bool private_work = false;
    bool source_work = false;
    std::array<SourceLease, source_state::Tracker::capacity> source_leases{};
    std::size_t source_lease_count = 0;
  };
  struct SourceCandidate {
    ID3D12Resource* native = nullptr;  // Registry identity only; never a GPU lease.
    std::uint64_t device_key = 0, generation = 0;
    D3D12_RESOURCE_DESC description{};
  };
  Device* device(std::uint64_t) noexcept;
  List* list(ID3D12GraphicsCommandList*) noexcept;
  bool register_list(ID3D12GraphicsCommandList*, std::uint64_t, std::uint64_t, bool observed) noexcept;
  void observe_unknown_lists(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
  void retire_list(List&) noexcept;
  static void release_source_leases(std::array<SourceLease, source_state::Tracker::capacity>&, std::size_t&) noexcept;
  static bool retain_source_lease(std::array<SourceLease, source_state::Tracker::capacity>&,
                                  std::size_t&,
                                  source_state::Key,
                                  ID3D12Resource*) noexcept;
  void quarantine(Packet&) noexcept;
  void fail_device(Device&) noexcept;
  void collect() noexcept;
  SourceCandidate* source_candidate(ID3D12Resource*) noexcept;
  bool may_be_source(ID3D12Resource*) const noexcept;
  bool prepare_tail(Packet&, Device&) noexcept;
  void record_queue_tail(Transaction&) noexcept;
  bool compatible_queue(ID3D12CommandQueue*, const Device&) noexcept;
  bool capture_source(List&,
                      Device&,
                      const SceneCopyMatch&,
                      ID3D12Resource*,
                      bool render_target,
                      ID3D12GraphicsCommandList7* enhanced_list = nullptr,
                      bool allow_copy_rewrite = false,
                      const char** copy_refusal = nullptr,
                      Packet* required_packet = nullptr) noexcept;
  bool record_copy(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, bool, std::uint64_t, bool) noexcept;
  Submission begin_transaction(Device&, ID3D12CommandQueue*, std::uint16_t, bool) noexcept;
  bool finish_transaction(std::uint64_t, bool refused) noexcept;

  SceneHandoff& handoff_;
  mutable std::mutex mutex_;
  std::mutex submission_mutex_;
  std::array<Device, MaximumDevices> devices_{};
  std::array<List, MaximumLists> lists_{};
  std::unordered_map<ID3D12GraphicsCommandList*, std::size_t> list_indices_;
  std::array<Packet, MaximumPackets> packets_{};
  std::array<SourceCandidate, MaximumDevices * 128> sources_{};
  std::array<std::atomic<std::uint64_t>, MaximumDevices * 128> source_handles_{}, source_generations_{}, source_device_keys_{};
  std::array<std::atomic<std::uint64_t>, 16> source_filter_{};
  bool source_tracking_ = false;
  std::uint32_t source_rate_ = 15;
  std::array<std::uint64_t, 2> last_tail_us_{};
  Transaction transaction_;
  std::uint64_t next_token_ = 0, next_receipt_ = 0;
  Statistics stats_;
  UnknownListObserver unknown_list_observer_ = nullptr;
  void* unknown_list_context_ = nullptr;
};

}  // namespace taxi_camera
