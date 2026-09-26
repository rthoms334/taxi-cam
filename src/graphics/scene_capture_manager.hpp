#pragma once

#include "../hooks/queue_submit_observer.hpp"
#include "../shared/bounded_lock.hpp"
#include "../shared/camera_rate.hpp"
#include "../shared/lock_hold_stats.hpp"
#include "owned_gpu_timing.hpp"
#include "pfd_submission_pool.hpp"
#include "scene_capture_d3d12.hpp"
#include "scene_handoff.hpp"
#include "scene_source_state.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <mutex>
#include <unordered_map>

namespace taxi_camera {
// Hold times of every SceneCaptureManager mutex_, drained by the bridge log.
LockHoldStats& manager_lock_holds() noexcept;

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
  struct FrameOrder {
    // Actual serialized device timeline and list position within that submit.
    // Allocation tokens and CPU retirement times do not order rendered images.
    std::uint64_t submission = 0;
    std::uint32_t position = 0;
    bool newer_than(const FrameOrder& previous) const noexcept {
      return submission && position &&
             (submission > previous.submission || (submission == previous.submission && position > previous.position));
    }
  };
  struct Frame {
    std::uint64_t token = 0;
    std::uint64_t device_key = 0;
    SceneCopyMatch match;
    ID3D12Resource* resource = nullptr;  // Borrowed owned snapshot, COPY_DEST.
    FrameOrder order;
    std::uint64_t session_generation = 0;
  };
  struct Submission {
    std::uint64_t receipt = 0;
    ID3D12Fence* fence = nullptr;  // Process-lifetime borrowed timeline.
    std::uint64_t value = 0;       // Queued only by successful end.
    bool deferred = false;         // No work queued; a private caller may retry.
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
  // Every writer of a global source-model wipe (Tracker::invalidate_all or
  // clear) names itself. A skipped bounded wait is never a writer: skipped
  // evidence and retirements are replayed under the lock instead. Only a
  // recording the manager genuinely lost track of may wipe: an unregistered or
  // unobserved list in a batch, an invalid recording, a lost deferred entry,
  // and device or session ends. An escaped or refused batch retires the live
  // models in place instead (retire_sources); unordered_consumer and the
  // bounded deferred_sources origins remain in the enum for old logs only.
  enum class WipeSite : std::uint8_t {
    none,
    fail_device,
    unordered_consumer,
    deferred_sources,
    unknown_lists_no_owner,
    unknown_lists,
    invalid_recording,
    lease_overflow,
    refused_transaction,
    observer_disabled,
    session_reset,
    count
  };
  static constexpr const char* wipe_site_name(WipeSite site) noexcept {
    constexpr const char* names[]{
        "none",          "fail_device",       "unordered_consumer", "deferred_sources",    "unknown_lists_no_owner",
        "unknown_lists", "invalid_recording", "lease_overflow",     "refused_transaction", "observer_disabled",
        "session_reset"};
    return static_cast<unsigned>(site) < static_cast<unsigned>(WipeSite::count) ? names[static_cast<unsigned>(site)] : "invalid_site";
  }
  // Publishers of the deferred source invalidation, one bit each; the bits
  // pending at a deferred_sources wipe are copied into last_wipe_origins.
  // Only GlobalOrigins still wipe (Tracker::invalidate_all): a lost ring entry
  // may have been a Reset of any recording. The bounded-escape origins mark
  // the device's live models other, keep retained_rt and rearm it in place
  // (retire_sources): an escaped batch never saw a different RT model than the
  // last positively observed one, so the next ordered draw captures again.
  enum UncertaintyOrigin : std::uint32_t {
    OriginEscapeUnordered = 1u << 0,
    OriginRefusedCompleted = 1u << 1,
    OriginRefusedContended = 1u << 2,
    OriginDeferredOverflow = 1u << 3,
    OriginUnorderedConsumer = 1u << 4,
  };
  static constexpr std::size_t OriginCount = 5;
  static constexpr std::uint32_t GlobalOrigins = OriginDeferredOverflow;
  static constexpr const char* uncertainty_origin_name(std::size_t bit) noexcept {
    constexpr const char* names[]{"escape_unordered", "refused_completed", "refused_contended", "deferred_overflow", "unordered_consumer"};
    return bit < OriginCount ? names[bit] : "invalid_origin";
  }
  struct Statistics {
    GpuTimingStatistics capture_copy_gpu;
    std::uint64_t captures = 0, submissions = 0, resets = 0;
    std::uint64_t display_copies = 0;
    std::uint64_t completed = 0, skipped = 0, quarantined = 0, bytes = 0;
    // Snapshot textures created. Steady captures reuse packets of their shape.
    std::uint64_t allocations = 0;
    std::uint64_t render_target_writes = 0, render_target_rewrites = 0;
    std::array<RenderTargetDiagnostic, 3> render_targets{};
    std::uint64_t copy_writes = 0, copy_rewrites = 0;
    std::array<CopyDiagnostic, 3> copies{};
    std::uint64_t source_candidates = 0, source_draws = 0, tail_submissions = 0, tail_captures = 0;
    const char* tail_status = "not_started";
    std::uint64_t unknown_submitted_lists = 0, invalid_source_recordings = 0, scoped_source_invalidations = 0;
    // Limited global reports (PassBegin, PassState, unsupported work) on a list
    // that never named a published camera source; nothing was retired.
    std::uint64_t ignored_source_recordings = 0;
    // Limited reports that named a published camera source: that source's live
    // model was retired to other while its retained RT history stayed.
    std::uint64_t retired_source_recordings = 0;
    std::uint64_t invalid_draws = 0, source_lease_failures = 0, global_aliases = 0, recording_overflows = 0;
    std::uint32_t last_invalidation_reasons = 0;
    // Simulator-thread waits that hit their budget and skipped. evidence: barrier,
    // draw, copy and invalidation observers; lifecycle: list Reset/destroy and
    // source registration; submissions: ExecuteCommandLists ordering fallbacks.
    // unordered: batches forwarded without a transaction because of the budget
    // or a closed gate. deferred_retirements: Reset/destroy retired later.
    // deferred_evidence: barrier, target and report evidence replayed onto its
    // recording later. deferred_overflows: the ring lost entries; every current
    // recording was invalidated and the models wiped.
    std::uint64_t contended_evidence = 0, contended_lifecycle = 0, contended_submissions = 0;
    std::uint64_t unordered_submissions = 0, deferred_retirements = 0, gated_submissions = 0;
    // CPU signals of a timeline value already passed to queue Wait, issued when
    // the submission gate closes. One count per Signal that actually ran.
    std::uint64_t released_waits = 0;
    std::uint64_t deferred_evidence = 0, deferred_overflows = 0;
    // Consumer recordings forwarded without ordering by a bounded escape: the
    // source model was invalidated and the device kept, unlike a contended escape.
    std::uint64_t unordered_consumers = 0;
    // Global source-model wipes by writer. last_wipe_us is steady_clock time.
    std::uint64_t wipes = 0, last_wipe_us = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(WipeSite::count)> wipe_counts{};
    WipeSite last_wipe_site = WipeSite::none;
    std::uint32_t last_wipe_origins = 0;
    // deferred_sources wipes by publisher bit: names the origin that still wipes.
    std::array<std::uint64_t, OriginCount> wipe_origin_counts{};
    // Scoped retirements (retire_live_models + rearm_retained_rt in place) by
    // origin bit, with how many RT models the in-place rearm restored in total.
    std::uint64_t source_retirements = 0, retirement_restored = 0, last_retirement_us = 0;
    std::array<std::uint64_t, OriginCount> retirement_origin_counts{};
    std::uint32_t last_retirement_origins = 0;
  };

  explicit SceneCaptureManager(SceneHandoff& handoff) noexcept;
  ~SceneCaptureManager();
  SceneCaptureManager(const SceneCaptureManager&) = delete;
  SceneCaptureManager& operator=(const SceneCaptureManager&) = delete;

  bool register_device(std::uint64_t device_key, ID3D12Device* native_device) noexcept;
  void destroy_device(std::uint64_t device_key) noexcept;
  // Flight invalidation never retires executable application recordings. Their
  // packet/consumer leases and timeline ordering survive until native Reset,
  // destruction and covering GPU completion. New capture waits for resume and
  // a recording belonging to the new session.
  std::uint64_t reset_session(std::uint64_t device_key) noexcept;
  bool resume_session(std::uint64_t device_key, std::uint64_t generation) noexcept;
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
  // Restore last observed RT models after an AA/upscaler wipe that left the
  // same camera allocations registered. Does not invent RT from a draw.
  // Returns how many sources were restored.
  unsigned rearm_source_states() noexcept;
  void set_source_rate(std::uint32_t frames_per_second) noexcept;
  // Suppress NEW GPU capture work only. Source state/alias/draw observation,
  // immutable replay effects and existing packet/consumer ordering must continue
  // while idle, including for persistent textures created directly in RT state.
  void set_capture_enabled(bool enabled) noexcept;
  void set_gpu_timing_enabled(bool enabled) noexcept;
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
  // Equivalent to adjacent stage/after calls after the actual native draw,
  // with one lookup/lock and the same generation and resource-lease checks.
  void observe_source_draw_after(ID3D12GraphicsCommandList*,
                                 std::uint64_t object_generation,
                                 UINT count,
                                 ID3D12Resource* const* targets,
                                 const std::uint64_t* resource_generations,
                                 bool allowed) noexcept;
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
  struct DisplaySubmissionPlan {
    struct Item {
      UINT after_list = 0;
      pfd_submission::Pool::Copy copy;
      bool before = false;
    };
    std::uint64_t device_key = 0, generation = 0;
    const std::atomic<std::uint64_t>* current_generation = nullptr;
    std::array<Item, engine_hook::queue_submit::kMaximumInsertions> items{};
    UINT count = 0;
    // Planner transfers one reference per nonnull source/target. Releases
    // happen after manager locks; the pool takes independent GPU leases.
    ~DisplaySubmissionPlan();
    bool current() const noexcept;
  };
  using DisplaySubmissionPlanner = void (*)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*, DisplaySubmissionPlan&) noexcept;
  bool set_display_submission_planner(DisplaySubmissionPlanner, void*) noexcept;
  void service_display_submissions() noexcept;
  UINT augment_submission(ID3D12CommandQueue*, std::uint64_t, engine_hook::queue_submit::Insertion*, UINT) noexcept;
  void augmentation_result(ID3D12CommandQueue*, std::uint64_t, UINT) noexcept;
  // Discovery runs without submission serialization. Only fully observed,
  // unrelated recordings bypass it; all other batches revalidate under both
  // manager and submission locks before retaining leases or queuing a fence.
  // Source-only contention invalidates state after forwarding. Escaped capture
  // packets are quarantined; lost stable-output reader ordering fails only that
  // device. Proven unrelated helpers need neither lock nor invalidation.
  std::uint64_t before_submission(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
  void after_submission(ID3D12CommandQueue*, std::uint64_t receipt) noexcept;
  // Paired with a zero before receipt after the native forward: publishes the
  // post-forward source invalidation of a batch before could not order.
  void forwarded_unordered(ID3D12CommandQueue*) noexcept;
  // Closed by the presentation watchdog. While closed, before_submission opens
  // no transaction and treats every batch as an escaped contended batch; the
  // simulator thread returns immediately. The transition to closed CPU-signals
  // each published timeline value already passed to queue Wait, without taking
  // a bridge mutex, so a flush blocked behind that GPU wait can finish.
  // Atomic; callable from any thread.
  void set_submission_gate(bool open) noexcept;
  bool submission_gate_open() const noexcept;
  // True when the most recent evidence or registration call on this thread
  // skipped because its bounded wait expired, not because it was refused.
  static bool last_call_contended() noexcept;
  void submission_refused(ID3D12CommandQueue*, engine_hook::queue_submit::Refusal) noexcept;
  std::uint64_t submission_refused_batch(ID3D12CommandQueue*, engine_hook::queue_submit::Refusal, UINT, ID3D12CommandList* const*) noexcept;
  void submission_refused_completed(std::uint64_t token) noexcept;

  // begin/end MUST run on the same thread, with no intervening manager API that
  // begins another submission. The private DIRECT queue must not be registered
  // for ordinary capture notifications. No CPU waits for GPU completion occur.
  Submission begin_private_submission(std::uint64_t device_key, ID3D12CommandQueue*) noexcept;
  bool end_private_submission(std::uint64_t receipt) noexcept;
  void abort_private_submission(std::uint64_t receipt) noexcept;

  // Each returned frame is leased exactly once, after successful native Reset
  // or destruction retires its application recording, all receipts return, and
  // its producer fence completes. Caller supplies fixed bounded storage.
  // Buffer-slot order is unspecified. Consumers must retain the newest order
  // per device/feed, including across polls, to reject delayed older captures.
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
    std::uint64_t session_generation = 1;
    bool session_active = true;
    source_state::Tracker source_states;
    pfd_submission::Pool display_copies;
  };
  struct SourceLease {
    source_state::Key key;
    ID3D12Resource* native = nullptr;
  };
  struct List {
    ID3D12GraphicsCommandList* native = nullptr;
    std::uint64_t device_key = 0, object_generation = 0, recording = 1;
    std::uint64_t session_generation = 0;
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
    OwnedGpuTiming<1> tail_timing;
    D3D12_RESOURCE_DESC description{};
    SceneCopyMatch match;
    std::uint64_t token = 0, device_key = 0, submitted = 0;
    std::uint64_t session_generation = 0;
    FrameOrder order;
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
    // Query ownership belongs to the actual private lists executed by this
    // receipt, not to a packet that may later host an application capture.
    std::uint16_t timed_tail_packets = 0;
    std::array<SourceLease, source_state::Tracker::capacity> source_leases{};
    std::size_t source_lease_count = 0;
    std::array<std::uint32_t, MaximumPackets> packet_positions{};
    std::uint32_t last_position = 0;
    std::array<engine_hook::queue_submit::Insertion, engine_hook::queue_submit::kMaximumInsertions> display_insertions{};
    std::array<unsigned, engine_hook::queue_submit::kMaximumInsertions> display_slots{};
    UINT display_count = 0, display_accepted = 0;
    std::uint64_t session_generation = 0;
  };
  struct SourceCandidate {
    ID3D12Resource* native = nullptr;  // Registry identity only; never a GPU lease.
    std::uint64_t device_key = 0, generation = 0;
    D3D12_RESOURCE_DESC description{};
  };
  // Contended callbacks cannot acquire mutex_. Publish only the bounded
  // classification of an actual native recording, never borrowed COM state.
  struct PublishedList {
    std::atomic<std::uint64_t> revision{};
    std::atomic<ID3D12GraphicsCommandList*> native{};
    std::atomic<std::uint64_t> effects{};
  };
  struct UnobservedBatch {
    std::uint32_t sources = 0, uncertain = 0;
    bool unrelated = true;
  };
  // Work a simulator thread could not do because its bounded wait expired.
  // Retirements and evidence share one ring so a Reset skipped after evidence
  // on the same list is still replayed after it. Resource pointers are
  // registry identities compared against the candidate table, never read.
  struct DeferredWork {
    enum class Kind : std::uint8_t { none, reset, destroy, legacy_barrier, enhanced_barrier, recording_report, target_report };
    static constexpr UINT TruncatedTargets = UINT_MAX;
    Kind kind = Kind::none;
    ID3D12GraphicsCommandList* native = nullptr;
    std::uint64_t generation = 0;
    D3D12_RESOURCE_BARRIER legacy{};
    D3D12_TEXTURE_BARRIER enhanced{};
    bool global = false;
    std::uint32_t reasons = 0;
    UINT target_count = 0;
    std::array<ID3D12Resource*, 8> targets{};
    std::array<std::uint64_t, 8> target_generations{};
  };
  struct TailBatch {
    std::array<ID3D12CommandList*, 2> lists{};
    unsigned count = 0;
  };
  // Simulator-thread lock acquisition with a budget. Failure records
  // contention and returns false; the caller defers or skips its own work.
  // Nothing global is invalidated. Success first drains the deferred ring so
  // a skipped Reset or earlier evidence on the same list precedes this call.
  // mutex_ reports how long each site held it (manager_lock_holds()).
  using ManagerMutex = TimedMutex<std::mutex>;
  bool evidence_lock(std::unique_lock<ManagerMutex>& lock, std::uint32_t budget_us, std::atomic<std::uint64_t>& counter) noexcept;
  void defer(const DeferredWork&) noexcept;
  void publish_source_uncertainty(std::uint32_t origin, std::uint32_t devices) noexcept;
  void escape_unordered(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
  void apply_deferred_work() noexcept;
  void apply_legacy_barrier(List&, const D3D12_RESOURCE_BARRIER&) noexcept;
  void apply_enhanced_barrier(List&, const D3D12_TEXTURE_BARRIER&) noexcept;
  void apply_recording_report(List&, bool global, std::uint32_t reasons) noexcept;
  void apply_target_report(List&, UINT count, ID3D12Resource* const*, const std::uint64_t*) noexcept;
  void wipe(Device&, WipeSite, std::uint32_t origins = 0) noexcept;
  void note_wipe(WipeSite, std::uint32_t origins = 0) noexcept;
  void retire_sources(Device&, std::uint32_t origins) noexcept;
  void retire_native_list(List&, bool destroy) noexcept;
  Device* device(std::uint64_t) noexcept;
  List* list(ID3D12GraphicsCommandList*) noexcept;
  bool register_list(ID3D12GraphicsCommandList*, std::uint64_t, std::uint64_t, bool observed) noexcept;
  bool observe_unknown_lists(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
  void retire_list(List&) noexcept;
  void publish_list(const List&) noexcept;
  void touch_sources(List&) noexcept;
  std::uint32_t queue_devices(ID3D12CommandQueue*) const noexcept;
  UnobservedBatch classify_unobserved(ID3D12CommandQueue*,
                                      UINT,
                                      ID3D12CommandList* const*,
                                      bool mark_owned = false,
                                      bool bounded = false) noexcept;
  void apply_recording_refusal(std::uint32_t effects, bool fatal) noexcept;
  void apply_deferred() noexcept;
  void apply_source_draw(List&, source_state::Key, bool allowed) noexcept;
  static void release_source_leases(std::array<SourceLease, source_state::Tracker::capacity>&, std::size_t&) noexcept;
  static bool retain_source_lease(std::array<SourceLease, source_state::Tracker::capacity>&,
                                  std::size_t&,
                                  source_state::Key,
                                  ID3D12Resource*) noexcept;
  void quarantine(Packet&) noexcept;
  void fail_device(Device&) noexcept;
  unsigned rearm_source_states_locked() noexcept;
  void collect() noexcept;
  SourceCandidate* source_candidate(ID3D12Resource*) noexcept;
  bool may_be_source(ID3D12Resource*) const noexcept;
  bool prepare_tail(Packet&, Device&) noexcept;
  // Packet slots in the order a new capture should try them: idle packets
  // already holding this device's texture shape, then empty slots, then the
  // rest. First-fit released and recreated a texture under mutex_ whenever
  // feeds of different sizes alternated.
  std::array<std::uint8_t, MaximumPackets> reuse_order(std::uint64_t device_key, const D3D12_RESOURCE_DESC&) const noexcept;
  // Records and closes this receipt's private tail captures under mutex_ and
  // returns them for the caller to Execute after releasing it.
  void record_queue_tail(Transaction&, TailBatch&) noexcept;
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
  void publish_queued_wait(Device&) noexcept;
  void release_queued_waits() noexcept;
  bool finish_transaction(std::uint64_t, bool refused, bool fatal = true) noexcept;

  SceneHandoff& handoff_;
  mutable ManagerMutex mutex_{manager_lock_holds()};
  std::mutex submission_mutex_;
  std::array<Device, MaximumDevices> devices_{};
  std::array<List, MaximumLists> lists_{};
  std::array<PublishedList, MaximumLists> published_lists_{};
  std::array<std::atomic<ID3D12Device*>, MaximumDevices> published_devices_{};
  // Fence pointer and the timeline value already passed to queue Wait. The
  // watchdog reads these without mutex_ so a stuck submit can still be released.
  struct PublishedTimeline {
    std::atomic<ID3D12Fence*> fence{nullptr};
    std::atomic<std::uint64_t> waited{0};
    std::atomic<std::uint64_t> released{0};
  };
  std::array<PublishedTimeline, MaximumDevices> published_timelines_{};
  std::atomic<std::uint32_t> deferred_sources_{}, deferred_uncertain_{}, deferred_origins_{};
  std::atomic<bool> deferred_recordings_{};
  DeferredRing<DeferredWork, 256> deferred_work_;
  std::atomic<bool> submission_gate_{true};
  std::atomic<std::uint64_t> contended_evidence_{}, contended_lifecycle_{}, contended_submissions_{};
  std::atomic<std::uint64_t> unordered_submissions_{}, deferred_retirement_count_{}, gated_submissions_{}, released_waits_{};
  std::atomic<std::uint64_t> deferred_evidence_count_{};
  std::unordered_map<ID3D12GraphicsCommandList*, std::size_t> list_indices_;
  std::array<Packet, MaximumPackets> packets_{};
  std::array<SourceCandidate, MaximumDevices * 128> sources_{};
  std::array<std::atomic<std::uint64_t>, MaximumDevices * 128> source_handles_{}, source_generations_{}, source_device_keys_{};
  std::array<std::atomic<std::uint64_t>, 16> source_filter_{};
  bool source_tracking_ = false;
  bool draining_ = false;  // apply_deferred_work is running on the lock holder; nested calls return.
  bool capture_enabled_ = true;
  bool gpu_timing_enabled_ = false;
  std::uint32_t source_rate_ = kDefaultCameraRate;
  std::array<std::uint64_t, 3> last_tail_us_{};
  Transaction transaction_;
  std::uint64_t next_token_ = 0, next_receipt_ = 0;
  Statistics stats_;
  UnknownListObserver unknown_list_observer_ = nullptr;
  void* unknown_list_context_ = nullptr;
  DisplaySubmissionPlanner display_planner_ = nullptr;
  void* display_planner_context_ = nullptr;
};

}  // namespace taxi_camera
