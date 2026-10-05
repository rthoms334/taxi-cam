#include "scene_handoff.hpp"

#include <limits>
#include <new>

namespace taxi_camera {

bool SceneHandoff::advance(std::uint64_t& counter) {
  if (exhausted_ || counter == std::numeric_limits<std::uint64_t>::max()) {
    exhausted_ = true;
    active_ = false;
    publication_ = {};
    for (auto& handle : candidate_handles_)
      handle.store(0, std::memory_order_release);
    for (auto& handle : observed_handles_)
      handle.store(0, std::memory_order_release);
    return false;
  }
  ++counter;
  return true;
}

bool SceneHandoff::lifecycle_event(std::uint64_t changed_handle) {
  // Unrelated uploads/streaming allocations cannot change the identity of our
  // two already registered resources. Still invalidate every open inspection
  // ticket so an address freed/reused while its graph is being read is refused.
  if (!changed_handle || observed_feed(changed_handle) >= 0) {
    for (auto& handle : candidate_handles_)
      handle.store(0, std::memory_order_release);
    for (auto& handle : observed_handles_)
      handle.store(0, std::memory_order_release);
    publication_ = {};
    ++diagnostics_.target_invalidations;
  } else {
    ++diagnostics_.unrelated_events;
  }
  return advance(lifecycle_epoch_);
}

SceneHandoff::Device* SceneHandoff::device(std::uint64_t key) {
  if (key != 0)
    for (auto& item : devices_)
      if (item.key == key)
        return &item;
  return nullptr;
}

const SceneHandoff::Device* SceneHandoff::device(std::uint64_t key) const {
  if (key != 0)
    for (const auto& item : devices_)
      if (item.key == key)
        return &item;
  return nullptr;
}

std::uint64_t SceneHandoff::register_device(std::uint64_t key) {
  const std::lock_guard lock(mutex_);
  if (!lifecycle_event() || key == 0)
    return 0;
  auto* item = device(key);
  if (item == nullptr)
    for (auto& candidate : devices_)
      if (candidate.key == 0) {
        item = &candidate;
        break;
      }
  if (item == nullptr || !advance(device_counter_))
    return 0;
  item->resources.clear();
  item->key = key;
  item->epoch = device_counter_;
  return item->epoch;
}

void SceneHandoff::unregister_device(std::uint64_t key) {
  const std::lock_guard lock(mutex_);
  lifecycle_event();
  if (auto* item = device(key)) {
    item->resources.clear();
    item->key = 0;
    item->epoch = 0;
  }
}

bool SceneHandoff::register_resource(std::uint64_t key, std::uint64_t handle, std::uint64_t id) {
  const std::lock_guard lock(mutex_);
  if (!lifecycle_event(handle))
    return false;
  auto* item = device(key);
  if (item == nullptr || handle == 0)
    return false;
  if (id == 0) {
    item->resources.erase(handle);
    return false;
  }
  const auto existing = item->resources.find(handle);
  if (existing == item->resources.end() && item->resources.size() >= maximum_resources_per_device)
    return false;
  if (!advance(resource_counter_))
    return false;
  try {
    item->resources.insert_or_assign(handle, Resource{id, resource_counter_});
  } catch (const std::bad_alloc&) {
    // A duplicate registration must not leave the previous incarnation usable.
    item->resources.erase(handle);
    return false;
  }
  return true;
}

void SceneHandoff::unregister_resource(std::uint64_t key, std::uint64_t handle) {
  const std::lock_guard lock(mutex_);
  lifecycle_event(handle);
  if (auto* item = device(key))
    item->resources.erase(handle);
}
bool SceneHandoff::try_unregister_resource(std::uint64_t key, std::uint64_t handle, std::uint32_t budget_us) noexcept {
  const BoundedLock lock(mutex_, budget_us, nullptr, hook_timing::handoff_lock);
  if (!lock)
    return false;
  lifecycle_event(handle);
  if (auto* item = device(key))
    item->resources.erase(handle);
  return true;
}

std::uint64_t SceneHandoff::begin_scene() {
  const std::lock_guard lock(mutex_);
  for (auto& handle : candidate_handles_)
    handle.store(0, std::memory_order_release);
  for (auto& handle : observed_handles_)
    handle.store(0, std::memory_order_release);
  publication_ = {};
  owner_bound_ = false;
  owner_ = {};
  owned_ids_ = {};
  active_ = advance(scene_epoch_);
  return active_ ? scene_epoch_ : 0;
}

void SceneHandoff::stop_scene() {
  const std::lock_guard lock(mutex_);
  for (auto& handle : candidate_handles_)
    handle.store(0, std::memory_order_release);
  for (auto& handle : observed_handles_)
    handle.store(0, std::memory_order_release);
  active_ = false;
  owner_bound_ = false;
  publication_ = {};
  advance(scene_epoch_);
}

SceneCaptureTicket SceneHandoff::begin_capture() {
  const std::lock_guard lock(mutex_);
  // A refresh is not a resource/owner lifecycle event. Keep the last complete
  // pair usable while the new ticket proves its own independently checked graph.
  if (!active_ || !advance(capture_sequence_))
    return {};
  return {scene_epoch_, lifecycle_epoch_, capture_sequence_};
}

bool SceneHandoff::publish(SceneCaptureTicket ticket,
                           SceneManagerIdentity manager,
                           const std::array<std::uint64_t, 3>& ids,
                           const std::array<std::uint64_t, 3>& handles) {
  const std::lock_guard lock(mutex_);
  // An old concurrent completion may not revoke or replace a newer capture.
  if (active_ && ticket.scene_epoch == scene_epoch_ && ticket.sequence == capture_sequence_ && ticket.lifecycle_epoch != lifecycle_epoch_)
    ++diagnostics_.ticket_invalidations;
  if (!active_ || exhausted_ || ticket.scene_epoch != scene_epoch_ || ticket.lifecycle_epoch != lifecycle_epoch_ || ticket.sequence == 0 ||
      ticket.sequence != capture_sequence_ || (publication_.valid && publication_.sequence == ticket.sequence))
    return false;
  const auto refuse_latest = [&] {
    publication_ = {};
    for (auto& handle : candidate_handles_)
      handle.store(0, std::memory_order_release);
    return false;
  };
  if (manager.identity == 0 || manager.generation == 0 || ids[0] == 0 || ids[1] == 0 || ids[0] == ids[1] || handles[0] == 0 ||
      handles[1] == 0 || handles[0] == handles[1])
    return refuse_latest();
  if ((ids[2] == 0) != (handles[2] == 0))
    return refuse_latest();
  if (ids[2] && (ids[2] == ids[0] || ids[2] == ids[1] || handles[2] == 0 || handles[2] == handles[0] || handles[2] == handles[1]))
    return refuse_latest();
  if (owner_bound_ && (owner_ != manager || owned_ids_ != ids)) {
    // A new manager/pair requires a new authorized scene epoch.
    active_ = false;
    for (auto& handle : candidate_handles_)
      handle.store(0, std::memory_order_release);
    for (auto& handle : observed_handles_)
      handle.store(0, std::memory_order_release);
    advance(scene_epoch_);
    return refuse_latest();
  }
  Publication pending;
  pending.scene_epoch = scene_epoch_;
  pending.sequence = ticket.sequence;
  pending.manager = manager;
  pending.entry_ids = ids;
  pending.handles = handles;
  for (std::size_t feed = 0; feed < handles.size(); ++feed) {
    if (!handles[feed])
      continue;
    const Device* found_device = nullptr;
    Resource found;
    for (const auto& item : devices_) {
      if (item.key == 0)
        continue;
      if (const auto resource = item.resources.find(handles[feed]); resource != item.resources.end()) {
        if (found_device != nullptr)
          return refuse_latest();  // Opaque native address is ambiguous across devices.
        found_device = &item;
        found = resource->second;
      }
    }
    if (found_device == nullptr || (pending.device_key != 0 && pending.device_key != found_device->key))
      return refuse_latest();
    pending.device_key = found_device->key;
    pending.resources[feed] = {found_device->epoch, found.id, found.generation};
  }
  owner_bound_ = true;
  owner_ = manager;
  owned_ids_ = ids;
  pending.valid = true;
  publication_ = pending;
  for (std::size_t i = 0; i < handles.size(); ++i) {
    candidate_handles_[i].store(handles[i], std::memory_order_release);
    observed_handles_[i].store(handles[i], std::memory_order_release);
  }
  ++diagnostics_.publications;
  return true;
}

SceneCopyMatch SceneHandoff::match(std::uint64_t key, std::uint64_t handle) const {
  if (!active_ || exhausted_ || !publication_.valid || publication_.scene_epoch != scene_epoch_ || publication_.device_key != key ||
      handle == 0)
    return {};
  const auto* item = device(key);
  if (item == nullptr)
    return {};
  const auto resource = item->resources.find(handle);
  if (resource == item->resources.end())
    return {};
  const SceneResourceIdentity identity{item->epoch, resource->second.id, resource->second.generation};
  for (std::uint32_t feed = 0; feed < publication_.handles.size(); ++feed)
    if (publication_.handles[feed] && publication_.handles[feed] == handle && publication_.resources[feed] == identity)
      return {true, feed, scene_epoch_, publication_.sequence, publication_.manager, publication_.entry_ids[feed], identity};
  return {};
}

SceneCopyObservation SceneHandoff::observe_copy(std::uint64_t key, std::uint64_t source, std::uint64_t destination) const {
  bool candidate = false;
  for (const auto& slot : candidate_handles_) {
    const auto handle = slot.load(std::memory_order_acquire);
    if (handle && (handle == source || handle == destination))
      candidate = true;
  }
  if (!candidate)
    return {};
  const std::lock_guard lock(mutex_);
  return {match(key, source), match(key, destination)};
}

bool SceneHandoff::may_match_resource(std::uint64_t handle) const noexcept {
  if (!handle)
    return false;
  for (const auto& slot : candidate_handles_)
    if (slot.load(std::memory_order_acquire) == handle)
      return true;
  return false;
}

bool SceneHandoff::is_current(const SceneCopyMatch& value) const {
  const std::lock_guard lock(mutex_);
  if (!value.matched || value.feed >= publication_.handles.size() || !publication_.valid || !publication_.handles[value.feed])
    return false;
  const auto current = match(publication_.device_key, publication_.handles[value.feed]);
  return current.matched && value.scene_epoch == current.scene_epoch && value.manager == current.manager &&
         value.entry_id == current.entry_id && value.resource == current.resource && value.capture_sequence != 0 &&
         value.capture_sequence <= current.capture_sequence;
}

int SceneHandoff::observed_feed(std::uint64_t handle) const noexcept {
  if (!handle)
    return -1;
  for (int i = 0; i < static_cast<int>(observed_handles_.size()); ++i)
    if (observed_handles_[i].load(std::memory_order_acquire) == handle)
      return i;
  return -1;
}

SceneHandoff::Diagnostics SceneHandoff::diagnostics() const {
  const std::lock_guard lock(mutex_);
  auto result = diagnostics_;
  result.published = publication_.valid && active_ && !exhausted_;
  if (result.published)
    for (std::size_t i = 0; i < result.resource_ids.size(); ++i)
      result.resource_ids[i] = publication_.resources[i].resource_id;
  return result;
}

SceneHandoff& scene_handoff() {
  static auto* const instance = new SceneHandoff;
  return *instance;
}

}  // namespace taxi_camera
