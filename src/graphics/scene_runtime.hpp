#pragma once

#include "../profiles/catalog.hpp"
#include "pfd_stamp_d3d12.hpp"
#include "scene_capture_manager.hpp"

namespace taxi_camera::scene_runtime {
// Control-thread demand for prepared queue-injected display copies. Formats
// describe OUR output encoding; they are not inferred application RTV evidence.
struct QueuePatchConfig {
  std::uint64_t generation = 0;
  std::uint32_t profile = 0;
  unsigned camera_mask = 0, calibration_mask = 0;
  std::array<DXGI_FORMAT, MaxDisplaySides> formats{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
  // Camera sides still inside their minimum PLEASE WAIT time. The runtime also
  // shows the page, on profiles that have one, while the image is missing or stale.
  unsigned waiting_mask = 0;
  bool operator==(const QueuePatchConfig&) const = default;
};
struct QueuePatch {
  ID3D12Resource* buffer = nullptr;  // Borrowed, retained for process lifetime.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  D3D12_RECT destination{}, content{};
  bool calibration = false;
};
struct QueuePatchSnapshot {
  std::uint64_t generation = 0;
  std::uint32_t profile = 0;
  unsigned ready_mask = 0;
  std::array<QueuePatch, MaxDisplaySides> sides{};
};
// Metadata only; allocation/recording/submission happens in service(). An empty
// config disables snapshots immediately without freeing replayable buffers.
bool configure_queue_patches(std::uint64_t device_key, const QueuePatchConfig&);
// Call BEFORE the manager's submission lock. No COM, allocation, or blocking
// synchronization; false/empty on contention, mismatched generation, or cold
// output. Every actual consumer must join the SAME manager timeline, and the
// bridge must revalidate its target/config generation before queue admission.
bool try_snapshot_queue_patches(std::uint64_t device_key, std::uint64_t expected_generation, QueuePatchSnapshot&) noexcept;
// Profile configuration changes only future private patch recordings.
bool set_patch_profile(std::uint64_t device_key, std::uint32_t profile);
// Exact, positively observed native RT-exit boundary only. Never infer a
// barrier model from an RTV bind, draw, OM switch or Close. No app draw/state
// setters; the original application barrier remains the caller's responsibility.
// A validated cold request reserves bounded metadata and returns false without
// GPU work. service() builds that exact patch for subsequent copy opportunities.
// waiting marks a side inside its minimum PLEASE WAIT time; the retained
// waiting page is also used when the camera image is missing or stale.
bool copy_patch(ID3D12GraphicsCommandList*,
                std::uint64_t device_key,
                ID3D12Resource* target,
                const D3D12_RESOURCE_DESC& target_desc,
                DXGI_FORMAT view_format,
                const D3D12_RECT& destination,
                const D3D12_RECT& content,
                ID3D12GraphicsCommandList7* enhanced = nullptr,
                bool waiting = false);
struct Snapshot {
  std::uint64_t session_generation = 0;
  bool session_active = false;
  bool gpu_timing_enabled = false;
  GpuTimingStatistics composition_gpu, output_copy_gpu, patch_gpu;
  bool initialized = false;
  bool output = false;
  bool failed = false;
  std::uint64_t frames = 0;
  std::uint64_t completed_frames = 0;
  std::uint64_t stamps = 0;
  std::uint64_t state_skips = 0;
  // Close/barrier-time PFD writes skipped because the runtime lock was held
  // (by the bridge worker's compose/prepare) past the simulator thread's budget.
  std::uint64_t contended_writes = 0;
  std::uint64_t stale_frames = 0;
  // Completed captures accepted per feed, compared with activations per feed.
  std::array<std::uint64_t, 3> accepted_frames{};
  // Submitted renders of the retained PLEASE WAIT page.
  std::uint64_t waiting_pages = 0;
  std::uint32_t patch_requests = 0;
  std::uint64_t patch_draws = 0;
  float display_exposure_ev = -8.8f;
  // The simulator's main-view exposure for the camera images (0: Taxi Cam's
  // exposure), and whether the latest composition used its tone curve.
  float tone_exposure = 0;
  bool tone_active = false;
  // Camera texels to the aircraft display's codes (0: not used).
  float screen_scale = 0;
  float ground_speed_knots = 0;
  bool ground_speed_valid = false;
  // False skips the GS overlay entirely (PMDG 777: no readout, no black box).
  bool ground_speed_overlay = true;
  const char* message = "Start the scene test to prepare the PFD feed.";
  SceneCaptureManager::Statistics capture;
};
SceneCaptureManager& manager();
void set_gpu_timing_enabled(bool enabled);
// Camera-image age that brings the PLEASE WAIT page back while a side stays on.
// 0, the default, disables the age rule; the bridge sets WaitingPageStaleMs.
// Validation hosts that hold one composed image for a long time leave it off.
void set_waiting_stale_ms(std::uint64_t ms);
bool init_device(std::uint64_t key, ID3D12Device* device);
void destroy_device(std::uint64_t key);
bool init_queue(std::uint64_t key, ID3D12CommandQueue* queue);
bool prepare(std::uint64_t key);
// Format-26 SDR display exposure only; finite EV is clamped to [-16, +4].
// Applied to the next completed source pair without restarting the scene.
bool set_display_exposure(std::uint64_t key, float ev);
// The simulator's tone mapping for the next compositions: its main-view
// exposure (0 returns to Taxi Cam's) and, when new, its 64^3 tone-curve table
// (copied here; R10G10B10A2 texels, x fastest).
bool set_tone_curve(std::uint64_t key, float exposure, const std::uint32_t* table);
// Scene light for the aircraft display, which then shines it into the main
// view (CameraCompositorD3D12::set_screen_scale); 0 turns it off.
bool set_screen_scale(std::uint64_t key, float scale);
// A fresh public SimConnect sample; unavailable samples render GS --.
void set_ground_speed(std::uint64_t key, float knots, bool valid);
// Skip the GS overlay entirely (no glyphs, no black panel). Font fixtures
// re-enable GS via set_ground_speed and keep the default panel geometry.
void hide_ground_speed(std::uint64_t key);
void reset_feed(std::uint64_t key);
// Full aircraft/airport boundary. Stop new camera/calibration publications and
// submissions immediately; replayable buffers and in-flight leases drain on
// their existing fences. Resume requires this exact returned generation.
std::uint64_t reset_session(std::uint64_t key);
bool resume_session(std::uint64_t key, std::uint64_t generation);
void set_composition(std::uint64_t key, const profiles::Composition& layout);
void set_reference_guides(std::uint64_t key, bool enabled);
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
                            const D3D12_RECT* content = nullptr,
                            bool waiting = false);
}  // namespace taxi_camera::scene_runtime
