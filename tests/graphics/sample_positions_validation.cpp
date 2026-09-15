// Test-owned native command list only. The forwarding trace refuses a draw when
// custom sample count disagrees with these single-sample PSOs; no undefined GPU
// work is submitted. Microsoft SetSamplePositions documents this draw contract.
#define main depth_bias_fixture_main_not_called
#include "dynamic_state_validation.cpp"
#undef main
#include "../../src/bridge/native_hooks.hpp"
#include <algorithm>

namespace {
taxi_camera::standalone::NativeSlot sample_slot, draw_slot;
ID3D12GraphicsCommandList* traced_list{};
UINT samples{}, pixels{}, calls{}, invalid_draws{};
std::array<D3D12_SAMPLE_POSITION, 16> positions{};
void STDMETHODCALLTYPE sample_call(ID3D12GraphicsCommandList1* list, UINT count, UINT pixel_count, D3D12_SAMPLE_POSITION* values) {
  using F = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList1*, UINT, UINT, D3D12_SAMPLE_POSITION*);
  if (list == traced_list) {
    ++calls;
    samples = count;
    pixels = pixel_count;
    positions = {};
    if (values && count * pixel_count <= positions.size())
      std::copy_n(values, count * pixel_count, positions.begin());
  }
  sample_slot.forward<F>()(list, count, pixel_count, values);
}
void STDMETHODCALLTYPE draw_call(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances, UINT first, UINT instance) {
  using F = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
  if (list == traced_list) {
    if (samples && samples != 1) {
      ++invalid_draws;
      return;  // Contract failure is evidence, not permission to submit undefined work.
    }
  }
  draw_slot.forward<F>()(list, vertices, instances, first, instance);
}
std::vector<UINT> run_samples(Fixture& fixture, bool overlay, UINT sample_count) {
  Ref<ID3D12CommandAllocator> alloc;
  Ref<ID3D12GraphicsCommandList> list;
  Ref<ID3D12GraphicsCommandList1> list1;
  check(fixture.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put())), "allocator");
  check(fixture.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.value, nullptr, IID_PPV_ARGS(list.put())), "list");
  check(list->QueryInterface(IID_PPV_ARGS(list1.put())), "sample interface");
  require(list1.value == list.value, "sample interface identity");
  if (!sample_slot.address) {
    require(sample_slot.install(list.value, 63, reinterpret_cast<void*>(&sample_call)), "sample trace hook");
    require(draw_slot.install(list.value, 12, reinterpret_cast<void*>(&draw_call)), "draw trace hook");
  }
  traced_list = list.value;
  samples = pixels = 0;
  positions = {};
  Ref<ID3D12Resource> color, depth, readback;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_D32_FLOAT;
  clear.DepthStencil.Depth = 1;
  create(fixture.device.value, texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_HEAP_TYPE_DEFAULT,
         D3D12_RESOURCE_STATE_RENDER_TARGET, color.put());
  create(fixture.device.value, texture(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_HEAP_TYPE_DEFAULT,
         D3D12_RESOURCE_STATE_DEPTH_WRITE, depth.put(), &clear);
  create(fixture.device.value, buffer(64 * 64 * 4), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback.put());
  Ref<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.NumDescriptors = 1;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  check(fixture.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtvs.put())), "RTV heap");
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  check(fixture.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsvs.put())), "DSV heap");
  const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  fixture.device->CreateRenderTargetView(color.value, nullptr, rtv);
  fixture.device->CreateDepthStencilView(depth.value, nullptr, dsv);
  list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
  list->SetPipelineState(fixture.pipeline.value);
  list->SetGraphicsRootSignature(fixture.root.value);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT vp{0, 0, 64, 64, 0, 1};
  D3D12_RECT sc{0, 0, 64, 64};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &sc);
  taxi_camera::PfdGraphicsState state;
  state.reset(1, true);
  state.bind_pipeline(fixture.pipeline.value);
  taxi_camera::PfdRootLayout layout{};
  layout.valid = true;
  state.bind_root(fixture.root.value, 1, layout, true);
  state.topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  state.viewports(0, 1, &vp);
  state.scissors(0, 1, &sc);
  // Legal transient app state: prepare the next MSAA target before rebinding OM.
  // No application draw occurs until the sample count is made compatible again.
  std::array<D3D12_SAMPLE_POSITION, 4> pattern{{{-2, -6}, {6, -2}, {-6, 2}, {2, 6}}};
  list1->SetSamplePositions(sample_count, 1, pattern.data());
#ifndef TAXI_SAMPLE_POSITIONS_BASELINE
  state.sample_positions(list1.value, sample_count, 1, pattern.data());
  require(state.has_sample_positions() && state.can_restore(list.value), "custom sample state must be restorable");
  auto snapshot = state;
  snapshot.bind_pipeline(fixture.pipeline.value);
  require(snapshot.has_sample_positions(), "PSO assignment must preserve sample positions");
  snapshot.sample_positions(list1.value, 0, 4, reinterpret_cast<D3D12_SAMPLE_POSITION*>(1));
  require(!snapshot.complete(), "partial-zero samples must be refused");
  snapshot = state;
  snapshot.sample_positions(list1.value, 4, 0, reinterpret_cast<D3D12_SAMPLE_POSITION*>(1));
  require(!snapshot.complete(), "partial-zero pixels must be refused");
  snapshot = state;
  snapshot = state;
  snapshot.sample_positions(list1.value, 0, 0, nullptr);
  require(!snapshot.has_sample_positions() && snapshot.complete(), "canonical reset must restore default pattern");
  snapshot = state;
  snapshot.sample_positions(list1.value, 0, 0, pattern.data());
  require(!snapshot.complete(), "nonnull zero-count form must be refused");
  snapshot = state;
  snapshot.sample_positions(list1.value, 16, 4, pattern.data());
  require(!snapshot.complete(), "oversized pattern must be refused before reading it");
  snapshot = state;
  const D3D12_SAMPLE_POSITION invalid{8, 0};
  snapshot.sample_positions(list1.value, 1, 1, &invalid);
  require(!snapshot.complete(), "sample coordinates outside API range must be refused");
  snapshot = state;
  snapshot.sample_positions(reinterpret_cast<ID3D12GraphicsCommandList1*>(1), 1, 1, pattern.data());
  require(!snapshot.complete(), "foreign recording sample interface must be refused");
  snapshot = state;
  snapshot.reset(2, true);
  require(!snapshot.has_sample_positions(), "recording Reset must clear the prior pattern");
  if (overlay) {
    const D3D12_RECT invalid_rect{0, 0, 65, 64};
    const auto rejected_calls = calls;
    require(!fixture.stamp.record_buffer(list.value, state, fixture.device.value, fixture.pixels->GetGPUVirtualAddress(), 64, 64,
                                         &invalid_rect),
            "out-of-bounds stamp must be refused");
    require(calls == rejected_calls + 2 && samples == sample_count && pixels == 1 &&
                std::memcmp(positions.data(), pattern.data(), sample_count * sizeof(D3D12_SAMPLE_POSITION)) == 0,
            "declined stamp must leave the exact sample pattern intact");
  }
#endif
  const auto before = calls;
  if (overlay)
    require(fixture.stamp.record_buffer(list.value, state, fixture.device.value, fixture.pixels->GetGPUVirtualAddress(), 64, 64),
            "sample-position stamp");
  require(samples == sample_count && pixels == 1 &&
              std::memcmp(positions.data(), pattern.data(), sample_count * sizeof(D3D12_SAMPLE_POSITION)) == 0,
          "stamp must restore exact application sample pattern");
#ifndef TAXI_SAMPLE_POSITIONS_BASELINE
  require(calls == before + (overlay ? 2u : 0u), "stamp must normalize then restore exactly once");
#else
  require(calls == before, "baseline unexpectedly changed sample state");
#endif
  // Continue valid native work with no PSO/root/OM/viewport rebinding.
  list1->SetSamplePositions(0, 0, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.value, color.value, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = color.value;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = readback.value;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  submit(fixture.device.value, fixture.queue.value, list.value);
  void* bytes{};
  D3D12_RANGE range{0, 64 * 64 * 4};
  check(readback->Map(0, &range, &bytes), "sample readback");
  std::vector<UINT> result(64 * 64);
  std::memcpy(result.data(), bytes, result.size() * 4);
  D3D12_RANGE empty{};
  readback->Unmap(0, &empty);
  traced_list = nullptr;
  return result;
}
}  // namespace
int main(int argc, char** argv) {
  const bool warp = argc == 2 && std::strcmp(argv[1], "--warp") == 0;
  try {
    Fixture fixture;
    if (!fixture.initialize(warp))
      return 77;
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
    check(fixture.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)), "sample capability");
    if (!options.ProgrammableSamplePositionsTier) {
      std::puts("SKIP programmable sample positions unsupported");
      return 77;
    }
    for (UINT count : {1u, 4u}) {
      const auto off = run_samples(fixture, false, count), on = run_samples(fixture, true, count);
      require(off == on && std::all_of(on.begin(), on.end(), [](UINT pixel) { return pixel == 0xff00ff00u; }),
              "native draw pixels changed after sample-position stamp");
    }
#ifndef TAXI_SAMPLE_POSITIONS_BASELINE
    require(!invalid_draws, "fixed stamp violated PSO/sample-count contract");
#else
    require(invalid_draws == 1, "baseline did not reproduce one mismatched single-sample stamp");
#endif
    std::printf("PASS backend=%s sampleTier=%u invalidInjectedDraws=%u nativePixels=8192 invalidDrawsForwarded=0\n",
                warp ? "WARP" : "hardware", UINT(options.ProgrammableSamplePositionsTier), invalid_draws);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL sample positions: %s\n", error.what());
    return 1;
  }
}
