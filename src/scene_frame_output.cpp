#include "scene_frame_output.hpp"
#include "camera_compositor_d3d12.hpp"

#include <limits>
#include <new>

namespace taxi_camera {
bool SceneFrameOutput::set_display_exposure(float ev) noexcept {
  return compositor_ && !prepared_ && !failed_ && compositor_->set_display_exposure(ev);
}
bool SceneFrameOutput::set_composition(const profiles::Composition& layout) noexcept {
  if (!compositor_ || prepared_ || failed_)
    return false;
  compositor_->set_composition(layout);
  return true;
}
bool SceneFrameOutput::set_ground_speed(float knots, bool valid) noexcept {
  if (!compositor_ || prepared_ || failed_)
    return false;
  compositor_->set_ground_speed(knots, valid);
  return true;
}
float SceneFrameOutput::display_exposure() const noexcept {
  return compositor_ ? compositor_->display_exposure() : CameraCompositorD3D12::DefaultExposureEv;
}
bool SceneFrameOutput::fail(const char* error) noexcept {
  error_ = error;
  failed_ = true;
  return false;
}

bool SceneFrameOutput::initialize(ID3D12Device* device) noexcept {
  if (failed_)
    return false;
  if (device_)
    return device_ == device && !failed_;
  if (!device)
    return fail("A live native D3D12 device is required for the PFD output.");
  device_ = device;
  device_->AddRef();
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_))) ||
      FAILED(device_->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator_))) ||
      FAILED(device_->CreateCommandList(0, queue_desc.Type, allocator_, nullptr, IID_PPV_ARGS(&list_))) || FAILED(list_->Close()) ||
      FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
    return fail("Creating the private PFD composition queue failed.");
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = BufferBytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(&buffer_))))
    return fail("Creating the stable PFD GPU buffer failed.");
  address_ = buffer_->GetGPUVirtualAddress();
  compositor_ = new (std::nothrow) CameraCompositorD3D12;
  if (!address_ || !compositor_ || FAILED(compositor_->initialize(device_)))
    return fail("Initializing the two-camera compositor failed.");
  return true;
}

bool SceneFrameOutput::idle() const noexcept {
  if (failed_ || !fence_)
    return false;
  const auto completed = fence_->GetCompletedValue();
  return completed != UINT64_MAX && completed >= submitted_;
}

bool SceneFrameOutput::prepare(ID3D12Resource* nose, DXGI_FORMAT nose_format, ID3D12Resource* tail, DXGI_FORMAT tail_format) noexcept {
  if (!idle() || prepared_)
    return false;
  if (FAILED(device_->GetDeviceRemovedReason()))
    return fail("The PFD composition device was removed.");
  if (FAILED(compositor_->set_inputs(nose, nose_format, tail, tail_format))) {
    error_ = compositor_->last_error();
    return false;
  }
  if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_, nullptr)))
    return fail("Resetting the completed private composition list failed.");
  if (FAILED(compositor_->record(list_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST)))
    return fail("Recording the two-camera composition failed.");
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {buffer_, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
  list_->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
  destination.pResource = buffer_;
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, Width, Height, 1, RowPitch};
  source.pResource = compositor_->output();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  // Buffers begin each ExecuteCommandLists scope in COMMON. The application
  // stamp promotes this buffer for its root SRV read and it decays afterward.
  // https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  list_->ResourceBarrier(1, &barrier);
  if (FAILED(list_->Close()))
    return fail("Closing the private composition list failed.");
  prepared_ = true;
  return true;
}

bool SceneFrameOutput::submit() noexcept {
  if (!prepared_ || failed_ || submitted_ >= std::numeric_limits<std::uint64_t>::max() - 1)
    return false;
  ID3D12CommandList* lists[]{list_};
  queue_->ExecuteCommandLists(1, lists);
  prepared_ = false;
  ++submitted_;
  if (FAILED(queue_->Signal(fence_, submitted_)))
    return fail("Signaling the private composition completion fence failed.");
  return true;
}

bool SceneFrameOutput::discard_prepared() noexcept {
  if (!prepared_ || !idle())
    return false;
  if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_, nullptr)) || FAILED(list_->Close()))
    return fail("Discarding the never-submitted private composition list failed.");
  prepared_ = false;
  return true;
}
}  // namespace taxi_camera
