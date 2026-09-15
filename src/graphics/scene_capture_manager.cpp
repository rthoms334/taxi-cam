#include "scene_capture_manager.hpp"
#include "../hooks/render_boundary_observer.hpp"
#include "../profiles/catalog.hpp"
#include "native_device_identity.hpp"

#include <chrono>
#include <limits>

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
constexpr auto MaximumCounter = std::numeric_limits<std::uint64_t>::max() - 1;
bool same_description(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) noexcept {
  return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
         a.MipLevels == b.MipLevels && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
         a.SampleDesc.Quality == b.SampleDesc.Quality;
}
}  // namespace

SceneCaptureManager::SceneCaptureManager(SceneHandoff& handoff) noexcept : handoff_(handoff) {
  list_indices_.reserve(MaximumLists);
}
SceneCaptureManager::~SceneCaptureManager() = default;  // Native device/timeline leases intentionally process-lifetime.

SceneCaptureManager::Device* SceneCaptureManager::device(std::uint64_t key) noexcept {
  for (auto& item : devices_)
    if (item.key == key && key != 0)
      return &item;
  return nullptr;
}
SceneCaptureManager::List* SceneCaptureManager::list(ID3D12GraphicsCommandList* native) noexcept {
  const auto found = list_indices_.find(native);
  return found == list_indices_.end() ? nullptr : &lists_[found->second];
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
  owner.source_states.invalidate_all();
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
  const std::lock_guard lock(mutex_);
  auto* owner = device(key);
  if (!native || !generation || !owner || !owner->active || owner->failed || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  if (!same_native_device(native, owner->native))
    return false;
  if (const auto* existing = list(native))
    return existing->object_generation == generation && existing->device_key == key;
  for (std::size_t index = 0; index < lists_.size(); ++index) {
    if (lists_[index].native)
      continue;
    lists_[index] = {};
    lists_[index].native = native;
    lists_[index].device_key = key;
    lists_[index].object_generation = generation;
    lists_[index].awaiting_native_reset = !observed;
    list_indices_.emplace(native, index);
    return true;
  }
  return false;
}

bool SceneCaptureManager::set_unknown_list_observer(UnknownListObserver observer, void* context) noexcept {
  const std::lock_guard lock(mutex_);
  if (!observer || (unknown_list_observer_ && (unknown_list_observer_ != observer || unknown_list_context_ != context)))
    return false;
  unknown_list_observer_ = observer;
  unknown_list_context_ = context;
  return true;
}

void SceneCaptureManager::observe_unknown_lists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* native_lists) noexcept {
  std::array<ID3D12GraphicsCommandList*, engine_hook::queue_submit::kMaximumCommandLists> missing{};
  UINT missing_count = 0;
  std::uint64_t key = 0;
  UnknownListObserver observer = nullptr;
  void* context = nullptr;
  {
    const std::lock_guard lock(mutex_);
    observer = unknown_list_observer_;
    context = unknown_list_context_;
    if (!observer)
      return;
    for (UINT i = 0; i < count; ++i) {
      auto* native = static_cast<ID3D12GraphicsCommandList*>(native_lists[i]);
      if (native && !list(native))
        missing[missing_count++] = native;
    }
    if (!missing_count)
      return;
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
bool SceneCaptureManager::may_be_source(ID3D12Resource* resource) const noexcept {
  const auto hash = (reinterpret_cast<std::uint64_t>(resource) >> 4) * 0x9e3779b97f4a7c15ull;
  return resource && (source_filter_[(hash >> 6) & 15].load(std::memory_order_acquire) & (1ull << (hash & 63)));
}
void SceneCaptureManager::begin_source_tracking() noexcept {
  const std::lock_guard lock(mutex_);
  source_tracking_ = true;
  last_tail_us_ = {};
  stats_.tail_status = "awaiting_ordered_source_state";
}
void SceneCaptureManager::stop_source_tracking() noexcept {
  const std::lock_guard lock(mutex_);
  source_tracking_ = false;
  for (auto& item : lists_)
    release_source_leases(item.source_leases, item.source_lease_count);
  stats_.tail_status = "stopped";
}
void SceneCaptureManager::set_source_rate(std::uint32_t rate) noexcept {
  const std::lock_guard lock(mutex_);
  source_rate_ = rate < 15 ? 15 : rate > 60 ? 60 : rate;
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
  const std::lock_guard lock(mutex_);
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
    const auto hash = (reinterpret_cast<std::uint64_t>(resource) >> 4) * 0x9e3779b97f4a7c15ull;
    source_filter_[(hash >> 6) & 15].fetch_or(1ull << (hash & 63), std::memory_order_release);
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
    source_generations_[index].compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
  }
  // Rejection-only filter bits remain set; retirement can never hide a new key.
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
  const std::lock_guard lock(mutex_);
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
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  for (UINT n = 0; n < stage.count; ++n) {
    const auto& key = stage.keys[n];
    const auto* source = source_candidate(reinterpret_cast<ID3D12Resource*>(key.handle));
    if (!source || source->generation != key.generation || source->device_key != item->device_key)
      continue;
    item->source_touched = true;
    if (!allowed) {
      ++stats_.invalid_draws;
      item->source_effects.invalidate();
    } else {
      item->source_effects.append({key, source_state::Effect::Kind::draw});
      // The exact application Draw has completed but has not returned to its
      // caller: its actual bound RTV source is still a live resource argument.
      // Keep one reference per recording, never a registry-lifetime reference.
      if (source_tracking_ && !retain_source_lease(item->source_leases, item->source_lease_count, key, source->native)) {
        ++stats_.source_lease_failures;
        item->source_effects.invalidate();
      }
      ++stats_.source_draws;
    }
  }
}
void SceneCaptureManager::invalidate_source_recording(ID3D12GraphicsCommandList* native,
                                                      std::uint64_t generation,
                                                      bool global,
                                                      std::uint32_t reasons) noexcept {
  if (source_stage.owner == this && source_stage.list == native)
    source_stage = {};
  const std::lock_guard lock(mutex_);
  if (auto* item = list(native); item && item->object_generation == generation) {
    stats_.last_invalidation_reasons = reasons;
    item->source_effects.invalidate();
    item->source_touched |= global;
  }
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
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  if (count > 8 || !targets || !generations) {
    item->source_touched = true;
    item->source_effects.invalidate();
    return;
  }
  for (UINT index = 0; index < count; ++index) {
    const auto* candidate = source_candidate(targets[index]);
    if (!candidate || candidate->device_key != item->device_key || candidate->generation != generations[index])
      continue;
    item->source_touched = true;
    item->source_effects.append(
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
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  if (!item || item->object_generation != generation)
    return;
  if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
    if (!barrier.Aliasing.pResourceBefore || !barrier.Aliasing.pResourceAfter) {
      ++stats_.global_aliases;
      item->source_touched = true;
      item->source_effects.invalidate();
    } else {
      for (auto* aliased : {barrier.Aliasing.pResourceBefore, barrier.Aliasing.pResourceAfter})
        if (const auto* candidate = source_candidate(aliased); candidate && candidate->device_key == item->device_key) {
          item->source_touched = true;
          item->source_effects.append(
              {{reinterpret_cast<std::uint64_t>(aliased), candidate->generation}, source_state::Effect::Kind::other});
        }
    }
    return;
  }
  const auto* source = source_candidate(barrier.Transition.pResource);
  if (!source || source->device_key != item->device_key)
    return;
  item->source_touched = true;
  if (barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
      (barrier.Transition.Subresource != 0 && barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
    item->source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, source_state::Effect::Kind::other});
    return;
  }
  const auto kind = barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET ? source_state::Effect::Kind::legacy_rt
                                                                                        : source_state::Effect::Kind::other;
  item->source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, kind});
}
void SceneCaptureManager::observe_source_enhanced(ID3D12GraphicsCommandList* native,
                                                  std::uint64_t generation,
                                                  const D3D12_TEXTURE_BARRIER& barrier) noexcept {
  if (!may_be_source(barrier.pResource))
    return;
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  const auto* source = source_candidate(barrier.pResource);
  if (!item || item->object_generation != generation || !source || source->device_key != item->device_key)
    return;
  item->source_touched = true;
  const auto& range = barrier.Subresources;
  const bool whole = range.NumMipLevels == 0 ? range.IndexOrFirstMipLevel == 0 || range.IndexOrFirstMipLevel == UINT_MAX
                                             : range.IndexOrFirstMipLevel == 0 && range.NumMipLevels == 1 && range.FirstArraySlice == 0 &&
                                                   range.NumArraySlices == 1 && range.FirstPlane == 0 && range.NumPlanes == 1;
  if (!whole || barrier.Flags != D3D12_TEXTURE_BARRIER_FLAG_NONE || (barrier.SyncBefore & D3D12_BARRIER_SYNC_SPLIT) ||
      (barrier.SyncAfter & D3D12_BARRIER_SYNC_SPLIT)) {
    item->source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, source_state::Effect::Kind::other});
    return;
  }
  const auto kind = barrier.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET && barrier.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET
                        ? source_state::Effect::Kind::enhanced_rt
                        : source_state::Effect::Kind::other;
  item->source_effects.append({{reinterpret_cast<std::uint64_t>(source->native), source->generation}, kind});
}
void SceneCaptureManager::successful_reset(ID3D12GraphicsCommandList* native, std::uint64_t generation) noexcept {
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  if (item && item->object_generation == generation) {
    retire_list(*item);
    item->awaiting_native_reset = false;
    ++stats_.resets;
    collect();
  }
}
void SceneCaptureManager::destroy_command_list(ID3D12GraphicsCommandList* native, std::uint64_t generation) noexcept {
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  if (item && item->object_generation == generation) {
    retire_list(*item);
    item->native = nullptr;
    list_indices_.erase(native);
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
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || (native_observation && item->object_generation != generation))
    return false;
  const auto matches =
      handoff_.observe_copy(item->device_key, reinterpret_cast<std::uint64_t>(source), reinterpret_cast<std::uint64_t>(destination));
  // A copy between the two feeds is ambiguous and must never label one as both.
  if (matches.source.matched && matches.destination.matched && matches.source.feed != matches.destination.feed)
    return false;
  const auto match = matches.source.matched ? matches.source : matches.destination;
  if (!match.matched || match.feed > 1)
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
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || item->object_generation != object_generation)
    return false;
  const auto match = handoff_.observe_copy(item->device_key, reinterpret_cast<std::uint64_t>(target), 0).source;
  if (!match.matched || match.feed > 1)
    return false;
  return capture_source(*item, *owner, match, target, true);
}

bool SceneCaptureManager::record_render_target_before_enhanced_transition(ID3D12GraphicsCommandList7* native,
                                                                          ID3D12Resource* target,
                                                                          bool proven_enhanced_render_target,
                                                                          std::uint64_t object_generation) noexcept {
  if (!native || !target || !proven_enhanced_render_target || !handoff_.may_match_resource(reinterpret_cast<std::uint64_t>(target)))
    return false;
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed || item->object_generation != object_generation)
    return false;
  const auto match = handoff_.observe_copy(item->device_key, reinterpret_cast<std::uint64_t>(target), 0).source;
  if (!match.matched || match.feed > 1)
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
  for (std::size_t index = 0; index < packets_.size(); ++index) {
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
      packet.description = desc;
      packet.device_key = owner.key;
    }
    if (!record(packet)) {
      refusal(packet.gpu.error());
      continue;
    }
    packet.match = match;
    packet.token = ++next_token_;
    packet.submitted = 0;
    packet.order = {};
    packet.producer = nullptr;
    packet.in_flight = 0;
    packet.assigned = true;
    packet.retired = packet.leased = false;
    item.packets |= static_cast<std::uint16_t>(1u << index);
    item.feeds |= 1u << match.feed;
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
  return SUCCEEDED(packet.tail_allocator->Reset()) && SUCCEEDED(packet.tail_list->Reset(packet.tail_allocator, nullptr));
}
void SceneCaptureManager::record_queue_tail(Transaction& pending) noexcept {
  auto& owner = *pending.device;
  if (!owner.active || owner.failed)
    return;
  if (!engine_hook::render_boundary::operational()) {
    owner.source_states.invalidate_all();
    stats_.tail_status = "observer_disabled";
    return;
  }
  collect();
  unsigned feeds = 0;
  stats_.tail_status = "no_candidate_draw";
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  for (std::size_t source_index = 0; source_index < sources_.size(); ++source_index) {
    const auto& source = sources_[source_index];
    if (!source.native || source.device_key != owner.key ||
        source_generations_[source_index].load(std::memory_order_acquire) != source.generation)
      continue;
    const auto state = owner.source_states.state({reinterpret_cast<std::uint64_t>(source.native), source.generation});
    const auto observed_feed = handoff_.observed_feed(reinterpret_cast<std::uint64_t>(source.native));
    if (state.model == source_state::Model::unknown) {
      if (observed_feed >= 0)
        stats_.tail_status = "unknown_source_state";
      continue;
    }
    if (!state.drawn || (state.model != source_state::Model::legacy_rt && state.model != source_state::Model::enhanced_rt))
      continue;
    const auto match = handoff_.observe_copy(owner.key, reinterpret_cast<std::uint64_t>(source.native), 0).source;
    if (!match.matched || match.feed > 1) {
      if (observed_feed >= 0)
        stats_.tail_status = "waiting_for_publication";
      continue;
    }
    if (feeds & (1u << match.feed))
      continue;
    ID3D12Resource* leased_source = nullptr;
    for (std::size_t index = 0; index < pending.source_lease_count; ++index) {
      const auto& lease = pending.source_leases[index];
      if (lease.key == source_state::Key{reinterpret_cast<std::uint64_t>(source.native), source.generation}) {
        leased_source = lease.native;
        break;
      }
    }
    if (!leased_source) {
      stats_.tail_status = "source_lease_unavailable";
      continue;
    }
    const auto previous = last_tail_us_[match.feed];
    if (previous && (now < previous || now - previous < (1000000u + source_rate_ - 1) / source_rate_)) {
      stats_.tail_status = "sample_interval";
      continue;
    }
    stats_.tail_status = "tail_packet_unavailable";
    for (auto& packet : packets_) {
      if (!prepare_tail(packet, owner))
        continue;
      List private_recording;
      private_recording.native = packet.tail_list;
      private_recording.device_key = owner.key;
      const bool enhanced = state.model == source_state::Model::enhanced_rt;
      const bool recorded =
          (!enhanced || packet.tail_list7) && capture_source(private_recording, owner, match, leased_source, true,
                                                             enhanced ? packet.tail_list7 : nullptr, false, nullptr, &packet);
      const auto closed = packet.tail_list->Close();
      if (!recorded)
        continue;
      packet.retired = true;  // Private list is submitted once and never replayed.
      if (FAILED(closed)) {
        quarantine(packet);
        stats_.tail_status = "tail_close_failed";
        break;
      }
      ++packet.in_flight;
      pending.packets |= private_recording.packets;
      pending.packet_positions[&packet - packets_.data()] = ++pending.last_position;
      // Explicitly attach this owned submission to the OUTER receipt. The
      // native queue wrapper bypasses nested observation from its after phase.
      ID3D12CommandList* executable = packet.tail_list;
      pending.queue->ExecuteCommandLists(1, &executable);
      ++stats_.tail_submissions;
      ++stats_.tail_captures;
      last_tail_us_[match.feed] = now;
      stats_.tail_status = "captured";
      feeds |= 1u << match.feed;
      break;
    }
  }
  if (feeds)
    stats_.tail_status = "captured";
}

bool SceneCaptureManager::register_consumer_recording(ID3D12GraphicsCommandList* native) noexcept {
  const std::lock_guard lock(mutex_);
  auto* item = list(native);
  auto* owner = item ? device(item->device_key) : nullptr;
  if (!owner || !owner->active || owner->failed)
    return false;
  item->consumer = true;
  return true;
}

bool SceneCaptureManager::compatible_queue(ID3D12CommandQueue* queue, const Device& owner) noexcept {
  if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;
  return same_native_device(queue, owner.native);
}

SceneCaptureManager::Submission SceneCaptureManager::begin_transaction(Device& owner,
                                                                       ID3D12CommandQueue* queue,
                                                                       std::uint16_t packets,
                                                                       bool private_work) noexcept {
  // Both mutexes held; no future transaction enters until end/refused releases
  // submission_mutex_. Therefore every Wait targets an ALREADY queued Signal.
  if (!owner.active || owner.failed || !compatible_queue(queue, owner) || next_receipt_ >= MaximumCounter ||
      owner.last_signal >= MaximumCounter) {
    fail_device(owner);
    return {};
  }
  if (owner.last_signal && FAILED(queue->Wait(owner.timeline, owner.last_signal))) {
    fail_device(owner);
    return {};
  }
  transaction_ = {++next_receipt_, &owner, queue, owner.last_signal + 1, packets, private_work};
  for (std::size_t index = 0; index < packets_.size(); ++index)
    if (packets & (1u << index))
      ++packets_[index].in_flight;
  transaction_owner = this;
  thread_receipt = transaction_.id;
  return {transaction_.id, owner.timeline, transaction_.value};
}

std::uint64_t SceneCaptureManager::before_submission(ID3D12CommandQueue* queue,
                                                     UINT count,
                                                     ID3D12CommandList* const* native_lists) noexcept {
  if (transaction_owner || !native_lists || !count || count > engine_hook::queue_submit::kMaximumCommandLists)
    return 0;
  // Discovery can acquire the bridge registry lock. Run it before submission
  // serialization to avoid registry -> runtime -> submission -> registry.
  // The actual Execute arguments keep these native objects alive throughout.
  observe_unknown_lists(queue, count, native_lists);
  {
    const std::lock_guard lock(mutex_);
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
    if (known_unrelated)
      return 0;
  }
  std::unique_lock submission_lock(submission_mutex_);
  const std::lock_guard lock(mutex_);
  // Re-read every recording after serialization: Reset/retirement may have
  // changed its identity, effects or leases while this submission was waiting.
  Device* owner = nullptr;
  std::uint16_t mask = 0;
  bool consumer = false;
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
    mask |= item->packets;
    consumer |= item->consumer;
    source_work |= item->source_touched;
  }
  if (unknown_lists && !owner)
    for (auto& candidate : devices_)
      if (candidate.active && compatible_queue(queue, candidate)) {
        candidate.source_states.invalidate_all();
        break;
      }
  if (!owner || (!mask && !consumer && !source_work))
    return 0;
  const auto result = begin_transaction(*owner, queue, mask, false);
  if (result.receipt) {
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
      owner->source_states.invalidate_all();
    else
      for (UINT index = 0; index < count; ++index) {
        const auto* item = list(static_cast<ID3D12GraphicsCommandList*>(native_lists[index]));
        if (item && item->source_touched) {
          if (!owner->source_states.apply(item->source_effects)) {
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
                owner->source_states.invalidate_all();
            }
        }
      }
    submission_lock.release();  // Same native wrapper thread must complete it.
  }
  return result.receipt;
}

bool SceneCaptureManager::finish_transaction(std::uint64_t receipt, bool refused) noexcept {
  if (transaction_owner != this || thread_receipt != receipt || !receipt)
    return false;
  bool success = false;
  {
    const std::lock_guard lock(mutex_);
    auto& pending = transaction_;
    if (pending.id == receipt && pending.device) {
      if (!refused && pending.source_work && source_tracking_)
        record_queue_tail(pending);
      // Signal even a refused/aborted receipt to retire already forwarded work;
      // this is ordering evidence, never publication of its capture contents.
      success = SUCCEEDED(pending.queue->Signal(pending.device->timeline, pending.value));
      if (success)
        pending.device->last_signal = pending.value;
      if (!success || refused)
        fail_device(*pending.device);
      for (std::size_t index = 0; index < packets_.size(); ++index) {
        if (!(pending.packets & (1u << index)))
          continue;
        auto& packet = packets_[index];
        --packet.in_flight;
        if (success && !refused) {
          packet.producer = pending.queue;
          packet.order = {pending.value, pending.packet_positions[index]};
          ++packet.submitted;
        } else {
          quarantine(packet);
        }
      }
      ++stats_.submissions;
      release_source_leases(pending.source_leases, pending.source_lease_count);
      pending = {};
      collect();
    }
  }
  transaction_owner = nullptr;
  thread_receipt = 0;
  submission_mutex_.unlock();
  return success && !refused;
}

void SceneCaptureManager::after_submission(ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
  if (transaction_owner == this && transaction_.queue == queue)
    finish_transaction(receipt, false);
}
void SceneCaptureManager::submission_refused(ID3D12CommandQueue* queue, engine_hook::queue_submit::Refusal) noexcept {
  if (transaction_owner == this) {
    finish_transaction(thread_receipt, true);
    return;
  }
  // The wrapper may refuse before taking a receipt (e.g. oversized batch). It
  // forwards first, so exact referenced packets are unknown: quarantine device.
  const std::lock_guard lock(mutex_);
  for (auto& owner : devices_)
    if (owner.active && compatible_queue(queue, owner))
      fail_device(owner);
}

SceneCaptureManager::Submission SceneCaptureManager::begin_private_submission(std::uint64_t key, ID3D12CommandQueue* queue) noexcept {
  if (transaction_owner)
    return {};
  std::unique_lock submission_lock(submission_mutex_);
  const std::lock_guard lock(mutex_);
  auto* owner = device(key);
  if (!owner)
    return {};
  const auto result = begin_transaction(*owner, queue, 0, true);
  if (result.receipt)
    submission_lock.release();
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
  for (std::size_t index = 0; index < sources_.size(); ++index) {
    auto& source = sources_[index];
    if (!source.native || source_generations_[index].load(std::memory_order_acquire))
      continue;
    if (auto* owner = device(source.device_key))
      owner->source_states.unregister_source({reinterpret_cast<std::uint64_t>(source.native), source.generation});
    source = {};
    --stats_.source_candidates;
  }
  for (auto& packet : packets_) {
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
    if (!handoff_.is_current(packet.match)) {
      // No external reader has seen this frame; the already-queued timeline
      // point safely returns the packet after the next collect.
      if (!packet.gpu.finish_consumption(owner->timeline, owner->last_signal))
        quarantine(packet);
      continue;
    }
    if (count == capacity)
      break;
    packet.leased = true;
    frames[count++] = {packet.token, packet.device_key, packet.match, packet.gpu.ready_resource(), packet.order};
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
  return stats_;
}
engine_hook::queue_submit::Callbacks SceneCaptureManager::callbacks() noexcept {
  return {this,
          [](void* context, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
            return static_cast<SceneCaptureManager*>(context)->before_submission(queue, count, lists);
          },
          [](void* context, ID3D12CommandQueue* queue, std::uint64_t receipt) noexcept {
            static_cast<SceneCaptureManager*>(context)->after_submission(queue, receipt);
          },
          [](void* context, ID3D12CommandQueue* queue, engine_hook::queue_submit::Refusal reason) noexcept {
            static_cast<SceneCaptureManager*>(context)->submission_refused(queue, reason);
          }};
}
}  // namespace taxi_camera
