#pragma once

#include "../profiles/catalog.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {

using ViewDimensions = std::array<std::array<std::int32_t, 2>, 3>;

// Native outputs fit inside the profile camera border: A380 panes are736x251 and736x496.
// The four-row separator belongs only to the compositor. Inherited render,
// display and output pairs are independently bounded and may differ under
// upscaling; the requested three pairs must equal the exact profile pane.
inline constexpr auto kCameraPaneDimensions = profiles::A380.camera_panes;
bool plan_view_resize(const ViewDimensions& inherited,
                      unsigned feed,
                      ViewDimensions& desired,
                      const profiles::CameraPanes& panes = profiles::A380.camera_panes) noexcept;

// The original manager returns before initializing its primary-size cache when
// it has no entries. Newly created views must survive one ORIGINAL update with
// closed gates before resizing, or that update can overwrite their dimensions.
// This gate uses the observer iteration counter, not a timer or guessed cache
// write. The caller must keep both gates closed while pending() is true.
//
// The warmup also stays pending until the requested output of both views has
// been observed after the resize. The output routine is called once; later
// updates only reread the chain. A bounded number of updates without the
// output is a startup failure for the normal guarded retirement path.
class ViewResizeWarmup {
 public:
  static constexpr unsigned MaximumOutputWaits = 12;
  using Ids = std::array<std::uint64_t, profiles::CameraViews>;
  static unsigned feed_count(const Ids& ids) noexcept {
    unsigned n = 0;
    for (auto id : ids)
      if (id)
        ++n;
    return n;
  }
  static bool distinct_nonzero(const Ids& ids) noexcept {
    const unsigned n = feed_count(ids);
    if (n < 2)
      return false;
    for (unsigned i = 0; i < ids.size(); ++i) {
      if (!ids[i])
        continue;
      for (unsigned j = i + 1; j < ids.size(); ++j)
        if (ids[j] && ids[j] == ids[i])
          return false;
    }
    // Trailing zeros only: once a zero appears, later slots must stay zero.
    bool seen_zero = false;
    for (auto id : ids) {
      if (!id)
        seen_zero = true;
      else if (seen_zero)
        return false;
    }
    return true;
  }
  bool begin(const Ids& ids, std::uint64_t iteration) noexcept {
    if (!distinct_nonzero(ids) || !iteration)
      return false;
    ids_ = ids;
    iteration_ = iteration;
    pending_ = true;
    output_waits_ = 0;
    return true;
  }
  bool may_resize(const Ids& ids, std::uint64_t iteration) const noexcept {
    return pending_ && ids == ids_ && iteration > iteration_;
  }
  bool finish(const Ids& ids, std::uint64_t iteration) noexcept {
    if (!may_resize(ids, iteration))
      return false;
    pending_ = false;
    output_waits_ = 0;
    return true;
  }
  // Dimensions were applied but the output chain is not yet observed. Returns
  // false once the bounded wait is exhausted; the warmup then stays pending
  // only for the caller's failure handling, never for another gate decision.
  bool await_output(const Ids& ids, std::uint64_t iteration) noexcept {
    if (!may_resize(ids, iteration))
      return false;
    return ++output_waits_ <= MaximumOutputWaits;
  }
  unsigned output_waits() const noexcept { return output_waits_; }
  bool pending() const noexcept { return pending_; }
  void clear() noexcept { *this = {}; }

 private:
  Ids ids_{};
  std::uint64_t iteration_ = 0;
  unsigned output_waits_ = 0;
  bool pending_ = false;
};

enum class ViewResizeStatus {
  not_attempted,
  resized,
  dimensions_restored,
  output_mismatch,
  unchanged,
  invalid_snapshot,
  invalid_dimensions,
  gate_open,
  invalid_mapping,
  changed,
  read_failed,
  write_failed,
  refresh_failed,
  allocation_failed,
};

struct ViewResizeResult {
  ViewResizeStatus status = ViewResizeStatus::not_attempted;
  // A failed/partial write is also a mutation attempt. Caller must retain its
  // owned ID and keep the gate closed for normal engine cleanup on ANY failure.
  bool write_attempted = false;
  bool complete = false;
};

struct ViewResizeCallbacks {
  void* context = nullptr;
  bool (*refresh_projection)(void*, std::uint64_t view_address) noexcept = nullptr;
  std::uint64_t (*ensure_output)(void*, std::uint64_t view_address) noexcept = nullptr;
};

// Caller supplies a freshly validated owned view in the same engine update
// phase, with its render gate closed. Only the exact 24 bytes P+16..39 can be
// written; current dimensions and P+48/+56 must still match the snapshot.
// Requires committed PAGE_READWRITE MEM_PRIVATE memory and makes no protection
// change. The write is immediately reread before calling projection refresh,
// then the captured output routine. Its return must equal P+144.
//
// This does not acquire object ownership or prove completed GPU allocation.
// Callers must revalidate the complete owned-view chain afterwards. Native
// callbacks must have their build fingerprints/ABI and engine phase verified.
// No calls occur for refused inputs; failed callbacks never reopen the gate.
ViewResizeResult resize_owned_view(const engine_camera::OwnedViewSnapshot& view,
                                   unsigned feed,
                                   const ViewDimensions& desired,
                                   const ViewResizeCallbacks& callbacks,
                                   const profiles::CameraPanes& panes = profiles::A380.camera_panes) noexcept;

// Established mode2 views can have their three size fields overwritten when
// the primary view changes. Restore only when the fully reread existing Bitmap
// already has the desired dimensions. No output allocation/replacement occurs;
// ensure_output is never called. Caller keeps both gates closed, retains the
// entry IDs, and revalidates the complete chain/resource after this operation.
ViewResizeResult restore_owned_view_dimensions(const engine_camera::OwnedViewSnapshot& view,
                                               unsigned feed,
                                               const ViewDimensions& desired,
                                               const ViewResizeCallbacks& callbacks,
                                               const profiles::CameraPanes& panes = profiles::A380.camera_panes) noexcept;

const char* view_resize_status_name(ViewResizeStatus status) noexcept;

}  // namespace taxi_camera::native_camera
