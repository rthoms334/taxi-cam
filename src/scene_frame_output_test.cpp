#include "scene_frame_output.hpp"
#include "camera_compositor_d3d12.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include "../validation/reference_overlay_oracle.hpp"
#include "scene_capture_manager.hpp"

namespace {
using Output = taxi_camera::SceneFrameOutput;
using Manager = taxi_camera::SceneCaptureManager;
unsigned checks = 0;
template <class T>
struct Ref {
  T* p = nullptr;
  ~Ref() {
    if (p)
      p->Release();
  }
  T* operator->() const { return p; }
  T** put() { return &p; }
  Ref() = default;
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void check(HRESULT value, const char* message) {
  require(SUCCEEDED(value), message);
}
template <class F>
void wait(F&& predicate) {
  const auto start = GetTickCount64();
  while (!predicate()) {
    require(GetTickCount64() - start < 10000, "GPU timeout");
    Sleep(1);
  }
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = result.VisibleNodeMask = 1;
  return result;
}
struct Commands {
  Ref<ID3D12CommandQueue> queue;
  Ref<ID3D12CommandAllocator> allocator;
  Ref<ID3D12GraphicsCommandList> list;
  void initialize(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.put())), "Create queue");
    check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(allocator.put())), "Create allocator");
    check(device->CreateCommandList(0, desc.Type, allocator.p, nullptr, IID_PPV_ARGS(list.put())), "Create list");
  }
};
void run(bool warp) {
  Ref<ID3D12Debug> debug;
  const bool debug_layer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_layer)
    debug->EnableDebugLayer();
  Ref<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "DXGI factory");
  Ref<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP");
  Ref<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Device");
  Ref<ID3D12InfoQueue> info;
  if (debug_layer)
    check(device->QueryInterface(IID_PPV_ARGS(info.put())), "Info queue");
  taxi_camera::SceneHandoff handoff;
  auto manager = std::make_unique<Manager>(handoff);
  require(manager->register_device(7, device.p), "Register manager device");
  auto output = std::make_unique<Output>();
  require(output->initialize(device.p), output->error());
  require(output->display_exposure() == taxi_camera::CameraCompositorD3D12::DefaultExposureEv, "Output exposure default");
  require(!output->set_display_exposure(std::numeric_limits<float>::quiet_NaN()) &&
              output->display_exposure() == taxi_camera::CameraCompositorD3D12::DefaultExposureEv,
          "Output accepted nonfinite exposure");
  require(!output->discard_prepared(), "Discard without prepared list succeeded");
  const auto stable_address = output->address();
  auto* stable_buffer = output->buffer();
  require(stable_address && stable_buffer, "Stable GPU buffer absent");
  Commands upload, consumer;
  upload.initialize(device.p);
  consumer.initialize(device.p);
  require(manager->register_command_list(consumer.list.p, 7, 19), "Register consumer");
  require(manager->register_consumer_recording(consumer.list.p), "Register consumer recording");
  Ref<ID3D12Fence> completed, blocked;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completed.put())), "Completion fence");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(blocked.put())), "Blocking fence");
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = Output::BufferBytes;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto read_heap = heap(D3D12_HEAP_TYPE_READBACK);
  Ref<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "Readback buffer");
  // Record this stable-buffer consumer ONCE. Its second execution must observe
  // the next private composition through the manager timeline without a reset.
  consumer.list->CopyBufferRegion(readback.p, 0, stable_buffer, 0, Output::BufferBytes);
  check(consumer.list->Close(), "Close stable consumer");
  std::uint64_t checked_pixels = 0;
  for (unsigned frame = 0; frame < 2; ++frame) {
    if (frame) {
      check(upload.allocator->Reset(), "Reset completed upload allocator");
      check(upload.list->Reset(upload.allocator.p, nullptr), "Reset upload list");
    }
    std::array<Ref<ID3D12Resource>, 2> inputs, uploads;
    const std::array<std::array<unsigned char, 4>, 2> colors{
        {{static_cast<unsigned char>(27 + frame * 39), 93, 177, 255}, {201, static_cast<unsigned char>(51 + frame * 67), 33, 255}}};
    for (unsigned feed = 0; feed < 2; ++feed) {
      D3D12_RESOURCE_DESC texture{};
      texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      texture.Width = 32;
      texture.Height = 24;
      texture.DepthOrArraySize = texture.MipLevels = 1;
      texture.SampleDesc.Count = 1;
      texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
      check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(inputs[feed].put())),
            "Owned input texture");
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      UINT64 required = 0;
      device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &required);
      auto upload_desc = buffer;
      upload_desc.Width = required;
      const auto upload_heap = heap(D3D12_HEAP_TYPE_UPLOAD);
      check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(uploads[feed].put())),
            "Test source upload");
      void* mapped = nullptr;
      D3D12_RANGE none{0, 0};
      check(uploads[feed]->Map(0, &none, &mapped), "Map test upload");
      for (unsigned y = 0; y < 24; ++y)
        for (unsigned x = 0; x < 32; ++x)
          std::memcpy(static_cast<unsigned char*>(mapped) + y * footprint.Footprint.RowPitch + x * 4, colors[feed].data(), 4);
      uploads[feed]->Unmap(0, nullptr);
      D3D12_TEXTURE_COPY_LOCATION from{}, to{};
      from.pResource = uploads[feed].p;
      from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      from.PlacedFootprint = footprint;
      to.pResource = inputs[feed].p;
      to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      upload.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    check(upload.list->Close(), "Close input upload");
    ID3D12CommandList* uploads_batch[]{upload.list.p};
    upload.queue->ExecuteCommandLists(1, uploads_batch);
    check(upload.queue->Signal(completed.p, frame * 2 + 1), "Input completion");
    wait([&] { return completed->GetCompletedValue() >= frame * 2 + 1; });
    require(output->idle(), "Output did not become idle");
    const float exposure_ev = frame ? 4.0f : -16.0f;
    require(output->set_display_exposure(exposure_ev) && output->display_exposure() == exposure_ev, "Output exposure forwarding");
    require(output->prepare(inputs[0].p, DXGI_FORMAT_R8G8B8A8_UNORM, inputs[1].p, DXGI_FORMAT_R8G8B8A8_UNORM), output->error());
    require(!output->set_display_exposure(-8) && output->display_exposure() == exposure_ev, "Prepared output exposure was changed");
    if (!frame) {
      require(output->discard_prepared(), "Discard closed never-submitted composition");
      require(!output->submit(), "Discarded composition was submitted");
      require(output->prepare(inputs[0].p, DXGI_FORMAT_R8G8B8A8_UNORM, inputs[1].p, DXGI_FORMAT_R8G8B8A8_UNORM), output->error());
    }
    // The compositor must retain these actual input objects through its fence.
    for (auto& input : inputs) {
      input.p->Release();
      input.p = nullptr;
    }
    require(!output->prepare(nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN), "Prepared descriptors changed");
    check(output->queue()->Wait(blocked.p, frame + 1), "Block output queue");
    const auto transaction = manager->begin_private_submission(7, output->queue());
    require(transaction.receipt != 0, "Private timeline receipt missing");
    require(output->submit(), output->error());
    require(manager->end_private_submission(transaction.receipt), "Private timeline signal");
    require(!output->idle(), "Output fence did not cover blocked work");
    require(!output->discard_prepared(), "Submitted work could be discarded");
    ID3D12CommandList* consumers[]{consumer.list.p};
    const auto receipt = manager->before_submission(consumer.queue.p, 1, consumers);
    require(receipt != 0, "Persistent consumer lost its timeline registration");
    consumer.queue->ExecuteCommandLists(1, consumers);
    manager->after_submission(consumer.queue.p, receipt);
    check(consumer.queue->Signal(completed.p, frame * 2 + 2), "Consumer completion");
    require(completed->GetCompletedValue() < frame * 2 + 2, "Consumer passed blocked composition");
    check(blocked->Signal(frame + 1), "Release output queue");
    wait([&] { return completed->GetCompletedValue() >= frame * 2 + 2; });
    require(output->idle() && output->submissions() == frame + 1, "Output submission did not retire");
    require(output->address() == stable_address && output->buffer() == stable_buffer, "Stable output address changed");
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(Output::BufferBytes)};
    check(readback->Map(0, &range, &mapped), "Read composed output");
    reference_overlay_oracle::FontCoverage font_coverage;
    for (unsigned y = 0; y < Output::Height; ++y)
      for (unsigned x = 0; x < Output::Width; ++x) {
        auto expected = y < 255 ? colors[0] : colors[1];
        reference_overlay_oracle::pixel(x, y, expected);
        const auto* pixel = static_cast<unsigned char*>(mapped) + y * Output::RowPitch + x * 4;
        const int font_cell = reference_overlay_oracle::font_cell(x, y);
        if (font_cell >= 0)
          require(font_coverage.observe(font_cell, pixel), "GS font lost opaque white-label/green-value colour contract");
        else
          require(std::memcmp(pixel, expected.data(), 4) == 0, "Nose/divider/tail output pixels mismatch");
        ++checked_pixels;
      }
    require(font_coverage.complete(), "GS glyphs are missing, filled rectangles or missing antialiasing");
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
  }
  manager->destroy_command_list(consumer.list.p, 19);
  unsigned debug_errors = 0;
  if (info.p)
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      check(info->GetMessage(i, nullptr, &size), "Debug size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(info->GetMessage(i, message, &size), "Debug message");
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        ++debug_errors;
    }
  require(debug_errors == 0, "D3D12 debug layer errors");
  std::printf(
      "{\"passed\":true,\"checks\":%u,\"checked_pixels\":%llu,\"frames\":2,\"stable_address\":true,"
      "\"consumer_recordings\":1,\"debugLayer\":%s,\"debugErrors\":%u,\"adapter\":\"%s\"}\n",
      checks, static_cast<unsigned long long>(checked_pixels), debug_layer ? "true" : "false", debug_errors, warp ? "WARP" : "hardware");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--warp") == 0), "Only --warp accepted");
    run(argc == 2);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
