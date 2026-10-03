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
// P+48 bit36. The engine's camera setup ORs it (0x1000200020 at 17642847) into
// every render-to-texture view. With it set, the per-view render driver
// (64721094..64721152) disables the AA dispatcher (pass 0x34), the post pass
// (0x3d) and pass 0x29, and the view walk requests no MainRenderTarget or
// PostAA target, so the scene draws straight into the output Bitmap. Decoded
// statically on observed_store_layout()'s image only; untested live.
inline constexpr std::uint64_t kViewDirectOutputFlag = std::uint64_t{1} << 36;
// The global frame-generation mode byte (UserCfg FrameGeneration: 0 off,
// 1 DLSSG, 2 FSRFG). A view whose effective bit36 is clear copies it into its
// render view (RV+0xdec), which then gets DLSS-G tagging and per-viewport
// slDLSSGSetOptions calls. Statically decoded on the image whose resolved
// camera layout is observed_store_layout() only.
inline constexpr std::uint32_t kObservedFrameGenerationRva = 180831257;

// What one closed-gate preparation does to bit36: keep leaves it as the engine
// set it, clear removes it (development values 4 and 5, only together with the
// AA bit), restore sets it again on a view this bridge cleared.
enum class ViewBit36 : std::uint8_t { keep, clear, restore };
struct ViewAaRequest {
  bool aa = false;
  ViewBit36 bit36 = ViewBit36::keep;
};
// The request for one preparation. A bit36 clear needs the AA bit and the
// caller's permission (the creation-time call defers it to the first pulse);
// otherwise a view this bridge cleared is owed a restore.
constexpr ViewAaRequest view_aa_request(bool aa, bool clear_bit36, bool bit36_owed, bool allow_bit36_clear = true) noexcept {
  return {aa, aa && clear_bit36 && allow_bit36_clear ? ViewBit36::clear : bit36_owed ? ViewBit36::restore : ViewBit36::keep};
}
// P+48 as the request leaves it. Only bit31 and bit36 can differ from word.
constexpr std::uint64_t requested_view_flags(std::uint64_t word, ViewAaRequest request) noexcept {
  word = request.aa ? word | kViewAaFlag : word & ~kViewAaFlag;
  if (request.bit36 == ViewBit36::clear)
    word &= ~kViewDirectOutputFlag;
  else if (request.bit36 == ViewBit36::restore)
    word |= kViewDirectOutputFlag;
  return word;
}

struct ViewAaResult {
  bool complete = false;
  bool write_attempted = false;
  // The P+48 bits the attempted write changes; set before the write, so a
  // failed write or reread still names what may have changed. 0 without one.
  std::uint64_t changed = 0;
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
// With a ViewAaRequest it also writes bit36 in the same 8-byte write, under the
// same gate, span, snapshot and reread rules: a clear refuses without the AA
// bit (bit36_without_aa), on any other image layout (bit36_unverified_image),
// while the set override alone holds bit36 (bit36_forced_by_global_override)
// and unless the frame-generation byte reads 0 (bit36_frame_generation_on).
// A restore is a hand-back and is not refused by any of these.
ViewAaResult set_owned_view_aa(const engine_camera::OwnedViewSnapshot& view,
                               discovery::ImageReader& image,
                               const CameraImageLayout& layout,
                               ViewAaRequest request) noexcept;
// bit31 only; bit36 is kept.
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
// Read-only, through the bounded image reader: the frame-generation byte at
// kObservedFrameGenerationRva, or -1 for any other resolved layout or a
// refused read. A bit36 clear requires 0.
std::int32_t read_global_frame_generation(discovery::ImageReader& image, const CameraImageLayout& layout) noexcept;

// Undo this bridge's own bit31 clear on a pooled view. Pool views keep P+48
// across entries and the captured setup only ORs/ANDs other bits, so a view
// this bridge modified would otherwise enter its next entry's setup with a
// flag state no engine path produces. Only the exact P this bridge modified,
// only while its gate bit is set, same 16-byte writable-span and reread rules.
// A set bit is left alone. No engine call, no other bit, no P+56 write.
ViewAaResult restore_view_aa_flag(std::uint64_t view_address) noexcept;
// The same hand-back for bits, a nonempty subset of bit31 and bit36, in one
// write (aa_invalid_restore_bits otherwise). Only the given bits are set.
ViewAaResult restore_view_flags(std::uint64_t view_address, std::uint64_t bits) noexcept;

// Bounded ledger of pooled views whose bit31 (cleared_aa) or bit36
// (cleared_bit36) this bridge cleared and has not yet restored. Addresses are
// comparison tokens; the caller must prove each one through a fresh pool
// snapshot before any write.
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
// The bits a view is owed back under renderer: bit31 from aa, bit36 from bit36.
inline std::uint64_t owed_view_flags(const ClearedAaLedger& aa,
                                     const ClearedAaLedger& bit36,
                                     std::uint64_t renderer,
                                     std::uint64_t view) noexcept {
  return (aa.contains(renderer, view) ? kViewAaFlag : 0) | (bit36.contains(renderer, view) ? kViewDirectOutputFlag : 0);
}

}  // namespace taxi_camera::native_camera
