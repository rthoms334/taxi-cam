#include "../../src/camera/retained_profile.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "../../src/camera/scene_recovery.hpp"
namespace {
namespace ec = taxi_camera::engine_camera;
namespace nc = taxi_camera::native_camera;
using Transition = nc::RetainedProfileTransition;
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Engine {
  unsigned initializations{}, creates{}, erases{};
  unsigned fail_create{};
  bool fail_initialize{}, refuse_erase{};
  static bool initialize(void* opaque, ec::DescriptorStorage& descriptor) noexcept {
    auto& self = *static_cast<Engine*>(opaque);
    ++self.initializations;
    descriptor.bytes.fill(0);
    descriptor.bytes[44] = 1;
    return !self.fail_initialize;
  }
  static ec::EntryId create(void* opaque, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    auto& self = *static_cast<Engine*>(opaque);
    ++self.creates;
    return self.creates == self.fail_create ? 0 : 1000 + self.creates;
  }
  static bool erase(void* opaque, ec::ManagerToken, ec::EntryId) noexcept {
    auto& self = *static_cast<Engine*>(opaque);
    ++self.erases;
    return !self.refuse_erase;
  }
  ec::EngineCallbacks callbacks() noexcept { return {this, initialize, create, erase}; }
};
bool same_snapshot(const ec::Snapshot& a, const ec::Snapshot& b) {
  return a.state == b.state && a.failure == b.failure && a.blocked == b.blocked && a.owner == b.owner && a.owned_ids == b.owned_ids &&
         a.request_pending == b.request_pending && a.creation_pending == b.creation_pending;
}
bool clean_empty(const ec::Snapshot& pair) {
  return same_snapshot(pair, {});
}
Transition::Views ready_views() {
  Transition::Views views{};
  for (unsigned i = 0; i < 2; ++i) {
    auto& view = views[i];
    view.complete = view.ready = view.resource_present = true;
    view.status = ec::OwnedViewStatus::ready;
    view.mode = 2;
    view.view_index = static_cast<int>(i);
    view.view_address = 100 + i;
    view.node_address = 200 + i;
    view.camera_address = 300 + i;
    view.resource_address = 400 + i;
    view.dimensions = {{{736, i ? 496 : 251}, {736, i ? 496 : 251}, {736, i ? 496 : 251}}};
    view.output_dimensions = view.dimensions[0];
    view.flags[0] = 1;
  }
  return views;
}
void cancel_empty_requests() {
  const ec::ManagerToken owner{11, 1};
  for (unsigned command = 0; command < 3; ++command) {
    Engine engine;
    ec::PairController pair;
    if (command == 0)
      pair.request_disable();  // Start queues this even before its first camera.
    else if (command == 1)
      pair.request_independent_pose();
    else
      pair.request_enable({});
    require(pair.snapshot().request_pending, "Fixture did not queue an empty request");
    Transition transition;
    transition.begin(1, pair.snapshot(), {});
    require(transition.failed(), "Pending snapshot bypassed strict transition admission");
    require(pair.cancel_uncreated_request() == ec::EmptyPairCancel::cancelled, "Uncreated request could not be cancelled");
    require(clean_empty(pair.snapshot()), "Cancellation did not publish clean disabled state");
    transition.begin(1, pair.snapshot(), {});
    require(transition.ready() && !transition.pending() && !transition.awaiting_pair() && !transition.can_resume(pair.snapshot()),
            "Cancelled empty transition could not complete safely");
    require(pair.process_update(owner, engine.callbacks()), "Empty post-cancellation update refused");
    require(clean_empty(pair.snapshot()) && engine.initializations == 0 && engine.creates == 0 && engine.erases == 0,
            "Empty cancellation invoked or left pending native work");
    pair.request_independent_pose();
    pair.process_update(owner, engine.callbacks());
    require(pair.snapshot().state == ec::State::active && engine.creates == 2 && engine.erases == 0,
            "A later explicit Start was cancelled by the prior transition");
  }
  {
    Engine engine;
    ec::PairController pair;
    pair.request_independent_pose();
    pair.process_update({}, engine.callbacks());
    require(pair.snapshot().creation_pending && pair.snapshot().blocked == ec::Blocked::invalid_manager,
            "Fixture did not leave an unmaterialized creation pending");
    require(pair.cancel_uncreated_request() == ec::EmptyPairCancel::cancelled && clean_empty(pair.snapshot()),
            "Pending uncreated request retained stale failure/creation state");
    pair.process_update(owner, engine.callbacks());
    require(engine.initializations == 0 && engine.creates == 0 && engine.erases == 0,
            "Cancelled pending creation reached an engine callback");
  }
  {
    Engine engine;
    engine.fail_initialize = true;
    ec::PairController pair;
    pair.request_independent_pose();
    pair.process_update(owner, engine.callbacks());
    require(pair.snapshot().failure == ec::Failure::initializer_failed, "Fixture did not retain initializer failure");
    require(pair.cancel_uncreated_request() == ec::EmptyPairCancel::cancelled && clean_empty(pair.snapshot()),
            "Cancelled empty failure was not reset");
    require(engine.initializations == 1 && engine.creates == 0 && engine.erases == 0, "Empty failure cancellation called the engine");
  }
  for (bool partial : {false, true}) {
    Engine engine;
    engine.fail_create = partial ? 2 : 0;
    engine.refuse_erase = partial;
    ec::PairController pair;
    pair.request_independent_pose();
    pair.process_update(owner, engine.callbacks());
    require(pair.snapshot().owned_ids[0] != 0 && (partial || pair.snapshot().owned_ids[1] != 0), "Owned fixture was empty");
    pair.request_disable();
    const auto before = pair.snapshot();
    const auto callbacks_before = std::array{engine.initializations, engine.creates, engine.erases};
    require(pair.cancel_uncreated_request() == ec::EmptyPairCancel::owned, "Owned or partial pair was cancelled as empty");
    require(same_snapshot(before, pair.snapshot()) && callbacks_before == std::array{engine.initializations, engine.creates, engine.erases},
            "Owned cancellation changed identity, queued request, failure or native calls");
    Transition transition;
    transition.begin(1, pair.snapshot(), {});
    require(transition.failed(), "Owned pending/partial pair bypassed retained validation");
  }
}
struct BlockingEngine {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{}, released{}, block_create{};
  unsigned initializations{}, creates{}, erases{};
  void pause() {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_one();
    changed.wait(lock, [&] { return released; });
  }
  static bool initialize(void* opaque, ec::DescriptorStorage& descriptor) noexcept {
    auto& self = *static_cast<BlockingEngine*>(opaque);
    if (++self.initializations == 1 && !self.block_create)
      self.pause();
    descriptor.bytes.fill(0);
    descriptor.bytes[44] = 1;
    return true;
  }
  static ec::EntryId create(void* opaque, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    auto& self = *static_cast<BlockingEngine*>(opaque);
    if (++self.creates == 1 && self.block_create)
      self.pause();
    return 1000 + self.creates;
  }
  static bool erase(void* opaque, ec::ManagerToken, ec::EntryId) noexcept {
    ++static_cast<BlockingEngine*>(opaque)->erases;
    return true;
  }
};
void cancel_during_creation() {
  const ec::ManagerToken owner{11, 1};
  for (bool during_create : {false, true}) {
    BlockingEngine engine;
    engine.block_create = during_create;
    ec::PairController pair;
    pair.request_independent_pose();
    bool processed = false;
    std::thread update([&] {
      processed = pair.process_update(owner, {&engine, BlockingEngine::initialize, BlockingEngine::create, BlockingEngine::erase});
    });
    bool entered;
    {
      std::unique_lock lock(engine.mutex);
      entered = engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; });
    }
    const auto stale = pair.snapshot();
    // Keep a last-wins request in the mailbox too: busy must not consume it.
    pair.request_independent_pose();
    const auto before = pair.snapshot();
    const auto cancelled = pair.cancel_uncreated_request();
    const auto after = pair.snapshot();
    Transition transition;
    transition.defer_pair(2);
    const bool deferred = transition.holding() && transition.pending() && transition.awaiting_pair() && !transition.ready() &&
                          !transition.failed() && !transition.can_resume(after) &&
                          transition.inspect(owner, after, ready_views()) == Transition::Decision::wait;
    {
      const std::lock_guard lock(engine.mutex);
      engine.released = true;
    }
    engine.changed.notify_one();
    update.join();
    require(entered && processed, "Blocking creation fixture did not execute");
    require(clean_empty(stale), "Fixture did not expose the stale empty in-flight snapshot");
    require(cancelled == ec::EmptyPairCancel::busy && same_snapshot(before, after),
            "In-flight callback was cancelled or its mailbox changed");
    require(deferred, "Deferred pair acknowledged readiness before creation settled");
    require(pair.snapshot().request_pending && pair.snapshot().state == ec::State::active && engine.creates == 2 && engine.erases == 0,
            "Busy cancellation lost work or modified native ownership");
    const auto callbacks = ec::EngineCallbacks{&engine, BlockingEngine::initialize, BlockingEngine::create, BlockingEngine::erase};
    pair.process_update(owner, callbacks);  // Same enable is idempotent.
    require(pair.cancel_uncreated_request() == ec::EmptyPairCancel::owned, "Settled owned pair was acknowledged as empty");
    const auto views = ready_views();
    transition.begin(2, pair.snapshot(), {views[0].dimensions, views[1].dimensions});
    require(!transition.awaiting_pair() && transition.pending() && !transition.ready(), "Fresh begin kept stale deferral/readiness");
    require(transition.inspect(owner, pair.snapshot(), views) == Transition::Decision::ready && transition.can_resume(pair.snapshot()),
            "Settled pair could not follow the ordinary retained guards");
    require(engine.initializations == 2 && engine.creates == 2 && engine.erases == 0, "Deferred transition recreated or erased cameras");
    transition.defer_pair(1);
    transition.refuse();
    require(transition.failed() && !transition.awaiting_pair() && !transition.ready(), "Refusal retained a pending deferral");
    transition.defer_pair(1);
    transition.consume();
    require(!transition.holding() && !transition.awaiting_pair() && !transition.pending(), "Consumed deferral could later acknowledge");
  }
}
void ownership_before_allocation_publication() {
  Engine engine;
  ec::PairController pair;
  const ec::ManagerToken owner{11, 1};
  Transition transition;
  Transition::AllocationEvidence published;
  pair.request_independent_pose();
  require(pair.process_update(owner, engine.callbacks()), "Publication-gap fixture failed to create");
  const auto created = pair.snapshot();
  require(created.state == ec::State::active && pair.cancel_uncreated_request() == ec::EmptyPairCancel::owned,
          "Publication-gap fixture did not finish controller processing");
  const auto views = ready_views();
  const Transition::Dimensions dimensions{views[0].dimensions, views[1].dimensions};
  // The controller is no longer busy, but the observer has not reached its
  // later publication. A transition must not freeze these stale zero sizes.
  transition.begin_published(2, created, published);
  require(transition.awaiting_pair() && transition.pending() && !transition.ready() && !transition.failed(),
          "New ownership captured unpublished allocation dimensions");
  require(transition.inspect(owner, created, views) == Transition::Decision::wait && !transition.can_resume(created),
          "Allocation publication gap admitted native transition work");
  published = {created.owner, created.owned_ids, dimensions};
  transition.begin_published(2, created, published);
  require(!transition.awaiting_pair() && transition.pending() && !transition.ready(),
          "Matching allocation evidence skipped ordinary retained validation");
  require(transition.inspect(owner, created, views) == Transition::Decision::ready && transition.can_resume(created),
          "Freshly published owned allocation could not complete transition");
  for (unsigned changed = 0; changed < 3; ++changed) {
    auto stale = published;
    if (changed == 0)
      ++stale.owner.identity;
    else if (changed == 1)
      ++stale.owner.generation;
    else
      ++stale.ids[1];
    transition.begin_published(1, created, stale);
    require(transition.awaiting_pair() && !transition.ready() && !transition.can_resume(created),
            "Another pair or manager lifetime supplied allocation expectations");
  }
  auto partial = created;
  partial.owned_ids[1] = 0;
  transition.begin_published(1, partial, {});
  require(transition.failed() && !transition.awaiting_pair(), "Missing evidence weakened a partial-pair refusal");
  transition.begin_published(0, created, {});
  require(!transition.holding() && !transition.ready(), "Missing evidence accepted an invalid profile");
  transition.begin_published(1, {}, published);
  require(transition.ready() && !transition.can_resume({}), "Old evidence prevented a clean empty transition");
  require(engine.initializations == 2 && engine.creates == 2 && engine.erases == 0 && same_snapshot(created, pair.snapshot()),
          "Allocation evidence deferral changed native work or ownership");
}
void run() {
  cancel_empty_requests();
  cancel_during_creation();
  ownership_before_allocation_publication();
  Engine engine;
  ec::PairController pair;
  const ec::ManagerToken owner{11, 1};
  const ec::EngineCallbacks callbacks{&engine, Engine::initialize, Engine::create, Engine::erase};
  pair.request_independent_pose();
  pair.process_update(owner, callbacks);
  const auto original = pair.snapshot();
  auto views = ready_views();
  const Transition::Dimensions dimensions{views[0].dimensions, views[1].dimensions};
  require(original.state == ec::State::active, "Fixture pair was not created");
  Transition transition;
  for (const auto profile : {1u, 2u, 1u, 1u}) {
    transition.begin(profile, pair.snapshot(), dimensions);
    require(transition.pending() && !transition.ready(), "New request reused a stale ready acknowledgement");
    views[0].flags[0] = 0;
    require(transition.inspect(owner, pair.snapshot(), views) == Transition::Decision::close, "Open gate was admitted");
    views[0].flags[0] = 1;
    require(transition.inspect(owner, pair.snapshot(), views) == Transition::Decision::ready, "Closed retained pair was refused");
    require(transition.can_resume(pair.snapshot()), "Valid retained pair could not resume");
    pair.process_update(owner, callbacks);
    require(pair.snapshot().owned_ids == original.owned_ids && engine.creates == 2 && engine.erases == 0,
            "Aircraft transition removed or recreated native cameras");
  }
  const auto expect_refused = [&](auto mutate) {
    auto invalid = ready_views();
    mutate(invalid);
    transition.begin(2, original, dimensions);
    require(transition.inspect(owner, original, invalid) == Transition::Decision::refused, "Invalid output contract was admitted");
    require(!transition.can_resume(original), "Refused transition could resume");
  };
  expect_refused([](auto& v) { v[0].mode = 1; });
  expect_refused([](auto& v) { v[0].dimensions[0][0] = 774; });
  expect_refused([](auto& v) { v[0].output_dimensions[0] = 774; });
  expect_refused([](auto& v) { v[0].resource_address = 0; });
  expect_refused([](auto& v) { v[1].resource_address = v[0].resource_address; });
  expect_refused([](auto& v) { v[1].view_index = v[0].view_index; });
  expect_refused([](auto& v) { v[1].view_address = v[0].view_address; });
  transition.begin(2, original, dimensions);
  require(transition.inspect({11, 2}, original, ready_views()) == Transition::Decision::refused, "Changed manager generation was accepted");
  transition.begin(2, original, dimensions);
  auto changed = original;
  changed.owned_ids[1]++;
  require(transition.inspect(owner, changed, ready_views()) == Transition::Decision::refused, "Replaced camera ID was accepted");
  transition.begin(2, original, dimensions);
  views = ready_views();
  views[0].complete = false;
  require(transition.inspect(owner, original, views) == Transition::Decision::wait && !transition.can_resume(original),
          "Missing inspection authorized camera work");
  require(Transition::session_changed(original, 4, 5), "Observer failed to notice session change before bridge");
  require(!Transition::session_changed(original, 4, 4) && !Transition::session_changed({}, 4, 5),
          "Unchanged/no-pair session required transition");
  require(nc::temporary_pose_unavailable("aircraft_session_changed"), "Flight-load pose reset requested permanent cleanup");
  require(!nc::temporary_pose_unavailable("invalid_geometry") && !nc::temporary_pose_unavailable("unknown_error"),
          "Permanent pose failures lost their guards");
  // In-place resize for another aircraft's panes: once rebased, the views must
  // carry the new dimensions while their new output may still be pending;
  // resized() requires the output again.
  transition.begin(2, original, dimensions);
  require(transition.inspect(owner, original, ready_views()) == Transition::Decision::ready, "Fixture pair was not validated");
  auto resized = ready_views();
  for (auto& view : resized)
    if (view.complete)
      view.dimensions = {{{736, 268}, {736, 268}, {736, 268}}};
  require(transition.inspect(owner, original, resized) == Transition::Decision::refused, "Changed dimensions were accepted unrebased");
  transition.begin(2, original, dimensions);
  transition.rebase({resized[0].dimensions, resized[1].dimensions});
  require(transition.inspect(owner, original, resized) == Transition::Decision::ready, "A rebased pair awaiting output was refused");
  require(transition.inspect(owner, original, ready_views()) == Transition::Decision::refused, "Old dimensions passed after a rebase");
  transition.begin(2, original, dimensions);
  transition.rebase({resized[0].dimensions, resized[1].dimensions});
  transition.resized();
  require(transition.inspect(owner, original, resized) == Transition::Decision::refused, "A resized pair passed without its new output");
  for (auto& view : resized)
    view.output_dimensions = view.dimensions[0];
  transition.begin(2, original, dimensions);
  transition.rebase({resized[0].dimensions, resized[1].dimensions});
  transition.resized();
  require(transition.inspect(owner, original, resized) == Transition::Decision::ready, "A resized pair with its new output was refused");
  transition.begin(1, {}, {});
  require(transition.ready() && !transition.can_resume({}), "Empty startup pretended to retain an existing pair");
  transition.begin(2, original, dimensions);
  transition.inspect(owner, original, ready_views());
  transition.consume();
  require(!transition.holding() && !transition.can_resume(original), "Consumed completion authorized another resume");
}
}  // namespace
int main() {
  try {
    run();
    std::puts("Retained aircraft transition policy PASS: same IDs, no erase/recreate, identity/dimension guards");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
