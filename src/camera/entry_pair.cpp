#include "entry_pair.hpp"

#include <algorithm>

namespace taxi_camera::engine_camera {
namespace {

struct ProcessingGuard {
  std::atomic_flag& flag;
  ~ProcessingGuard() { flag.clear(std::memory_order_release); }
};

bool zero_dword(const DescriptorStorage& descriptor, std::size_t offset) noexcept {
  return std::all_of(descriptor.bytes.begin() + offset, descriptor.bytes.begin() + offset + 4,
                     [](std::uint8_t value) { return value == 0; });
}

unsigned clamp_feeds(unsigned feeds) noexcept {
  if (feeds < 1)
    return 1;
  if (feeds > kMaxOwnedViews)
    return kMaxOwnedViews;
  return feeds;
}

Failure create_failure_for(unsigned index) noexcept {
  if (index == 0)
    return Failure::first_create_failed;
  if (index == 1)
    return Failure::second_create_failed;
  if (index == 2)
    return Failure::third_create_failed;
  return Failure::fourth_create_failed;
}

}  // namespace

bool pack_mode_zero(DescriptorStorage& descriptor, const PoseKey& key) noexcept {
  if (descriptor.bytes[0] != 0 || !zero_dword(descriptor, 32) || descriptor.bytes[64] != 0 || !zero_dword(descriptor, 192))
    return false;
  std::fill_n(descriptor.bytes.begin() + 40, 4, 0);
  descriptor.bytes[44] = 1;
  std::copy(key.begin(), key.end(), descriptor.bytes.begin() + 48);
  return true;
}

bool pack_independent_pose(DescriptorStorage& descriptor) noexcept {
  if (!pack_mode_zero(descriptor, {}))
    return false;
  descriptor.bytes[40] = 2;
  return true;
}

void PairController::request_enable(const PairKeys& keys) noexcept {
  const std::lock_guard lock(mailbox_mutex_);
  mailbox_ = {Command::enable, keys, false, 2};
}

void PairController::request_independent_pose(unsigned feeds) noexcept {
  const std::lock_guard lock(mailbox_mutex_);
  mailbox_ = {Command::enable, {}, true, clamp_feeds(feeds)};
}

void PairController::request_disable() noexcept {
  const std::lock_guard lock(mailbox_mutex_);
  mailbox_ = {Command::disable, {}};
}

Snapshot PairController::snapshot() const noexcept {
  const std::lock_guard lock(mailbox_mutex_);
  auto result = published_;
  result.request_pending = mailbox_.command != Command::none;
  return result;
}

EmptyPairCancel PairController::cancel_uncreated_request() noexcept {
  if (processing_.test_and_set(std::memory_order_acquire))
    return EmptyPairCancel::busy;
  const ProcessingGuard guard{processing_};
  const std::lock_guard lock(mailbox_mutex_);
  if (has_owned_ids() || owner_ != ManagerToken{})
    return EmptyPairCancel::owned;
  mailbox_ = {};
  desired_enabled_ = false;
  creation_pending_ = false;
  active_pair_ = false;
  failure_ = Failure::none;
  published_ = {};
  return EmptyPairCancel::cancelled;
}

bool PairController::has_owned_ids() const noexcept {
  for (auto id : ids_)
    if (id != 0)
      return true;
  return false;
}

bool PairController::complete_owned_set() const noexcept {
  if (owned_feeds_ < 1 || owned_feeds_ > kMaxOwnedViews)
    return false;
  for (unsigned i = 0; i < owned_feeds_; ++i)
    if (ids_[i] == 0)
      return false;
  for (unsigned i = owned_feeds_; i < kMaxOwnedViews; ++i)
    if (ids_[i] != 0)
      return false;
  return true;
}

void PairController::publish(State state, Blocked blocked) noexcept {
  const std::lock_guard lock(mailbox_mutex_);
  published_ = {state, failure_, blocked, owner_, ids_, false, creation_pending_};
}

void PairController::cleanup(const EngineCallbacks& engine) noexcept {
  active_pair_ = false;  // A failed confirmation may still have removed an ID.
  // Reverse creation order. Keep an ID if the wrapper cannot confirm removal;
  // never retry one already confirmed absent, and never erase a zero ID.
  for (std::size_t index = ids_.size(); index != 0; --index) {
    auto& id = ids_[index - 1];
    if (id != 0 && engine.erase(engine.context, owner_, id))
      id = 0;
  }
  if (!has_owned_ids()) {
    owner_ = {};
    owned_feeds_ = 0;
  }
}

bool PairController::prepare(const EngineCallbacks& engine, DescriptorStorage& descriptor, const PoseKey& key) noexcept {
  if (!engine.initialize(engine.context, descriptor)) {
    failure_ = Failure::initializer_failed;
    return false;
  }
  if (!(desired_independent_pose_ ? pack_independent_pose(descriptor) : pack_mode_zero(descriptor, key))) {
    failure_ = Failure::initializer_contract;
    return false;
  }
  return true;
}

void PairController::creation_failed(const EngineCallbacks& engine, Failure failure) noexcept {
  failure_ = failure;
  if (has_owned_ids())
    cleanup(engine);
  publish(has_owned_ids() ? State::cleanup_pending : State::failed);
}

bool PairController::process_update(ManagerToken manager, const EngineCallbacks& engine) noexcept {
  if (processing_.test_and_set(std::memory_order_acquire))
    return false;
  const ProcessingGuard guard{processing_};
  Request request;
  {
    const std::lock_guard lock(mailbox_mutex_);
    request = mailbox_;
    mailbox_ = {};
  }
  if (request.command == Command::enable) {
    desired_keys_ = request.keys;
    desired_independent_pose_ = request.independent_pose;
    desired_feeds_ = clamp_feeds(request.feeds);
    desired_enabled_ = true;
    creation_pending_ = true;
    failure_ = Failure::none;
    if (active_pair_ && complete_owned_set() && owned_feeds_ == desired_feeds_ && owned_keys_ == desired_keys_ &&
        owned_independent_pose_ == desired_independent_pose_)
      creation_pending_ = false;  // Re-enabling the active set is idempotent.
  } else if (request.command == Command::disable) {
    desired_enabled_ = false;
    creation_pending_ = false;
    failure_ = Failure::none;
  }

  if (!has_owned_ids() && !creation_pending_) {
    publish(failure_ == Failure::none ? State::disabled : State::failed);
    return true;
  }
  if (!manager.valid()) {
    publish(State::blocked, Blocked::invalid_manager);
    return true;
  }
  if (has_owned_ids() && manager != owner_) {
    publish(State::blocked, Blocked::manager_mismatch);
    return true;
  }
  if (engine.initialize == nullptr || engine.create == nullptr || engine.erase == nullptr) {
    publish(State::blocked, Blocked::missing_callbacks);
    return true;
  }
  if (has_owned_ids()) {
    if (desired_enabled_ && active_pair_ && !creation_pending_ && failure_ == Failure::none && complete_owned_set()) {
      publish(State::active);
      return true;
    }
    cleanup(engine);
    if (has_owned_ids()) {
      publish(State::cleanup_pending);
      return true;
    }
  }
  if (!desired_enabled_ || !creation_pending_) {
    publish(failure_ == Failure::none ? State::disabled : State::failed);
    return true;
  }

  creation_pending_ = false;  // Consume once, before any callback can reenter.
  ids_ = {};
  owned_feeds_ = 0;
  for (unsigned index = 0; index < desired_feeds_; ++index) {
    DescriptorStorage descriptor{};
    if (!prepare(engine, descriptor, desired_keys_[index])) {
      creation_failed(engine, failure_);
      return true;
    }
    const auto id = engine.create(engine.context, manager, descriptor);
    if (id == 0) {
      creation_failed(engine, create_failure_for(index));
      return true;
    }
    for (unsigned previous = 0; previous < index; ++previous) {
      if (ids_[previous] == id) {
        creation_failed(engine, Failure::duplicate_id);
        return true;
      }
    }
    if (index == 0)
      owner_ = manager;
    ids_[index] = id;
  }
  owned_keys_ = desired_keys_;
  owned_independent_pose_ = desired_independent_pose_;
  owned_feeds_ = desired_feeds_;
  active_pair_ = true;
  publish(State::active);
  return true;
}

bool PairController::acknowledge_manager_destroyed(ManagerToken manager) noexcept {
  if (processing_.test_and_set(std::memory_order_acquire))
    return false;
  const ProcessingGuard guard{processing_};
  if (!has_owned_ids() || manager != owner_)
    return false;
  ids_ = {};
  owner_ = {};
  owned_feeds_ = 0;
  desired_enabled_ = false;
  creation_pending_ = false;
  active_pair_ = false;
  failure_ = Failure::manager_destroyed;
  {
    const std::lock_guard lock(mailbox_mutex_);
    mailbox_ = {};
  }
  publish(State::failed);
  return true;
}

}  // namespace taxi_camera::engine_camera
