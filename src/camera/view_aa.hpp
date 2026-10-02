#pragma once

#include "camera_layout.hpp"
#include "image_inventory.hpp"
#include "owned_view.hpp"

namespace taxi_camera::native_camera {

inline constexpr std::uint64_t kViewAaFlag = std::uint64_t{1} << 31;
inline constexpr std::uint32_t kViewFlagClearOverride = observed_store_layout().view_flag_clear_override;
inline constexpr std::uint32_t kViewFlagSetOverride = observed_store_layout().view_flag_set_override;
// The int32 global AA mode the engine's projection-jitter path requires to be
// at least 7 before it reads a view's effective bit31. Statically decoded on
// the image whose resolved camera layout is observed_store_layout() only.
inline constexpr std::uint32_t kObservedAaModeRva = 180831260;

struct ViewAaResult {
  bool complete = false;
  bool write_attempted = false;
  const char* error = "not_attempted";
};

// Only a freshly verified owned mode2 view with its gate closed, in the current
// validated engine observer. The caller validates the current code contract
// proving ToggleVpEffectAA's P+48 bit31 and the two global override locations.
// This sets (enabled) or clears that one per-view bit, preserving every other
// bit and P+56. The engine sees (P+48 | set) & ~clear: clearing refuses while
// the set override alone holds bit31 (aa_forced_by_global_override), setting
// refuses while the clear override holds it (aa_cleared_by_global_override).
// No engine command, global write, allocation, protection change or AA SDK call.
// Rereads both flag words and overrides; any failure leaves the gate closed.
// A write_attempted result requires a fresh full owned-view inspection before
// activation, including on success. This helper cannot establish ownership or
// freeze engine lifetime independently of its caller's observer contract.
ViewAaResult set_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                               discovery::ImageReader& image,
                               const CameraImageLayout& layout,
                               bool enabled) noexcept;

// set_owned_view_aa with enabled false: the bridge's default clear.
ViewAaResult disable_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                                   discovery::ImageReader& image,
                                   const CameraImageLayout& layout = observed_store_layout()) noexcept;

// Diagnostics only: the global AA mode at kObservedAaModeRva, read through the
// bounded image reader, or -1 for any other resolved layout or a refused read.
// Never a precondition for a write.
std::int32_t read_global_aa_mode(discovery::ImageReader& image, const CameraImageLayout& layout) noexcept;

// Undo this bridge's own bit31 clear on a pooled view. Pool views keep P+48
// across entries and the captured setup only ORs/ANDs other bits, so a view
// this bridge modified would otherwise enter its next entry's setup with a
// flag state no engine path produces. Only the exact P this bridge modified,
// only while its gate bit is set, same 16-byte writable-span and reread rules.
// A set bit is left alone. No engine call, no other bit, no P+56 write.
ViewAaResult restore_view_aa_flag(std::uint64_t view_address) noexcept;

// Bounded ledger of pooled views whose bit31 this bridge cleared and has not
// yet restored. Addresses are comparison tokens; the caller must prove each
// one through a fresh pool snapshot before any write.
class ClearedAaLedger {
 public:
  static constexpr unsigned Capacity = 4;
  bool note(std::uint64_t renderer, std::uint64_t view) noexcept {
    if (!renderer || !view)
      return false;
    if (renderer_ != renderer) {
      views_ = {};
      renderer_ = renderer;
    }
    for (auto existing : views_)
      if (existing == view)
        return true;
    for (auto& slot : views_)
      if (!slot) {
        slot = view;
        return true;
      }
    return false;
  }
  bool contains(std::uint64_t renderer, std::uint64_t view) const noexcept {
    if (renderer != renderer_ || !view)
      return false;
    for (auto existing : views_)
      if (existing == view)
        return true;
    return false;
  }
  void forget(std::uint64_t view) noexcept {
    for (auto& slot : views_)
      if (slot == view)
        slot = 0;
  }
  bool pending() const noexcept {
    for (auto existing : views_)
      if (existing)
        return true;
    return false;
  }
  std::uint64_t renderer() const noexcept { return renderer_; }
  const std::array<std::uint64_t, Capacity>& views() const noexcept { return views_; }

 private:
  std::uint64_t renderer_ = 0;
  std::array<std::uint64_t, Capacity> views_{};
};

}  // namespace taxi_camera::native_camera
