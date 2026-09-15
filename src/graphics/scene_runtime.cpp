#include "scene_runtime.hpp"
#include "../hooks/render_boundary_observer.hpp"
#include "../bridge/native_hooks.hpp"
#include "scene_frame_output.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace taxi_camera::scene_runtime {
namespace {
constexpr std::array<DXGI_FORMAT, 4> Formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                             DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
constexpr std::array<DXGI_FORMAT, 5> DepthFormats{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
                                                  DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
struct Device {
  std::uint64_t key = 0;
  ID3D12Device* native = nullptr;
  SceneFrameOutput output;
  std::array<PfdStampD3D12, Formats.size() * DepthFormats.size()> stamps;
  std::array<bool, Formats.size() * DepthFormats.size()> stamp_ready{};
  std::array<SceneCaptureManager::Frame, 2> pending;
  std::array<SceneCopyMatch, 2> committed;
  std::array<SceneCaptureManager::FrameOrder, 2> newest;
  Snapshot status;
  std::uint32_t patch_profile = 1;
};
struct Runtime {
  std::mutex mutex;
  std::array<Device, SceneCaptureManager::MaximumDevices> devices;
};
Runtime& runtime() {
  static auto* const instance = new Runtime;
  return *instance;
}
Device* find(std::uint64_t key) {
  for (auto& item : runtime().devices)
    if (item.key == key && key)
      return &item;
  return nullptr;
}
DXGI_FORMAT scene_format(ID3D12Resource* resource) {
  const auto format = resource->GetDesc().Format;
  // Capture preserves the bytes. A typeless 8-bit color output is sampled as
  // UNORM for this camera probe; this is not a calibrated color-space claim.
  if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  return format;
}
void discard(Device& item) {
  for (auto& frame : item.pending) {
    if (frame.token)
      manager().discard_frame(frame.token);
    frame = {};
  }
}
bool current_output(const Device& item) {
  return item.status.output && scene_handoff().is_current(item.committed[0]) && scene_handoff().is_current(item.committed[1]);
}
}  // namespace

SceneCaptureManager& manager() {
  static auto* const instance = new SceneCaptureManager(scene_handoff());
  return *instance;
}

bool init_device(std::uint64_t key, ID3D12Device* device) {
  const std::lock_guard lock(runtime().mutex);
  if (!key || !device || find(key))
    return false;
  for (auto& item : runtime().devices) {
    if (item.key)
      continue;
    if (!manager().register_device(key, device))
      return false;
    item.key = key;
    item.native = device;  // Manager retains this device until process exit.
    return true;
  }
  return false;
}
void destroy_device(std::uint64_t key) {
  const std::lock_guard lock(runtime().mutex);
  if (auto* item = find(key)) {
    discard(*item);
    item->status.failed = true;
    item->status.output = false;
    item->status.message = "Device destroyed; GPU resources retained for recorded command lists.";
  }
  manager().destroy_device(key);
}
bool init_queue(std::uint64_t key, ID3D12CommandQueue* queue) {
  if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return true;
  auto callbacks = manager().callbacks();
  callbacks.before = [](void*, ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) noexcept {
    const standalone::OwnedWork guard;
    return manager().before_submission(q, n, lists);
  };
  callbacks.after = [](void*, ID3D12CommandQueue* q, std::uint64_t receipt) noexcept {
    const standalone::OwnedWork guard;
    manager().after_submission(q, receipt);
  };
  callbacks.refused = [](void*, ID3D12CommandQueue* q, engine_hook::queue_submit::Refusal reason) noexcept {
    const standalone::OwnedWork guard;
    manager().submission_refused(q, reason);
  };
  const auto result = engine_hook::queue_submit::register_queue(queue, callbacks);
  const bool ready = result.protection_restored && (result.status == engine_hook::queue_submit::Status::registered ||
                                                    result.status == engine_hook::queue_submit::Status::already_registered);
  if (!ready) {
    manager().submission_refused(queue, engine_hook::queue_submit::Refusal::invalid_batch);
    const std::lock_guard lock(runtime().mutex);
    if (auto* item = find(key)) {
      item->status.failed = true;
      item->status.message = "Native queue observation failed; scene capture disabled.";
    }
  }
  return ready;
}
bool prepare(std::uint64_t key) {
  const standalone::OwnedWork owned_work_guard;
  const std::lock_guard lock(runtime().mutex);
  auto* item = find(key);
  if (!item || item->status.failed)
    return false;
  if (item->status.initialized)
    return true;
  if (!item->output.initialize(item->native)) {
    item->status.failed = true;
    item->status.message = item->output.error();
    return false;
  }
  if (!item->output.set_patch_profile(item->patch_profile))
    return false;
  for (std::size_t i = 0; i < Formats.size(); ++i) {
    for (std::size_t depth = 0; depth < DepthFormats.size(); ++depth) {
      const auto slot = i * DepthFormats.size() + depth;
      item->stamp_ready[slot] = SUCCEEDED(item->stamps[slot].initialize(item->native, Formats[i], DepthFormats[depth]));
      // A device may reject an optional DSV format. Keep the established no-DSV
      // path available and refuse only that exact unsupported combination.
      if (!item->stamp_ready[slot] && depth == 0) {
        item->status.failed = true;
        item->status.message = "PFD shader/pipeline initialization failed.";
        return false;
      }
    }
  }
  item->status.initialized = true;
  item->status.message = "Waiting for two completed camera snapshots; select the PFD and enable its camera feed.";
  return true;
}
bool set_patch_profile(std::uint64_t key, std::uint32_t profile) {
  if (!profiles::find(profile))
    return false;
  const std::lock_guard lock(runtime().mutex);
  if (auto* item = find(key)) {
    if (item->status.initialized && !item->output.set_patch_profile(profile))
      return false;
    if (item->patch_profile != profile)
      item->status.output = false;
    item->patch_profile = profile;
    return true;
  }
  return false;
}

bool copy_patch(ID3D12GraphicsCommandList* list,
                std::uint64_t key,
                ID3D12Resource* target,
                const D3D12_RESOURCE_DESC& desc,
                DXGI_FORMAT format,
                const D3D12_RECT& destination,
                const D3D12_RECT& content,
                ID3D12GraphicsCommandList7* enhanced) {
  const std::lock_guard lock(runtime().mutex);
  auto* item = find(key);
  if (!list || !target || !item || !current_output(*item) || item->status.failed ||
      (enhanced && static_cast<ID3D12GraphicsCommandList*>(enhanced) != list) || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || !desc.MipLevels || !desc.Width || desc.Width > 16384 || !desc.Height ||
      desc.Height > 16384 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || destination.left < 0 || destination.top < 0 ||
      destination.right <= destination.left || destination.bottom <= destination.top ||
      static_cast<UINT64>(destination.right) > desc.Width || static_cast<UINT>(destination.bottom) > desc.Height ||
      content.left < destination.left || content.top < destination.top || content.right > destination.right ||
      content.bottom > destination.bottom || content.right <= content.left || content.bottom <= content.top)
    return false;
  const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  const bool compatible = rgba ? desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                     desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                               : bgra && (desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                          desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
  if (!compatible)
    return false;
  const UINT width = static_cast<UINT>(destination.right - destination.left),
             height = static_cast<UINT>(destination.bottom - destination.top);
  const D3D12_RECT local{content.left - destination.left, content.top - destination.top, content.right - destination.left,
                         content.bottom - destination.top};
  // Only a fully validated native copy opportunity may request private work.
  // A cold request records no app commands; terminal delivery remains available
  // until a later composition publishes this exact typed patch.
  if (!item->output.request_patch(format, width, height, local)) {
    ++item->status.state_skips;
    return false;
  }
  const auto patch = item->output.patch(format, width, height, local);
  if (!patch.buffer || patch.buffer == target || patch.footprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
      patch.footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT || !manager().register_consumer_recording(list)) {
    ++item->status.state_skips;
    return false;
  }
  const engine_hook::render_boundary::ScopedBypass bypass;
  const auto transition = [&](bool begin) {
    if (enhanced) {
      D3D12_TEXTURE_BARRIER barrier{};
      barrier.SyncBefore = begin ? D3D12_BARRIER_SYNC_RENDER_TARGET : D3D12_BARRIER_SYNC_COPY;
      barrier.SyncAfter = begin ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_RENDER_TARGET;
      barrier.AccessBefore = begin ? D3D12_BARRIER_ACCESS_RENDER_TARGET : D3D12_BARRIER_ACCESS_COPY_DEST;
      barrier.AccessAfter = begin ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_RENDER_TARGET;
      barrier.LayoutBefore = begin ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : D3D12_BARRIER_LAYOUT_COPY_DEST;
      barrier.LayoutAfter = begin ? D3D12_BARRIER_LAYOUT_COPY_DEST : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
      barrier.pResource = target;
      barrier.Subresources = {0, 1, 0, 1, 0, 1};
      const D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1, {.pTextureBarriers = &barrier}};
      enhanced->Barrier(1, &group);
    } else {
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {target, 0, begin ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST,
                            begin ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET};
      list->ResourceBarrier(1, &barrier);
    }
  };
  transition(true);
  D3D12_TEXTURE_COPY_LOCATION source{}, dest{};
  source.pResource = patch.buffer;
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = patch.footprint;
  dest.pResource = target;
  dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const D3D12_BOX box{0, 0, 0, width, height, 1};
  list->CopyTextureRegion(&dest, static_cast<UINT>(destination.left), static_cast<UINT>(destination.top), 0, &source, &box);
  transition(false);
  ++item->status.stamps;
  return true;
}
void set_composition(std::uint64_t key, const profiles::Composition& layout) {
  const std::lock_guard lock(runtime().mutex);
  if (auto* item = find(key))
    item->output.set_composition(layout);
}
void reset_feed(std::uint64_t key) {
  const std::lock_guard lock(runtime().mutex);
  if (auto* item = find(key); item && !item->status.failed) {
    discard(*item);
    item->status.output = false;
    item->committed = {};
    item->status.message = "Waiting for fresh completed camera images from this scene test.";
  }
}
bool set_display_exposure(std::uint64_t key, float ev) {
  if (!std::isfinite(ev))
    return false;
  const std::lock_guard lock(runtime().mutex);
  auto* item = find(key);
  if (!item || item->status.failed)
    return false;
  item->status.display_exposure_ev = std::clamp(ev, -16.0f, 4.0f);
  return true;
}
void set_ground_speed(std::uint64_t key, float knots, bool valid) {
  const std::lock_guard lock(runtime().mutex);
  if (auto* item = find(key)) {
    item->status.ground_speed_valid = valid && std::isfinite(knots) && knots >= 0 && knots <= 999;
    item->status.ground_speed_knots = item->status.ground_speed_valid ? knots : 0;
  }
}
void service() {
  const standalone::OwnedWork owned_work_guard;
  const std::lock_guard lock(runtime().mutex);
  std::array<SceneCaptureManager::Frame, SceneCaptureManager::MaximumPackets> incoming{};
  const auto count = manager().poll_completed_frames(incoming.data(), incoming.size());
  for (std::size_t i = 0; i < count; ++i) {
    auto& frame = incoming[i];
    auto* item = find(frame.device_key);
    if (!item || !item->status.initialized || item->status.failed || frame.match.feed >= 2 || !scene_handoff().is_current(frame.match)) {
      manager().discard_frame(frame.token);
      continue;
    }
    auto& newest = item->newest[frame.match.feed];
    if (!frame.order.newer_than(newest)) {
      manager().discard_frame(frame.token);
      ++item->status.stale_frames;
      continue;
    }
    // Keep this high-water mark after composition/reset. Device timeline values
    // never restart, and an older recording may retire in a later service call.
    newest = frame.order;
    auto& previous = item->pending[frame.match.feed];
    if (previous.token)
      manager().discard_frame(previous.token);
    previous = frame;
  }
  for (auto& item : runtime().devices) {
    if (item.status.output && !current_output(item)) {
      item.status.output = false;
      item.status.message = "Scene output identity changed; waiting for fresh completed camera images.";
    }
    if (!item.key || !item.status.initialized || item.status.failed || !item.output.idle())
      continue;
    for (auto& frame : item.pending) {
      if (frame.token && !scene_handoff().is_current(frame.match)) {
        manager().discard_frame(frame.token);
        frame = {};
      }
    }
    auto& first = item.pending[0];
    auto& second = item.pending[1];
    if (!first.token || !second.token)
      continue;
    if (first.match.scene_epoch != second.match.scene_epoch || first.match.manager != second.match.manager) {
      discard(item);
      continue;
    }
    if (!item.output.set_display_exposure(item.status.display_exposure_ev) ||
        !item.output.set_ground_speed(item.status.ground_speed_knots, item.status.ground_speed_valid) ||
        !item.output.prepare(first.resource, scene_format(first.resource), second.resource, scene_format(second.resource))) {
      item.status.failed = true;
      item.status.message = item.output.error();
      // A failed recording may already reference these resources. Retain leases.
      continue;
    }
    const auto submission = manager().begin_private_submission(item.key, item.output.queue());
    if (!submission.receipt) {
      if (item.output.discard_prepared())
        discard(item);
      item.status.failed = true;
      item.status.message = "Camera output queue ordering failed.";
      continue;
    }
    const bool submitted = item.output.submit();
    const bool ordered = manager().end_private_submission(submission.receipt);
    if (!submitted || !ordered) {
      item.status.failed = true;
      item.status.message = "Camera composition submission failed; snapshot leases retained.";
      continue;
    }
    item.committed = {first.match, second.match};
    manager().finish_consumption(first.token, submission.fence, submission.value);
    manager().finish_consumption(second.token, submission.fence, submission.value);
    item.pending = {};
    item.status.output = true;
    ++item.status.frames;
    item.status.message = "Live camera composition available: inset upper PFD, lower trim area preserved.";
  }
}
Snapshot snapshot(std::uint64_t key) {
  const std::lock_guard lock(runtime().mutex);
  Snapshot result;
  if (const auto* item = find(key)) {
    result = item->status;
    result.completed_frames = item->output.completed_submissions();
    result.patch_requests = item->output.patch_requests();
    result.patch_draws = item->output.patch_draws();
    if (result.output && !current_output(*item)) {
      result.output = false;
      result.message = "Scene output identity changed; waiting for fresh completed camera images.";
    }
  }
  result.capture = manager().statistics();
  return result;
}
bool stamp_at_recording_end(ID3D12GraphicsCommandList* list,
                            const PfdGraphicsState& state,
                            std::uint64_t key,
                            DXGI_FORMAT format,
                            UINT width,
                            UINT height,
                            DXGI_FORMAT depth_format,
                            const D3D12_RECT* destination,
                            const D3D12_RECT* content) {
  const std::lock_guard lock(runtime().mutex);
  auto* item = find(key);
  if (!item || !current_output(*item) || item->status.failed)
    return false;
  for (std::size_t i = 0; i < Formats.size(); ++i) {
    if (Formats[i] != format)
      continue;
    for (std::size_t d = 0; d < DepthFormats.size(); ++d) {
      if (DepthFormats[d] != depth_format)
        continue;
      const auto slot = i * DepthFormats.size() + d;
      if (list && item->stamp_ready[slot] && state.can_restore(list) && manager().register_consumer_recording(list) &&
          item->stamps[slot].record_final_buffer(list, state, item->native, item->output.address(), width, height, destination, content)) {
        ++item->status.stamps;
        return true;
      }
      ++item->status.state_skips;
      return false;
    }
  }
  ++item->status.state_skips;
  return false;
}
}  // namespace taxi_camera::scene_runtime
