#include "../../src/camera/view_resize_recovery.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
namespace ec = taxi_camera::engine_camera;
using Policy = taxi_camera::native_camera::ViewResizeRecovery;
using Action = Policy::Action;
unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
constexpr ec::ManagerToken owner{7, 1};
constexpr Policy::Ids ids{1001, 1002};
Policy::Views ready_views() {
  Policy::Views views{};
  for (unsigned i = 0; i < views.size(); ++i) {
    auto& view = views[i];
    view.complete = view.ready = true;
    view.status = ec::OwnedViewStatus::ready;
    view.view_address = 0x1000 + i * 0x100;
    view.node_address = 0x2000 + i * 0x100;
    view.camera_address = 0x3000 + i * 0x100;
    view.view_index = static_cast<int>(i);
    view.fov = 0.62f;
    view.flags[0] = 1;
  }
  return views;
}
struct Engine {
  unsigned creates = 0, erases = 0;
  static bool initialize(void*, ec::DescriptorStorage& descriptor) noexcept {
    descriptor.bytes.fill(0);
    return true;
  }
  static ec::EntryId create(void* context, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    return 1000 + ++static_cast<Engine*>(context)->creates;
  }
  static bool erase(void* context, ec::ManagerToken, ec::EntryId) noexcept {
    ++static_cast<Engine*>(context)->erases;
    return true;
  }
  ec::EngineCallbacks callbacks() { return {this, initialize, create, erase}; }
};
}  // namespace

int main() {
  Policy policy;
  auto views = ready_views();
  require(policy.observe(owner, ids, 1, views) == Action::wait, "Idle observation never starts recovery");
  require(!policy.finish(owner, ids), "Idle finish is refused");
  for (const auto invalid_owner : {ec::ManagerToken{}, ec::ManagerToken{7, 0}, ec::ManagerToken{0, 1}}) {
    policy.clear();
    require(!policy.begin(invalid_owner, ids) && policy.failed(), "Invalid manager is refused");
  }
  for (const auto invalid_ids : {Policy::Ids{}, Policy::Ids{1001, 0}, Policy::Ids{0, 1002}, Policy::Ids{1001, 1001}}) {
    policy.clear();
    require(!policy.begin(owner, invalid_ids) && policy.failed(), "Invalid pair is refused");
  }
  policy.clear();
  require(policy.begin(owner, ids) && policy.pending(), "Begin retains valid pair");
  require(!policy.finish(owner, ids), "Cannot finish before resize permission");
  require(policy.observe(owner, ids, 10, views) == Action::wait, "First closed observation waits");
  require(policy.begin(owner, ids), "Repeated begin is idempotent");
  require(policy.observe(owner, ids, 10, views) == Action::wait, "Same update cannot establish later phase");
  require(policy.observe(owner, ids, 11, views) == Action::resize, "Two closed updates permit resize");
  require(policy.observe(owner, ids, 11, views) == Action::wait && policy.observe(owner, ids, 12, views) == Action::wait,
          "Resize permission cannot be issued twice");
  require(policy.finish(owner, ids) && !policy.pending() && !policy.failed(), "Success ends only this cycle");

  for (unsigned feed = 0; feed < 2; ++feed) {
    policy.clear();
    require(policy.begin(owner, ids), "Open-gate cycle begins");
    require(policy.observe(owner, ids, 10, views) == Action::wait, "Closed evidence established");
    views[feed].flags[0] = 0x20;
    require(policy.observe(owner, ids, 11, views) == Action::close_gates, "Either open gate requests closure");
    views[feed].flags[0] = 0x21;
    require(policy.observe(owner, ids, 11, views) == Action::wait, "Closing in same update starts new proof");
    require(policy.observe(owner, ids, 11, views) == Action::wait, "Closure is not its own warmup");
    require(policy.observe(owner, ids, 12, views) == Action::resize, "Later closed update permits resize");
    require(policy.finish(owner, ids), "Open-gate cycle succeeds");
  }
  for (const auto status : {ec::OwnedViewStatus::pending, ec::OwnedViewStatus::not_inspected, ec::OwnedViewStatus::read_failed,
                            ec::OwnedViewStatus::changed, ec::OwnedViewStatus::pool_changed}) {
    for (unsigned feed = 0; feed < 2; ++feed) {
      policy.clear();
      policy.begin(owner, ids);
      require(policy.observe(owner, ids, 20, views) == Action::wait, "Transient cycle begins closed");
      auto temporary = views;
      temporary[feed] = {};
      temporary[feed].status = status;
      temporary[feed].complete = status == ec::OwnedViewStatus::pending;
      require(policy.observe(owner, ids, 21, temporary) == Action::wait && !policy.failed(), "Transient snapshot waits");
      require(policy.observe(owner, ids, 22, views) == Action::wait, "Transient gap discards old closed evidence");
      require(policy.observe(owner, ids, 23, views) == Action::resize, "Fresh closed observations can recover");
    }
  }
  for (unsigned value = 0; value <= static_cast<unsigned>(ec::OwnedViewStatus::read_budget_exhausted); ++value) {
    const auto status = static_cast<ec::OwnedViewStatus>(value);
    if (status == ec::OwnedViewStatus::ready || status == ec::OwnedViewStatus::pending || status == ec::OwnedViewStatus::not_inspected ||
        status == ec::OwnedViewStatus::read_failed || status == ec::OwnedViewStatus::changed || status == ec::OwnedViewStatus::pool_changed)
      continue;
    policy.clear();
    policy.begin(owner, ids);
    auto refused = views;
    refused[1].status = status;
    require(policy.observe(owner, ids, 30, refused) == Action::blocked && policy.failed(), "Refused inspection blocks");
    require(!policy.begin(owner, ids) && policy.observe(owner, ids, 31, views) == Action::blocked, "Failure never automatically retries");
  }
  for (unsigned field = 0; field < 13; ++field) {
    policy.clear();
    policy.begin(owner, ids);
    auto malformed = views;
    auto& view = malformed[0];
    switch (field) {
      case 0:
        view.complete = false;
        break;
      case 1:
        view.ready = false;
        break;
      case 2:
        view.view_address = 0;
        break;
      case 3:
        view.node_address = 0;
        break;
      case 4:
        view.camera_address = 0;
        break;
      case 5:
        view.view_index = -1;
        break;
      case 6:
        view.view_index = 8;
        break;
      case 7:
        view.read_failures = 1;
        break;
      case 8:
        view.error = "refused";
        break;
      case 9:
        view.view_address |= 1;
        break;
      case 10:
        view.fov = std::numeric_limits<float>::quiet_NaN();
        break;
      case 11:
        view.view_address = malformed[1].view_address;
        break;
      case 12:
        view.view_index = malformed[1].view_index;
        break;
    }
    require(policy.observe(owner, ids, 40, malformed) == Action::blocked, "Malformed ready chain refuses resize");
  }
  for (unsigned change = 0; change < 5; ++change) {
    policy.clear();
    policy.begin(owner, ids);
    require(policy.observe(owner, ids, 50, views) == Action::wait, "Identity cycle begins");
    auto next_owner = owner;
    auto next_ids = ids;
    if (change == 0)
      ++next_owner.identity;
    if (change == 1)
      ++next_owner.generation;
    if (change == 2)
      ++next_ids[0];
    if (change == 3)
      next_ids = {ids[1], ids[0]};
    if (change == 4)
      next_owner = {};
    require(policy.observe(next_owner, next_ids, 51, views) == Action::blocked, "Owner or ordered ID change blocks");
    require(policy.observe(owner, ids, 52, views) == Action::blocked, "Returning identity cannot clear failure");
  }
  policy.clear();
  policy.begin(owner, ids);
  policy.observe(owner, ids, 100, views);
  require(policy.observe(owner, ids, 99, views) == Action::blocked, "Counter rollback blocks");
  policy.clear();
  policy.begin(owner, ids);
  require(policy.observe(owner, ids, 0, views) == Action::blocked, "Zero update is not phase evidence");
  policy.clear();
  policy.begin(owner, ids);
  policy.observe(owner, ids, 1, views);
  require(policy.observe(owner, ids, 2, views) == Action::resize, "Failure test obtains one authorization");
  policy.mark_failed();
  require(policy.pending() && policy.failed() && !policy.finish(owner, ids) && !policy.begin(owner, ids),
          "Failed native attempt retains failed transaction");
  require(policy.observe(owner, ids, 3, views) == Action::blocked, "Failed mutation cannot be repeated");
  policy.clear();
  policy.begin(owner, ids);
  auto mixed = views;
  mixed[0] = {};
  mixed[1].status = ec::OwnedViewStatus::id_mismatch;
  require(policy.observe(owner, ids, 1, mixed) == Action::blocked, "Identity refusal dominates another view's transient churn");

  // Exercise the real ID owner through repeated simulated dimension changes.
  // The callbacks count ownership changes only; no GPU/native resize is modeled.
  Engine engine;
  ec::PairController pair;
  pair.request_independent_pose();
  require(pair.process_update(owner, engine.callbacks()), "Establish the actual owned pair");
  const auto original_ids = pair.snapshot().owned_ids;
  require(original_ids == ids && engine.creates == 2, "Exactly one pair was created");
  policy.clear();
  for (std::uint64_t cycle = 0; cycle < 20; ++cycle) {
    require(policy.begin(owner, original_ids), "Retained-pair recovery begins");
    views[cycle % 2].flags[0] &= ~1ull;
    const auto update = cycle * 3 + 1;
    require(policy.observe(owner, original_ids, update, views) == Action::close_gates, "Cycle closes existing gates");
    for (auto& view : views)
      view.flags[0] |= 1;
    require(pair.process_update(owner, engine.callbacks()), "Closed pair survives first owner update");
    require(policy.observe(owner, original_ids, update + 1, views) == Action::wait, "Cycle first closed phase waits");
    require(pair.process_update(owner, engine.callbacks()), "Closed pair survives later owner update");
    require(policy.observe(owner, original_ids, update + 2, views) == Action::resize, "Cycle reaches resize phase");
    require(policy.finish(owner, original_ids), "Successful cycle finishes");
    require(
        pair.snapshot().owned_ids == original_ids && pair.snapshot().state == ec::State::active && engine.creates == 2 && !engine.erases,
        "Recovery never erases or replaces either retained ID");
  }
  std::printf("View resize recovery: PASS %u checks (CPU phase policy only)\n", checks);
}
