#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace taxi_camera::engine_camera {

using EntryId = std::uint64_t;
using PoseKey = std::array<std::uint8_t, 16>;
// Nose, tail or left wing, right wing (idle on two-camera aircraft) and the
// door camera (profiles::CameraViews).
inline constexpr unsigned kMaxOwnedViews = 4;
using PairKeys = std::array<PoseKey, kMaxOwnedViews>;
using OwnedIds = std::array<EntryId, kMaxOwnedViews>;
// The set's views are created in order, so its owned IDs come first.
inline bool any_owned(const OwnedIds& ids) noexcept {
  for (const auto id : ids)
    if (id)
      return true;
  return false;
}
inline unsigned owned_count(const OwnedIds& ids) noexcept {
  unsigned count = 0;
  while (count < ids.size() && ids[count])
    ++count;
  return count;
}
// The first two views are owned and every owned ID is distinct.
inline bool distinct_owned(const OwnedIds& ids) noexcept {
  if (!ids[0] || !ids[1])
    return false;
  for (std::size_t i = 0; i < ids.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (ids[i] && ids[i] == ids[j])
        return false;
  return true;
}
// Ready, a flag per view: the first two always, the others where owned.
template <class Flags>
inline bool owned_ready(const OwnedIds& ids, const Flags& ready) noexcept {
  if (!ready[0] || !ready[1])
    return false;
  for (std::size_t i = 2; i < ids.size(); ++i)
    if (ids[i] && !ready[i])
      return false;
  return true;
}

// Opaque adapter identity, not a dereferenceable pointer. Generation must change
// if an address/identity is reused for a different manager lifetime.
struct ManagerToken {
  std::uint64_t identity = 0;
  std::uint64_t generation = 0;
  bool valid() const noexcept { return identity != 0 && generation != 0; }
  bool operator==(const ManagerToken&) const = default;
};

// The observed initializer touches at least 196 bytes. This deliberately padded
// backing store and its alignment are local choices, NOT a recovered sizeof or
// engine ABI guarantee. A native adapter must establish compatibility separately.
struct alignas(16) DescriptorStorage {
  static constexpr std::size_t minimum_observed_extent = 196;
  std::array<std::uint8_t, 256> bytes{};
};

// Confirms the initializer left both names empty and inline, then sets the
// observed mode-zero fields and opaque key. Does not initialize or free strings.
bool pack_mode_zero(DescriptorStorage& descriptor, const PoseKey& key) noexcept;
// Experimental mode observed to bypass both built-in pose branches. This only
// packs the descriptor; the native adapter owns setup, pose and view checks.
bool pack_independent_pose(DescriptorStorage& descriptor) noexcept;

struct EngineCallbacks {
  void* context = nullptr;
  bool (*initialize)(void*, DescriptorStorage&) noexcept = nullptr;
  EntryId (*create)(void*, ManagerToken, const DescriptorStorage&) noexcept = nullptr;
  // True ONLY when the adapter has confirmed this ID is absent after erase.
  // The recovered native erase takes (manager, uint64 ID by value), with no
  // established status return. This bool is the wrapper's verification result.
  bool (*erase)(void*, ManagerToken, EntryId) noexcept = nullptr;
};

enum class State { disabled, active, cleanup_pending, failed, blocked };
enum class Failure {
  none,
  initializer_failed,
  initializer_contract,
  first_create_failed,
  second_create_failed,
  third_create_failed,
  fourth_create_failed,
  duplicate_id,
  manager_destroyed
};
enum class Blocked { none, invalid_manager, missing_callbacks, manager_mismatch };
enum class EmptyPairCancel { cancelled, busy, owned };

struct Snapshot {
  State state = State::disabled;
  Failure failure = Failure::none;
  Blocked blocked = Blocked::none;
  ManagerToken owner{};
  // Capacity four; views not requested stay 0.
  OwnedIds owned_ids{};
  // Last-wins mailbox: querying/publishing requests never calls the engine.
  bool request_pending = false;
  bool creation_pending = false;
};

class PairController {
 public:
  void request_enable(const PairKeys& keys) noexcept;
  // feeds is clamped to 1..kMaxOwnedViews. Existing two-feed callers keep the default.
  void request_independent_pose(unsigned feeds = 2) noexcept;
  void request_disable() noexcept;
  Snapshot snapshot() const noexcept;

  // Cancel only uncreated work, without a manager or engine callback. This may
  // run on a request thread: the processing guard excludes in-flight creation
  // even when its last published snapshot still looks empty. Busy/owned leave
  // every request and ownership field unchanged.
  EmptyPairCancel cancel_uncreated_request() noexcept;

  // Call ONLY from the approved update observer with a live manager. All engine
  // callbacks happen here, synchronously and without the mailbox mutex held.
  // False means a concurrent/reentrant update was refused, with no engine call.
  // Failures consume the creation request; only a new enable retries creation.
  bool process_update(ManagerToken manager, const EngineCallbacks& engine) noexcept;

  // Explicit observer-side lifetime event, not a cleanup shortcut. Call only
  // AFTER this exact manager lifetime has been destroyed by its owner. Forgets
  // its invalidated IDs and clears queued requests without calling any engine.
  bool acknowledge_manager_destroyed(ManagerToken manager) noexcept;

 private:
  enum class Command { none, enable, disable };
  struct Request {
    Command command = Command::none;
    PairKeys keys{};
    bool independent_pose = false;
    unsigned feeds = 2;
  };
  bool has_owned_ids() const noexcept;
  bool complete_owned_set() const noexcept;
  void publish(State state, Blocked blocked = Blocked::none) noexcept;
  void cleanup(const EngineCallbacks& engine) noexcept;
  bool prepare(const EngineCallbacks& engine, DescriptorStorage& descriptor, const PoseKey& key) noexcept;
  void creation_failed(const EngineCallbacks& engine, Failure failure) noexcept;

  mutable std::mutex mailbox_mutex_;
  Request mailbox_{};
  Snapshot published_{};
  std::atomic_flag processing_ = ATOMIC_FLAG_INIT;
  // Below: accessed only while processing_ is held by update/lifetime/cancel.
  ManagerToken owner_{};
  std::array<EntryId, kMaxOwnedViews> ids_{};
  PairKeys desired_keys_{};
  PairKeys owned_keys_{};
  bool desired_independent_pose_ = false;
  bool owned_independent_pose_ = false;
  unsigned desired_feeds_ = 2;
  unsigned owned_feeds_ = 0;
  bool desired_enabled_ = false;
  bool creation_pending_ = false;
  bool active_pair_ = false;
  Failure failure_ = Failure::none;
};

}  // namespace taxi_camera::engine_camera
