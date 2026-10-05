#pragma once
#include <array>
#include <cstdint>

#include "aircraft_mounts.hpp"

// Per-view sun-shadow slice count for the camera views (view_cascades_contract.hpp).
// The simulator rewrites each view's slice count from its defaults every frame
// on its main thread, then View::PrepareScene calls the scene's cascade setup
// (World_Z vtable slot 23) with the view's render context. A slot hook lowers
// the count for Taxi Cam's own views just before that setup reads it; every
// other view, including the main view, is passed through unchanged.
// The camera views always render kCameraShadowSlices of the simulator's four:
// shadows to about 55-100 m depending on the camera's field of view. Farther
// shadows are not visible at camera-display size, and each dropped slice
// removes a shadow culling pass and its draws from every camera render.
namespace taxi_camera::native_camera::view_cascades {

struct Statistics {
  bool installed = false;
  unsigned requested = 0;  // The owned views' slice cap, kCameraShadowSlices.
  // Setup calls for an owned view, and those whose count was lowered.
  std::uint64_t hits = 0, writes = 0;
  const char* error = "";
};

inline constexpr unsigned kCameraShadowSlices = 2;
// Camera-manager observer (simulator main thread) only. The hook writes only
// into the view the simulator is preparing; these addresses decide which
// views are Taxi Cam's. Zero entries are ignored.
void publish_views(const std::array<std::uint64_t, kMaxCameraFeeds>& views) noexcept;
// Installs the slot hook once, from the observer, for the slot and setup body
// the camera contract proved (image RVAs). section_rva/section_size bound the
// main image's read-only, non-executable data section that holds the slot
// (the Xbox loader maps such pages executable/write-copy). Later calls return
// the first result.
bool install(std::uintptr_t image_base,
             std::uint32_t setup_rva,
             std::uint32_t slot_rva,
             std::uint32_t section_rva,
             std::uint32_t section_size) noexcept;
Statistics statistics() noexcept;

}  // namespace taxi_camera::native_camera::view_cascades
