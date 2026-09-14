#pragma once

#include "../profiles/catalog.hpp"
#include "pfd_stamp_d3d12.hpp"
#include "scene_capture_manager.hpp"

namespace taxi_camera::scene_runtime {
struct Snapshot {
  bool initialized = false;
  bool output = false;
  bool failed = false;
  std::uint64_t frames = 0;
  std::uint64_t stamps = 0;
  std::uint64_t state_skips = 0;
  float display_exposure_ev = -8.8f;
  float ground_speed_knots = 0;
  bool ground_speed_valid = false;
  const char* message = "Start the scene test to prepare the PFD feed.";
  SceneCaptureManager::Statistics capture;
};
SceneCaptureManager& manager();
bool init_device(std::uint64_t key, ID3D12Device* device);
void destroy_device(std::uint64_t key);
bool init_queue(std::uint64_t key, ID3D12CommandQueue* queue);
bool prepare(std::uint64_t key);
// Format-26 SDR display exposure only; finite EV is clamped to [-16, +4].
// Applied to the next completed source pair without restarting the scene.
bool set_display_exposure(std::uint64_t key, float ev);
// A fresh public SimConnect sample; unavailable samples render GS --.
void set_ground_speed(std::uint64_t key, float knots, bool valid);
void reset_feed(std::uint64_t key);
void set_composition(std::uint64_t key, const profiles::Composition& layout);
void service();
Snapshot snapshot(std::uint64_t key);
// Optional half-open content bounds inside destination; nullptr fills the
// destination. Any surrounding destination border is opaque black.
bool stamp(ID3D12GraphicsCommandList*,
           const PfdGraphicsState&,
           std::uint64_t key,
           DXGI_FORMAT format,
           UINT width,
           UINT height,
           DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN,
           const D3D12_RECT* destination = nullptr,
           const D3D12_RECT* content = nullptr);
}  // namespace taxi_camera::scene_runtime
