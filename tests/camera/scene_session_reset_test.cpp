#include "../../src/camera/scene_session_reset.hpp"
#include "../../src/camera/view_retirement.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
namespace ec = taxi_camera::engine_camera;
namespace nc = taxi_camera::native_camera;
unsigned checks{};
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

// The real PairController owns all IDs and the real ViewRetirement policy
// authorizes removal. These fake native endpoints provide deterministic engine
// observations; they do not treat a public flight event as native destruction.
struct Fixture {
  ec::PairController pair;
  nc::SceneSessionReset reset;
  nc::ViewRetirement retirement;
  ec::ManagerToken owner{0x1000, 1};
  std::array<ec::EntryId, 2> live{};
  std::array<bool, 2> closed{};
  bool public_ready{true};
  bool identity_ready{true};
  bool entry_absence_proven{true};
  bool reset_during_create{};
  bool reset_saw_busy{};
  bool reset_saw_empty{};
  unsigned creates{}, erases{}, closes{};
  std::uint64_t update{};

  static bool initialize(void* opaque, ec::DescriptorStorage& descriptor) noexcept {
    auto& f = *static_cast<Fixture*>(opaque);
    if (!f.public_ready || f.reset.holding())
      return false;
    descriptor.bytes.fill(0);
    descriptor.bytes[44] = 1;
    return true;
  }
  static ec::EntryId create(void* opaque, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    auto& f = *static_cast<Fixture*>(opaque);
    if (!f.public_ready || f.reset.holding())
      return 0;
    const auto id = ec::EntryId{1000} + ++f.creates;
    for (unsigned i = 0; i < f.live.size(); ++i)
      if (!f.live[i]) {
        f.live[i] = id;
        f.closed[i] = false;
        break;
      }
    if (f.reset_during_create) {
      f.reset_during_create = false;
      f.public_ready = false;
      f.reset.begin(3);
      const auto cancelled = f.pair.cancel_uncreated_request();
      const auto snapshot = f.pair.snapshot();
      f.reset_saw_busy = cancelled == ec::EmptyPairCancel::busy;
      f.reset_saw_empty = !snapshot.owned_ids[0] && !snapshot.owned_ids[1];
      f.reset.observe_empty(cancelled, snapshot);
    }
    // A reset during the native allocation cannot discard its returned ID.
    return id;
  }
  static bool erase(void* opaque, ec::ManagerToken owner, ec::EntryId id) noexcept {
    auto& f = *static_cast<Fixture*>(opaque);
    if (!f.identity_ready || owner != f.owner)
      return false;
    for (unsigned i = 0; i < f.live.size(); ++i) {
      if (f.live[i] != id)
        continue;
      ec::OwnedViewSnapshot view;
      view.complete = view.ready = true;
      view.status = ec::OwnedViewStatus::ready;
      view.view_address = 2000 + i;
      view.node_address = 3000 + i;
      view.camera_address = 4000 + i;
      view.view_index = static_cast<int>(i);
      view.flags[0] = f.closed[i] ? 1 : 0;
      const auto action = f.retirement.observe(owner, id, f.update, view);
      if (action == nc::ViewRetirement::Action::close_gate) {
        f.closed[i] = true;
        ++f.closes;
      } else if (action == nc::ViewRetirement::Action::erase && f.public_ready) {
        ++f.erases;
        f.live[i] = 0;
        if (f.entry_absence_proven) {
          f.retirement.forget(owner, id);
          return true;
        }
      }
      return false;
    }
    return f.entry_absence_proven;
  }
  ec::EngineCallbacks callbacks() noexcept { return {this, initialize, create, erase}; }
  void start() {
    pair.request_independent_pose();
    pair.process_update(owner, callbacks());
  }
  void begin_reset(unsigned profile = 3) {
    reset.begin(profile);
    const auto result = pair.cancel_uncreated_request();
    reset.observe_empty(result, pair.snapshot());
  }
  void service(ec::ManagerToken current) {
    ++update;
    const auto cancelled = pair.cancel_uncreated_request();
    if (cancelled == ec::EmptyPairCancel::owned) {
      pair.request_disable();
      pair.process_update(current, callbacks());
    }
    const auto drained = pair.cancel_uncreated_request();
    reset.observe_empty(drained, pair.snapshot());
  }
  void service() { service(owner); }
  bool ready() const { return reset.ready(public_ready, pair.snapshot()); }
};

void invalid_world_before_public_epoch() {
  Fixture f;
  f.start();
  const auto old = f.pair.snapshot().owned_ids;
  require(f.creates == 2 && old[0] && old[1], "Initial pair missing");
  // Invalid WORLD closes admission immediately, before a FlightLoaded epoch.
  f.public_ready = false;
  f.begin_reset();
  require(!f.ready() && f.reset.holding(), "Invalid WORLD did not latch reset");
  f.service();
  require(f.closes == 2 && f.closed[0] && f.closed[1], "Loading did not close both guarded views");
  for (unsigned i = 0; i < 20; ++i)
    f.service();
  require(f.creates == 2 && f.erases == 0 && f.pair.snapshot().owned_ids == old, "Loading recreated or prematurely destroyed the old pair");
  require(!f.reset.consume(false, f.pair.snapshot()), "Loading consumed reset");

  // Fresh telemetry alone cannot authorize removal through an unknown manager.
  f.public_ready = true;
  f.identity_ready = false;
  f.service();
  require(!f.ready() && f.pair.snapshot().owned_ids == old && f.erases == 0, "Public readiness discarded unresolved native ownership");
  f.identity_ready = true;
  f.service({f.owner.identity, f.owner.generation + 1});
  require(!f.ready() && f.pair.snapshot().owned_ids == old && f.erases == 0, "Reused manager address authorized prior lifetime removal");
  f.service();
  require(f.erases == 2 && f.ready() && nc::SceneSessionReset::empty(f.pair.snapshot()),
          "Guarded retirement failed to acknowledge exact absence");

  // A subsequent load revokes an already-ready receipt until fresh readiness.
  f.public_ready = false;
  require(!f.ready() && !f.reset.consume(false, f.pair.snapshot()), "Load begin reused stale reset readiness");
  f.public_ready = true;
  require(f.reset.consume(true, f.pair.snapshot()), "Completed fresh reset could not be consumed");
  require(!f.reset.consume(true, f.pair.snapshot()), "Reset consumed twice");
  f.start();
  require(f.creates == 4 && f.erases == 2 && f.pair.snapshot().owned_ids != old, "New session did not create one fresh pair");
  for (unsigned i = 0; i < 20; ++i)
    f.pair.process_update(f.owner, f.callbacks());
  require(f.creates == 4, "Completed reset caused repeated pair recreation");
}

void creation_in_flight_and_partial_retirement() {
  Fixture f;
  f.reset_during_create = true;
  f.start();
  require(f.reset_saw_busy && f.reset_saw_empty, "Fixture did not reset during unpublished native creation");
  require(f.creates == 1 && !f.ready() && f.pair.snapshot().owned_ids[0] && !f.pair.snapshot().owned_ids[1],
          "Reset lost the in-flight ID or created the second camera while loading");
  for (unsigned i = 0; i < 3; ++i)
    f.service();
  require(f.erases == 0 && !f.ready(), "Partial pair erased during loading");
  f.public_ready = true;
  f.entry_absence_proven = false;
  f.service();
  require(f.erases == 1 && !f.ready() && f.pair.snapshot().owned_ids[0], "Native erase return replaced confirmed absence proof");
  f.service();
  require(!f.ready(), "Missing absence evidence was forgotten");
  f.entry_absence_proven = true;
  f.service();
  require(f.ready() && f.creates == 1, "Confirmed absent partial pair did not complete reset");
  require(f.reset.consume(true, f.pair.snapshot()), "Partial reset completion refused");
  f.start();
  require(f.creates == 3 && f.pair.snapshot().state == ec::State::active, "Partial reset did not permit one new complete pair");
}

void empty_and_pending_requests() {
  for (unsigned command = 0; command < 3; ++command) {
    Fixture f;
    f.public_ready = false;
    if (command == 0)
      f.pair.request_independent_pose();
    else if (command == 1)
      f.pair.request_disable();
    f.begin_reset(4);
    require(nc::SceneSessionReset::empty(f.pair.snapshot()) && f.reset.profile() == 4 && !f.ready(),
            "Empty queued startup did not cancel without native work");
    f.service();
    require(!f.creates && !f.erases, "Empty reset invoked engine lifecycle");
    f.public_ready = true;
    require(f.ready(), "Empty reset required an unavailable native observer");
  }
  nc::SceneSessionReset reset;
  reset.begin(2);
  ec::Snapshot snapshot;
  reset.observe_empty(ec::EmptyPairCancel::busy, snapshot);
  require(!reset.ready(true, snapshot), "Unpublished empty snapshot bypassed cancellation exclusion");
  reset.observe_empty(ec::EmptyPairCancel::cancelled, snapshot);
  require(reset.ready(true, snapshot), "Cancelled empty snapshot was not accepted");
  snapshot.owner = {42, 7};
  require(!reset.ready(true, snapshot), "Unexpected owner reused an old empty acknowledgement");
  snapshot = {};
  snapshot.creation_pending = true;
  require(!reset.ready(true, snapshot), "Pending creation reused an old empty acknowledgement");
  snapshot = {};
  snapshot.request_pending = true;
  require(!reset.ready(true, snapshot), "Pending request reused an old empty acknowledgement");
}

void exact_epoch_admission() {
  using Reset = nc::SceneSessionReset;
  require(!Reset::observer_can_retire(false, false), "Uninstalled hook was treated as an observer that can retire views");
  require(!Reset::observer_can_retire(false, true), "Enabled-without-hook was treated as an observer that can retire views");
  require(!Reset::observer_can_retire(true, false),
          "Hooked-but-never-enabled observer was treated as able to retire; empty reset would wait forever");
  require(Reset::observer_can_retire(true, true), "Installed enabled observer lost retirement permission");
  {
    Reset reset;
    reset.begin(2);
    ec::Snapshot snapshot;
    require(!reset.ready(true, snapshot), "Reset without empty acknowledgement was already ready");
    if (!Reset::observer_can_retire(true, false))
      reset.observe_empty(ec::EmptyPairCancel::cancelled, snapshot);
    require(reset.ready(true, snapshot), "Refused first request that installed the hook deadlocked the next empty session reset");
  }
  require(Reset::work_allowed(false, 0, 0, true), "Loaded-flight Connect with fresh epoch-zero telemetry was refused");
  require(!Reset::work_allowed(false, 0, 0, false), "Epoch-zero readiness was inferred without fresh telemetry");
  require(Reset::work_allowed(false, 8, 8, true), "Fresh authorized flight refused camera work");
  require(!Reset::work_allowed(false, 8, 8, false), "Unready flight admitted camera work");
  require(!Reset::work_allowed(true, 8, 8, true), "Latched reset admitted camera work");
  // The provider can receive begin/completion/fresh samples while an observer
  // is inspecting a prior flight, before the bridge has submitted its reset.
  require(!Reset::work_allowed(false, 8, 9, true), "New public readiness authorized old-flight creation/resize/activation");
  require(!Reset::work_allowed(false, 9, 8, true), "Old public snapshot authorized new-flight camera work");
  require(Reset::work_allowed(false, 9, 9, true), "A newly authorized flight could not resume");

  Fixture f;
  f.start();
  f.begin_reset();
  f.service();
  f.service();
  f.service();
  require(f.erases == 2 && f.ready(), "Work admission epoch blocked guarded retirement of old ownership");
}
}  // namespace

int main() {
  try {
    invalid_world_before_public_epoch();
    creation_in_flight_and_partial_retirement();
    empty_and_pending_requests();
    exact_epoch_admission();
    std::printf("scene_session_reset: PASS %u checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "scene_session_reset: FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
