#include "scene_frame_output.hpp"
#include "camera_compositor_d3d12.hpp"
#include "pfd_stamp_d3d12.hpp"

#include <limits>
#include <new>

namespace taxi_camera {
namespace {
constexpr std::array<DXGI_FORMAT, 4> PatchFormats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
bool same_rect(const D3D12_RECT& a, const D3D12_RECT& b) noexcept {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
}  // namespace
bool SceneFrameOutput::set_patch_profile(std::uint32_t profile) noexcept {
  if (!profiles::find(profile) || prepared_ || failed_)
    return false;
  patch_profile_ = profile;
  return true;
}
bool SceneFrameOutput::valid_patch_request(DXGI_FORMAT format, UINT width, UINT height, const D3D12_RECT& content) const noexcept {
  const auto* profile = profiles::find(patch_profile_);
  if (!profile || failed_ ||
      !profiles::matches_display(*profile, profile->width, profile->height, profile->mips ? profile->mips : 1,
                                 static_cast<unsigned>(format)))
    return false;
  const auto outer = profiles::display_rect(*profile, 0), inner = profiles::display_content_rect(*profile, 0);
  const D3D12_RECT local{static_cast<LONG>(inner.left - outer.left), static_cast<LONG>(inner.top - outer.top),
                         static_cast<LONG>(inner.right - outer.left), static_cast<LONG>(inner.bottom - outer.top)};
  return width && width <= 16384 && height && height <= 16384 && width == outer.right - outer.left && height == outer.bottom - outer.top &&
         same_rect(content, local);
}
bool SceneFrameOutput::request_patch(DXGI_FORMAT format, UINT width, UINT height, const D3D12_RECT& content) noexcept {
  if (!valid_patch_request(format, width, height, content))
    return false;
  unsigned drawer = 0;
  while (drawer < PatchFormats.size() && PatchFormats[drawer] != format)
    ++drawer;
  if (drawer == PatchFormats.size())
    return false;
  for (const auto& p : patches_)
    if (p.width == width && p.height == height && p.format == format && same_rect(p.content, content))
      return true;
  for (auto& p : patches_)
    if (!p.width) {
      p.format = format;
      p.width = width;
      p.height = height;
      p.content = content;
      p.drawer = drawer;
      ++patch_requests_;
      return true;
    }
  return false;
}
SceneFrameOutput::Patch SceneFrameOutput::patch(DXGI_FORMAT format, UINT width, UINT height, const D3D12_RECT& content) const noexcept {
  if (valid_patch_request(format, width, height, content) && submitted_)
    for (const auto& p : patches_)
      if (p.written && p.view.footprint.Footprint.Format == format && p.view.footprint.Footprint.Width == width &&
          p.view.footprint.Footprint.Height == height && same_rect(p.content, content))
        return p.view;
  return {};
}
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
  D3D12_DESCRIPTOR_HEAP_DESC patch_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, static_cast<UINT>(patches_.size()),
                                             D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(device_->CreateDescriptorHeap(&patch_heap_desc, IID_PPV_ARGS(&patch_heap_))))
    return fail("Creating the private patch descriptor heap failed.");
  for (unsigned i = 0; i < patch_drawers_.size(); ++i) {
    patch_drawers_[i] = new (std::nothrow) PfdStampD3D12;
    if (!patch_drawers_[i] || FAILED(patch_drawers_[i]->initialize(device_, PatchFormats[i])))
      return fail("Creating a private typed PFD patch pipeline failed.");
  }
  return true;
}

bool SceneFrameOutput::prepare_patches() noexcept {
  const auto base = patch_heap_->GetCPUDescriptorHandleForHeapStart();
  const auto stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (auto& storage : patches_) {
    auto* patch = &storage;
    if (!patch->width)
      continue;
    const auto width = patch->width, height = patch->height;
    const D3D12_RECT destination{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (!patch->texture) {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      heap.CreationNodeMask = heap.VisibleNodeMask = 1;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
      desc.Format = patch->format;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                  IID_PPV_ARGS(&patch->texture))))
        return fail("Creating a private PFD patch texture failed.");
      UINT64 bytes{};
      device_->GetCopyableFootprints(&desc, 0, 1, 0, &patch->view.footprint, nullptr, nullptr, &bytes);
      desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = bytes;
      desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&patch->view.buffer))))
        return fail("Creating a stable private PFD patch buffer failed.");
      patch->rtv = {base.ptr + static_cast<SIZE_T>(patch - patches_.data()) * stride};
      device_->CreateRenderTargetView(patch->texture, nullptr, patch->rtv);
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {patch->texture, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
    list_->ResourceBarrier(1, &barrier);
    list_->OMSetRenderTargets(1, &patch->rtv, FALSE, nullptr);
    if (!patch_drawers_[patch->drawer]->record_private_patch(list_, device_, address_, width, height, &destination, &patch->content))
      return fail("Recording the private PFD patch failed.");
    barrier.Transition = {patch->texture, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list_->ResourceBarrier(1, &barrier);
    barrier.Transition = {patch->view.buffer, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list_->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = patch->texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.pResource = patch->view.buffer;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = patch->view.footprint;
    list_->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(1, &barrier);
    patch->recorded = true;
    ++patch_draws_;
  }
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
  // Buffers begin each ExecuteCommandLists scope in COMMON. Our private patch
  // draw promotes this buffer for its root SRV read and it decays afterward.
  // https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  list_->ResourceBarrier(1, &barrier);
  if (!prepare_patches())
    return false;
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
  for (auto& patch : patches_)
    if (patch.recorded) {
      patch.written = true;
      patch.recorded = false;
    }
  return true;
}

bool SceneFrameOutput::discard_prepared() noexcept {
  if (!prepared_ || !idle())
    return false;
  if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_, nullptr)) || FAILED(list_->Close()))
    return fail("Discarding the never-submitted private composition list failed.");
  for (auto& patch : patches_)
    patch.recorded = false;
  prepared_ = false;
  return true;
}
}  // namespace taxi_camera
