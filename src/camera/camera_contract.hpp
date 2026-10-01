#pragma once

#include "camera_layout.hpp"
#include "image_inventory.hpp"

#include <cstdint>
#include <string>

namespace taxi_camera::native_camera {

struct CameraFunctions {
  std::uint32_t initialize_descriptor = 0;
  std::uint32_t activate_entry = 0;
  std::uint32_t create_entry = 0;
  std::uint32_t erase_entry = 0;
  std::uint32_t set_position = 0;
  std::uint32_t set_up = 0;
  std::uint32_t set_target = 0;
  std::uint32_t set_fov = 0;
  std::uint32_t update_view = 0;
  std::uint32_t refresh_output = 0;
  std::uint32_t manager_update = 0;
  // Read-only release ABI anchors; never invoked by the bridge.
  std::uint32_t release_view = 0;
  std::uint32_t drain_views = 0;
  // Scene-node parenting (camera_parent_contract.hpp). Zero when this image's
  // shapes did not resolve; the cameras then keep world placement.
  std::uint32_t attach_child = 0;
  std::uint32_t detach_node = 0;
  bool operator==(const CameraFunctions&) const = default;
};

// Published only after the entire instruction/data contract resolves. The
// runtime owns this value for the loaded image's lifetime and never updates it
// after installing its observer. Discovery does not create or own engine objects.
struct CameraContract {
  CameraFunctions functions;
  CameraImageLayout layout;
};

struct CameraContractResolution {
  bool valid = false;
  std::string error;
  CameraContract contract;
  std::uint64_t scanned_bytes = 0;
  std::uint32_t matched_ranges = 0;
  // Why the parenting extension did not resolve (empty when it did).
  std::string parent_error;
};

// Read-only discovery and verification. Resolves moved code/data while keeping
// instruction semantics, field offsets and cross-reference relationships pinned
// to the reviewed camera ABI. No version/whole-image hash whitelist is used.
// Ambiguous, incomplete or changed contracts publish no callable addresses.
// The release contract, then the additive parenting extension
// (camera_parent_contract.hpp): attach_child/detach_node are set only when a
// second complete resolution declaring their bodies agrees with every other
// binding; otherwise they stay zero and parent_error says why.
CameraContractResolution resolve_camera_contract(discovery::ImageReader& reader,
                                                 const discovery::Inventory& image,
                                                 std::uint64_t loaded_image_base);
// The release contract alone, never with parenting.
CameraContractResolution resolve_release_camera_contract(discovery::ImageReader& reader,
                                                         const discovery::Inventory& image,
                                                         std::uint64_t loaded_image_base);

// Bounded runtime edge check only; full method bodies are already part of the
// startup contract. The caller freshly captures/rechecks the renderer vptr.
bool verify_renderer_release_methods(discovery::ImageReader& reader,
                                     const discovery::Inventory& image,
                                     std::uint64_t loaded_image_base,
                                     std::uint64_t vtable,
                                     const CameraFunctions& functions);

}  // namespace taxi_camera::native_camera
