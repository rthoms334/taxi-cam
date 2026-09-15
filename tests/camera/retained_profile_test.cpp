#include "../../src/camera/retained_profile.hpp"
#include <cstdio>
#include <stdexcept>
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
  unsigned creates{}, erases{};
  static bool initialize(void*, ec::DescriptorStorage& descriptor) noexcept {
    descriptor.bytes.fill(0);
    descriptor.bytes[44] = 1;
    return true;
  }
  static ec::EntryId create(void* opaque, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    return 1000 + ++static_cast<Engine*>(opaque)->creates;
  }
  static bool erase(void* opaque, ec::ManagerToken, ec::EntryId) noexcept {
    ++static_cast<Engine*>(opaque)->erases;
    return true;
  }
};
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
void run() {
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