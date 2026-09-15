#include "../../src/camera/view_readiness_wait.hpp"

#include <cassert>

inline void test_view_readiness_wait() {
  namespace ec = taxi_camera::engine_camera;
  using taxi_camera::native_camera::ViewReadinessWait;
  ec::Snapshot pair;
  pair.state = ec::State::active;
  pair.owner = {10, 1};
  pair.owned_ids = {1001, 1002};
  ec::OwnedViewSnapshot ready;
  ready.complete = ready.ready = true;
  ready.status = ec::OwnedViewStatus::ready;
  ec::OwnedViewSnapshot pending;
  pending.complete = true;
  pending.status = ec::OwnedViewStatus::pending;
  ViewReadinessWait wait;
  assert(!wait.observe(0, pair, false, {pending, pending}));  // Startup still uses its creation guards.
  assert(!wait.observe(1, pair, true, {ready, ready}));
  assert(wait.observe(100, pair, true, {pending, ready}));
  assert(wait.episodes() == 1);
  assert(wait.observe(1099, pair, true, {ready, pending}));  // Other feed remains pending without changing ownership.
  assert(wait.observe(1100, pair, true, {pending, pending}));
  assert(wait.observe(1101, pair, true, {pending, ready}));
  assert(!wait.observe(1102, pair, true, {ready, ready}));  // Full fresh inspection resumes normally.
  assert(wait.observe(1200, pair, true, {ready, pending}));
  assert(wait.episodes() == 2);
  assert(wait.observe(60000, pair, true, {ready, pending}));  // No timeout permits deleting an unavailable view.
  assert(!wait.observe(1199, pair, true, {ready, pending}));  // Clock regression fails closed.
  wait.clear();
  assert(wait.observe(0, pair, true, {pending, pending}));  // Zero is a valid start time.
  assert(wait.observe(1000, pair, true, {pending, pending}));
  wait.clear();
  assert(wait.observe(1, pair, true, {pending, ready}));
  auto changed = pair;
  ++changed.owner.generation;
  assert(!wait.observe(2, changed, true, {pending, ready}));
  changed = pair;
  ++changed.owned_ids[1];
  assert(!wait.observe(2, changed, true, {pending, ready}));
  for (auto status : {ec::OwnedViewStatus::not_inspected, ec::OwnedViewStatus::invalid_request, ec::OwnedViewStatus::invalid_pool,
                      ec::OwnedViewStatus::invalid_pointer, ec::OwnedViewStatus::id_mismatch, ec::OwnedViewStatus::invalid_ready_byte,
                      ec::OwnedViewStatus::invalid_view_index, ec::OwnedViewStatus::pool_changed, ec::OwnedViewStatus::node_unavailable,
                      ec::OwnedViewStatus::node_mismatch, ec::OwnedViewStatus::wrong_camera_type, ec::OwnedViewStatus::invalid_fov,
                      ec::OwnedViewStatus::material_mismatch, ec::OwnedViewStatus::read_failed, ec::OwnedViewStatus::changed,
                      ec::OwnedViewStatus::read_budget_exhausted}) {
    auto failed = pending;
    failed.status = status;
    assert(!wait.observe(20, pair, true, {pending, failed}));
    assert(!wait.observe(20, pair, true, {failed, pending}));
  }
  // A pending feed plus an interrupted read was previously sent straight to
  // removal, skipping the grace period. Failed snapshots are never made ready.
  for (auto status : {ec::OwnedViewStatus::read_failed, ec::OwnedViewStatus::changed, ec::OwnedViewStatus::pool_changed,
                      ec::OwnedViewStatus::not_inspected}) {
    wait.clear();
    ec::OwnedViewSnapshot interrupted;
    interrupted.status = status;
    assert(wait.observe(30, pair, true, {pending, interrupted}));
    assert(wait.observe(31, pair, true, {interrupted, ready}));
    assert(wait.observe(1030, pair, true, {interrupted, pending}));
    assert(!wait.observe(1031, pair, true, {ready, ready}));
  }
  auto incomplete = pending;
  incomplete.complete = false;
  assert(!wait.observe(20, pair, true, {pending, incomplete}));
  for (unsigned kind = 0; kind < 8; ++kind) {
    changed = pair;
    switch (kind) {
      case 0:
        changed.request_pending = true;
        break;  // OFF is never delayed.
      case 1:
        changed.creation_pending = true;
        break;
      case 2:
        changed.failure = ec::Failure::manager_destroyed;
        break;
      case 3:
        changed.blocked = ec::Blocked::manager_mismatch;
        break;
      case 4:
        changed.state = ec::State::cleanup_pending;
        break;
      case 5:
        changed.owner = {};
        break;
      case 6:
        changed.owned_ids[0] = 0;
        break;
      case 7:
        changed.owned_ids[1] = changed.owned_ids[0];
        break;
    }
    assert(!wait.observe(20, changed, true, {pending, ready}));
  }
}
