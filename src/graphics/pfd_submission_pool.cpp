#include "pfd_submission_pool.hpp"

#include "../bridge/native_hooks.hpp"
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
  packet.target = packet.source = nullptr;
  packet.covering_value = 0;
}

bool Pool::service(ID3D12Device* device, std::uint64_t completed) noexcept {
  const standalone::OwnedWork owned;
  if (!device || (device_ && device_ != device))
    return false;
  if (completed == std::numeric_limits<std::uint64_t>::max()) {
    for (auto& packet : packets_)
      packet.state = State::quarantined;
    return false;
  }
  if (!device_) {
    device_ = device;
    device_->AddRef();
  }
  for (auto& packet : packets_) {
    if (packet.state == State::submitted && packet.covering_value <= completed)
      packet.state = State::retired;
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
  const std::uint64_t bytes = std::uint64_t{copy.height - 1} * footprint.RowPitch + std::uint64_t{copy.width} * 4;
  return copy.footprint.Offset <= source.Width && bytes <= source.Width - copy.footprint.Offset;
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
    packet.target = copy.target;
    packet.source = copy.source;
    packet.state = State::recorded;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {copy.target, 0, copy.state, D3D12_RESOURCE_STATE_COPY_DEST};
    packet.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = copy.source;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = copy.footprint;
    target.pResource = copy.target;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, copy.width, copy.height, 1};
    packet.list->CopyTextureRegion(&target, copy.x, copy.y, 0, &source, &box);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = copy.state;
    packet.list->ResourceBarrier(1, &barrier);
    if (FAILED(packet.list->Close())) {
      packet.state = State::quarantined;
      return {};
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
    packet.state = State::quarantined;
    return;
  }
  packet.covering_value = covering_value;
  packet.state = State::submitted;
}

void Pool::cancel(unsigned slot) noexcept {
  if (slot < packets_.size()) {
    auto& packet = packets_[slot];
    if (packet.state == State::recorded)
      packet.state = State::retired;
    else if (packet.state == State::submitted)
      packet.state = State::quarantined;
  }
}

void Pool::quarantine(unsigned slot) noexcept {
  if (slot < packets_.size())
    packets_[slot].state = State::quarantined;
}

unsigned Pool::ready_count() const noexcept {
  unsigned result = 0;
  for (const auto& packet : packets_)
    result += packet.state == State::ready;
  return result;
}

}  // namespace taxi_camera::pfd_submission
