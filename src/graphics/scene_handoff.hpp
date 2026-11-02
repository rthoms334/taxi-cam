#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "../shared/bounded_lock.hpp"

namespace taxi_camera {
// Camera feeds of one owned set (native_camera::kMaxCameraFeeds, asserted by
// the capture manager): nose, tail or left wing, right wing, door camera.
inline constexpr std::size_t SceneFeeds = 4;

struct SceneManagerIdentity {
  // Opaque internal identity and generation. Never dereference or serialize.
  std::uint64_t identity = 0;
  std::uint64_t generation = 0;
  bool operator==(const SceneManagerIdentity&) const = default;
};

struct SceneResourceIdentity {
  std::uint64_t device_epoch = 0;
  std::uint64_t resource_id = 0;  // Existing graphics registry target ID.
  std::uint64_t generation = 0;   // Unique registration within this handoff.
  bool operator==(const SceneResourceIdentity&) const = default;
};

struct SceneCaptureTicket {
  std::uint64_t scene_epoch = 0;
  std::uint64_t lifecycle_epoch = 0;
  std::uint64_t sequence = 0;
};

struct SceneCopyMatch {
  bool matched = false;
  std::uint32_t feed = 0;  // Entry order:0 nose,1 left/tail,2 right; no inference from pixels.
  std::uint64_t scene_epoch = 0;
  std::uint64_t capture_sequence = 0;
  SceneManagerIdentity manager;
  std::uint64_t entry_id = 0;
  SceneResourceIdentity resource;
};

struct SceneCopyObservation {
  SceneCopyMatch source;
  SceneCopyMatch destination;
};

// CPU identity matching only: no graphics/engine calls, pointer dereference,
// AddRef, resource-state inference or GPU completion/lifetime guarantee.
//
// Lifecycle callbacks must remain registered even when activity discovery is
// paused. Register device/resource creation before any capture may consume it;
// unregister at the corresponding destruction callback. Every such event
// invalidates open inspection tickets, including untracked resources. A completed
// publication survives unrelated resource churn; touching either published handle
// (even on another device) or a device lifecycle event invalidates publication.
// A repeated native address receives a fresh registration generation.
//
// The authorized scene-start path calls begin_scene. Stop and any failed or
// retired engine-owner phase call stop_scene immediately, before native erase.
// For each engine snapshot: begin_capture BEFORE inspecting the complete live
// owned pair, then publish with its same manager generation, ordered nonzero
// entry IDs and freshly read opaque resource members. A lifecycle event in
// between, ambiguous resource address, mixed devices or owner change refuses.
// No unregistered pointer can become a resource merely by being published.
// Opening a refresh or rejecting its ticket for unrelated resource churn keeps
// the previous complete publication. A latest valid ticket with malformed or
// unresolvable pair data revokes it; actual owner/resource/scene invalidation
// remains immediate. A failed engine inspection must still call stop_scene.
//
// observe_copy accepts ONLY handles from actual native GPU callbacks. Its
// match identifies a source or destination; the adapter separately proves the
// copy extent/state and recording/submission/retirement contract. A returned
// match is a value snapshot, not a lease. is_current checks it again before
// consuming a completed owned GPU capture. A completed pair stays current while
// a new observer capture is in progress. Old captures may remain current across
// later successful observations of the SAME pair and resource generations.
//
// No method calls external code under the mutex. Never hold this class's lock
// while invoking the engine or graphics API. The four-device and per-device
// 16384-resource caps bound metadata; cap exhaustion simply refuses matching.
class SceneHandoff {
 public:
  static constexpr std::size_t maximum_devices = 4;
  static constexpr std::size_t maximum_resources_per_device = 16384;

  std::uint64_t register_device(std::uint64_t device_key);
  void unregister_device(std::uint64_t device_key);
  bool register_resource(std::uint64_t device_key, std::uint64_t resource_handle, std::uint64_t registry_resource_id);
  void unregister_resource(std::uint64_t device_key, std::uint64_t resource_handle);
  // Same as unregister_resource, waiting at most budget_us for the lock. False
  // means nothing was changed; the caller must retry from a thread it owns.
  bool try_unregister_resource(std::uint64_t device_key, std::uint64_t resource_handle, std::uint32_t budget_us) noexcept;

  std::uint64_t begin_scene();
  void stop_scene();
  SceneCaptureTicket begin_capture();
  bool publish(SceneCaptureTicket ticket,
               SceneManagerIdentity manager,
               const std::array<std::uint64_t, SceneFeeds>& entry_ids,
               const std::array<std::uint64_t, SceneFeeds>& resource_handles);
  SceneCopyObservation observe_copy(std::uint64_t device_key, std::uint64_t source_handle, std::uint64_t destination_handle) const;
  // Atomic rejection-only hint for high-frequency native RT boundaries. True
  // is never ownership proof: callers must still perform authoritative matching.
  bool may_match_resource(std::uint64_t resource_handle) const noexcept;
  // Metadata-only hint: last completely inspected pair, including while the next
  // inspection is open. Returns -1 for unrelated handles. NEVER capture/admission
  // proof; resource/device/scene changes clear it, and authoritative matching is
  // still required before recording GPU work.
  int observed_feed(std::uint64_t resource_handle) const noexcept;
  struct Diagnostics {
    bool published = false;
    std::uint64_t publications = 0, unrelated_events = 0, target_invalidations = 0, ticket_invalidations = 0;
    std::array<std::uint64_t, SceneFeeds> resource_ids{};  // Registry IDs only; no pointer or capture authority.
  };
  Diagnostics diagnostics() const;
  bool is_current(const SceneCopyMatch& match) const;

 private:
  struct Resource {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
  };
  struct Device {
    std::uint64_t key = 0;
    std::uint64_t epoch = 0;
    std::unordered_map<std::uint64_t, Resource> resources;
  };
  struct Publication {
    bool valid = false;
    std::uint64_t scene_epoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t device_key = 0;
    SceneManagerIdentity manager;
    std::array<std::uint64_t, SceneFeeds> entry_ids{};
    std::array<std::uint64_t, SceneFeeds> handles{};
    std::array<SceneResourceIdentity, SceneFeeds> resources{};
  };
  bool advance(std::uint64_t& counter);
  bool lifecycle_event(std::uint64_t changed_handle = 0);
  Device* device(std::uint64_t key);
  const Device* device(std::uint64_t key) const;
  SceneCopyMatch match(std::uint64_t device_key, std::uint64_t handle) const;

  mutable std::mutex mutex_;
  std::array<Device, maximum_devices> devices_{};
  std::uint64_t device_counter_ = 0;
  std::uint64_t resource_counter_ = 0;
  std::uint64_t lifecycle_epoch_ = 1;
  std::uint64_t scene_epoch_ = 0;
  std::uint64_t capture_sequence_ = 0;
  bool exhausted_ = false;
  bool active_ = false;
  bool owner_bound_ = false;
  SceneManagerIdentity owner_;
  std::array<std::uint64_t, SceneFeeds> owned_ids_{};
  Publication publication_;
  // Rejection-only hint for unrelated high-frequency copy callbacks. Every
  // possible match still takes the mutex and verifies the full publication.
  std::array<std::atomic<std::uint64_t>, SceneFeeds> candidate_handles_{};
  std::array<std::atomic<std::uint64_t>, SceneFeeds> observed_handles_{};
  Diagnostics diagnostics_;
};

// Process-lifetime storage, shared by the native observer and add-on callbacks.
// Creation of this object performs no engine/graphics operation.
SceneHandoff& scene_handoff();

}  // namespace taxi_camera
