#include "scene_runtime.hpp"
#include "../standalone/native_hooks.hpp"
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
  Snapshot status;
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
    if (!item || !item->status.initialized || item->status.failed || frame.match.feed >= 2) {
      manager().discard_frame(frame.token);
      continue;
    }
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
    if (result.output && !current_output(*item)) {
      result.output = false;
      result.message = "Scene output identity changed; waiting for fresh completed camera images.";
    }
  }
  result.capture = manager().statistics();
  return result;
}
bool stamp(ID3D12GraphicsCommandList* list,
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
      if (item->stamp_ready[slot] && state.complete() && manager().register_consumer_recording(list) &&
          item->stamps[slot].record_buffer(list, state, item->native, item->output.address(), width, height, destination, content)) {
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
