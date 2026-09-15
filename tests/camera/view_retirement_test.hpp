#pragma once
#include <cassert>
#include "../../src/camera/view_retirement.hpp"

inline void test_view_retirement() {
  namespace ec = taxi_camera::engine_camera;
  using taxi_camera::native_camera::ViewRetirement;
  using Action = ViewRetirement::Action;
  const ec::ManagerToken owner{10, 1};
  ec::OwnedViewSnapshot ready;
  ready.complete = ready.ready = true;
  ready.status = ec::OwnedViewStatus::ready;
  ready.view_address = 100;
  ready.node_address = 200;
  ready.camera_address = 300;
  ready.view_index = 1;
  ViewRetirement policy;
  assert(policy.observe(owner, 1001, 10, ready) == Action::close_gate);
  ready.flags[0] = 1;
  assert(policy.observe(owner, 1001, 11, ready) == Action::wait);
  assert(policy.observe(owner, 1001, 11, ready) == Action::wait);
  for (unsigned status = 0; status <= static_cast<unsigned>(ec::OwnedViewStatus::read_budget_exhausted); ++status) {
    if (status == static_cast<unsigned>(ec::OwnedViewStatus::ready))
      continue;
    auto invalid = ready;
    invalid.status = static_cast<ec::OwnedViewStatus>(status);
    assert(policy.observe(owner, 1001, 12, invalid) == Action::wait);
    assert(policy.observe(owner, 1001, 13, ready) == Action::wait);
    assert(policy.observe(owner, 1001, 14, ready) == Action::erase);
  }
  assert(policy.observe(owner, 1001, 12, ready) == Action::wait);  // Update regression resets proof.
  assert(policy.observe({}, 1001, 20, ready) == Action::wait);
  assert(policy.observe(owner, 0, 20, ready) == Action::wait);
  policy.forget(owner, 1001);
  assert(policy.observe(owner, 1001, 21, ready) == Action::wait);

  // Replay the crash transition through the real pair controller: after
  // captures, inspection becomes unavailable, cleanup is requested, and a new
  // enable arrives. No removal or replacement may occur while either owned
  // entry is unavailable. A pending OFF remains immediate in the controller.
  struct Engine {
    ViewRetirement retirement;
    std::array<ec::OwnedViewSnapshot, 2> views{};
    std::uint64_t update = 1;
    unsigned creates = 0, closes = 0, erases = 0;
    static bool initialize(void*, ec::DescriptorStorage& descriptor) noexcept {
      descriptor.bytes.fill(0);
      return true;
    }
    static ec::EntryId create(void* context, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
      return 1000 + ++static_cast<Engine*>(context)->creates;
    }
    static bool erase(void* context, ec::ManagerToken token, ec::EntryId id) noexcept {
      auto& e = *static_cast<Engine*>(context);
      auto& view = e.views[(id - 1001) % 2];
      const auto action = e.retirement.observe(token, id, e.update, view);
      if (action == Action::close_gate) {
        ++e.closes;
        view.flags[0] |= 1;
      }
      if (action != Action::erase)
        return false;
      ++e.erases;
      e.retirement.forget(token, id);
      return true;
    }
    ec::EngineCallbacks callbacks() { return {this, initialize, create, erase}; }
  } engine;
  ready.flags[0] = 0;
  engine.views = {ready, ready};
  ec::PairController pair;
  pair.request_independent_pose();
  pair.process_update(owner, engine.callbacks());
  assert(pair.snapshot().state == ec::State::active && engine.creates == 2);
  pair.request_disable();
  pair.process_update(owner, engine.callbacks());
  assert(engine.closes == 2 && engine.erases == 0);
  assert(pair.snapshot().state == ec::State::cleanup_pending);
  pair.process_update(owner, engine.callbacks());
  assert(engine.erases == 0);  // Same observer cannot certify a later engine update.
  engine.views[0] = {};
  engine.views[0].complete = true;
  engine.views[0].status = ec::OwnedViewStatus::pending;
  engine.views[1] = {};
  engine.views[1].status = ec::OwnedViewStatus::changed;
  pair.request_independent_pose();
  for (engine.update = 2; engine.update < 200; ++engine.update) {
    pair.process_update(owner, engine.callbacks());
    assert(engine.creates == 2 && engine.erases == 0 && engine.closes == 2);
    assert((pair.snapshot().owned_ids == std::array<ec::EntryId, 2>{1001, 1002}));
  }
  ready.flags[0] = 1;
  engine.views = {ready, ready};
  pair.process_update(owner, engine.callbacks());
  pair.process_update(owner, engine.callbacks());
  assert(engine.erases == 0);
  ++engine.update;
  pair.process_update(owner, engine.callbacks());
  assert(engine.erases == 2 && engine.creates == 4 && pair.snapshot().state == ec::State::active);
}
