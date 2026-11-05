#include "pfd_submission_pool.hpp"

#include "../bridge/native_hooks.hpp"
#include "display_keep.hpp"
#include "native_device_identity.hpp"

#include <limits>

namespace taxi_camera::pfd_submission {
namespace {
unsigned format_family(DXGI_FORMAT format) noexcept {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 2;
    default:
      return 0;
  }
}
bool typed_format(DXGI_FORMAT format) noexcept {
  return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
}  // namespace

Pool::~Pool() {
  const standalone::OwnedWork owned;
  bool pending = false;
  for (auto& packet : packets_) {
    if (packet.state == State::recorded || packet.state == State::submitted || packet.state == State::quarantined) {
      pending = true;
      continue;
    }
    release_resources(packet);
    if (packet.list)
      packet.list->Release();
    if (packet.allocator)
      packet.allocator->Release();
  }
  if (device_ && !pending)
    device_->Release();
}

void Pool::release_resources(Packet& packet) noexcept {
  if (packet.target)
    packet.target->Release();
  if (packet.source)
    packet.source->Release();
  if (packet.save)
    packet.save->Release();
  packet.target = packet.source = packet.save = nullptr;
  packet.completed = packet.dropped = packet.drop_sources = nullptr;
  packet.completed_value = 0;
  packet.covering_value = 0;
}

void Pool::report_dropped(Packet& packet, DropSource source) noexcept {
  if (!packet.dropped)
    return;
  count_drop(packet.drop_sources, source);
  auto seen = packet.dropped->load(std::memory_order_relaxed);
  while (seen < packet.completed_value &&
         !packet.dropped->compare_exchange_weak(seen, packet.completed_value, std::memory_order_release, std::memory_order_relaxed)) {
  }
  packet.dropped = packet.drop_sources = nullptr;
}

bool Pool::service(ID3D12Device* device, std::uint64_t completed) noexcept {
  const standalone::OwnedWork owned;
  if (!device || (device_ && device_ != device))
    return false;
  if (completed == std::numeric_limits<std::uint64_t>::max()) {
    for (auto& packet : packets_) {
      if (packet.state == State::recorded || packet.state == State::submitted)
        report_dropped(packet, DropSource::device_lost);
      packet.state = State::quarantined;
    }
    return false;
  }
  if (!device_) {
    device_ = device;
    device_->AddRef();
  }
  for (auto& packet : packets_) {
    if (packet.state == State::submitted && packet.covering_value <= completed) {
      if (packet.completed) {
        auto seen = packet.completed->load(std::memory_order_relaxed);
        while (seen < packet.completed_value && !packet.completed->compare_exchange_weak(
                                                    seen, packet.completed_value, std::memory_order_release, std::memory_order_relaxed)) {
        }
      }
      packet.dropped = nullptr;
      packet.state = State::retired;
    }
    if (packet.state == State::retired) {
      release_resources(packet);
      if (FAILED(packet.allocator->Reset()) || FAILED(packet.list->Reset(packet.allocator, nullptr))) {
        packet.state = State::quarantined;
        continue;
      }
      packet.state = State::ready;
    } else if (packet.state == State::empty) {
      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&packet.allocator))) ||
          FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, packet.allocator, nullptr, IID_PPV_ARGS(&packet.list)))) {
        // Never retry failed driver allocation in a native submission callback.
        // Successful packets can still service later submissions.
        packet.state = State::quarantined;
        continue;
      }
      packet.state = State::ready;
    }
  }
  return ready_count() != 0;
}

bool Pool::valid_copy(const Copy& copy) const noexcept {
  if (!device_ || !copy.target || !copy.source || copy.target == copy.source || !copy.width || !copy.height || copy.width > 16384 ||
      copy.height > 16384 || !same_native_device(copy.target, device_) || !same_native_device(copy.source, device_))
    return false;
  constexpr auto allowed = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (copy.state != D3D12_RESOURCE_STATE_RENDER_TARGET && copy.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
      (static_cast<unsigned>(copy.state) & ~static_cast<unsigned>(allowed)) != 0)
    return false;
  const auto target = copy.target->GetDesc();
  const auto source = copy.source->GetDesc();
  const auto& footprint = copy.footprint.Footprint;
  if (target.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || target.Width > 16384 || target.Height > 16384 || !target.MipLevels ||
      target.DepthOrArraySize != 1 || target.SampleDesc.Count != 1 || target.SampleDesc.Quality != 0 ||
      !(target.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
      (copy.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && !(target.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
      (target.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) || source.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      !typed_format(footprint.Format) || format_family(target.Format) != format_family(footprint.Format) || footprint.Width != copy.width ||
      footprint.Height != copy.height || footprint.Depth != 1 || footprint.RowPitch < copy.width * 4 ||
      footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0 || copy.footprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0 ||
      copy.x > target.Width || copy.width > target.Width - copy.x || copy.y > target.Height || copy.height > target.Height - copy.y)
    return false;
  if ((copy.completed || copy.dropped) && !copy.completed_value)
    return false;
  if (copy.readback && (copy.save || copy.shadow_source))
    return false;
  const auto heap_type = [](ID3D12Resource* buffer) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    return SUCCEEDED(buffer->GetHeapProperties(&heap, nullptr)) ? heap.Type : D3D12_HEAP_TYPE{};
  };
  if (copy.readback && heap_type(copy.source) != D3D12_HEAP_TYPE_READBACK)
    return false;
  if (copy.shadow_source && heap_type(copy.source) != D3D12_HEAP_TYPE_DEFAULT)
    return false;
  const auto fits = [&](ID3D12Resource* buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& placed) noexcept {
    const std::uint64_t width = buffer->GetDesc().Width;
    const std::uint64_t bytes = std::uint64_t{copy.height - 1} * placed.Footprint.RowPitch + std::uint64_t{copy.width} * 4;
    return placed.Offset <= width && bytes <= width - placed.Offset;
  };
  if (copy.save) {
    const auto& saved = copy.save_footprint.Footprint;
    if (copy.save == copy.target || copy.save == copy.source || !same_native_device(copy.save, device_) ||
        copy.save->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || heap_type(copy.save) != D3D12_HEAP_TYPE_DEFAULT ||
        !typed_format(saved.Format) || format_family(target.Format) != format_family(saved.Format) || saved.Width != copy.width ||
        saved.Height != copy.height || saved.Depth != 1 || saved.RowPitch < copy.width * 4 ||
        saved.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0 ||
        copy.save_footprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0 || !fits(copy.save, copy.save_footprint))
      return false;
  }
  return fits(copy.source, copy.footprint);
}

Pool::Recording Pool::record(const Copy& copy) noexcept {
  const standalone::OwnedWork owned;
  if (!valid_copy(copy))
    return {};
  for (unsigned i = 0; i < packets_.size(); ++i) {
    auto& packet = packets_[i];
    if (packet.state != State::ready)
      continue;
    copy.target->AddRef();
    copy.source->AddRef();
    if (copy.save)
      copy.save->AddRef();
    packet.target = copy.target;
    packet.source = copy.source;
    packet.save = copy.save;
    packet.completed = copy.completed;
    packet.dropped = copy.dropped;
    packet.drop_sources = copy.drop_sources;
    packet.completed_value = copy.completed_value;
    packet.state = State::recorded;
    const auto transition = [](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {resource, 0, before, after};
      return barrier;
    };
    D3D12_TEXTURE_COPY_LOCATION buffer{}, texture{};
    buffer.pResource = copy.source;
    buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buffer.PlacedFootprint = copy.footprint;
    texture.pResource = copy.target;
    texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX region{copy.x, copy.y, 0, copy.x + copy.width, copy.y + copy.height, 1};
    if (copy.readback) {
      auto barrier = transition(copy.target, copy.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      packet.list->ResourceBarrier(1, &barrier);
      packet.list->CopyTextureRegion(&buffer, 0, 0, 0, &texture, &region);
      barrier = transition(copy.target, D3D12_RESOURCE_STATE_COPY_SOURCE, copy.state);
      packet.list->ResourceBarrier(1, &barrier);
    } else {
      // The save reads the panel's pixels before the source replaces them.
      auto target_state = copy.state;
      if (copy.save) {
        const D3D12_RESOURCE_BARRIER enter[]{transition(copy.target, target_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                             transition(copy.save, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
        packet.list->ResourceBarrier(2, enter);
        D3D12_TEXTURE_COPY_LOCATION saved{};
        saved.pResource = copy.save;
        saved.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        saved.PlacedFootprint = copy.save_footprint;
        packet.list->CopyTextureRegion(&saved, 0, 0, 0, &texture, &region);
        const auto leave = transition(copy.save, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        packet.list->ResourceBarrier(1, &leave);
        target_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      }
      D3D12_RESOURCE_BARRIER enter[2]{transition(copy.target, target_state, D3D12_RESOURCE_STATE_COPY_DEST)};
      UINT entering = 1;
      if (copy.shadow_source)
        enter[entering++] = transition(copy.source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      packet.list->ResourceBarrier(entering, enter);
      // Camera copies and shadow restores both leave the panel marks alone.
      standalone::display_keep::copy_region(packet.list, &texture, copy.x, copy.y, &buffer, copy.width, copy.height);
      D3D12_RESOURCE_BARRIER leave[2]{transition(copy.target, D3D12_RESOURCE_STATE_COPY_DEST, copy.state)};
      UINT leaving = 1;
      if (copy.shadow_source)
        leave[leaving++] = transition(copy.source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      packet.list->ResourceBarrier(leaving, leave);
    }
    if (FAILED(packet.list->Close())) {
      packet.state = State::quarantined;
      report_dropped(packet, DropSource::close_failed);
      Recording failed;
      failed.reported = true;
      return failed;
    }
    return {i, packet.list};
  }
  return {};
}

void Pool::submit(unsigned slot, std::uint64_t covering_value) noexcept {
  if (slot >= packets_.size())
    return;
  auto& packet = packets_[slot];
  if (packet.state != State::recorded || !covering_value || covering_value == std::numeric_limits<std::uint64_t>::max()) {
    if (packet.state == State::recorded || packet.state == State::submitted)
      report_dropped(packet, DropSource::unsubmitted);
    packet.state = State::quarantined;
    return;
  }
  packet.covering_value = covering_value;
  packet.state = State::submitted;
}

void Pool::cancel(unsigned slot) noexcept {
  if (slot < packets_.size()) {
    auto& packet = packets_[slot];
    if (packet.state == State::recorded || packet.state == State::submitted)
      report_dropped(packet, DropSource::cancelled);
    if (packet.state == State::recorded)
      packet.state = State::retired;
    else if (packet.state == State::submitted)
      packet.state = State::quarantined;
  }
}

void Pool::quarantine(unsigned slot) noexcept {
  if (slot < packets_.size()) {
    auto& packet = packets_[slot];
    if (packet.state == State::recorded || packet.state == State::submitted)
      report_dropped(packet, DropSource::quarantined);
    packet.state = State::quarantined;
  }
}

unsigned Pool::ready_count() const noexcept {
  unsigned result = 0;
  for (const auto& packet : packets_)
    result += packet.state == State::ready;
  return result;
}

}  // namespace taxi_camera::pfd_submission
