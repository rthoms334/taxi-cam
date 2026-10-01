#pragma once

#include <cstdint>

namespace taxi_camera::native_camera {

// Image-relative identities only. Member offsets, native signatures and lifetime
// rules remain a separate validated ABI contract; supplying RVAs proves none of
// those obligations. Runtime callers must pass their resolved image layout.
struct CameraImageLayout {
  std::uint32_t manager_owner_global = 0;
  std::uint32_t renderer_global = 0;
  std::uint32_t manager_vtable = 0;
  std::uint32_t aircraft_worlds_global = 0;
  std::uint32_t aircraft_facade_vtable = 0;
  std::uint32_t aircraft_facade_method = 0;
  std::uint32_t aircraft_controller_vtable = 0;
  std::uint32_t aircraft_controller_method = 0;
  std::uint32_t aircraft_selected_vtable = 0;
  std::uint32_t aircraft_selected_method = 0;
  std::uint32_t aircraft_key_component_vtable = 0;
  std::uint32_t scene_node_vtable = 0;
  std::uint32_t scene_model_vtable = 0;
  std::uint32_t activation_disable_mask = 0;
  std::uint32_t view_flag_clear_override = 0;
  std::uint32_t view_flag_set_override = 0;
  bool operator==(const CameraImageLayout&) const = default;
};

// Legacy diagnostic/test compatibility. These observations are not a resolver
// and must never admit an unknown build merely because its addresses match.
constexpr CameraImageLayout observed_store_layout() noexcept {
  return {173790440, 173790384, 133571232, 173790496, 133538936, 56053760,  133534840, 56030192,
          133503528, 55874752,  133516648, 134592040, 136368168, 130434096, 176053024, 176053040};
}

namespace camera_layout_detail {
// Shared structural ceiling, not proof of a particular image's section bounds.
// Readers and metadata inspectors retain their stricter actual-image checks.
constexpr bool image_rva(std::uint32_t rva, std::uint32_t bytes, std::uint32_t alignment = 1) noexcept {
  return rva && rva < 0x80000000u && bytes && bytes <= 0x80000000u - rva && alignment && rva % alignment == 0;
}
}  // namespace camera_layout_detail
}  // namespace taxi_camera::native_camera
