#include "view_resize.hpp"

#include <windows.h>

#include <algorithm>
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

bool dimensions_valid(const ViewDimensions& dimensions) noexcept {
  return dimensions[0] == dimensions[1] && dimensions[1] == dimensions[2] && dimensions_bounded(dimensions);
}

bool writable_private(std::uint64_t address, std::size_t bytes) noexcept {
  if (!address || address > std::numeric_limits<std::uintptr_t>::max() - bytes)
    return false;
  const auto end = address + bytes;
  void* allocation = nullptr;
  while (address < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        region.Type != MEM_PRIVATE || region.Protect != PAGE_READWRITE || !region.AllocationBase)
      return false;
    if (allocation && allocation != region.AllocationBase)
      return false;
    allocation = region.AllocationBase;
    const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (begin > address || region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - begin || begin + region.RegionSize <= address)
      return false;
    address = std::min<std::uint64_t>(end, begin + region.RegionSize);
  }
  return true;
}

template <typename T>
bool read(std::uint64_t address, T& output) noexcept {
  SIZE_T count = 0;
  T local{};
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &local, sizeof(local), &count) || count != sizeof(local))
    return false;
  output = local;
  return true;
}
}  // namespace

bool plan_view_resize(const ViewDimensions& inherited,
                      unsigned feed,
                      ViewDimensions& desired,
                      const profiles::CameraPanes& panes) noexcept {
  desired = {};
  if (!dimensions_valid(inherited) || feed >= panes.size() || panes[feed][0] < 32 || panes[feed][0] > 2048 || panes[feed][1] < 32 ||
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
  // An AA/upscaling switch can copy different primary render/display sizes
  // into the three pairs. For retained mode2 output, bound every inherited
  // field and validate the requested pane independently. The exact existing
  // Bitmap dimensions below, closed gate, and repeated fields still govern
  // restoration; this path never calls the output allocator.
  const bool inherited_valid = initialize_output ? dimensions_valid(view.dimensions) : dimensions_bounded(view.dimensions);
  if (!inherited_valid || !plan_view_resize(desired, feed, planned, panes) || desired != planned)
    return fail(ViewResizeStatus::invalid_dimensions);
  if (!(view.flags[0] & 1u))
    return fail(ViewResizeStatus::gate_open);
  if (!initialize_output && (view.mode != 2 || !view.resource_present || view.output_dimensions != desired[0]))
    return fail(ViewResizeStatus::output_mismatch);
  const auto field = view.view_address + 16;
  if (!writable_private(field, sizeof(desired)) || !writable_private(view.view_address + 48, sizeof(view.flags)))
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
  SIZE_T written = 0;
  result.write_attempted = true;
  if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(field), desired.data(), sizeof(desired), &written) ||
      written != sizeof(desired))
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
