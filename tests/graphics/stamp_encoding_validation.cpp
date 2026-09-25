// Focused native GPU regression: the production PFD stamp on all four typed
// PSO formats. Test-owned device, buffers and targets; no simulator access.
// Camera-flagged source pixels (alpha 255) must store their compositor codes
// through UNORM and sRGB views. Overlay-flagged pixels (alpha 0) must pass
// through, so an sRGB view encodes them the way it encodes aircraft UI.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "../../src/graphics/pfd_stamp_d3d12.hpp"
#include "../support/gpu_queue.hpp"

namespace {
using Frame = taxi_camera::PfdStampFrame;
constexpr UINT Width = Frame::Width, Height = Frame::Height;
template <class T>
struct Ref {
  T* value = nullptr;
  Ref() = default;
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  ~Ref() {
    if (value)
      value->Release();
  }
  T* operator->() const { return value; }
  T** put() { return &value; }
};
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
void check(HRESULT hr, const char* message) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s HRESULT=0x%08lx\n", message, static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
  }
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type;
  h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
void create(ID3D12Device* device, const D3D12_RESOURCE_DESC& d, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
  const auto h = heap(type);
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(out)), "create resource");
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
// Compositor-buffer bytes: every code in every channel, alternating camera and
// overlay rows, plus the PMDG 777 T colour #1C1B22 under both flags.
std::array<unsigned char, 4> source_pixel(UINT x, UINT y) {
  if (y >= 700 && y < 710)
    return {28, 27, 34, 0};
  if (y >= 720 && y < 730)
    return {28, 27, 34, 255};
  const UINT c = x & 255;
  return {static_cast<unsigned char>(c), static_cast<unsigned char>((c + 85) & 255), static_cast<unsigned char>((c + 170) & 255),
          static_cast<unsigned char>(y % 2 ? 0 : 255)};
}
// Independent double-precision sRGB OETF: what an sRGB view stores for a code.
int encoded(int code) {
  const double v = code / 255.;
  return static_cast<int>(std::lround(255 * (v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055)));
}
struct Case {
  DXGI_FORMAT typeless, typed, footprint;
  bool bgra, srgb;
  const char* name;
};
constexpr std::array<Case, 4> Cases{{
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, false, "RGBA8_UNORM"},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, false, true, "RGBA8_UNORM_SRGB"},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, true, false, "BGRA8_UNORM"},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, true, true, "BGRA8_UNORM_SRGB"},
}};
struct Counts {
  std::uint64_t camera = 0, camera_exact = 0, overlay = 0, overlay_exact = 0;
};

void run(bool warp) {
  Ref<ID3D12Debug> debug;
  const bool debug_layer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_layer)
    debug->EnableDebugLayer();
  Ref<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "factory");
  Ref<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP adapter");
  Ref<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "device");
  Ref<ID3D12InfoQueue> info;
  if (debug_layer)
    check(device->QueryInterface(IID_PPV_ARGS(info.put())), "info queue");
  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Ref<ID3D12CommandQueue> queue;
  Ref<ID3D12CommandAllocator> allocator;
  Ref<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.put())), "queue");
  check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(allocator.put())), "allocator");
  check(device->CreateCommandList(0, q.Type, allocator.value, nullptr, IID_PPV_ARGS(list.put())), "list");

  Ref<ID3D12Resource> upload, pixels;
  create(device.value, buffer(Frame::Bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload.put());
  create(device.value, buffer(Frame::Bytes), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, pixels.put());
  void* data = nullptr;
  const D3D12_RANGE none{};
  check(upload->Map(0, &none, &data), "upload map");
  for (UINT y = 0; y < Height; ++y)
    for (UINT x = 0; x < Width; ++x)
      std::memcpy(static_cast<unsigned char*>(data) + SIZE_T{y} * Frame::RowPitch + x * 4, source_pixel(x, y).data(), 4);
  upload->Unmap(0, nullptr);
  list->CopyBufferRegion(pixels.value, 0, upload.value, 0, Frame::Bytes);
  transition(list.value, pixels.value, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  std::array<taxi_camera::PfdStampD3D12, Cases.size()> stamps;
  std::array<Ref<ID3D12Resource>, 2> targets;
  std::array<Ref<ID3D12Resource>, Cases.size()> readbacks;
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, Cases.size()> footprints{};
  Ref<ID3D12DescriptorHeap> rtvs;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, static_cast<UINT>(Cases.size()), D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtvs.put())), "RTV heap");
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const D3D12_RECT destination{0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height)};
  for (UINT n = 0; n < Cases.size(); ++n) {
    const auto& c = Cases[n];
    auto& target = targets[c.bgra ? 1 : 0];
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = Width;
    texture.Height = Height;
    texture.DepthOrArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = c.typeless;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!target.value)
      create(device.value, texture, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, target.put());
    else
      transition(list.value, target.value, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprints[n], nullptr, nullptr, &bytes);
    footprints[n].Footprint.Format = c.footprint;
    create(device.value, buffer(bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readbacks[n].put());
    check(stamps[n].initialize(device.value, c.typed), "production stamp PSO");
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = c.typed;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvs->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{n} * stride};
    device->CreateRenderTargetView(target.value, &view, rtv);
    // A half-alpha sentinel proves the 1:1 stamp writes every target pixel.
    constexpr float sentinel[]{.25f, .5f, .75f, .5f};
    list->ClearRenderTargetView(rtv, sentinel, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    require(stamps[n].record_private_patch(list.value, device.value, pixels->GetGPUVirtualAddress(), Width, Height, &destination),
            "record production private stamp");
    transition(list.value, target.value, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = target.value;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readbacks[n].value;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprints[n];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  check(list->Close(), "close");
  ID3D12CommandList* lists[]{list.value};
  queue->ExecuteCommandLists(1, lists);
  if (!taxi_camera::drain_copy_queue(queue.value, device.value)) {
    // Keep every submitted resource alive; exit this isolated host immediately.
    for (auto& stamp : stamps)
      stamp.abandon();
    std::fputs("FAIL: stamp encoding GPU completion could not be established.\n", stderr);
    ExitProcess(1);
  }

  std::array<Counts, Cases.size()> counts{};
  std::array<std::array<int, 4>, 2> headline{};
  std::array<std::array<int, 3>, 2> overlay_t{}, camera_t{};
  for (UINT n = 0; n < Cases.size(); ++n) {
    const auto& c = Cases[n];
    const auto pitch = footprints[n].Footprint.RowPitch;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, SIZE_T{pitch} * Height};
    check(readbacks[n]->Map(0, &range, &mapped), "readback map");
    const auto* bytes = static_cast<const unsigned char*>(mapped);
    const auto stored = [&](UINT x, UINT y, UINT channel) {
      const auto* p = bytes + SIZE_T{y} * pitch + x * 4;
      return static_cast<int>(c.bgra && channel < 3 ? p[2 - channel] : p[channel]);
    };
    for (UINT y = 0; y < Height; ++y)
      for (UINT x = 0; x < Width; ++x) {
        const auto source = source_pixel(x, y);
        const bool camera = source[3] == 255;
        require(stored(x, y, 3) == 255, "Every stamped pixel is opaque on the display");
        bool exact = true;
        for (UINT channel = 0; channel < 3; ++channel) {
          const int code = source[channel];
          const int expected = c.srgb && !camera ? encoded(code) : code;
          const int tolerance = !c.srgb || (camera && (code == 0 || code == 255)) ? 0 : 1;
          const int actual = stored(x, y, channel);
          if (std::abs(actual - expected) > tolerance) {
            char message[256]{};
            std::snprintf(message, sizeof(message), "%s %s pixel (%u,%u) channel %u stored %d, expected %d +-%d", c.name,
                          camera ? "camera" : "overlay", x, y, channel, actual, expected, tolerance);
            throw std::runtime_error(message);
          }
          exact &= actual == expected;
        }
        auto& count = counts[n];
        (camera ? count.camera : count.overlay) += 1;
        (camera ? count.camera_exact : count.overlay_exact) += exact;
      }
    // The PMDG 777 T colour and the dark camera codes that doubled encoding
    // turned into 22/64/102/122.
    for (UINT channel = 0; channel < 3; ++channel) {
      if (c.srgb) {
        overlay_t[c.bgra][channel] = stored(0, 700, channel);
        camera_t[c.bgra][channel] = stored(0, 720, channel);
      } else {
        require(stored(0, 700, channel) == source_pixel(0, 700)[channel] && stored(0, 720, channel) == source_pixel(0, 720)[channel],
                "UNORM views store the T colour #1C1B22 exactly under both flags");
      }
    }
    if (c.srgb) {
      constexpr std::array<int, 4> dark{2, 13, 34, 50};
      for (UINT i = 0; i < dark.size(); ++i) {
        headline[c.bgra][i] = stored(dark[i], 0, 0);
        require(std::abs(headline[c.bgra][i] - dark[i]) <= 1, "sRGB views keep dark camera codes instead of encoding them again");
      }
      constexpr std::array<int, 3> t_encoded{93, 92, 102}, t_code{28, 27, 34};
      for (UINT channel = 0; channel < 3; ++channel)
        require(
            std::abs(overlay_t[c.bgra][channel] - t_encoded[channel]) <= 1 && std::abs(camera_t[c.bgra][channel] - t_code[channel]) <= 1,
            "sRGB views store the overlay T as #5D5C66 and a camera-flagged #1C1B22 unchanged");
    }
    readbacks[n]->Unmap(0, &none);
  }

  unsigned debug_errors = 0;
  if (info.value)
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      check(info->GetMessage(i, nullptr, &size), "debug message size");
      std::vector<unsigned char> storage(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      check(info->GetMessage(i, message, &size), "debug message");
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
        ++debug_errors;
        std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
      }
    }
  require(debug_errors == 0, "D3D12 debug layer errors");
  std::printf("PASS stamp encoding %s: debugLayer=%s debugErrors=%u\n", warp ? "WARP" : "hardware", debug_layer ? "true" : "false",
              debug_errors);
  for (UINT n = 0; n < Cases.size(); ++n)
    std::printf("  %-16s camera exact %llu/%llu, overlay exact %llu/%llu (channels within +-1 otherwise)\n", Cases[n].name,
                static_cast<unsigned long long>(counts[n].camera_exact), static_cast<unsigned long long>(counts[n].camera),
                static_cast<unsigned long long>(counts[n].overlay_exact), static_cast<unsigned long long>(counts[n].overlay));
  for (UINT bgra = 0; bgra < 2; ++bgra)
    std::printf(
        "  %s sRGB: camera 2/13/34/50 -> %d/%d/%d/%d (double encoding stores 22/64/102/122); overlay T -> %d/%d/%d; "
        "camera-flagged T -> %d/%d/%d\n",
        bgra ? "BGRA8" : "RGBA8", headline[bgra][0], headline[bgra][1], headline[bgra][2], headline[bgra][3], overlay_t[bgra][0],
        overlay_t[bgra][1], overlay_t[bgra][2], camera_t[bgra][0], camera_t[bgra][1], camera_t[bgra][2]);
}
}  // namespace

int main(int argc, char** argv) {
  bool warp = false;
  for (int n = 1; n < argc; ++n) {
    if (std::strcmp(argv[n], "--warp") == 0)
      warp = true;
    else
      return 2;
  }
  try {
    run(warp);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL stamp encoding: %s\n", error.what());
    return 1;
  }
}
