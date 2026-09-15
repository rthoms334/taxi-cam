#pragma once

#include "../profiles/catalog.hpp"
#include "pfd_stamp_d3d12.hpp"
#include "scene_capture_manager.hpp"

namespace taxi_camera::scene_runtime {
// Profile configuration changes only future private patch recordings.
bool set_patch_profile(std::uint64_t device_key, std::uint32_t profile);
// Exact, positively observed native RT-exit boundary only. Never infer a
// barrier model from an RTV bind, draw, OM switch or Close. No app draw/state
// setters; the original application barrier remains the caller's responsibility.
// A validated cold request reserves bounded metadata and returns false without
// GPU work. service() builds that exact patch for subsequent copy opportunities.
bool copy_patch(ID3D12GraphicsCommandList*,
                std::uint64_t device_key,
                ID3D12Resource* target,
                const D3D12_RESOURCE_DESC& target_desc,
                DXGI_FORMAT view_format,
                const D3D12_RECT& destination,
                const D3D12_RECT& content,
                ID3D12GraphicsCommandList7* enhanced = nullptr);
struct Snapshot {
  bool initialized = false;
  bool output = false;
  bool failed = false;
  std::uint64_t frames = 0;
  std::uint64_t stamps = 0;
  std::uint64_t state_skips = 0;
  std::uint32_t patch_requests = 0;
  std::uint64_t patch_draws = 0;
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
// Terminal DIRECT-list entry only: immediately before native Close, after all
// application commands. Caller supplies the guarded retained RTV; successful
// recording intentionally leaves our state bound and never replays app roots.
// Optional half-open content bounds leave an opaque black destination border.
bool stamp_at_recording_end(ID3D12GraphicsCommandList*,
                            const PfdGraphicsState&,
                            std::uint64_t key,
                            DXGI_FORMAT format,
                            UINT width,
                            UINT height,
                            DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN,
                            const D3D12_RECT* destination = nullptr,
                            const D3D12_RECT* content = nullptr);
}  // namespace taxi_camera::scene_runtime
