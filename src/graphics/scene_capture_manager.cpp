#include "scene_capture_manager.hpp"
#include "../camera/aircraft_mounts.hpp"
#include "../hooks/render_boundary_observer.hpp"
#include "../profiles/catalog.hpp"
#include "native_device_identity.hpp"

#include <chrono>
#include <limits>
#include <utility>

namespace taxi_camera {
namespace {
thread_local SceneCaptureManager* transaction_owner = nullptr;
thread_local std::uint64_t thread_receipt = 0;
struct SourceDrawStage {
  SceneCaptureManager* owner = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  std::uint64_t generation = 0;
  std::array<source_state::Key, 8> keys{};
  UINT count = 0;
};
thread_local SourceDrawStage source_stage;
// Draw-repeat filter. Repeated draws to one source collapse into one draw
// effect with a count (Recording::append), and capture_phase::decide only asks
// whether a batch drew more than once. Once each candidate target's last
// effect in this recording is a draw counted at least twice, with its lease
// held (or none to take), a further allowed draw from this thread changes no
// decision: it skips mutex_ and its count is added at this thread's next
// evidence lock or Execute observation (other mutex_ holders do not settle).
//
// Thread safety: the filter is thread-local and armed only under mutex_. Every
// event that could change what a locked draw does disarms it without the lock:
// - Reset changes the adapter recording (Close stops the adapter's draws).
// - Every locked list() lookup of the list, from any thread, bumps its
//   list_epochs_ slot.
//   D3D12 lists are not free-threaded, so work another thread did on this
//   list is ordered before this thread's next draw by the application, and
//   that store is visible here.
// - defer() bumps draw_repeat_epoch_ after its push. The epoch is sampled
//   before the arming lock drains the ring, so a sample that includes a bump
//   (release/acquire) also saw its entry drained.
// - Session, tracking, device, candidate, overflow and capture-phase changes
//   bump draw_repeat_epoch_ under mutex_ (unregister without it).
// A skip racing such an event is a locked draw ordered before it. The skipped
// count is added only to the effect it collapsed into (same List recording and
// index); otherwise, or if this thread never locks again, diagnostics lose it.
struct DrawRepeat {
  SceneCaptureManager* owner = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  std::uint64_t generation = 0, recording = 0, list_recording = 0;
  std::uint64_t epoch = 0, list_epoch = 0;
  std::size_t slot = 0;
  std::array<source_state::Key, 8> keys{};
  std::array<std::uint16_t, 8> effects{};  // Index of each key's draw effect.
  std::array<std::uint32_t, 8> pending{};  // Draws skipped per key.
  UINT count = 0;
};
thread_local DrawRepeat draw_repeat;
// Source devices of a batch this thread forwarded without ordering; published
// after the native forward through forwarded_unordered.
thread_local std::uint32_t escaped_sources = 0;
thread_local bool contended_call = false;
constexpr auto MaximumCounter = std::numeric_limits<std::uint64_t>::max() - 1;
constexpr std::uint32_t ConsumerEffect = 1u << 16, SourceEffect = 1u << 17, UnobservedEffect = 1u << 18;
constexpr unsigned DeviceShift = 24;
constexpr std::uint64_t EscapedRecording = 1u << 28, RecordingVersion = 1ull << 32;
constexpr std::uint64_t ExhaustedRecording = 1u << 29, RecordingVersionMask = 0xffffffff00000000ull;
// Escaped because OUR wait budget expired or the watchdog gate was closed, not
// because another submitter owned the queue. The same packets are quarantined,
// but a consumer recording invalidates the source model instead of failing the
// device: the stable output it read is process-lifetime, so the worst case is
// one torn camera frame, and the session must be able to continue.
constexpr std::uint64_t BoundedEscapedRecording = 1u << 30;
bool same_description(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) noexcept {
  return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
         a.MipLevels == b.MipLevels && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
         a.SampleDesc.Quality == b.SampleDesc.Quality;
}
std::uint64_t steady_now_us() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Least spacing of two captures of one feed at 1..60 captures/s. The render
// schedule sets the cadence: it opens a feed once 1/rate has passed on
// GetTickCount64 (about 15.6 ms steps) at observer time, and the render is
// submitted a varying time later. The full interval here discarded 0.6-8 % of
// finished parked renders (rate 2, 2026-10-04 log), so it allows
// min(interval / 4, 125 ms) for that rounding and lag, but never shortens the
// spacing below 100 ms: rates from 10 up keep the full interval, and a render
// one frame after a capture is still refused above 10 fps.
constexpr std::uint64_t capture_spacing_us(std::uint32_t rate) noexcept {
  const std::uint64_t interval = (1000000u + rate - 1) / rate;
  return std::max(interval - std::min<std::uint64_t>(interval / 4, 125000), std::min<std::uint64_t>(interval, 100000));
}
static_assert(capture_spacing_us(1) == 875000 && capture_spacing_us(2) == 375000 && capture_spacing_us(5) == 150000);
static_assert(capture_spacing_us(9) == 100000 && capture_spacing_us(10) == 100000 && capture_spacing_us(60) == 16667);
}  // namespace

SceneCaptureManager::SceneCaptureManager(SceneHandoff& handoff) noexcept : handoff_(handoff) {
  list_indices_.reserve(MaximumLists);
}
SceneCaptureManager::~SceneCaptureManager() {
  // Native device/timeline leases are intentionally process-lifetime, and so
  // is each slot's last-Signal queue lease in production, where the manager is
  // never destroyed. No submission can own the manager while it is destroyed.
  for (auto& owner : devices_)
    if (owner.last_signal_queue)
      owner.last_signal_queue->Release();
}

bool SceneCaptureManager::last_call_contended() noexcept {
  return contended_call;
}
void SceneCaptureManager::publish_source_uncertainty(std::uint32_t origin, std::uint32_t devices) noexcept {
  if (!devices)
    return;
  // Origin before devices: a reader that sees the devices also sees who set them.
  deferred_origins_.fetch_or(origin, std::memory_order_release);
  deferred_sources_.fetch_or(devices, std::memory_order_release);
}
LockHoldStats& manager_lock_holds() noexcept {
  static LockHoldStats stats;
  return stats;
}
// Out of line so the hold is attributed to the hook that asked for evidence.
__attribute__((noinline)) bool SceneCaptureManager::evidence_lock(std::unique_lock<ManagerMutex>& lock,
                                                                  std::uint32_t budget_us,
                                                                  std::atomic<std::uint64_t>& counter) noexcept {
  BoundedLock bounded(mutex_, budget_us, &counter, hook_timing::manager_lock);
  contended_call = !bounded;
  if (!bounded)
    return false;
  bounded.release();
  mutex_.attribute(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  lock = std::unique_lock<ManagerMutex>(mutex_, std::adopt_lock);
  apply_deferred_work();
  settle_draw_repeat();
  return true;
}
void SceneCaptureManager::defer(const DeferredWork& work) noexcept {
  // Overflow is remembered by the ring and handled by the next drain; the
  // producer has nothing else it may touch without the lock.
  deferred_work_.push(work);
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);  // After the push: see DrawRepeat.
  if (work.kind == DeferredWork::Kind::reset || work.kind == DeferredWork::Kind::destroy)
    deferred_retirement_count_.fetch_add(1, std::memory_order_relaxed);
  else
    deferred_evidence_count_.fetch_add(1, std::memory_order_relaxed);
}
void SceneCaptureManager::note_wipe(WipeSite site, std::uint32_t origins) noexcept {
  forget_capture_phase();  // Every global wipe names itself here.
  ++stats_.wipes;
  ++stats_.wipe_counts[static_cast<std::size_t>(site)];
  stats_.last_wipe_site = site;
  stats_.last_wipe_origins = origins;
  stats_.last_wipe_us = steady_now_us();
  for (std::size_t bit = 0; bit < OriginCount; ++bit)
    if (origins & (1u << bit))
      ++stats_.wipe_origin_counts[bit];
}
void SceneCaptureManager::wipe(Device& owner, WipeSite site, std::uint32_t origins) noexcept {
  owner.source_states.invalidate_all();
  note_wipe(site, origins);
}
void SceneCaptureManager::retire_sources(Device& owner, std::uint32_t origins) noexcept {
  // The escaped batch's own recordings were never applied, so nothing here can
  // have moved a model away from its last positively observed RT state. Retire
  // the live models like a named-source pass report and restore that RT model
  // now, on this lock holder, rather than after CaptureProgress::StallMs. The
  // drawn flag is cleared, so the next ordered draw is the first capture.
  owner.source_states.retire_live_models();
  forget_capture_phase();
  const auto restored = owner.source_states.rearm_retained_rt();
  ++stats_.source_retirements;
  stats_.retirement_restored += restored;
  stats_.last_retirement_origins = origins;
  stats_.last_retirement_us = steady_now_us();
  for (std::size_t bit = 0; bit < OriginCount; ++bit)
    if (origins & (1u << bit))
      ++stats_.retirement_origin_counts[bit];
  if (restored)
    stats_.tail_status = "awaiting_ordered_source_state";
}
void SceneCaptureManager::escape_unordered(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  // Same shape as the PR 31 contended path: owned recordings are marked before
  // the forward, source effects publish after it. The mark is the bounded one:
  // this escape is our budget or gate, so it never fails the device.
  const auto batch = classify_unobserved(queue, count, lists, true, true);
  publish_source_uncertainty(OriginEscapeUnordered, batch.uncertain);
  // Evidence still deferred in the ring cannot be attributed to this batch's
  // lists without the lock: an escape with a non-empty ring is genuinely
  // unknown, so its source models are published after the forward.
  escaped_sources |= batch.sources | (deferred_work_.empty() ? 0u : queue_devices(queue));
  unordered_submissions_.fetch_add(1, std::memory_order_relaxed);
}
void SceneCaptureManager::forwarded_unordered(ID3D12CommandQueue*) noexcept {
  const auto sources = escaped_sources;
  escaped_sources = 0;
  if (sources)
    submission_refused_completed(sources);
}
void SceneCaptureManager::set_submission_gate(bool open) noexcept {
  const bool was_open = submission_gate_.exchange(open, std::memory_order_acq_rel);
  if (!open && was_open)
    release_queued_waits();
}
void SceneCaptureManager::release_queued_waits() noexcept {
  // Called on the watchdog thread while a simulator thread may hold
  // submission_mutex_ inside the queue Wait that ReShade's immediate-list
  // flush is stuck behind.
  // ID3D12Fence::Signal from the CPU satisfies that GPU wait. It does not take
  // either bridge mutex, and it signals only the value already passed to Wait.
  constexpr auto removed = std::numeric_limits<std::uint64_t>::max();
  for (auto& slot : published_timelines_) {
    auto* fence = slot.fence.load(std::memory_order_acquire);
    const auto waited = slot.waited.load(std::memory_order_acquire);
    if (!fence || !waited)
      continue;
    auto released = slot.released.load(std::memory_order_acquire);
    if (waited <= released)
      continue;
    const auto completed = fence->GetCompletedValue();
    if (completed == removed || completed >= waited) {
      slot.released.compare_exchange_strong(released, waited, std::memory_order_release, std::memory_order_relaxed);
      continue;
    }
    if (FAILED(fence->Signal(waited)))
      continue;
    slot.released.store(waited, std::memory_order_release);
    released_waits_.fetch_add(1, std::memory_order_relaxed);
  }
}
void SceneCaptureManager::publish_queued_wait(Device& owner) noexcept {
  const auto index = static_cast<std::size_t>(&owner - devices_.data());
  if (index >= published_timelines_.size() || !owner.timeline || !owner.last_signal)
    return;
  auto& slot = published_timelines_[index];
  slot.fence.store(owner.timeline, std::memory_order_release);
  auto seen = slot.waited.load(std::memory_order_relaxed);
  while (seen < owner.last_signal &&
         !slot.waited.compare_exchange_weak(seen, owner.last_signal, std::memory_order_release, std::memory_order_relaxed)) {
  }
}
bool SceneCaptureManager::submission_gate_open() const noexcept {
  return submission_gate_.load(std::memory_order_acquire);
}
void SceneCaptureManager::retire_native_list(List& item, bool destroy) noexcept {
  // Every caller found item through list_indices_, so its position in lists_
  // is its published slot. One publish per retirement: a list being Reset or
  // destroyed is in no concurrent batch, so no reader could see a middle word,
  // and the one exchange still consumes the old recording's escape marks.
  const auto index = static_cast<std::size_t>(&item - lists_.data());
  const auto* owner = device(item.device_key);
  // Only the version and the counters change for a list this clean.
  const bool clean = !destroy && owner && item.session_generation == owner->session_generation && !item.awaiting_native_reset &&
                     !item.packets && !item.feeds && !item.consumer && !item.source_touched && !item.source_lease_count &&
                     !item.source_effects.count && !item.source_effects.invalid && !item.source_effects.overflowed;
  retire_list(item);
  if (destroy) {
    publish_list(item, index);
    published_lists_[index].native.store(nullptr, std::memory_order_release);
    list_indices_.erase(item.native);
    item.native = nullptr;
    return;
  }
  if (owner)
    item.session_generation = owner->session_generation;
  item.awaiting_native_reset = false;
  publish_list(item, index);
  ++stats_.resets;
  stats_.clean_resets += clean;
  // Retiring it again would change only its version, recording counter and
  // statistics until a change forgets this (successful_reset). Never when the
  // next retirement would exhaust the recording counter and fail the device.
  if (owner && item.recording < MaximumCounter)
    published_lists_[index].clean.store(item.object_generation, std::memory_order_release);
}
void SceneCaptureManager::forget_clean(const List& item) noexcept {
  published_lists_[static_cast<std::size_t>(&item - lists_.data())].forget_clean();
}
void SceneCaptureManager::apply_deferred_work() noexcept {
  // Retiring a list reenters apply_deferred; the outer loop owns the ring. An
  // idle ring would pop nothing, so return before writing draining_, which
  // shares a line with flags every holder reads.
  if (draining_ || deferred_work_.idle())
    return;
  draining_ = true;
  if (deferred_work_.overflowed()) {
    // A lost entry may have been a Reset, so every current recording may carry
    // effects of a retired one. No recording may be applied again before its
    // next observed Reset renews it, and the models it fed are wiped once.
    // Clean marks are forgotten before the flag is cleared: a lock-free Reset
    // that reads it cleared (acquire, from this exchange) finds its list dirty,
    // and nothing marks a list again before the drain retires one. Recordings
    // are invalidated after the exchange, as before, so an entry lost during
    // that loop sets the flag again for the next holder.
    for (std::size_t index = 0; index < lists_.size(); ++index)
      if (lists_[index].native)
        published_lists_[index].forget_clean();
    deferred_work_.take_overflow();
    for (auto& item : lists_)
      if (item.native)
        item.source_effects.invalidate();
    draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
    ++stats_.deferred_overflows;
    publish_source_uncertainty(OriginDeferredOverflow, queue_devices(nullptr));
  }
  DeferredWork entry;
  // Commit after applying: a lock-free Reset that sees the ring idle has then
  // seen the entry's list forgotten. A head entry still being written stops
  // the drain, as before.
  for (; deferred_work_.peek(entry); deferred_work_.commit()) {
    auto* item = list(entry.native);
    if (!item || item->object_generation != entry.generation)
      continue;
    forget_clean(*item);
    switch (entry.kind) {
      case DeferredWork::Kind::reset:
        retire_native_list(*item, false);
        break;
      case DeferredWork::Kind::destroy:
        retire_native_list(*item, true);
        break;
      case DeferredWork::Kind::legacy_barrier:
        apply_legacy_barrier(*item, entry.legacy);
        break;
      case DeferredWork::Kind::enhanced_barrier:
        apply_enhanced_barrier(*item, entry.enhanced);
        break;
      case DeferredWork::Kind::recording_report:
        apply_recording_report(*item, entry.global, entry.reasons);
        break;
      case DeferredWork::Kind::target_report:
        apply_target_report(*item, entry.target_count, entry.targets.data(), entry.target_generations.data());
        break;
      case DeferredWork::Kind::none:
        break;
    }
  }
  draining_ = false;
}

SceneCaptureManager::DisplaySubmissionPlan::~DisplaySubmissionPlan() {
  for (auto& item : items) {
    if (item.copy.target)
      item.copy.target->Release();
    if (item.copy.source)
      item.copy.source->Release();
  }
}
bool SceneCaptureManager::DisplaySubmissionPlan::current() const noexcept {
  return count && count <= items.size() && device_key && generation && current_generation &&
         current_generation->load(std::memory_order_acquire) == generation;
}
bool SceneCaptureManager::set_display_submission_planner(DisplaySubmissionPlanner planner, void* context) noexcept {
  const std::lock_guard lock(mutex_);
  if (display_planner_ && (display_planner_ != planner || display_planner_context_ != context))
    return false;
  display_planner_ = planner;
  display_planner_context_ = context;
  return true;
}
void SceneCaptureManager::service_display_submissions() noexcept {
  const std::unique_lock submission_lock(submission_mutex_, std::try_to_lock);
  if (!submission_lock.owns_lock())
    return;
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  std::array<std::pair<Device*, std::uint64_t>, MaximumDevices> ready{};
  unsigned count = 0;
  for (auto& owner : devices_)
    if (owner.active && !owner.failed)
      ready[count++] = {&owner, owner.timeline->GetCompletedValue()};
  // Releasing the last target lease can run its native lifetime callback,
  // which reenters manager metadata. Submission serialization alone protects
  // the packet pools, so do not retain the metadata lock across COM Release.
  lock.unlock();
  for (unsigned i = 0; i < count; ++i)
    ready[i].first->display_copies.service(ready[i].first->native, ready[i].second);
}
UINT SceneCaptureManager::augment_submission(ID3D12CommandQueue* queue,
                                             std::uint64_t receipt,
                                             engine_hook::queue_submit::Insertion* output,
                                             UINT capacity) noexcept {
  // No mutex_: only the receipt's own thread passes the owner check, and it
  // holds submission_mutex_, which every writer of transaction_ holds. The
  // session fields change under mutex_, so read their mirror. A concurrent
  // reset_session lands before or after this load, as it did around a lock.
  if (transaction_owner != this || transaction_.id != receipt || transaction_.queue != queue || !transaction_.device ||
      transaction_.display_count > capacity || !output)
    return 0;
  const auto session = transaction_.device->published_session.load(std::memory_order_acquire);
  if (!(session & 1u) || session >> 1 != transaction_.session_generation)
    return 0;
  for (UINT i = 0; i < transaction_.display_count; ++i)
    output[i] = transaction_.display_insertions[i];
  return transaction_.display_count;
}
void SceneCaptureManager::augmentation_result(ID3D12CommandQueue* queue, std::uint64_t receipt, UINT inserted) noexcept {
  if (transaction_owner == this && transaction_.id == receipt && transaction_.queue == queue)
    transaction_.display_accepted = inserted == transaction_.display_count ? inserted : 0;
}

SceneCaptureManager::Device* SceneCaptureManager::device(std::uint64_t key) noexcept {
  for (auto& item : devices_)
    if (item.key == key && key != 0)
      return &item;
  return nullptr;
}
SceneCaptureManager::List* SceneCaptureManager::list(ID3D12GraphicsCommandList* native) noexcept {
  const auto found = list_indices_.find(native);
  if (found == list_indices_.end())
    return nullptr;
  // Under mutex_, the only writer, before any work on this list. Odd: a
  // draw-repeat filter was armed on it since; even disarms it on every thread.
  auto& epoch = list_epochs_[found->second];
  if (const auto value = epoch.load(std::memory_order_relaxed); value & 1)
    epoch.store(value + 1, std::memory_order_release);
  return &lists_[found->second];
}

bool SceneCaptureManager::register_device(std::uint64_t key, ID3D12Device* native) noexcept {
  const std::lock_guard lock(mutex_);
  if (!key || !native)
    return false;
  if (auto* existing = device(key))
    return existing->active && !existing->failed && existing->native == native;
  for (auto& item : devices_) {
    if (item.key)
      continue;
    ID3D12Fence* timeline = nullptr;
    if (FAILED(native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline))))
      return false;
    native->AddRef();
    item.key = key;
    item.native = native;
    item.timeline = timeline;
    item.active = true;
    const auto index = static_cast<std::size_t>(&item - devices_.data());
    published_devices_[index].store(native, std::memory_order_release);
    published_timelines_[index].fence.store(timeline, std::memory_order_release);
    return true;
  }
  return false;
}

void SceneCaptureManager::quarantine(Packet& packet) noexcept {
  if (!packet.quarantined) {
    packet.quarantined = true;
    ++stats_.quarantined;
  }
}
void SceneCaptureManager::fail_device(Device& owner) noexcept {
  owner.failed = true;
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  wipe(owner, WipeSite::fail_device);
  for (auto& packet : packets_)
    if (packet.assigned && packet.device_key == owner.key)
      quarantine(packet);
}
void SceneCaptureManager::destroy_device(std::uint64_t key) noexcept {
  const std::lock_guard lock(mutex_);
  if (auto* owner = device(key)) {
    owner->active = false;
    fail_device(*owner);
    for (auto& item : lists_)
      if (item.device_key == key)
        release_source_leases(item.source_leases, item.source_lease_count);
  }
}

std::uint64_t SceneCaptureManager::reset_session(std::uint64_t key) noexcept {
  const std::lock_guard lock(mutex_);
  auto* owner = device(key);
  if (!owner || !owner->active)
    return 0;
  owner->session_active = false;
  owner->publish_session();
  if (owner->session_generation >= MaximumCounter) {
    fail_device(*owner);
    return 0;
  }
  // Every list's next Reset must adopt the new session. A lock-free Reset that
  // still saw its list clean is ordered before this call, like a locked Reset
  // that won mutex_ first. Other devices' lists only lose one lock-free Reset.
  for (const auto& entry : list_indices_)
    published_lists_[entry.second].forget_clean();
  ++owner->session_generation;
  owner->publish_session();
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  owner->source_states.clear();
  note_wipe(WipeSite::session_reset);
  // Retain identities, not the previous flight's state or last-known RT model.
  for (std::size_t index = 0; index < sources_.size(); ++index) {
    const auto& source = sources_[index];
    if (source.native && source.device_key == key && source_generations_[index].load(std::memory_order_acquire) == source.generation)
      owner->source_states.register_source({reinterpret_cast<std::uint64_t>(source.native), source.generation});
  }
  last_tail_us_ = {};
  phase_feeds_ = {};
  stats_.tail_status = "session_stopped";
  // Never retire lists, clear packet masks/consumer flags, release recording
  // leases or reset a GPU allocator here. Old native lists remain replayable.
  return owner->session_generation;
}

bool SceneCaptureManager::resume_session(std::uint64_t key, std::uint64_t generation) noexcept {
  const std::lock_guard lock(mutex_);
  auto* owner = device(key);
  if (!owner || !owner->active || owner->failed || !generation || owner->session_generation != generation)
    return false;
  owner->session_active = true;
  owner->publish_session();
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);  // Draws take leases again.
  return true;
}

bool SceneCaptureManager::register_command_list(ID3D12GraphicsCommandList* native, std::uint64_t key, std::uint64_t generation) noexcept {
  return register_list(native, key, generation, true);
}
bool SceneCaptureManager::register_unobserved_command_list(ID3D12GraphicsCommandList* native,
                                                           std::uint64_t key,
                                                           std::uint64_t generation) noexcept {
  return register_list(native, key, generation, false);
}
bool SceneCaptureManager::register_list(ID3D12GraphicsCommandList* native,
                                        std::uint64_t key,
                                        std::uint64_t generation,
                                        bool observed) noexcept {
  // A skipped registration publishes nothing: the list stays unknown, and an
  // unknown list in a batch already invalidates the model at submission.
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::lifecycle_us, contended_lifecycle_))
    return false;
  auto* owner = device(key);
  if (!native || !generation || !owner || !owner->active || owner->failed || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  if (!same_native_device(native, owner->native))
    return false;
  if (auto* existing = list(native)) {
    if (existing->object_generation == generation)
      return existing->device_key == key;
    // The adapter registers one object per address; an older generation here
    // is a destroyed list whose retirement was lost. Retire it and admit.
    retire_native_list(*existing, true);
  }
  for (std::size_t index = 0; index < lists_.size(); ++index) {
    if (lists_[index].native)
      continue;
    lists_[index] = {};
    lists_[index].native = native;
    lists_[index].device_key = key;
    lists_[index].object_generation = generation;
    lists_[index].session_generation = owner->session_generation;
    lists_[index].awaiting_native_reset = !observed;
    list_indices_.emplace(native, index);
    publish_list(lists_[index]);
    return true;
  }
  return false;
}

void SceneCaptureManager::publish_list(const List& item, std::size_t slot) noexcept {
  if (slot >= published_lists_.size()) {
    const auto found = list_indices_.find(item.native);
    if (found == list_indices_.end())
      return;  // A private tail recording has no application admission identity.
    slot = found->second;
  }
  auto& published = published_lists_[slot];
  published.forget_clean();  // A retirement marks it clean again after this.
  std::uint32_t devices = 0;
  for (std::size_t index = 0; index < devices_.size(); ++index)
    if (devices_[index].key == item.device_key)
      devices = 1u << index;
  published.revision.fetch_add(1);
  const auto previous_version = published.effects.load() & RecordingVersionMask;
  const auto version =
      previous_version == RecordingVersionMask ? previous_version | ExhaustedRecording : previous_version + RecordingVersion;
  const auto previous = published.effects.exchange(version | item.packets | (item.consumer ? ConsumerEffect : 0u) |
                                                   (item.source_touched ? SourceEffect : 0u) |
                                                   (item.awaiting_native_reset ? UnobservedEffect : 0u) | (devices << DeviceShift));
  // Retirement/Reset must consume a notice before its packet slots can be
  // recycled, even if the notifying thread has not yet published the wake bit.
  if (previous & (EscapedRecording | BoundedEscapedRecording))
    apply_recording_refusal(static_cast<std::uint32_t>(previous), (previous & EscapedRecording) != 0);
  published.native.store(item.native);
  published.revision.fetch_add(1);
}
void SceneCaptureManager::touch_sources(List& item) noexcept {
  if (!item.source_touched) {
    item.source_touched = true;
    publish_list(item);
  }
}
std::uint32_t SceneCaptureManager::queue_devices(ID3D12CommandQueue* queue) const noexcept {
  std::uint32_t present = 0;
  for (std::size_t index = 0; index < published_devices_.size(); ++index)
    if (auto* native = published_devices_[index].load(std::memory_order_acquire)) {
      present |= 1u << index;
      if (queue && same_native_device(queue, native))
        return 1u << index;
    }
  return present;  // Failed identity is uncertainty, never evidence of no work.
}
SceneCaptureManager::UnobservedBatch SceneCaptureManager::classify_unobserved(ID3D12CommandQueue* queue,
                                                                              UINT count,
                                                                              ID3D12CommandList* const* native_lists,
                                                                              bool mark_owned,
                                                                              bool bounded) noexcept {
  UnobservedBatch result;
  const auto mark = bounded ? BoundedEscapedRecording : EscapedRecording;
  if (!native_lists || !count || count > engine_hook::queue_submit::kMaximumCommandLists) {
    result.unrelated = false;
    result.uncertain = queue_devices(queue);
    return result;
  }
  // Skipped evidence still waiting in the ring may belong to a list in this
  // batch whose published word does not show it yet. Such a batch must take
  // the ordered path, where the drain precedes the recheck and the apply.
  result.unrelated = deferred_work_.empty();
  bool unknown = false;
  for (UINT index = 0; index < count; ++index) {
    bool found = false;
    for (auto& published : published_lists_) {
      if (published.native.load() != native_lists[index] || !native_lists[index])
        continue;
      found = true;
      const auto revision = published.revision.load();
      auto word = published.effects.load();
      if ((revision & 1u) || (word & ExhaustedRecording) || revision != published.revision.load() ||
          published.native.load() != native_lists[index]) {
        result.unrelated = false;
        result.uncertain |= queue_devices(queue);
        break;  // Never spin on a concurrent recording update.
      }
      const auto effects = static_cast<std::uint32_t>(word);
      const auto devices = (effects >> DeviceShift) & 15u;
      const bool owned = (effects & (ConsumerEffect | 0xffffu)) != 0;
      if (owned && mark_owned) {
        // One CAS, no retry/spin. The version binds the notice to this recording
        // and makes concurrent Reset consume it before reusing any packet.
        if (published.effects.compare_exchange_strong(word, word | mark))
          deferred_recordings_.store(true, std::memory_order_release);
        else if (bounded)
          result.sources |= devices;
        else
          result.uncertain |= devices;
      }
      if (effects & (SourceEffect | UnobservedEffect))
        result.sources |= devices;
      result.unrelated &= (effects & (ConsumerEffect | SourceEffect | UnobservedEffect | 0xffffu)) == 0;
      break;
    }
    // An unregistered native list cannot contain bridge-recorded work. Its
    // application source effects are unknown and must invalidate the model.
    unknown |= !found;
  }
  if (unknown) {
    result.unrelated = false;
    result.sources |= queue_devices(queue);
  }
  return result;
}
void SceneCaptureManager::apply_recording_refusal(std::uint32_t effects, bool fatal) noexcept {
  const auto packets = static_cast<std::uint16_t>(effects);
  for (std::size_t index = 0; index < packets_.size(); ++index)
    if ((packets & (1u << index)) && packets_[index].assigned)
      quarantine(packets_[index]);
  if (!(effects & ConsumerEffect))
    return;
  for (std::size_t index = 0; index < devices_.size(); ++index) {
    if (!((effects >> DeviceShift) & (1u << index)) || !devices_[index].active)
      continue;
    if (fatal) {
      fail_device(devices_[index]);
    } else {
      retire_sources(devices_[index], OriginUnorderedConsumer);
      ++stats_.unordered_consumers;
    }
  }
}
void SceneCaptureManager::apply_deferred() noexcept {
  apply_deferred_work();
  // Every lock holder drains these. Read before exchanging so the common empty
  // case does not write lines that recording threads set.
  if (deferred_recordings_.load(std::memory_order_acquire) && deferred_recordings_.exchange(false, std::memory_order_acq_rel))
    for (auto& published : published_lists_) {
      auto word = published.effects.load();
      const auto marks = word & (EscapedRecording | BoundedEscapedRecording);
      if (marks && published.effects.compare_exchange_strong(word, word & ~marks))
        apply_recording_refusal(static_cast<std::uint32_t>(word), (marks & EscapedRecording) != 0);
    }
  const auto failed = deferred_uncertain_.load(std::memory_order_acquire) ? deferred_uncertain_.exchange(0, std::memory_order_acq_rel) : 0u;
  const auto sources = deferred_sources_.load(std::memory_order_acquire) ? deferred_sources_.exchange(0, std::memory_order_acq_rel) : 0u;
  const auto origins = sources ? deferred_origins_.exchange(0, std::memory_order_acq_rel) : 0u;
  for (std::size_t index = 0; index < devices_.size(); ++index) {
    auto& owner = devices_[index];
    if (!owner.active)
      continue;
    if (failed & (1u << index))
      fail_device(owner);
    else if (!(sources & (1u << index)))
      continue;
    else if (origins & GlobalOrigins)
      wipe(owner, WipeSite::deferred_sources, origins);
    else
      retire_sources(owner, origins);
  }
}

bool SceneCaptureManager::set_unknown_list_observer(UnknownListObserver observer, void* context) noexcept {
  const std::lock_guard lock(mutex_);
  if (!observer || (unknown_list_observer_ && (unknown_list_observer_ != observer || unknown_list_context_ != context)))
    return false;
  unknown_list_observer_ = observer;
  unknown_list_context_ = context;
  return true;
}

bool SceneCaptureManager::observe_unknown_lists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* native_lists) noexcept {
  std::array<ID3D12GraphicsCommandList*, engine_hook::queue_submit::kMaximumCommandLists> missing{};
  UINT missing_count = 0;
  std::uint64_t key = 0;
  UnknownListObserver observer = nullptr;
  void* context = nullptr;
  {
    const std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
      return false;
    settle_draw_repeat();  // Skipped draws reach the batch's effects before it is applied.
    observer = unknown_list_observer_;
    context = unknown_list_context_;
    if (!observer)
      return true;
    for (UINT i = 0; i < count; ++i) {
      auto* native = static_cast<ID3D12GraphicsCommandList*>(native_lists[i]);
      if (native && !list(native))
        missing[missing_count++] = native;
    }
    if (!missing_count)
      return true;
    for (const auto& owner : devices_)
      if (owner.active && !owner.failed && compatible_queue(queue, owner)) {
        key = owner.key;
        break;
      }
  }
  // Hook installation can call back into this manager. Never hold mutex_ here.
  // The application's Execute arguments keep these exact objects alive.
  if (key)
    for (UINT i = 0; i < missing_count; ++i)
      observer(context, missing[i], key);
  return true;
}

void SceneCaptureManager::release_source_leases(std::array<SourceLease, source_state::Tracker::capacity>& leases,
                                                std::size_t& count) noexcept {
  // Release can synchronously call unregister_source_candidate, which is
  // deliberately lock-free. Clear each owned slot before that callback.
  while (count) {
    auto& lease = leases[--count];
    auto* resource = lease.native;
    lease = {};
    resource->Release();
  }
}
bool SceneCaptureManager::retain_source_lease(std::array<SourceLease, source_state::Tracker::capacity>& leases,
                                              std::size_t& count,
                                              source_state::Key key,
                                              ID3D12Resource* resource) noexcept {
  for (std::size_t index = 0; index < count; ++index)
    if (leases[index].key == key)
      return leases[index].native == resource;
  if (!resource || count == leases.size())
    return false;
  // Caller must already have a live application draw argument or an owned
  // recording lease. The candidate identity registry is never sufficient.
  resource->AddRef();
  leases[count++] = {key, resource};
  return true;
}
void SceneCaptureManager::retire_list(List& item) noexcept {
  apply_deferred();
  for (std::size_t index = 0; index < packets_.size(); ++index)
    if (item.packets & (1u << index))
      packets_[index].retired = true;
  item.packets = 0;
  item.feeds = 0;
  item.consumer = false;
  item.source_effects.reset();
  item.source_touched = false;
  release_source_leases(item.source_leases, item.source_lease_count);
  if (item.recording < MaximumCounter)
    ++item.recording;
  else if (auto* owner = device(item.device_key))
    fail_device(*owner);
}

SceneCaptureManager::SourceCandidate* SceneCaptureManager::source_candidate(ID3D12Resource* resource) noexcept {
  for (std::size_t index = 0; index < sources_.size(); ++index)
    if (sources_[index].native == resource && resource && source_generations_[index].load(std::memory_order_acquire))
      return &sources_[index];
  return nullptr;
}
SceneCaptureManager::SourceFilterBits SceneCaptureManager::source_filter_bits(const void* resource) noexcept {
  // Fibonacci hashing mixes into the high bits: word from the top 8, two bits
  // in that word from the next 12.
  const auto hash = (reinterpret_cast<std::uint64_t>(resource) >> 4) * 0x9e3779b97f4a7c15ull;
  static_assert(SourceFilterWords == 256);
  return {static_cast<std::size_t>(hash >> 56), (1ull << ((hash >> 50) & 63)) | (1ull << ((hash >> 44) & 63))};
}
bool SceneCaptureManager::may_be_source(ID3D12Resource* resource) const noexcept {
  if (!resource)
    return false;
  const auto bits = source_filter_bits(resource);
  if ((source_filter_[bits.word].load(std::memory_order_acquire) & bits.mask) != bits.mask)
    return false;
  // Registration publishes the handle and generation, then the slot bound,
  // then the filter bits; the acquire above makes all of them visible here.
  const auto handle = reinterpret_cast<std::uint64_t>(resource);
  const auto used = std::min(source_slots_used_.load(std::memory_order_acquire), sources_.size());
  for (std::size_t index = 0; index < used; ++index)
    if (source_handles_[index].load(std::memory_order_relaxed) == handle && source_generations_[index].load(std::memory_order_acquire))
      return true;
  return false;
}
void SceneCaptureManager::rebuild_source_filter() noexcept {
  // Every live candidate's bits are set in both the old and the new word, so a
  // concurrent reader never misses one; only retired candidates' bits clear.
  // Registration also holds mutex_, so no new bit can be lost.
  std::array<std::uint64_t, SourceFilterWords> words{};
  for (const auto& source : sources_)
    if (source.native) {
      const auto bits = source_filter_bits(source.native);
      words[bits.word] |= bits.mask;
    }
  for (std::size_t word = 0; word < words.size(); ++word)
    if (source_filter_[word].load(std::memory_order_relaxed) != words[word])
      source_filter_[word].store(words[word], std::memory_order_release);
}
unsigned SceneCaptureManager::rearm_source_states_locked() noexcept {
  unsigned restored = 0;
  for (auto& owner : devices_)
    if (owner.active && !owner.failed)
      restored += owner.source_states.rearm_retained_rt();
  if (restored) {
    forget_capture_phase();  // A restored model carries no ordering evidence.
    stats_.tail_status = "awaiting_ordered_source_state";
  }
  return restored;
}

void SceneCaptureManager::begin_source_tracking() noexcept {
  const std::lock_guard lock(mutex_);
  source_tracking_ = true;
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  last_tail_us_ = {};
  phase_feeds_ = {};
  rearm_source_states_locked();
  stats_.tail_status = "awaiting_ordered_source_state";
}

unsigned SceneCaptureManager::rearm_source_states() noexcept {
  const std::lock_guard lock(mutex_);
  return rearm_source_states_locked();
}
void SceneCaptureManager::stop_source_tracking() noexcept {
  const std::lock_guard lock(mutex_);
  source_tracking_ = false;
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  phase_feeds_ = {};
  for (auto& item : lists_)
    release_source_leases(item.source_leases, item.source_lease_count);
  stats_.tail_status = "stopped";
}
void SceneCaptureManager::set_source_rate(std::uint32_t rate) noexcept {
  const std::lock_guard lock(mutex_);
  source_rate_ = rate < kMinimumParkedCameraRate ? kMinimumParkedCameraRate : rate > kMaximumCameraRate ? kMaximumCameraRate : rate;
}
void SceneCaptureManager::forget_capture_phase() noexcept {
  for (auto& phase : phase_feeds_)
    capture_phase::forget(phase.state);
  // decide() reads only draws > 1, which a filtered draw cannot change; the
  // filter is still disarmed on every phase reset.
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
}
void SceneCaptureManager::set_gpu_timing_enabled(bool enabled) noexcept {
  const std::lock_guard lock(mutex_);
  gpu_timing_enabled_ = enabled;
}
void SceneCaptureManager::set_capture_enabled(bool enabled) noexcept {
  const std::lock_guard lock(mutex_);
  capture_enabled_ = enabled;
  phase_feeds_ = {};
  draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  if (enabled)
    rearm_source_states_locked();
}
bool SceneCaptureManager::register_source_candidate(std::uint64_t key,
                                                    ID3D12Resource* resource,
                                                    std::uint64_t generation,
                                                    const D3D12_RESOURCE_DESC& desc,
                                                    source_state::Model initial) noexcept {
  if (!resource || !generation || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      !profiles::camera_candidate(static_cast<UINT>(desc.Width), desc.Height) || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
      (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) != 0 ||
      (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
       desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
    return false;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::lifecycle_us, contended_lifecycle_))
    return false;
  auto* owner = device(key);
  if (!owner || !owner->active || owner->failed)
    return false;
  collect();
  if (auto* existing = source_candidate(resource))
    return existing->device_key == key && existing->generation == generation;
  for (std::size_t index = 0; index < sources_.size(); ++index) {
    auto& candidate = sources_[index];
    if (candidate.native)
      continue;
    if (!owner->source_states.register_source({reinterpret_cast<std::uint64_t>(resource), generation}, initial))
      return false;
    candidate = {resource, key, generation, desc};
    source_device_keys_[index].store(key, std::memory_order_relaxed);
    source_handles_[index].store(reinterpret_cast<std::uint64_t>(resource), std::memory_order_relaxed);
    source_generations_[index].store(generation, std::memory_order_release);
    if (source_slots_used_.load(std::memory_order_relaxed) <= index)
      source_slots_used_.store(index + 1, std::memory_order_release);
    const auto bits = source_filter_bits(resource);
    source_filter_[bits.word].fetch_or(bits.mask, std::memory_order_release);
    draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
    ++stats_.source_candidates;
    return true;
  }
  return false;
}
void SceneCaptureManager::unregister_source_candidate(std::uint64_t key, ID3D12Resource* resource, std::uint64_t generation) noexcept {
  // Last source Release may synchronously enter the app's destruction callback
  // while this manager holds its mutex. Publish a generation-specific tombstone
  // without locking; the next collect removes metadata. No raw object is read.
  for (std::size_t index = 0; index < sources_.size(); ++index) {
    if (source_handles_[index].load(std::memory_order_acquire) != reinterpret_cast<std::uint64_t>(resource) ||
        source_device_keys_[index].load(std::memory_order_relaxed) != key)
      continue;
    auto expected = generation;
    if (source_generations_[index].compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
      draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  }
  // Filter bits remain set until collect() retires the slot and rebuilds the
  // filter under mutex_; retirement can never hide a live key.
}
void SceneCaptureManager::stage_source_draw(ID3D12GraphicsCommandList* native,
                                            std::uint64_t generation,
                                            UINT count,
                                            ID3D12Resource* const* targets,
                                            const std::uint64_t* generations) noexcept {
  source_stage = {};
  if (!native || !generation || !count || count > 8 || !targets || !generations)
    return;
  bool candidate_present = false;
  for (UINT n = 0; n < count; ++n)
    candidate_present |= may_be_source(targets[n]);
  if (!candidate_present)
    return;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_))
    return;
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  source_stage.owner = this;
  source_stage.list = native;
  source_stage.generation = generation;
  for (UINT n = 0; n < count; ++n)
    if (const auto* source = source_candidate(targets[n]);
        source && source->device_key == item->device_key && source->generation == generations[n])
      source_stage.keys[source_stage.count++] = {reinterpret_cast<std::uint64_t>(targets[n]), source->generation};
}
void SceneCaptureManager::after_source_draw(ID3D12GraphicsCommandList* native, std::uint64_t generation, bool allowed) noexcept {
  if (source_stage.owner != this || source_stage.list != native || source_stage.generation != generation)
    return;
  const auto stage = source_stage;
  source_stage = {};
  if (!stage.count)
    return;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_))
    return;
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  for (UINT n = 0; n < stage.count; ++n)
    apply_source_draw(*item, stage.keys[n], allowed);
}
void SceneCaptureManager::observe_source_draw_after(ID3D12GraphicsCommandList* native,
                                                    std::uint64_t generation,
                                                    UINT count,
                                                    ID3D12Resource* const* targets,
                                                    const std::uint64_t* generations,
                                                    bool allowed,
                                                    std::uint64_t recording) noexcept {
  // Runs after every observed draw. Only stage_source_draw sets owner, after
  // clearing the stage, so an ownerless stage is already empty.
  if (source_stage.owner)
    source_stage = {};
  if (!native || !generation || !count || count > 8 || !targets || !generations)
    return;
  unsigned candidates = 0;
  for (UINT n = 0; n < count; ++n)
    candidates |= may_be_source(targets[n]) ? 1u << n : 0u;
  if (!candidates)
    return;
  auto& repeat = draw_repeat;
  if (recording && allowed && repeat.owner == this && repeat.list == native && repeat.generation == generation &&
      repeat.recording == recording && repeat.epoch == draw_repeat_epoch_.load(std::memory_order_acquire) &&
      repeat.list_epoch == list_epochs_[repeat.slot].load(std::memory_order_acquire)) {
    const auto armed = [&](UINT n) noexcept {
      UINT index = 0;
      while (index < repeat.count && repeat.keys[index] != source_state::Key{reinterpret_cast<std::uint64_t>(targets[n]), generations[n]})
        ++index;
      return index;
    };
    bool repeated = true;
    for (UINT n = 0; n < count && repeated; ++n)
      repeated = !(candidates & (1u << n)) || armed(n) < repeat.count;
    if (repeated) {
      for (UINT n = 0; n < count; ++n)
        if (candidates & (1u << n)) {
          auto& pending = repeat.pending[armed(n)];
          pending += pending != UINT32_MAX;
        }
      contended_call = false;
      return;
    }
  }
  // Sampled before evidence_lock drains the deferred ring (see DrawRepeat).
  const auto epoch = draw_repeat_epoch_.load(std::memory_order_acquire);
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_))
    return;
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  for (UINT n = 0; n < count; ++n)
    apply_source_draw(*item, {reinterpret_cast<std::uint64_t>(targets[n]), generations[n]}, allowed);
  if (recording && allowed)
    arm_draw_repeat(*item, recording, epoch, count, targets, generations, candidates);
}
void SceneCaptureManager::arm_draw_repeat(List& item,
                                          std::uint64_t recording,
                                          std::uint64_t epoch,
                                          UINT count,
                                          ID3D12Resource* const* targets,
                                          const std::uint64_t* generations,
                                          unsigned candidates) noexcept {
  // evidence_lock settled this thread's filter, so nothing is armed here.
  const auto* owner = device(item.device_key);
  const auto& recorded = item.source_effects;
  if (!owner || item.session_generation != owner->session_generation || recorded.invalid || recorded.count > recorded.effects.size())
    return;
  DrawRepeat armed;
  for (UINT n = 0; n < count; ++n) {
    if (!(candidates & (1u << n)))
      continue;
    // The checks apply_source_draw made, so a skipped draw would have appended.
    const auto* source = source_candidate(targets[n]);
    if (!source || source->generation != generations[n] || source->device_key != item.device_key)
      return;
    const source_state::Key key{reinterpret_cast<std::uint64_t>(targets[n]), generations[n]};
    // The effect a further draw collapses into: this key's last (Recording::append).
    std::size_t index = recorded.count;
    while (index && recorded.effects[index - 1].key != key)
      --index;
    if (!index || recorded.effects[index - 1].kind != source_state::Effect::Kind::draw || recorded.effects[index - 1].draws < 2)
      return;
    // A further draw must not need a lease it does not already hold.
    if (source_tracking_ && owner->session_active) {
      std::size_t lease = 0;
      while (lease < item.source_lease_count && item.source_leases[lease].key != key)
        ++lease;
      if (lease == item.source_lease_count || item.source_leases[lease].native != source->native)
        return;
    }
    armed.keys[armed.count] = key;
    armed.effects[armed.count++] = static_cast<std::uint16_t>(index - 1);
  }
  armed.owner = this;
  armed.list = item.native;
  armed.generation = item.object_generation;
  armed.recording = recording;
  armed.list_recording = item.recording;
  armed.epoch = epoch;
  armed.slot = static_cast<std::size_t>(&item - lists_.data());
  // This draw's list() left it even; odd makes the next lookup bump it.
  auto& list_epoch = list_epochs_[armed.slot];
  armed.list_epoch = list_epoch.load(std::memory_order_relaxed) | 1;
  list_epoch.store(armed.list_epoch, std::memory_order_relaxed);
  draw_repeat = armed;
}
void SceneCaptureManager::settle_draw_repeat() noexcept {
  if (draw_repeat.owner != this)
    return;
  const auto repeat = draw_repeat;
  draw_repeat = {};
  for (UINT n = 0; n < repeat.count; ++n)
    stats_.source_draws += repeat.pending[n];
  // Only into the draw effect each skipped draw collapsed into. A List keeps
  // its entries in place until retire_list resets it and renews recording.
  auto* item = list(repeat.list);
  if (!item || item->object_generation != repeat.generation || item->recording != repeat.list_recording)
    return;
  auto& recorded = item->source_effects;
  for (UINT n = 0; n < repeat.count; ++n) {
    if (!repeat.pending[n] || repeat.effects[n] >= recorded.count || recorded.count > recorded.effects.size())
      continue;
    auto& effect = recorded.effects[repeat.effects[n]];
    if (effect.key == repeat.keys[n] && effect.kind == source_state::Effect::Kind::draw)
      effect.draws = repeat.pending[n] > UINT32_MAX - effect.draws ? UINT32_MAX : effect.draws + repeat.pending[n];
  }
}
void SceneCaptureManager::apply_source_draw(List& item, source_state::Key key, bool allowed) noexcept {
  const auto* owner = device(item.device_key);
  if (!owner || item.session_generation != owner->session_generation)
    return;
  const auto* source = source_candidate(reinterpret_cast<ID3D12Resource*>(key.handle));
  if (!source || source->generation != key.generation || source->device_key != item.device_key)
    return;
  touch_sources(item);
  if (!allowed) {
    ++stats_.invalid_draws;
    item.source_effects.invalidate();
  } else {
    item.source_effects.append({key, source_state::Effect::Kind::draw});
    // The exact application Draw has completed but has not returned to its
    // caller: its actual bound RTV source is still a live resource argument.
    // Keep one reference per recording, never a registry-lifetime reference.
    if (source_tracking_ && owner->session_active &&
        !retain_source_lease(item.source_leases, item.source_lease_count, key, source->native)) {
      ++stats_.source_lease_failures;
      item.source_effects.invalidate();
    }
    ++stats_.source_draws;
  }
}
void SceneCaptureManager::invalidate_source_recording(ID3D12GraphicsCommandList* native,
                                                      std::uint64_t generation,
                                                      bool global,
                                                      std::uint32_t reasons) noexcept {
  if (source_stage.owner == this && source_stage.list == native)
    source_stage = {};
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_)) {
    DeferredWork work;
    work.kind = DeferredWork::Kind::recording_report;
    work.native = native;
    work.generation = generation;
    work.global = global;
    work.reasons = reasons;
    defer(work);
    return;
  }
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  apply_recording_report(*item, global, reasons);
}
void SceneCaptureManager::apply_recording_report(List& item, bool global, std::uint32_t reasons) noexcept {
  // PassState, unsupported native commands and a PassBegin whose targets the
  // observer could not name (the global path taken when a list has no known
  // render targets, continuous under ReShade's D3D12 layer) are reported on
  // lists that never named a published camera source. Applying that invalid
  // recording wipes every tracked source. Ignore those reports there. A list
  // that did name a published source loses only that source's RT evidence;
  // unknown state is never stored as a render target. Barrier, alias, reset
  // and observer-disabled reports still discard the recording.
  using namespace engine_hook::render_boundary;
  constexpr std::uint32_t limited_reasons =
      static_cast<std::uint32_t>(InvalidationPassBegin) | static_cast<std::uint32_t>(InvalidationPassState) |
      static_cast<std::uint32_t>(InvalidationSplitBarrier) | static_cast<std::uint32_t>(InvalidationAliasOrDiscard) |
      static_cast<std::uint32_t>(InvalidationUnobservedWork);
  const bool limited = (reasons & (static_cast<std::uint32_t>(InvalidationPassBegin) | static_cast<std::uint32_t>(InvalidationPassState) |
                                   InvalidationUnobservedWork)) != 0 &&
                       (reasons & ~limited_reasons) == 0;
  if (limited) {
    const auto& recording = item.source_effects;
    // A truncated count is not evidence that the published sources were absent.
    if (recording.count <= recording.effects.size()) {
      std::array<source_state::Key, source_state::Recording::capacity> published{};
      std::size_t published_count = 0;
      for (std::size_t index = 0; index < recording.count; ++index) {
        const auto key = recording.effects[index].key;
        if (handoff_.observed_feed(key.handle) < 0)
          continue;
        bool seen = false;
        for (std::size_t found = 0; found < published_count; ++found)
          seen |= published[found] == key;
        if (!seen)
          published[published_count++] = key;
      }
      if (!published_count) {
        ++stats_.ignored_source_recordings;
        stats_.last_invalidation_reasons = reasons;
        return;
      }
      if (!recording.invalid) {
        for (std::size_t index = 0; index < published_count; ++index)
          item.source_effects.append({published[index], source_state::Effect::Kind::pass_other});
        touch_sources(item);
        ++stats_.retired_source_recordings;
      }
      stats_.last_invalidation_reasons = reasons;
      return;
    }
  }
  stats_.last_invalidation_reasons = reasons;
  forget_clean(item);  // A local report does not publish.
  item.source_effects.invalidate();
  if (global)
    touch_sources(item);
}
void SceneCaptureManager::invalidate_source_targets(ID3D12GraphicsCommandList* native,
                                                    std::uint64_t generation,
                                                    UINT count,
                                                    ID3D12Resource* const* targets,
                                                    const std::uint64_t* generations) noexcept {
  if (source_stage.owner == this && source_stage.list == native && source_stage.generation == generation)
    source_stage = {};
  if (!count)
    return;
  const bool truncated = count > 8 || !targets || !generations;
  if (!truncated) {
    // Only a registered camera source can take a scoped effect, and the filter
    // never clears a registered key's bit. With no possible source the report
    // would append nothing, so ordinary passes need not take the lock at all.
    bool candidate_present = false;
    for (UINT index = 0; index < count; ++index)
      candidate_present |= may_be_source(targets[index]);
    if (!candidate_present)
      return;
  }
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_)) {
    DeferredWork work;
    work.kind = DeferredWork::Kind::target_report;
    work.native = native;
    work.generation = generation;
    work.target_count = truncated ? DeferredWork::TruncatedTargets : count;
    if (!truncated)
      for (UINT index = 0; index < count; ++index) {
        work.targets[index] = targets[index];
        work.target_generations[index] = generations[index];
      }
    defer(work);
    return;
  }
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  apply_target_report(*item, truncated ? DeferredWork::TruncatedTargets : count, targets, generations);
}
void SceneCaptureManager::apply_target_report(List& item,
                                              UINT count,
                                              ID3D12Resource* const* targets,
                                              const std::uint64_t* generations) noexcept {
  if (count > 8 || !targets || !generations) {
    touch_sources(item);
    item.source_effects.invalidate();
    return;
  }
  for (UINT index = 0; index < count; ++index) {
    const auto* candidate = source_candidate(targets[index]);
    if (!candidate || candidate->device_key != item.device_key || candidate->generation != generations[index])
      continue;
    touch_sources(item);
    item.source_effects.append(
        {{reinterpret_cast<std::uint64_t>(candidate->native), candidate->generation}, source_state::Effect::Kind::other});
    ++stats_.scoped_source_invalidations;
  }
}
void SceneCaptureManager::observe_source_legacy(ID3D12GraphicsCommandList* native,
                                                std::uint64_t generation,
                                                const D3D12_RESOURCE_BARRIER& barrier) noexcept {
  if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_ALIASING &&
      (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !may_be_source(barrier.Transition.pResource)))
    return;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_)) {
    DeferredWork work;
    work.kind = DeferredWork::Kind::legacy_barrier;
    work.native = native;
    work.generation = generation;
    work.legacy = barrier;
    defer(work);
    return;
  }
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  apply_legacy_barrier(*item, barrier);
}
void SceneCaptureManager::apply_legacy_barrier(List& item, const D3D12_RESOURCE_BARRIER& barrier) noexcept {
  if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
    if (!barrier.Aliasing.pResourceBefore || !barrier.Aliasing.pResourceAfter) {
      ++stats_.global_aliases;
      touch_sources(item);
      item.source_effects.invalidate();
    } else {
      for (auto* aliased : {barrier.Aliasing.pResourceBefore, barrier.Aliasing.pResourceAfter})
        if (const auto* candidate = source_candidate(aliased); candidate && candidate->device_key == item.device_key) {
          touch_sources(item);
          item.source_effects.append(
              {{reinterpret_cast<std::uint64_t>(aliased), candidate->generation}, source_state::Effect::Kind::other});
        }
    }
    return;
  }
  const auto* source = source_candidate(barrier.Transition.pResource);
  if (!source || source->device_key != item.device_key)
    return;
  touch_sources(item);
  if (barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
      (barrier.Transition.Subresource != 0 && barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
    item.source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, source_state::Effect::Kind::other});
    return;
  }
  const auto kind = barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET ? source_state::Effect::Kind::legacy_rt
                                                                                        : source_state::Effect::Kind::other;
  item.source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, kind});
}
void SceneCaptureManager::observe_source_enhanced(ID3D12GraphicsCommandList* native,
                                                  std::uint64_t generation,
                                                  const D3D12_TEXTURE_BARRIER& barrier) noexcept {
  if (!may_be_source(barrier.pResource))
    return;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_)) {
    DeferredWork work;
    work.kind = DeferredWork::Kind::enhanced_barrier;
    work.native = native;
    work.generation = generation;
    work.enhanced = barrier;
    defer(work);
    return;
  }
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  apply_enhanced_barrier(*item, barrier);
}
void SceneCaptureManager::apply_enhanced_barrier(List& item, const D3D12_TEXTURE_BARRIER& barrier) noexcept {
  const auto* source = source_candidate(barrier.pResource);
  if (!source || source->device_key != item.device_key)
    return;
  touch_sources(item);
  const auto& range = barrier.Subresources;
  const bool whole = range.NumMipLevels == 0 ? range.IndexOrFirstMipLevel == 0 || range.IndexOrFirstMipLevel == UINT_MAX
                                             : range.IndexOrFirstMipLevel == 0 && range.NumMipLevels == 1 && range.FirstArraySlice == 0 &&
                                                   range.NumArraySlices == 1 && range.FirstPlane == 0 && range.NumPlanes == 1;
  if (!whole || barrier.Flags != D3D12_TEXTURE_BARRIER_FLAG_NONE || (barrier.SyncBefore & D3D12_BARRIER_SYNC_SPLIT) ||
      (barrier.SyncAfter & D3D12_BARRIER_SYNC_SPLIT)) {
    item.source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, source_state::Effect::Kind::other});
    return;
  }
  const auto kind = barrier.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET && barrier.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET
                        ? source_state::Effect::Kind::enhanced_rt
                        : source_state::Effect::Kind::other;
  item.source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, kind});
}
std::uint32_t SceneCaptureManager::successful_reset(ID3D12GraphicsCommandList* native,
                                                    std::uint64_t generation,
                                                    std::uint32_t slot) noexcept {
  // Lock-free for a list its last retirement left clean (PublishedList::clean):
  // retiring it again would change only its version, recording counter and
  // statistics, which nothing reads for a list that owns no packet, consumer,
  // lease or source effect. Taken only when the locked path would apply
  // nothing else either: no ring entry, overflow or deferred device work, and
  // no draw-repeat filter of this thread to settle. Ring first, mark second,
  // both acquire. Every change forgets the mark under mutex_ first:
  // - Evidence on this list comes from its recording thread and happens before
  //   this Reset (Close, Execute, Reset). A replayed entry is forgotten before
  //   its commit, which an idle ring has seen.
  // - A lost entry sets the overflow flag before push returns; the drain
  //   forgets every list before clearing it.
  // - reset_session forgets first, so a Reset that still sees the mark is
  //   ordered before it. Destroy and slot reuse publish, which forgets, and a
  //   generation names one list object.
  // A change racing these loads is ordered after this Reset, as it would be
  // after a locked Reset that won mutex_ first.
  auto& thread = hook_timing::own_slot();
  auto& counts = reset_counts_[static_cast<std::size_t>(&thread - hook_timing::slots.data())];
  if (slot < MaximumLists && generation && deferred_work_.idle() && !deferred_recordings_.load(std::memory_order_acquire) &&
      !deferred_uncertain_.load(std::memory_order_acquire) && !deferred_sources_.load(std::memory_order_acquire) &&
      draw_repeat.owner != this) {
    const auto& published = published_lists_[slot];
    if (published.clean.load(std::memory_order_acquire) == generation && published.native.load(std::memory_order_acquire) == native) {
      contended_call = false;
      hook_timing::add(thread, counts.fast, 1);
      return slot;
    }
  }
  hook_timing::add(thread, counts.locked, 1);
  // Every recording thread resets lists many times per frame, so this uses the
  // per-command budget rather than the creation/destruction one. A missed lock
  // defers the retirement, which the next holder applies before any evidence.
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_lifecycle_)) {
    hook_timing::add(thread, counts.expired, 1);
    // Retired by the next lock holder before any later evidence on this list
    // or any transaction; nothing global is invalidated for it.
    DeferredWork work;
    work.kind = DeferredWork::Kind::reset;
    work.native = native;
    work.generation = generation;
    defer(work);
    return slot;
  }
  // Under mutex_, a slot holding this native pointer is its only slot:
  // register_list and destroy change lists_ and list_indices_ together. The
  // hint skips list()'s list_epochs_ bump, which a Reset does not need: the
  // adapter advanced its recording first, which disarms a draw-repeat filter.
  auto* item = native && slot < MaximumLists && lists_[slot].native == native ? &lists_[slot] : list(native);
  if (item && item->object_generation == generation) {
    // Only a retired packet gives this Reset anything to collect. Every packet
    // assignment, submission and worker poll still collects first.
    const bool had_packets = item->packets != 0;
    retire_native_list(*item, false);
    if (had_packets)
      collect();
    return static_cast<std::uint32_t>(item - lists_.data());
  }
  return NoListSlot;
}
void SceneCaptureManager::destroy_command_list(ID3D12GraphicsCommandList* native, std::uint64_t generation) noexcept {
  // Runs from the D3D12 private-data release, on whichever thread drops the
  // last reference and possibly under runtime-internal locks. Never park it.
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::lifecycle_us, contended_lifecycle_)) {
    DeferredWork work;
    work.kind = DeferredWork::Kind::destroy;
    work.native = native;
    work.generation = generation;
    defer(work);
    return;
  }
  auto* item = list(native);
  if (item && item->object_generation == generation) {
    retire_native_list(*item, true);
    collect();
  }
}

bool SceneCaptureManager::record_copy_after_forward(ID3D12GraphicsCommandList* native,
                                                    ID3D12Resource* source,
                                                    ID3D12Resource* destination,
                                                    bool whole_copy) noexcept {
  return record_copy(native, source, destination, whole_copy, 0, false);
}

bool SceneCaptureManager::record_copy_after_forward(ID3D12GraphicsCommandList* native,
                                                    ID3D12Resource* source,
                                                    ID3D12Resource* destination,
                                                    bool whole_copy,
                                                    std::uint64_t generation) noexcept {
  return generation != 0 && record_copy(native, source, destination, whole_copy, generation, true);
}

bool SceneCaptureManager::record_texture_copy_after_forward(ID3D12GraphicsCommandList* native,
                                                            const D3D12_TEXTURE_COPY_LOCATION* destination,
                                                            UINT x,
                                                            UINT y,
                                                            UINT z,
                                                            const D3D12_TEXTURE_COPY_LOCATION* source,
                                                            const D3D12_BOX* box,
                                                            bool allowed,
                                                            std::uint64_t generation) noexcept {
  if (!allowed || !generation || !source || !destination || !source->pResource || !destination->pResource || x || y || z ||
      source->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX || destination->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
      source->SubresourceIndex != 0 || destination->SubresourceIndex != 0 ||
      (!handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(source->pResource)) &&
       !handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(destination->pResource))))
    return false;
  if (box) {
    const auto desc = source->pResource->GetDesc();
    if (desc.Width > UINT_MAX || box->left || box->top || box->front || box->right != desc.Width || box->bottom != desc.Height ||
        box->back != 1)
      return false;
  }
  return record_copy(native, source->pResource, destination->pResource, true, generation, true);
}

bool SceneCaptureManager::record_copy(ID3D12GraphicsCommandList* native,
                                      ID3D12Resource* source,
                                      ID3D12Resource* destination,
                                      bool whole_copy,
                                      std::uint64_t generation,
                                      bool native_observation) noexcept {
  if (!native || !source || !destination || !whole_copy || source == destination ||
      (!handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(source)) &&
       !handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(destination))))
    return false;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_))
    return false;
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || (native_observation && item->object_generation != generation))
    return false;
  const auto matches =
      handoff_.observe_copy(item->device_key, reinterpret_cast<std::uint64_t>(source), reinterpret_cast<std::uint64_t>(destination));
  // A copy between distinct published feeds is ambiguous and must never label one as both.
  if (matches.source.matched && matches.destination.matched && matches.source.feed != matches.destination.feed)
    return false;
  const auto match = matches.source.matched ? matches.source : matches.destination;
  if (!match.matched || match.feed > 2)
    return false;
  const auto desc = source->GetDesc();
  auto& diagnostic = stats_.copies[match.feed];
  ++diagnostic.matched_copies;
  diagnostic.width = desc.Width;
  diagnostic.height = desc.Height;
  diagnostic.format = static_cast<std::uint32_t>(desc.Format);
  diagnostic.mips = desc.MipLevels;
  diagnostic.last_refusal = "incompatible_whole_copy_descriptions";
  if (!same_description(desc, destination->GetDesc()))
    return false;
  if (!native_observation && (item->feeds & (1u << match.feed)) != 0) {
    diagnostic.last_refusal = "duplicate_public_copy";
    return false;
  }
  return capture_source(*item, *owner, match, source, false, nullptr, native_observation, &diagnostic.last_refusal);
}

bool SceneCaptureManager::record_render_target_before_transition(ID3D12GraphicsCommandList* native,
                                                                 ID3D12Resource* target,
                                                                 bool proven_legacy_render_target,
                                                                 std::uint64_t object_generation) noexcept {
  if (!native || !target || !proven_legacy_render_target || !handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(target)))
    return false;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_))
    return false;
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || item->object_generation != object_generation)
    return false;
  const auto match = handoff_.observe_copy(item->device_key, reinterpret_cast<std::uint64_t>(target), 0).source;
  if (!match.matched || match.feed > 2)
    return false;
  return capture_source(*item, *owner, match, target, true);
}

bool SceneCaptureManager::record_render_target_before_enhanced_transition(ID3D12GraphicsCommandList7* native,
                                                                          ID3D12Resource* target,
                                                                          bool proven_enhanced_render_target,
                                                                          std::uint64_t object_generation) noexcept {
  if (!native || !target || !proven_enhanced_render_target || !handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(target)))
    return false;
  std::unique_lock<ManagerMutex> lock;
  if (!evidence_lock(lock, wait_budget::recording_us, contended_evidence_))
    return false;
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || item->object_generation != object_generation)
    return false;
  const auto match = handoff_.observe_copy(item->device_key, reinterpret_cast<std::uint64_t>(target), 0).source;
  if (!match.matched || match.feed > 2)
    return false;
  return capture_source(*item, *owner, match, target, true, native);
}

bool SceneCaptureManager::capture_source(List& item,
                                         Device& owner,
                                         const SceneCopyMatch& match,
                                         ID3D12Resource* source,
                                         bool render_target,
                                         ID3D12GraphicsCommandList7* enhanced_list,
                                         bool allow_copy_rewrite,
                                         const char** copy_refusal,
                                         Packet* required_packet) noexcept {
  auto* diagnostic = render_target ? &stats_.render_targets[match.feed] : nullptr;
  const auto refusal = [&](const char* reason) {
    if (diagnostic)
      diagnostic->last_refusal = reason;
    if (copy_refusal)
      *copy_refusal = reason;
  };
  if (!capture_enabled_ || !owner.session_active) {
    refusal("capture_disabled");
    return false;
  }
  if (item.session_generation != owner.session_generation) {
    refusal("previous_session_recording");
    return false;
  }
  refusal("packet_pool_or_memory_budget");
  const auto source_desc = source->GetDesc();
  if (diagnostic) {
    ++diagnostic->matched_boundaries;
    diagnostic->width = source_desc.Width;
    diagnostic->height = source_desc.Height;
    diagnostic->format = static_cast<std::uint32_t>(source_desc.Format);
    diagnostic->mips = source_desc.MipLevels;
  }
  if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || source_desc.MipLevels != 1 || source_desc.DepthOrArraySize != 1 ||
      source_desc.SampleDesc.Count != 1 || source_desc.SampleDesc.Quality != 0) {
    refusal("unsupported_texture_shape_mips_or_samples");
    return false;
  }
  const auto record = [&](Packet& packet) {
    if (enhanced_list)
      return packet.gpu.record_render_target_source_enhanced(enhanced_list, source, true);
    return render_target ? packet.gpu.record_render_target_source(item.native, source, true)
                         : packet.gpu.record_copy_source(item.native, source, allow_copy_rewrite);
  };
  if (item.feeds & (1u << match.feed)) {
    if (!render_target && !allow_copy_rewrite)
      return false;
    for (std::size_t index = 0; index < packets_.size(); ++index) {
      auto& packet = packets_[index];
      if (!(item.packets & (1u << index)) || packet.match.feed != match.feed)
        continue;
      if (!packet.assigned || packet.retired || packet.quarantined || packet.leased || packet.submitted || packet.in_flight ||
          packet.match.scene_epoch != match.scene_epoch || packet.match.manager != match.manager ||
          packet.match.entry_id != match.entry_id || packet.match.resource != match.resource) {
        refusal("recording_busy_or_identity_changed");
        return false;
      }
      if (!record(packet)) {
        refusal(packet.gpu.error());
        return false;
      }
      packet.match = match;
      if (render_target) {
        ++stats_.render_target_writes;
        ++stats_.render_target_rewrites;
      } else {
        ++stats_.copy_writes;
        ++stats_.copy_rewrites;
      }
      refusal("none");
      return true;
    }
    refusal("recording_packet_missing");
    return false;
  }
  const auto& desc = source_desc;
  if (render_target &&
      ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0)) {
    refusal("unsupported_render_target_flags");
    return false;
  }
  collect();
  if (!owner.active || owner.failed)
    return false;
  for (const auto index : reuse_order(owner.key, desc)) {
    auto& packet = packets_[index];
    if (required_packet && &packet != required_packet)
      continue;
    if (packet.assigned || packet.quarantined)
      continue;
    if (next_token_ >= MaximumCounter)
      break;
    if (packet.gpu.state() == SceneCaptureD3D12::State::idle &&
        (packet.device_key != owner.key || !same_description(packet.description, desc))) {
      const auto bytes = packet.gpu.allocation_bytes();
      if (!packet.gpu.release_idle())
        continue;
      stats_.bytes -= bytes;
    }
    if (packet.gpu.state() == SceneCaptureD3D12::State::empty) {
      // Validate/count the actual committed allocation before admitting a packet.
      auto allocation_desc = desc;
      allocation_desc.Alignment = 0;
      allocation_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      const auto allocation = owner.native->GetResourceAllocationInfo(0, 1, &allocation_desc);
      if (!allocation.SizeInBytes || allocation.SizeInBytes > MaximumBytes - stats_.bytes)
        continue;
      if (FAILED(packet.gpu.initialize(owner.native, desc))) {
        refusal(packet.gpu.error());
        continue;
      }
      stats_.bytes += packet.gpu.allocation_bytes();
      ++stats_.allocations;
      packet.description = desc;
      packet.device_key = owner.key;
    }
    if (!record(packet)) {
      refusal(packet.gpu.error());
      continue;
    }
    packet.match = match;
    packet.token = ++next_token_;
    packet.session_generation = owner.session_generation;
    packet.submitted = 0;
    packet.order = {};
    packet.producer = nullptr;
    packet.in_flight = 0;
    packet.assigned = true;
    packet.retired = packet.leased = false;
    item.packets |= static_cast<std::uint16_t>(1u << index);
    item.feeds |= 1u << match.feed;
    publish_list(item);
    ++stats_.captures;
    if (render_target)
      ++stats_.render_target_writes;
    else
      ++stats_.copy_writes;
    refusal("none");
    return true;
  }
  ++stats_.skipped;
  return false;
}

std::array<std::uint8_t, SceneCaptureManager::MaximumPackets> SceneCaptureManager::reuse_order(
    std::uint64_t device_key,
    const D3D12_RESOURCE_DESC& desc) const noexcept {
  std::array<std::uint8_t, MaximumPackets> order{};
  std::size_t used = 0;
  for (unsigned rank = 0; rank < 3; ++rank)
    for (std::size_t index = 0; index < packets_.size(); ++index) {
      const auto& packet = packets_[index];
      const auto state = packet.gpu.state();
      const bool fits =
          state == SceneCaptureD3D12::State::idle && packet.device_key == device_key && same_description(packet.description, desc);
      if ((fits ? 0u : state == SceneCaptureD3D12::State::empty ? 1u : 2u) == rank)
        order[used++] = static_cast<std::uint8_t>(index);
    }
  return order;
}

bool SceneCaptureManager::prepare_tail(Packet& packet, Device& owner) noexcept {
  if (packet.assigned || packet.quarantined)
    return false;
  const auto clear_tail = [&packet]() noexcept {
    if (packet.tail_list7)
      packet.tail_list7->Release();
    if (packet.tail_list)
      packet.tail_list->Release();
    if (packet.tail_allocator)
      packet.tail_allocator->Release();
    packet.tail_list7 = nullptr;
    packet.tail_list = nullptr;
    packet.tail_allocator = nullptr;
    packet.tail_device_key = 0;
  };
  // The snapshot can be reused on another device independently of these
  // private command objects, so it cannot supply their ownership identity.
  if (packet.tail_device_key != owner.key)
    clear_tail();
  if (!packet.tail_allocator &&
      FAILED(owner.native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&packet.tail_allocator))))
    return false;
  packet.tail_device_key = owner.key;
  if (!packet.tail_list) {
    if (FAILED(owner.native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, packet.tail_allocator, nullptr,
                                               IID_PPV_ARGS(&packet.tail_list)))) {
      clear_tail();
      return false;
    }
    packet.tail_list->QueryInterface(IID_PPV_ARGS(&packet.tail_list7));
    if (FAILED(packet.tail_list->Close())) {
      clear_tail();
      return false;
    }
  }
  // An unassigned packet has completed its prior producer and all consumers,
  // or its closed private recording was discarded without submission.
  if (FAILED(packet.tail_allocator->Reset()) || FAILED(packet.tail_list->Reset(packet.tail_allocator, nullptr)))
    return false;
  packet.tail_timing.discard_unsubmitted();
  return true;
}
// A feed at or past TailFeeds is never captured at the tail: record_queue_tail
// skips it as waiting_for_publication.
static_assert(SceneCaptureManager::TailFeeds >= native_camera::kMaxCameraFeeds);
void SceneCaptureManager::record_queue_tail(Transaction& pending, TailBatch& tails) noexcept {
  auto& owner = *pending.device;
  if (!owner.active || owner.failed || !owner.session_active || pending.session_generation != owner.session_generation)
    return;
  if (!engine_hook::render_boundary::operational()) {
    wipe(owner, WipeSite::observer_disabled);
    stats_.tail_status = "observer_disabled";
    return;
  }
  collect();
  unsigned feeds = 0, drawn_feeds = 0;
  stats_.tail_status = "no_candidate_draw";
  const auto now = steady_now_us();
  for (std::size_t source_index = 0; source_index < sources_.size(); ++source_index) {
    const auto& source = sources_[source_index];
    if (!source.native || source.device_key != owner.key ||
        source_generations_[source_index].load(std::memory_order_acquire) != source.generation)
      continue;
    const source_state::Key key{reinterpret_cast<std::uint64_t>(source.native), source.generation};
    const auto state = owner.source_states.state(key);
    const auto observed_feed = handoff_.observed_feed(reinterpret_cast<std::uint64_t>(source.native));
    if (state.model != source_state::Model::legacy_rt && state.model != source_state::Model::enhanced_rt)
      for (auto& phase : phase_feeds_)
        if (phase.source == key)
          capture_phase::forget(phase.state);  // It left the RT state: the next drawing batch is not this render's rest.
    if (state.model == source_state::Model::unknown) {
      if (observed_feed >= 0)
        stats_.tail_status = "unknown_source_state";
      continue;
    }
    if (!state.drawn || (state.model != source_state::Model::legacy_rt && state.model != source_state::Model::enhanced_rt))
      continue;
    const auto match = handoff_.observe_copy(owner.key, reinterpret_cast<std::uint64_t>(source.native), 0).source;
    if (!match.matched || match.feed >= TailFeeds) {
      if (observed_feed >= 0)
        stats_.tail_status = "waiting_for_publication";
      continue;
    }
    if (feeds & (1u << match.feed))
      continue;
    auto& phase = phase_feeds_[match.feed];
    if (phase.source != key)
      phase = {key, {}};
    auto& diagnostic = stats_.phases[match.feed];
    if (!(drawn_feeds & (1u << match.feed))) {
      drawn_feeds |= 1u << match.feed;
      ++diagnostic.batches;
      diagnostic.one_draw += state.draws == 1;
      diagnostic.max_draws = std::max(diagnostic.max_draws, state.draws);
    }
    ID3D12Resource* leased_source = nullptr;
    for (std::size_t index = 0; index < pending.source_lease_count; ++index) {
      const auto& lease = pending.source_leases[index];
      if (lease.key == key) {
        leased_source = lease.native;
        break;
      }
    }
    // A hold covers only the drawing batch directly after it: when that batch
    // records no capture, for any reason, the hold is forgotten.
    if (!leased_source) {
      capture_phase::forget(phase.state);
      stats_.tail_status = "source_lease_unavailable";
      continue;
    }
    // Only refuses a second capture within one interval; the schedule sets the cadence.
    const auto previous = last_tail_us_[match.feed];
    if (previous && (now < previous || now - previous < capture_spacing_us(source_rate_))) {
      capture_phase::forget(phase.state);
      stats_.tail_status = "sample_interval";
      continue;
    }
    // Before any private recording: a held batch leaves no packet, lease or list work.
    const auto phase_decision = capture_phase::decide(phase.state, state.draws);
    if (!phase_decision.capture) {
      ++diagnostic.held;
      stats_.tail_status = "awaiting_complete_render";
      continue;
    }
    // Never close a list the batch cannot carry: it would be accounted and
    // published but never executed. One slot per feed and the feeds mask above
    // keep this from firing.
    if (tails.count == tails.lists.size()) {
      capture_phase::forget(phase.state);
      stats_.tail_status = "tail_batch_full";
      continue;
    }
    stats_.tail_status = "tail_packet_unavailable";
    for (const auto index : reuse_order(owner.key, leased_source->GetDesc())) {
      auto& packet = packets_[index];
      if (!prepare_tail(packet, owner))
        continue;
      List private_recording;
      private_recording.native = packet.tail_list;
      private_recording.device_key = owner.key;
      private_recording.session_generation = owner.session_generation;
      const bool timed = gpu_timing_enabled_ && packet.tail_timing.begin(owner.native, pending.queue, packet.tail_list);
      if (timed)
        packet.tail_timing.start(0);
      const bool enhanced = state.model == source_state::Model::enhanced_rt;
      const bool recorded =
          (!enhanced || packet.tail_list7) && capture_source(private_recording, owner, match, leased_source, true,
                                                             enhanced ? packet.tail_list7 : nullptr, false, nullptr, &packet);
      if (timed) {
        packet.tail_timing.end(0);
        packet.tail_timing.resolve();
      }
      const auto closed = packet.tail_list->Close();
      if (!recorded) {
        // No Execute can see this private list. prepare_tail resets it before
        // reuse; retain query objects until that successful Reset.
        continue;
      }
      packet.retired = true;  // Private list is submitted once and never replayed.
      if (FAILED(closed)) {
        packet.tail_timing.abandon();
        quarantine(packet);
        stats_.tail_status = "tail_close_failed";
        break;
      }
      ++packet.in_flight;
      pending.packets |= private_recording.packets;
      pending.packet_positions[&packet - packets_.data()] = ++pending.last_position;
      // The caller Executes this closed private list on the OUTER receipt's
      // queue after releasing mutex_; in_flight keeps the packet out of every
      // other path until finish_transaction accounts for it. The native queue
      // wrapper bypasses nested observation from its after phase.
      tails.lists[tails.count++] = packet.tail_list;
      if (timed)
        pending.timed_tail_packets |= private_recording.packets;
      ++stats_.tail_submissions;
      ++stats_.tail_captures;
      last_tail_us_[match.feed] = now;
      capture_phase::captured(phase.state);
      ++diagnostic.captures[static_cast<std::size_t>(phase_decision.kind)];
      stats_.tail_status = "captured";
      feeds |= 1u << match.feed;
      break;
    }
    if (!(feeds & (1u << match.feed)))
      capture_phase::forget(phase.state);  // Due but not recorded.
  }
  if (feeds)
    stats_.tail_status = "captured";
}

bool SceneCaptureManager::register_consumer_recording(ID3D12GraphicsCommandList* native) noexcept {
  // Called under the runtime lock from Close and barrier callbacks. A refused
  // registration only skips this recording's PFD write; publish no uncertainty.
  BoundedLock bounded(mutex_, wait_budget::close_us, &contended_evidence_, hook_timing::manager_lock);
  contended_call = !bounded;
  if (!bounded)
    return false;
  apply_deferred_work();  // A skipped Reset of this list must not keep the flag on the old recording.
  settle_draw_repeat();
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || !owner->session_active || item->session_generation != owner->session_generation)
    return false;
  item->consumer = true;
  publish_list(*item);
  return true;
}

bool SceneCaptureManager::compatible_queue(ID3D12CommandQueue* queue, const Device& owner) noexcept {
  if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  return same_native_device(queue, owner.native);
}
std::uint32_t SceneCaptureManager::compatible_devices(ID3D12CommandQueue* queue) const noexcept {
  // A queue's type and device never change, and a published slot keeps the
  // native device it was registered with. A device registered after this
  // runs reads as a clear bit, which begin_transaction rechecks exactly.
  if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return 0;
  std::uint32_t compatible = 0;
  for (std::size_t index = 0; index < published_devices_.size(); ++index)
    if (auto* native = published_devices_[index].load(std::memory_order_acquire); native && same_native_device(queue, native))
      compatible |= 1u << index;
  return compatible;
}

SceneCaptureManager::Submission SceneCaptureManager::begin_transaction(Device& owner,
                                                                       ID3D12CommandQueue* queue,
                                                                       std::uint16_t packets,
                                                                       bool private_work,
                                                                       std::uint32_t compatible) noexcept {
  // Both mutexes held; no future transaction enters until end/refused releases
  // submission_mutex_. The caller then releases mutex_ and queues this
  // receipt's Wait when its queue differs from the last Signal's
  // (queue_transaction_wait), which therefore still targets an ALREADY queued
  // Signal and precedes the forward.
  apply_deferred();
  const auto bit = 1u << static_cast<unsigned>(&owner - devices_.data());
  if (!owner.active || owner.failed || (!(compatible & bit) && !compatible_queue(queue, owner)) || next_receipt_ >= MaximumCounter ||
      owner.last_signal >= MaximumCounter) {
    fail_device(owner);
    return {};
  }
  transaction_ = {++next_receipt_, &owner, queue, owner.last_signal + 1, packets, private_work};
  transaction_.session_generation = owner.session_generation;
  for (std::size_t index = 0; index < packets_.size(); ++index)
    if (packets & (1u << index))
      ++packets_[index].in_flight;
  transaction_owner = this;
  thread_receipt = transaction_.id;
  return {transaction_.id, owner.timeline, transaction_.value};
}
bool SceneCaptureManager::queue_transaction_wait(Device& owner, ID3D12CommandQueue* queue) noexcept {
  // Only submission_mutex_ is held. last_signal and last_signal_queue change
  // only in finish_transaction on that mutex's owner, which is this thread,
  // and the receipt reaches the forward only after this returns.
  if (!owner.last_signal)
    return true;
  // The previous receipt's Signal is already on this queue. D3D12 executes one
  // queue's ExecuteCommandLists calls and Signals in submission order, and work
  // of separate calls does not overlap (the guarantee the private tails rely
  // on), so this receipt already runs after that Signal and all it covered.
  // No Wait is queued, so nothing is published for the watchdog: a same-queue
  // Wait never needs a CPU release, its Signal being ahead of it. The held
  // reference keeps the address from being reused by another queue; a wrapper
  // and its native queue compare unequal and keep the Wait.
  if (queue == owner.last_signal_queue)
    return true;
  if (SUCCEEDED(queue->Wait(owner.timeline, owner.last_signal))) {
    publish_queued_wait(owner);  // Still after its Wait, for the watchdog.
    return true;
  }
  // Undo the reservation without finish_transaction: its Signal would not be
  // ordered after the previous receipt's. This leaves the old failed-Wait
  // state: device failed, nothing in flight, no transaction on this thread.
  const std::lock_guard lock(mutex_);
  apply_deferred();
  fail_device(owner);
  for (std::size_t index = 0; index < packets_.size(); ++index)
    if (transaction_.packets & (1u << index))
      --packets_[index].in_flight;
  release_source_leases(transaction_.source_leases, transaction_.source_lease_count);
  transaction_ = {};
  transaction_owner = nullptr;
  thread_receipt = 0;
  return false;
}

std::uint64_t SceneCaptureManager::before_submission(ID3D12CommandQueue* queue,
                                                     UINT count,
                                                     ID3D12CommandList* const* native_lists) noexcept {
  if (transaction_owner || !native_lists || !count || count > engine_hook::queue_submit::kMaximumCommandLists)
    return 0;
  escaped_sources = 0;
  if (!submission_gate_open()) {
    // Watchdog closed the gate: forward everything as an escaped batch.
    gated_submissions_.fetch_add(1, std::memory_order_relaxed);
    escape_unordered(queue, count, native_lists);
    return 0;
  }
  // Discovery can acquire the bridge registry lock. Run it before submission
  // serialization to avoid registry -> runtime -> submission -> registry.
  // The actual Execute arguments keep these native objects alive throughout.
  const auto bypass_unrelated = [&]() noexcept {
    const auto batch = classify_unobserved(queue, count, native_lists);
    return batch.unrelated;
  };
  if (!observe_unknown_lists(queue, count, native_lists) && bypass_unrelated())
    return 0;
  DisplaySubmissionPlan display_plan;
  if (display_planner_)
    display_planner_(display_planner_context_, queue, count, native_lists, display_plan);
  // Genuine recorded GPU ownership still requires ordering, but this is the
  // application's submit or Present thread: wait only within the budget, then
  // escape like a contended batch. Never park it on the bridge worker.
  const auto ordered_lock = [&](auto& mutex, auto& lock) noexcept {
    // Its waits are reported as mutex_ or submission_mutex_ ones.
    constexpr auto waited_lock =
        std::is_same_v<std::remove_reference_t<decltype(mutex)>, ManagerMutex> ? hook_timing::manager_lock : hook_timing::submission_lock;
    BoundedLock bounded(mutex, wait_budget::submit_us, &contended_submissions_, waited_lock);
    if (!bounded) {
      escape_unordered(queue, count, native_lists);
      return false;
    }
    bounded.release();
    lock = std::remove_reference_t<decltype(lock)>(mutex, std::adopt_lock);
    return true;
  };
  {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      if (bypass_unrelated())
        return 0;
      if (!ordered_lock(mutex_, lock))
        return 0;
    }
    apply_deferred();
    bool known_unrelated = true;
    for (UINT index = 0; index < count; ++index) {
      const auto* item = list(static_cast<ID3D12GraphicsCommandList*>(native_lists[index]));
      if (!item || item->awaiting_native_reset || item->packets || item->consumer || item->source_touched) {
        known_unrelated = false;
        break;
      }
    }
    // Only fully observed recordings with no owned work or source-state effects
    // can bypass ordering. Unknown recordings keep conservative invalidation.
    if (known_unrelated && !display_plan.current())
      return 0;
  }
  // Ordering is needed: do the queue identity COM calls before either lock.
  const auto compatible = compatible_devices(queue);
  std::unique_lock submission_lock(submission_mutex_, std::try_to_lock);
  if (!submission_lock.owns_lock()) {
    if (bypass_unrelated())
      return 0;
    if (!ordered_lock(submission_mutex_, submission_lock))
      return 0;
  }
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    if (bypass_unrelated())
      return 0;
    if (!ordered_lock(mutex_, lock))
      return 0;
  }
  apply_deferred();
  // Re-read every recording after serialization: Reset/retirement may have
  // changed its identity, effects or leases while this submission was waiting.
  Device* owner = display_plan.current() ? device(display_plan.device_key) : nullptr;
  std::uint16_t mask = 0;
  bool consumer = owner != nullptr;
  bool source_work = false, unknown_lists = false;
  for (UINT index = 0; index < count; ++index) {
    auto* item = list(static_cast<ID3D12GraphicsCommandList*>(native_lists[index]));
    if (!item || item->awaiting_native_reset) {
      unknown_lists = true;
      ++stats_.unknown_submitted_lists;
      continue;
    }
    if (!item->packets && !item->consumer && !item->source_touched)
      continue;
    auto* current = device(item->device_key);
    if (!current || (owner && owner != current)) {
      if (owner)
        fail_device(*owner);
      if (current)
        fail_device(*current);
      return 0;
    }
    owner = current;
    // A stale source recording may still own capture packets or read stable
    // output. Preserve that ownership below while refusing its source proof.
    if (item->source_touched && item->session_generation != current->session_generation)
      unknown_lists = true;
    mask |= item->packets;
    consumer |= item->consumer;
    source_work |= item->source_touched;
  }
  if (unknown_lists && !owner)
    for (auto& candidate : devices_)
      if (candidate.active && compatible_queue(queue, candidate)) {
        wipe(candidate, WipeSite::unknown_lists_no_owner);
        break;
      }
  if (!owner || (!mask && !consumer && !source_work))
    return 0;
  const auto result = begin_transaction(*owner, queue, mask, false, compatible);
  if (result.receipt) {
    // Decided under mutex_, which reset_session and resume_session hold.
    const bool record_display = owner->session_active && display_plan.current() && display_plan.device_key == owner->key && !unknown_lists;
    // ExecuteCommandLists order can differ from recording/allocation order.
    // Replayed lists use their last occurrence in this exact batch. Private
    // queue-tail captures follow every application list in the same receipt.
    transaction_.last_position = count;
    for (UINT index = 0; mask && index < count; ++index) {
      const auto* item = list(static_cast<ID3D12GraphicsCommandList*>(native_lists[index]));
      if (!item || item->awaiting_native_reset || !item->packets)
        continue;
      for (std::size_t slot = 0; slot < packets_.size(); ++slot)
        if (item->packets & (1u << slot))
          transaction_.packet_positions[slot] = index + 1;
    }
    transaction_.source_work = source_work;
    owner->source_states.begin_batch();
    if (unknown_lists)
      wipe(*owner, WipeSite::unknown_lists);
    else
      for (UINT index = 0; index < count; ++index) {
        const auto* item = list(static_cast<ID3D12GraphicsCommandList*>(native_lists[index]));
        if (item && item->source_touched) {
          if (!owner->source_states.apply(item->source_effects)) {
            // The tracker wiped itself for this invalid recording.
            note_wipe(WipeSite::invalid_recording);
            ++stats_.invalid_source_recordings;
            stats_.recording_overflows += item->source_effects.overflowed;
          }
          // The source reference is already owned by the recording. Snapshot
          // it before original Execute so concurrent successful Reset cannot
          // retire the only lease before the post-submit tail acquires it.
          if (source_tracking_)
            for (std::size_t lease_index = 0; lease_index < item->source_lease_count; ++lease_index) {
              const auto& lease = item->source_leases[lease_index];
              if (!retain_source_lease(transaction_.source_leases, transaction_.source_lease_count, lease.key, lease.native))
                wipe(*owner, WipeSite::lease_overflow);
            }
        }
      }
    // The Wait and the PFD copy recording need only submission serialization:
    // last_signal, transaction_ and the copy pools belong to its owner. Driver
    // calls under mutex_ stalled every recording thread behind them.
    lock.unlock();
    if (!queue_transaction_wait(*owner, queue))
      return 0;
    if (record_display)
      for (UINT i = 0; i < display_plan.count; ++i) {
        const auto& planned = display_plan.items[i];
        if (planned.after_list >= count)
          continue;
        const auto recorded = owner->display_copies.record(planned.copy);
        if (!recorded) {
          ++transaction_.display_unrecorded;  // Counted by finish_transaction under mutex_.
          continue;
        }
        const auto slot = transaction_.display_count++;
        transaction_.display_slots[slot] = recorded.slot;
        transaction_.display_insertions[slot] = {planned.after_list, recorded.list, planned.before};
      }
    submission_lock.release();  // Same native wrapper thread must complete it.
  }
  return result.receipt;
}

bool SceneCaptureManager::finish_transaction(std::uint64_t receipt, bool refused, bool fatal) noexcept {
  if (transaction_owner != this || thread_receipt != receipt || !receipt)
    return false;
  bool success = false;
  TailBatch tails;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandQueue* replaced_queue = nullptr;
  ID3D12Fence* timeline = nullptr;
  std::uint64_t value = 0;
  {
    const std::lock_guard lock(mutex_);
    apply_deferred();
    auto& pending = transaction_;
    if (pending.id == receipt && pending.device) {
      refused |= pending.device->failed;
      if (!refused && pending.source_work && source_tracking_ && capture_enabled_)
        record_queue_tail(pending, tails);
      queue = pending.queue;
      timeline = pending.device->timeline;
      value = pending.value;
    }
  }
  if (queue) {
    // Private tails follow every application list of this receipt and the
    // Signal retires all of it. Both are driver-bounded queue calls and run
    // without mutex_: recording threads must not expire their evidence budgets
    // behind them. This thread still owns submission_mutex_ and transaction_.
    // One call of their own, after the application's: separate calls do not
    // overlap, so every tail runs after the receipt's lists. Lists within this
    // one call may overlap, so tails must touch disjoint resources; each has
    // its own feed's source, packet, barriers and timing. Packet positions
    // order publication, not GPU execution.
    if (tails.count)
      queue->ExecuteCommandLists(tails.count, tails.lists.data());
    // Signal even a refused/aborted receipt to retire already forwarded work;
    // this is ordering evidence, never publication of its capture contents.
    success = SUCCEEDED(queue->Signal(timeline, value));
    const std::lock_guard lock(mutex_);
    apply_deferred();
    auto& pending = transaction_;
    refused |= pending.device->failed;
    if (success) {
      pending.device->last_signal = pending.value;
      if (pending.device->last_signal_queue != pending.queue) {
        pending.queue->AddRef();
        replaced_queue = std::exchange(pending.device->last_signal_queue, pending.queue);
      }
    }
    if (!success || (refused && fatal))
      fail_device(*pending.device);
    else if (refused)
      wipe(*pending.device, WipeSite::refused_transaction);
    for (UINT i = 0; i < pending.display_count; ++i) {
      const auto slot = pending.display_slots[i];
      if (!success || refused)
        pending.device->display_copies.quarantine(slot);
      else if (pending.display_accepted)
        pending.device->display_copies.submit(slot, pending.value);
      else
        pending.device->display_copies.cancel(slot);
    }
    if (success && !refused)
      stats_.display_copies += pending.display_accepted;
    stats_.display_record_failures += pending.display_unrecorded;
    for (std::size_t index = 0; index < packets_.size(); ++index) {
      if (!(pending.packets & (1u << index)))
        continue;
      auto& packet = packets_[index];
      --packet.in_flight;
      if (success && !refused) {
        if (pending.timed_tail_packets & (1u << index))
          packet.tail_timing.submitted(pending.device->timeline, pending.value);
        packet.producer = pending.queue;
        packet.order = {pending.value, pending.packet_positions[index]};
        ++packet.submitted;
      } else {
        if (pending.timed_tail_packets & (1u << index))
          packet.tail_timing.abandon();
        quarantine(packet);
      }
    }
    ++stats_.submissions;
    release_source_leases(pending.source_leases, pending.source_lease_count);
    pending = {};
    collect();
  }
  if (replaced_queue)
    replaced_queue->Release();  // Outside mutex_: a queue is never released under it.
  transaction_owner = nullptr;
  thread_receipt = 0;
  submission_mutex_.unlock();
  return success && !refused;
}

void SceneCaptureManager::after_submission(ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  if (transaction_owner == this && transaction_.queue == queue)
    finish_transaction(receipt, false);
}
void SceneCaptureManager::submission_refused(ID3D12CommandQueue* queue, engine_hook::queue_submit::Refusal reason) noexcept {
  const bool fatal = reason != engine_hook::queue_submit::Refusal::contended_submission &&
                     reason != engine_hook::queue_submit::Refusal::reentrant_submission;
  if (transaction_owner == this) {
    finish_transaction(thread_receipt, true, fatal);
    return;
  }
  // Legacy callers have no batch identity. Publish uncertainty without taking
  // the mutex held by a possible native tail Execute/Signal on another thread.
  const auto devices = queue_devices(queue);
  if (fatal)
    deferred_uncertain_.fetch_or(devices, std::memory_order_release);
  else
    publish_source_uncertainty(OriginRefusedContended, devices);
}
std::uint64_t SceneCaptureManager::submission_refused_batch(ID3D12CommandQueue* queue,
                                                            engine_hook::queue_submit::Refusal reason,
                                                            UINT count,
                                                            ID3D12CommandList* const* lists) noexcept {
  if (reason != engine_hook::queue_submit::Refusal::contended_submission) {
    deferred_uncertain_.fetch_or(queue_devices(queue), std::memory_order_release);
    return 0;
  }
  // Runs before the exact native batch forwards: Reset cannot erase its packet
  // classification before this notice is published. No lock or GPU call here.
  const auto batch = classify_unobserved(queue, count, lists, true);
  deferred_uncertain_.fetch_or(batch.uncertain, std::memory_order_release);
  return batch.sources;
}
void SceneCaptureManager::submission_refused_completed(std::uint64_t token) noexcept {
  // Preserve post-forward invalidation: an intervening receipt cannot consume
  // this notice and rebuild source proof before the escaped batch is queued.
  publish_source_uncertainty(OriginRefusedCompleted, static_cast<std::uint32_t>(token));
}

SceneCaptureManager::Submission SceneCaptureManager::begin_private_submission(std::uint64_t key, ID3D12CommandQueue* queue) noexcept {
  if (transaction_owner)
    return {};
  const auto compatible = compatible_devices(queue);
  std::unique_lock submission_lock(submission_mutex_, std::try_to_lock);
  if (!submission_lock.owns_lock())
    return {0, nullptr, 0, true};
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return {0, nullptr, 0, true};
  auto* owner = device(key);
  if (!owner || !owner->session_active)
    return {};
  const auto result = begin_transaction(*owner, queue, 0, true, compatible);
  if (result.receipt) {
    lock.unlock();  // As in before_submission, the Wait needs only submission_mutex_.
    if (!queue_transaction_wait(*owner, queue))
      return {};
    submission_lock.release();
  }
  return result;
}
bool SceneCaptureManager::end_private_submission(std::uint64_t receipt) noexcept {
  return transaction_owner == this && transaction_.private_work && finish_transaction(receipt, false);
}
void SceneCaptureManager::abort_private_submission(std::uint64_t receipt) noexcept {
  if (transaction_owner == this && transaction_.private_work)
    finish_transaction(receipt, true);
}

void SceneCaptureManager::collect() noexcept {
  apply_deferred();
  bool retired = false;
  for (std::size_t index = 0; index < sources_.size(); ++index) {
    auto& source = sources_[index];
    if (!source.native || source_generations_[index].load(std::memory_order_acquire))
      continue;
    const source_state::Key key{reinterpret_cast<std::uint64_t>(source.native), source.generation};
    if (auto* owner = device(source.device_key))
      owner->source_states.unregister_source(key);
    for (auto& phase : phase_feeds_)
      if (phase.source == key)
        phase = {};
    source = {};
    --stats_.source_candidates;
    retired = true;
  }
  if (retired) {
    rebuild_source_filter();
    draw_repeat_epoch_.fetch_add(1, std::memory_order_release);
  }
  for (auto& packet : packets_) {
    packet.tail_timing.poll();
    if (!packet.assigned || packet.quarantined)
      continue;
    if (packet.gpu.state() == SceneCaptureD3D12::State::consuming && packet.gpu.recycle()) {
      packet.assigned = packet.leased = false;
      continue;
    }
    if (!packet.retired || packet.in_flight || packet.gpu.state() != SceneCaptureD3D12::State::recorded)
      continue;
    if (!packet.submitted) {
      if (packet.gpu.discard_unsubmitted_retired())
        packet.assigned = false;
      else
        quarantine(packet);
    } else if (!packet.gpu.retire_serialized_recording(packet.producer)) {
      quarantine(packet);
    }
  }
}

std::size_t SceneCaptureManager::poll_completed_frames(Frame* frames, std::size_t capacity) noexcept {
  if (!frames || !capacity)
    return 0;
  if (capacity > MaximumPackets)
    capacity = MaximumPackets;
  const std::lock_guard lock(mutex_);
  collect();
  std::size_t count = 0;
  for (auto& packet : packets_) {
    if (!packet.assigned || packet.quarantined || packet.leased || !packet.gpu.poll_ready())
      continue;
    auto* owner = device(packet.device_key);
    if (!owner || !owner->active || owner->failed) {
      quarantine(packet);
      continue;
    }
    if (!owner->session_active || packet.session_generation != owner->session_generation || !handoff_.is_current(packet.match)) {
      // No external reader has seen this frame; the already-queued timeline
      // point safely returns the packet after the next collect.
      if (!packet.gpu.finish_consumption(owner->timeline, owner->last_signal))
        quarantine(packet);
      continue;
    }
    if (count == capacity)
      break;
    packet.leased = true;
    frames[count++] = {packet.token, packet.device_key, packet.match, packet.gpu.ready_resource(), packet.order, packet.session_generation};
    ++stats_.completed;
  }
  return count;
}
bool SceneCaptureManager::finish_consumption(std::uint64_t token, ID3D12Fence* fence, std::uint64_t value) noexcept {
  const std::lock_guard lock(mutex_);
  for (auto& packet : packets_)
    if (packet.assigned && packet.leased && !packet.quarantined && packet.token == token) {
      if (!packet.gpu.finish_consumption(fence, value))
        return false;
      packet.leased = false;
      return true;
    }
  return false;
}
bool SceneCaptureManager::discard_frame(std::uint64_t token) noexcept {
  const std::lock_guard lock(mutex_);
  for (auto& packet : packets_) {
    if (!packet.assigned || !packet.leased || packet.quarantined || packet.token != token)
      continue;
    auto* owner = device(packet.device_key);
    if (!owner || !packet.gpu.finish_consumption(owner->timeline, owner->last_signal))
      return false;
    packet.leased = false;
    return true;
  }
  return false;
}

SceneCaptureManager::Statistics SceneCaptureManager::statistics() const noexcept {
  const std::lock_guard lock(mutex_);
  auto result = stats_;
  for (const auto& packet : packets_)
    result.capture_copy_gpu.merge(packet.tail_timing.statistics()[0]);
  result.contended_evidence = contended_evidence_.load(std::memory_order_relaxed);
  result.contended_lifecycle = contended_lifecycle_.load(std::memory_order_relaxed);
  result.contended_submissions = contended_submissions_.load(std::memory_order_relaxed);
  result.unordered_submissions = unordered_submissions_.load(std::memory_order_relaxed);
  result.deferred_retirements = deferred_retirement_count_.load(std::memory_order_relaxed);
  result.gated_submissions = gated_submissions_.load(std::memory_order_relaxed);
  result.released_waits = released_waits_.load(std::memory_order_relaxed);
  result.deferred_evidence = deferred_evidence_count_.load(std::memory_order_relaxed);
  for (const auto& counts : reset_counts_)
    result.fast_resets += counts.fast.load(std::memory_order_relaxed);
  result.resets += result.fast_resets;
  result.clean_resets += result.fast_resets;
  return result;
}
SceneCaptureManager::ResetPaths SceneCaptureManager::reset_paths() const noexcept {
  // Relaxed reads of totals that only grow, each written by one thread: a
  // Reset racing this read is counted by the next one.
  ResetPaths result;
  for (const auto& counts : reset_counts_) {
    result.fast += counts.fast.load(std::memory_order_relaxed);
    result.locked += counts.locked.load(std::memory_order_relaxed);
    result.expired += counts.expired.load(std::memory_order_relaxed);
  }
  return result;
}
engine_hook::queue_submit::Callbacks SceneCaptureManager::callbacks() noexcept {
  return {
      this,
      [](void* context, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
        return static_cast<SceneCaptureManager*>(context)->before_submission(queue, count, lists);
      },
      [](void* context, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
        static_cast<SceneCaptureManager*>(context)->after_submission(queue, receipt);
      },
      [](void* context, ID3D12CommandQueue* queue, engine_hook::queue_submit::Refusal reason) noexcept {
        static_cast<SceneCaptureManager*>(context)->submission_refused(queue, reason);
      },
      [](void* context, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
        return static_cast<SceneCaptureManager*>(context)->submission_refused_batch(
            queue, engine_hook::queue_submit::Refusal::contended_submission, count, lists);
      },
      [](void* context, ID3D12CommandQueue*, std::uint64_t token) noexcept {
        static_cast<SceneCaptureManager*>(context)->submission_refused_completed(token);
      },
      [](void* context, ID3D12CommandQueue* queue, std::uint64_t receipt, UINT, ID3D12CommandList* const*,
         engine_hook::queue_submit::Insertion* output, UINT capacity) noexcept {
        return static_cast<SceneCaptureManager*>(context)->augment_submission(queue, receipt, output, capacity);
      },
      [](void* context, ID3D12CommandQueue* queue, std::uint64_t receipt, UINT inserted) noexcept {
        static_cast<SceneCaptureManager*>(context)->augmentation_result(queue, receipt, inserted);
      },
      [](void* context, ID3D12CommandQueue* queue) noexcept { static_cast<SceneCaptureManager*>(context)->forwarded_unordered(queue); }};
}
}  // namespace taxi_camera
