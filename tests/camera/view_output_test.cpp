#include "../../src/camera/view_output.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
using namespace taxi_camera::native_camera;
namespace ec = taxi_camera::engine_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
constexpr ViewDimensions pane{{{774, 251}, {774, 251}, {774, 251}}};
ec::OwnedViewSnapshot ready_view() {
  ec::OwnedViewSnapshot view;
  view.complete = view.ready = true;
  view.status = ec::OwnedViewStatus::ready;
  view.mode = 2;
  view.view_index = 1;
  view.view_address = 0x1000;
  view.node_address = 0x2000;
  view.camera_address = 0x3000;
  view.resource_address = 0x4000;
  view.resource_present = true;
  view.dimensions = pane;
  view.output_dimensions = pane[0];
  view.output_slots[0] = {true, true, true};
  view.output_slots[2] = {true, true, true};
  return view;
}
void admission() {
  require(owned_view_output_ready(ready_view(), pane), "A complete pane output was refused");
  // A ready camera chain without its output is exactly the state the renderer
  // dereferences without a null check. Every missing element refuses alone.
  auto absent = ready_view();
  absent.resource_present = false;
  absent.resource_address = 0;
  require(!owned_view_output_ready(absent, pane), "A missing output resource was admitted");
  auto stale = ready_view();
  stale.resource_address = 0;
  require(!owned_view_output_ready(stale, pane), "A null resource member was admitted");
  auto previous = ready_view();
  previous.output_dimensions = {736, 251};
  require(!owned_view_output_ready(previous, pane), "A previous pane Bitmap was admitted for a different pane");
  auto primary = ready_view();
  primary.dimensions = {{{2560, 1440}, {2560, 1440}, {2560, 1440}}};
  require(!owned_view_output_ready(primary, pane), "Primary-sized view fields were admitted");
  auto partial = ready_view();
  partial.dimensions[2] = {2560, 1440};
  require(!owned_view_output_ready(partial, pane), "One overwritten size pair was admitted");
  auto mode = ready_view();
  mode.mode = 1;
  require(!owned_view_output_ready(mode, pane), "A non-mode2 entry was admitted");
  auto pending = ready_view();
  pending.ready = false;
  pending.status = ec::OwnedViewStatus::pending;
  require(!owned_view_output_ready(pending, pane), "A pending entry was admitted");
  auto incomplete = ready_view();
  incomplete.complete = false;
  require(!owned_view_output_ready(incomplete, pane), "An incomplete snapshot was admitted");
  auto changed = ready_view();
  changed.status = ec::OwnedViewStatus::changed;
  require(!owned_view_output_ready(changed, pane), "A changed snapshot was admitted");
  const ViewDimensions other{{{736, 496}, {736, 496}, {736, 496}}};
  require(!owned_view_output_ready(ready_view(), other), "A pane from another feed was admitted");
  // The retained fault state: pane output present, diffuse texture without a
  // render-target record. Pane output alone passes; admission refuses.
  auto recordless = ready_view();
  recordless.output_slots[0].render_target_record = false;
  require(
      owned_view_pane_output(recordless, pane) && !owned_view_render_target_ready(recordless) && !owned_view_output_ready(recordless, pane),
      "A diffuse texture without a render-target record was admitted");
  auto no_texture = ready_view();
  no_texture.output_slots[0] = {true, false, false};
  require(!owned_view_output_ready(no_texture, pane), "A Bitmap without a texture was admitted");
  auto depth_only_missing = ready_view();
  depth_only_missing.output_slots[2] = {};
  require(owned_view_output_ready(depth_only_missing, pane), "A missing depth-stencil observation blocked the diffuse gate");
  require(owned_view_slot_digits(ready_view()) == 0x707 && owned_view_slot_digits(recordless) == 0x307 &&
              owned_view_slot_digits(no_texture) == 0x107,
          "Slot digits did not encode Bitmap/texture/record per slot");
}
void output_wait() {
  ViewResizeWarmup warmup;
  const std::array<std::uint64_t, 4> ids{1003, 1004, 0, 0};
  require(!warmup.await_output(ids, 5), "An inactive warmup accepted an output wait");
  require(warmup.begin(ids, 10), "Warmup did not begin");
  require(!warmup.await_output(ids, 10), "An output wait was accepted before the original update passed");
  require(!warmup.await_output({1001, 1002, 0}, 11), "An output wait was accepted for a different pair");
  for (unsigned wait = 1; wait <= ViewResizeWarmup::MaximumOutputWaits; ++wait) {
    require(warmup.await_output(ids, 10 + wait), "A bounded output wait was refused early");
    require(warmup.pending(), "Waiting for the output ended the closed-gate warmup");
    require(warmup.output_waits() == wait, "Output waits were not counted");
  }
  require(!warmup.await_output(ids, 100), "The output wait was not bounded");
  require(warmup.pending(), "An exhausted wait silently completed the warmup");
  require(warmup.finish(ids, 101), "The warmup could not finish once the output was observed");
  require(!warmup.pending() && warmup.output_waits() == 0, "Finishing did not reset the output wait");
  require(warmup.begin(ids, 200), "Warmup did not restart");
  require(warmup.output_waits() == 0, "A new warmup inherited the previous output wait");
  warmup.clear();
  require(!warmup.await_output(ids, 300), "A cleared warmup accepted an output wait");
}
}  // namespace

int main() {
  try {
    admission();
    output_wait();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "view output admission test failed: %s\n", error.what());
    return 1;
  }
  std::printf("view output admission: %u checks passed\n", checks);
  return 0;
}
