// Own-device pixel and spacing checks for the composed GS panel.
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
#define wmain compositor_validation_entry_not_used
#include "compositor_main.cpp"
#undef wmain
#include <set>

int wmain(int argc, wchar_t** argv) {
  try {
    bool warp = false;
    const wchar_t* preview = nullptr;
    for (int i = 1; i < argc; ++i) {
      if (!std::wcscmp(argv[i], L"--warp"))
        warp = true;
      else if (!std::wcscmp(argv[i], L"--preview") && i + 1 < argc)
        preview = argv[++i];
      else
        return 2;
    }
    Reference<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Font factory");
    Reference<IDXGIAdapter> adapter;
    if (warp)
      check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Font WARP");
    Reference<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Font device");
    Compositor compositor;
    check(compositor.initialize(device.get()), compositor.last_error());
    compositor.set_reference_guides(false);
    std::array<Reference<ID3D12Resource>, 2> sources;
    for (auto& source : sources)
      create_texture(device.get(), texture_description(16, 16, DXGI_FORMAT_R8G8B8A8_UNORM), source.put());
    check(compositor.set_inputs(sources[0].get(), DXGI_FORMAT_R8G8B8A8_UNORM, sources[1].get(), DXGI_FORMAT_R8G8B8A8_UNORM),
          "Font sources");
    Reference<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Font RTVs");
    const auto base = heap->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    PrivateSubmission submission(device.get());
    for (unsigned i = 0; i < 2; ++i) {
      D3D12_CPU_DESCRIPTOR_HANDLE handle{base.ptr + i * stride};
      device->CreateRenderTargetView(sources[i].get(), nullptr, handle);
      constexpr float background[]{.125f, .25f, .375f, 1};
      submission.list()->ClearRenderTargetView(handle, background, 0, nullptr);
    }
    constexpr std::array<float, 18> speeds{0, 1,  2,  3,  4,  5,      6,     7,   8,
                                           9, 11, 42, 60, 99, 99.49f, 99.5f, 100, std::numeric_limits<float>::quiet_NaN()};
    constexpr unsigned Width = 144, Height = 56, Pitch = 768, Profiles = 3, Cases = speeds.size(), Frames = Cases * Profiles;
    constexpr UINT64 FrameBytes = Pitch * Height;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = Frames * FrameBytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Reference<ID3D12Resource> readback;
    const auto rh = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readback.put())),
          "Font readback");
    for (unsigned profile = 0; profile < Profiles; ++profile) {
      compositor.set_composition(taxi_camera::profiles::Catalog[profile]->composition);
      for (unsigned sample = 0; sample < Cases; ++sample) {
        compositor.set_ground_speed(speeds[sample], true);
        check(compositor.record(submission.list(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET),
              compositor.last_error());
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = compositor.output();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = {(profile * Cases + sample) * FrameBytes, {DXGI_FORMAT_R8G8B8A8_UNORM, Width, Height, 1, Pitch}};
        const D3D12_BOX region{0, 0, 0, Width, Height, 1};
        submission.list()->CopyTextureRegion(&to, 0, 0, 0, &from, &region);
      }
    }
    submission.finish(compositor);
    void* mapped{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bd.Width)};
    check(readback->Map(0, &range, &mapped), "Font pixels");
    const auto* data = static_cast<const unsigned char*>(mapped);
    std::uint64_t gap_pixels = 0, antialiased = 0;
    std::set<std::vector<unsigned char>> digit_shapes;
    for (unsigned profile = 0; profile < Profiles; ++profile) {
      for (unsigned sample = 0; sample < Cases; ++sample) {
        const bool valid = sample < 15;
        const unsigned count = !valid || speeds[sample] >= 9.5f ? 2 : 1;
        const unsigned panel_right = 16 + 80 + 16 * count;
        const auto* pixels = data + (profile * Cases + sample) * FrameBytes;
        std::array<unsigned, 4> lit{};
        std::vector<unsigned char> shape;
        for (unsigned y = 0; y < Height; ++y)
          for (unsigned x = 0; x < Width; ++x) {
            const auto* p = pixels + y * Pitch + x * 4;
            require(p[3] == 255, "GS panel and surroundings remain opaque");
            const bool panel = x >= 16 && x < panel_right && y >= 12 && y < 48;
            int cell = -1;
            if (y >= 20 && y < 40) {
              if (x >= 24 && x < 36)
                cell = 0;
              if (x >= 40 && x < 52)
                cell = 1;
              if (x >= 88 && x < 100)
                cell = 2;
              if (count == 2 && x >= 104 && x < 116)
                cell = 3;
            }
            if (cell >= 0) {
              if (cell < 2)
                require(p[0] == p[1] && p[1] == p[2], "GS label white antialias");
              else {
                const auto colour = taxi_camera::profiles::Catalog[profile]->composition.speed_color;
                require(p[0] == 0 && p[1] <= unsigned(std::lround(255 * colour[1])) &&
                            std::abs(int(p[2]) - int(std::lround(p[1] * colour[2] / colour[1]))) <= 1,
                        "GS speed retains configured colour");
                if (!profile && sample < 10 && cell == 2)
                  shape.push_back(p[1]);
              }
              lit[cell] += p[1] > 32;
              antialiased += p[1] > 0 && p[1] < 230;
            } else if (panel) {
              require(p[0] == 0 && p[1] == 0 && p[2] == 0, "GS padding and two blank character cells stay black");
              if (x >= 56 && x < 88 && y >= 20 && y < 40)
                ++gap_pixels;
            } else
              require(p[0] == 32 && p[1] == 64 && p[2] == 96, "GS font stays inside its padded panel");
          }
        require(lit[0] > 35 && lit[1] > 35 && lit[2] > 8 && (count == 1 || lit[3] > 8), "Each GS character remains visible");
        require(lit[0] < 170 && lit[1] < 170, "Thin GS strokes replace enlarged solid bitmap blocks");
        if (!profile && sample < 10)
          digit_shapes.insert(shape);
      }
    }
    // Compare composed numbers with the separately rendered digit cells, so
    // coverage alone cannot pass a wrong numeral, missing digit or overflow.
    for (unsigned sample = 0; sample < 15; ++sample) {
      const unsigned value = static_cast<unsigned>(std::floor(speeds[sample] + .5f));
      const unsigned count = value >= 10 ? 2 : 1;
      for (unsigned n = 0; n < count; ++n) {
        const unsigned digit = n == 0 && count == 2 ? value / 10 : value % 10;
        for (unsigned y = 20; y < 40; ++y)
          for (unsigned x = 0; x < 12; ++x) {
            const auto expected = data[digit * FrameBytes + y * Pitch + (88 + x) * 4 + 1];
            const auto actual = data[sample * FrameBytes + y * Pitch + (88 + n * 16 + x) * 4 + 1];
            require(std::abs(int(expected) - int(actual)) <= 1, "Rounded speed uses the correct numeral in each cell");
          }
      }
    }
    for (unsigned sample = 15; sample < Cases; ++sample)
      for (unsigned y = 20; y < 40; ++y)
        for (unsigned x = 0; x < 12; ++x)
          require(data[sample * FrameBytes + y * Pitch + (88 + x) * 4 + 1] == data[sample * FrameBytes + y * Pitch + (104 + x) * 4 + 1],
                  "Unavailable and overflow values display two matching dashes");
    require(digit_shapes.size() == 10 && antialiased > 100, "All ten digits distinct with antialiased stroke edges");
    if (preview) {
      FILE* file = _wfopen(preview, L"wb");
      require(file != nullptr, "Open GS font preview");
      std::fprintf(file, "P6\n%u %u\n255\n", Width * Profiles, Height * Cases);
      for (unsigned sample = 0; sample < Cases; ++sample)
        for (unsigned y = 0; y < Height; ++y)
          for (unsigned profile = 0; profile < Profiles; ++profile)
            for (unsigned x = 0; x < Width; ++x) {
              const auto* p = data + (profile * Cases + sample) * FrameBytes + y * Pitch + x * 4;
              require(std::fwrite(p, 1, 3, file) == 3, "Write GS font preview");
            }
      require(std::fclose(file) == 0, "Close GS font preview");
    }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    std::printf(
        "PASS GS font %s: %u cases, 10 distinct digits, %llu two-space gap pixels, %llu antialiased pixels; three profiles and overflow "
        "'--'.\n",
        warp ? "WARP" : "hardware", Frames, gap_pixels, antialiased);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL GS font: %s\n", e.what());
    return 1;
  }
}
