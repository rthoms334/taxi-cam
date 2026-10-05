#include "view_resize.hpp"
#include "local_memory.hpp"

#include <windows.h>

#include <cstring>
#include <limits>

namespace taxi_camera::native_camera {
namespace {
constexpr std::int32_t MaximumDimension = 16384;
constexpr std::int32_t MinimumDimension = 32;
static_assert(sizeof(ViewDimensions) == 24);

bool dimensions_bounded(const ViewDimensions& dimensions) noexcept {
  for (const auto& pair : dimensions)
    for (const auto value : pair)
      if (value < MinimumDimension || value > MaximumDimension)
        return false;
  return true;
}

// One exact, timed RPM of a whole field, published only when complete. RPM
// itself refuses inaccessible and guard pages, so each reread after a write or
// native call needs no earlier proof.
template <typename T>
bool read(std::uint64_t address, T& output) noexcept {
  static_assert(sizeof(T) <= kLocalBytesReadLimit);
  return read_local_bytes(address, &output, sizeof(output));
}
}  // namespace

bool plan_view_resize(const ViewDimensions& inherited,
                      unsigned feed,
                      ViewDimensions& desired,
                      const profiles::CameraPanes& panes) noexcept {
  desired = {};
  if (!dimensions_bounded(inherited) || feed >= panes.size() || panes[feed][0] < 32 || panes[feed][0] > 2048 || panes[feed][1] < 32 ||
      panes[feed][1] > 2048)
    return false;
  desired.fill(panes[feed]);
  return true;
}

static ViewResizeResult change_view_dimensions(const engine_camera::OwnedViewSnapshot& view,
                                               unsigned feed,
                                               const ViewDimensions& desired,
                                               const ViewResizeCallbacks& callbacks,
                                               const profiles::CameraPanes& panes,
                                               bool initialize_output) noexcept {
  ViewResizeResult result;
  const auto fail = [&](ViewResizeStatus status) {
    result.status = status;
    return result;
  };
  if (!view.complete || !view.ready || view.status != engine_camera::OwnedViewStatus::ready || view.read_failures ||
      (view.error && *view.error) || !view.view_address || (view.view_address & 7u) ||
      view.view_address > std::numeric_limits<std::uintptr_t>::max() - 160 || !callbacks.refresh_projection ||
      (initialize_output && !callbacks.ensure_output))
    return fail(ViewResizeStatus::invalid_snapshot);
  ViewDimensions planned{};
  // The first original manager update, as well as a later AA/upscaling switch,
  // can copy different primary render/display sizes into these pairs. Bound
  // every inherited field independently; the requested three pairs must still
  // equal the exact profile pane. Closed gates and full field rereads govern
  // both paths; restoration additionally proves the existing Bitmap below and
  // never calls the output allocator.
  if (!dimensions_bounded(view.dimensions) || !plan_view_resize(desired, feed, planned, panes) || desired != planned)
    return fail(ViewResizeStatus::invalid_dimensions);
  if (!(view.flags[0] & 1u))
    return fail(ViewResizeStatus::gate_open);
  if (!initialize_output && (view.mode != 2 || !view.resource_present || view.output_dimensions != desired[0]))
    return fail(ViewResizeStatus::output_mismatch);
  const auto field = view.view_address + 16;
  // One fresh proof of P+16..P+63: the dimensions, the gap before the flags
  // and both flag words, in one private allocation and exactly PAGE_READWRITE.
  // Resident pages take allocation and working-set metadata, not a region
  // scan of the heap; a page outside the working set takes the timed walk.
  static_assert(sizeof(desired) == 24 && sizeof(view.flags) == 16);
  if (!writable_private_span(field, 48))
    return fail(ViewResizeStatus::invalid_mapping);
  ViewDimensions current{};
  std::array<std::uint64_t, 2> flags{};
  if (!read(field, current) || !read(view.view_address + 48, flags))
    return fail(ViewResizeStatus::read_failed);
  if (current != view.dimensions || flags != view.flags)
    return fail(ViewResizeStatus::changed);
  if (current == desired) {
    result.status = ViewResizeStatus::unchanged;
    result.complete = true;
    return result;
  }
  // The write takes its own fresh proof of the 24 bytes immediately before it.
  result.write_attempted = true;
  if (!write_local_private(field, desired.data(), sizeof(desired)))
    return fail(ViewResizeStatus::write_failed);
  if (!read(field, current) || !read(view.view_address + 48, flags))
    return fail(ViewResizeStatus::read_failed);
  if (current != desired || flags != view.flags)
    return fail(ViewResizeStatus::changed);
  if (!callbacks.refresh_projection(callbacks.context, view.view_address))
    return fail(ViewResizeStatus::refresh_failed);
  if (!read(field, current) || !read(view.view_address + 48, flags))
    return fail(ViewResizeStatus::read_failed);
  if (current != desired || flags != view.flags)
    return fail(ViewResizeStatus::changed);
  if (initialize_output && callbacks.ensure_output(callbacks.context, view.view_address) != view.view_address + 144)
    return fail(ViewResizeStatus::allocation_failed);
  if (!read(field, current) || !read(view.view_address + 48, flags))
    return fail(ViewResizeStatus::read_failed);
  if (current != desired || flags != view.flags)
    return fail(ViewResizeStatus::changed);
  result.status = initialize_output ? ViewResizeStatus::resized : ViewResizeStatus::dimensions_restored;
  result.complete = true;
  return result;
}

ViewResizeResult resize_owned_view(const engine_camera::OwnedViewSnapshot& view,
                                   unsigned feed,
                                   const ViewDimensions& desired,
                                   const ViewResizeCallbacks& callbacks,
                                   const profiles::CameraPanes& panes) noexcept {
  return change_view_dimensions(view, feed, desired, callbacks, panes, true);
}

ViewResizeResult restore_owned_view_dimensions(const engine_camera::OwnedViewSnapshot& view,
                                               unsigned feed,
                                               const ViewDimensions& desired,
                                               const ViewResizeCallbacks& callbacks,
                                               const profiles::CameraPanes& panes) noexcept {
  return change_view_dimensions(view, feed, desired, callbacks, panes, false);
}

const char* view_resize_status_name(ViewResizeStatus status) noexcept {
  switch (status) {
    case ViewResizeStatus::not_attempted:
      return "not attempted";
    case ViewResizeStatus::resized:
      return "resized";
    case ViewResizeStatus::dimensions_restored:
      return "dimension fields restored; existing output retained";
    case ViewResizeStatus::output_mismatch:
      return "existing output does not match the requested pane; no reallocation attempted";
    case ViewResizeStatus::unchanged:
      return "already at requested resolution";
    case ViewResizeStatus::invalid_snapshot:
      return "owned-view proof is incomplete";
    case ViewResizeStatus::invalid_dimensions:
      return "unsupported inherited dimensions";
    case ViewResizeStatus::gate_open:
      return "render gate is not closed";
    case ViewResizeStatus::invalid_mapping:
      return "view fields are not committed writable private memory";
    case ViewResizeStatus::changed:
      return "view fields changed during resize";
    case ViewResizeStatus::read_failed:
      return "view field reread failed";
    case ViewResizeStatus::write_failed:
      return "dimension write failed";
    case ViewResizeStatus::refresh_failed:
      return "projection refresh refused";
    case ViewResizeStatus::allocation_failed:
      return "output routine returned an unexpected handle address";
  }
  return "unknown";
}
}  // namespace taxi_camera::native_camera
