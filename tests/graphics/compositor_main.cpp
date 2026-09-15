#include "../support/graphics_fixture.hpp"

namespace {
using namespace taxi_camera::testing;
struct Source {
  UINT width;
  UINT height;
  DXGI_FORMAT resource_format;
  DXGI_FORMAT srv_format;
  bool bgra;
  bool srgb;
  D3D12_RESOURCE_STATES before;
  bool floating = false;
  bool packed_float = false;
  bool night = false;
};

std::array<float, 4> float_color(UINT frame, UINT source) {
  if (!frame)
    return source == 0 ? std::array<float, 4>{0.25f, 0.5f, 0.75f, 0.25f} : std::array<float, 4>{2, -0.25f, 1, 0.5f};
  return source == 0 ? std::array<float, 4>{0, 2, 0.125f, 0} : std::array<float, 4>{1, 0.75f, -1, 1};
}

std::array<float, 4> packed_color(UINT, UINT source, bool night = false) {
  if (night)
    return source == 0 ? std::array<float, 4>{0.0625f, 0.25f, 1, 0} : std::array<float, 4>{0.125f, 0.125f, 0.125f, 0};
  // Exactly representable HDR channels stay unchanged between recorded frames:
  // only exposure changes. Direct UNORM conversion would erase all nose color.
  return source == 0 ? std::array<float, 4>{64, 256, 1024, 0} : std::array<float, 4>{0, 0.0625f, 16, 0};
}

struct Result {
  bool warp = false;
  bool debug_layer = false;
  std::uint64_t debug_errors = 0;
  std::uint64_t frames = 0;
  std::uint64_t checked_pixels = 0;
  std::uint64_t divider_pixels = 0;
  std::uint64_t lower_pixels = 0;
  std::uint64_t rejection_checks = 0;
  std::uint64_t msaa_rejection_checks = 0;
  std::uint64_t float_pixels = 0;
  std::uint64_t packed_float_pixels = 0;
  std::uint64_t exposure_checks = 0;
  std::uint64_t magenta_pixels = 0;
  std::uint64_t night_rgb_checks = 0;
  Compositor::Statistics statistics;
};

// Independent pixel oracle: UNORM quantization at source rendering, optional
// SRGB decode at each source texel, then bilinear filtering and output UNORM.
// A two-code-value tolerance covers permitted GPU filtering/rounding precision.
double source_channel(const Source& source, UINT x, UINT y, UINT channel, UINT frame, UINT source_index) {
  if (source.packed_float)
    return packed_color(frame, source_index, source.night)[channel];
  if (source.floating)
    return float_color(frame, source_index)[channel];
  double value = 0;
  if (channel == 0)
    value = frame == 0 ? double(x) / (source.width - 1) : 1.0 - double(x) / (source.width - 1);
  else if (channel == 1)
    value = source_index == 0 ? double(y) / (source.height - 1) : 1.0 - double(y) / (source.height - 1);
  else
    value = frame == 0 ? (source_index == 0 ? 0.2 : 0.8) : (source_index == 0 ? 0.6 : 0.4);
  value = std::floor(value * 255.0 + 0.5) / 255.0;
  if (source.srgb)
    value = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
  return value;
}

unsigned char expected_channel(const Source& source,
                               UINT x,
                               UINT y,
                               UINT region_height,
                               UINT channel,
                               UINT frame,
                               UINT source_index,
                               float exposure_ev = Compositor::DefaultExposureEv) {
  const double source_x = std::clamp((double(x) + 0.5) / 768.0 * source.width - 0.5, 0.0, double(source.width - 1));
  const double source_y = std::clamp((double(y) + 0.5) / region_height * source.height - 0.5, 0.0, double(source.height - 1));
  const auto left = static_cast<UINT>(source_x);
  const auto top = static_cast<UINT>(source_y);
  const auto right = std::min(left + 1, source.width - 1);
  const auto bottom = std::min(top + 1, source.height - 1);
  const double tx = source_x - left;
  const double ty = source_y - top;
  const double first = source_channel(source, left, top, channel, frame, source_index) * (1 - tx) +
                       source_channel(source, right, top, channel, frame, source_index) * tx;
  const double second = source_channel(source, left, bottom, channel, frame, source_index) * (1 - tx) +
                        source_channel(source, right, bottom, channel, frame, source_index) * tx;
  double value = first * (1 - ty) + second * ty;
  if (source.packed_float) {
    // Independent double-precision SDR reference after linear filtering.
    value = std::max(value, 0.0) * std::exp2(double(exposure_ev));
    value /= 1.0 + value;
    value = value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
  }
  return static_cast<unsigned char>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

void refused_inputs(ID3D12Device* device, Compositor& compositor, ID3D12Resource* nose, ID3D12Resource* tail, Result& result) {
  const auto reject = [&](HRESULT status) {
    require(FAILED(status), "Invalid compositor input was accepted");
    ++result.rejection_checks;
  };
  reject(compositor.set_inputs(nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R8G8B8A8_UNORM, nose, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(compositor.output(), DXGI_FORMAT_R8G8B8A8_UNORM, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R8G8B8A8_UNORM, compositor.output(), DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_B8G8R8A8_UNORM, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R8G8B8A8_TYPELESS, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R16G16B16A16_FLOAT, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R11G11B10_FLOAT, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  reject(compositor.set_inputs(nose, DXGI_FORMAT_R8G8B8A8_UINT, tail, DXGI_FORMAT_R8G8B8A8_UNORM));

  Reference<ID3D12Resource> array_texture;
  auto description = texture_description(32, 32, DXGI_FORMAT_R8G8B8A8_UNORM);
  description.DepthOrArraySize = 2;
  create_texture(device, description, array_texture.put());
  reject(compositor.set_inputs(array_texture.get(), DXGI_FORMAT_R8G8B8A8_UNORM, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
  Reference<ID3D12Resource> texture_1d;
  description.DepthOrArraySize = 1;
  description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
  description.Height = 1;
  create_texture(device, description, texture_1d.put());
  reject(compositor.set_inputs(texture_1d.get(), DXGI_FORMAT_R8G8B8A8_UNORM, tail, DXGI_FORMAT_R8G8B8A8_UNORM));

  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS samples{DXGI_FORMAT_R8G8B8A8_UNORM, 2, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &samples, sizeof(samples))) &&
      samples.NumQualityLevels) {
    Reference<ID3D12Resource> msaa;
    description = texture_description(32, 32, DXGI_FORMAT_R8G8B8A8_UNORM);
    description.SampleDesc.Count = 2;
    create_texture(device, description, msaa.put());
    reject(compositor.set_inputs(msaa.get(), DXGI_FORMAT_R8G8B8A8_UNORM, tail, DXGI_FORMAT_R8G8B8A8_UNORM));
    ++result.msaa_rejection_checks;
  }
}

void pixel_case(ID3D12Device* device,
                Compositor& compositor,
                GradientGenerator& generator,
                const std::array<Source, 2>& source_descriptions,
                bool check_refusals,
                Result& result,
                const std::array<float, 2>& exposures = {-8, -4},
                bool overlay_case = false,
                bool round_nose = false) {
  auto layout = taxi_camera::profiles::A380.composition;
  if (round_nose)
    layout.square_nose_markers = taxi_camera::profiles::A359.composition.square_nose_markers;
  compositor.set_composition(layout);
  std::array<Reference<ID3D12Resource>, 2> sources;
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const auto& source = source_descriptions[index];
    auto description = texture_description(source.width, source.height, source.resource_format);
    // The second pair also proves that nonzero unselected mips can exist.
    if (source.width == 320)
      description.MipLevels = 2;
    create_texture(device, description, sources[index].put());
  }
  if (check_refusals)
    refused_inputs(device, compositor, sources[0].get(), sources[1].get(), result);
  if (source_descriptions[0].packed_float) {
    for (const auto invalid_view : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT}) {
      require(FAILED(compositor.set_inputs(sources[0].get(), invalid_view, sources[1].get(), DXGI_FORMAT_R11G11B10_FLOAT)),
              "Packed RGB resource accepted an unrelated same-size typed SRV");
      ++result.rejection_checks;
    }
    Reference<ID3D12Resource> unrelated;
    create_texture(device, texture_description(17, 11, DXGI_FORMAT_R32_TYPELESS), unrelated.put());
    require(FAILED(compositor.set_inputs(unrelated.get(), DXGI_FORMAT_R11G11B10_FLOAT, sources[1].get(), DXGI_FORMAT_R11G11B10_FLOAT)),
            "R32 typeless resource was treated as a packed RGB format family");
    ++result.rejection_checks;
  }
  const auto writes_before = compositor.statistics().descriptor_writes;
  check(compositor.set_inputs(sources[0].get(), source_descriptions[0].srv_format, sources[1].get(), source_descriptions[1].srv_format),
        compositor.last_error());
  require(compositor.statistics().descriptor_writes == writes_before + 2, "An input change did not write exactly two SRVs");
  require(compositor.set_inputs(sources[0].get(), source_descriptions[0].srv_format, sources[1].get(), source_descriptions[1].srv_format) ==
                  S_FALSE &&
              compositor.statistics().descriptor_writes == writes_before + 2,
          "Unchanged inputs rewrote GPU descriptors");

  Reference<ID3D12Resource> pfd;
  const auto pfd_description = texture_description(768, 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
  create_texture(device, pfd_description, pfd.put());
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = 3;
  Reference<ID3D12DescriptorHeap> rtvs;
  check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtvs.put())), "Create generator/PFD RTVs");
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> handles{};
  for (std::size_t index = 0; index < handles.size(); ++index) {
    handles[index].ptr = rtvs->GetCPUDescriptorHandleForHeapStart().ptr + index * stride;
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = index == 2                            ? DXGI_FORMAT_R8G8B8A8_UNORM
                  : source_descriptions[index].floating ? source_descriptions[index].srv_format
                  : source_descriptions[index].bgra     ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                        : DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(index == 2 ? pfd.get() : sources[index].get(), &view, handles[index]);
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&pfd_description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  std::array<Reference<ID3D12Resource>, 2> readbacks;
  auto buffer = D3D12_RESOURCE_DESC{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = bytes;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  for (auto& readback : readbacks)
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readback.put())),
          "Create test-only readback");

  PrivateSubmission submission(device);
  auto* list = submission.list();
  if (check_refusals) {
    Reference<ID3D12CommandAllocator> copy_allocator;
    Reference<ID3D12GraphicsCommandList> copy_list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(copy_allocator.put())),
          "Create refusal-test copy allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_allocator.get(), nullptr, IID_PPV_ARGS(copy_list.put())),
          "Create refusal-test copy list");
    require(FAILED(compositor.record(copy_list.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET)),
            "A non-direct command list accepted a graphics composition");
    check(copy_list->Close(), "Close untouched refusal-test copy list");
    ++result.rejection_checks;
  }
  require(FAILED(compositor.record(nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET)), "Null list accepted");
  ++result.rejection_checks;
  require(FAILED(compositor.record(list, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_RENDER_TARGET)),
          "Invalid before state accepted");
  ++result.rejection_checks;
  constexpr std::array<float, 4> Sentinel{17.0f / 255.0f, 34.0f / 255.0f, 51.0f / 255.0f, 1.0f};
  list->ClearRenderTargetView(handles[2], Sentinel.data(), 0, nullptr);
  transition(list, pfd.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
  for (UINT frame = 0; frame < 2; ++frame) {
    const auto exposure_statistics = compositor.statistics();
    compositor.set_reference_guides(overlay_case && frame == 0);
    compositor.set_ground_speed(frame == 0 ? 11.f : std::numeric_limits<float>::quiet_NaN(), overlay_case);
    require(compositor.set_display_exposure(exposures[frame]) && compositor.display_exposure() == exposures[frame],
            "Valid exposure was not applied to the next recording");
    require(compositor.statistics().shader_compiles == exposure_statistics.shader_compiles &&
                compositor.statistics().descriptor_writes == exposure_statistics.descriptor_writes,
            "Exposure changed shaders or descriptors");
    ++result.exposure_checks;
    for (UINT source_index = 0; source_index < 2; ++source_index) {
      const auto& source = source_descriptions[source_index];
      if (source.floating) {
        const auto color = source.packed_float ? packed_color(frame, source_index, source.night) : float_color(frame, source_index);
        list->ClearRenderTargetView(handles[source_index], color.data(), 0, nullptr);
      } else {
        generator.record(list, handles[source_index], source.width, source.height, source.bgra, frame, source_index);
      }
      transition(list, sources[source_index].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, source.before);
    }
    const auto statistics = compositor.statistics();
    check(compositor.record(list, source_descriptions[0].before, source_descriptions[1].before), compositor.last_error());
    require(compositor.statistics().shader_compiles == statistics.shader_compiles &&
                compositor.statistics().descriptor_writes == statistics.descriptor_writes &&
                compositor.statistics().input_changes == statistics.input_changes,
            "A recorded frame rebuilt compositor shaders or descriptors");
    for (UINT source_index = 0; source_index < 2; ++source_index)
      transition(list, sources[source_index].get(), source_descriptions[source_index].before, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_TEXTURE_COPY_LOCATION output{};
    output.pResource = compositor.output();
    output.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = pfd.get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX upper{0, 0, 0, 768, 763, 1};
    list->CopyTextureRegion(&destination, 0, 0, 0, &output, &upper);
    transition(list, pfd.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION readback{};
    readback.pResource = readbacks[frame].get();
    readback.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readback.PlacedFootprint = footprint;
    list->CopyTextureRegion(&readback, 0, 0, 0, &destination, nullptr);
    if (frame == 0)
      transition(list, pfd.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  // Both changed frames were recorded before submission; the first snapshot
  // catches accidental descriptor reuse or deferred mutable per-frame contents.
  submission.finish(compositor);
  constexpr std::array<unsigned char, 4> SentinelBytes{17, 34, 51, 255};
  std::array<unsigned char, 3> night_day_pixel{};
  for (UINT frame = 0; frame < 2; ++frame) {
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readbacks[frame]->Map(0, &range, &mapped), "Map test-only composition readback");
    if (source_descriptions[0].night) {
      const auto* nose =
          static_cast<const unsigned char*>(mapped) + footprint.Offset + UINT64(127) * footprint.Footprint.RowPitch + 4 * 384;
      const auto* tail =
          static_cast<const unsigned char*>(mapped) + footprint.Offset + UINT64(511) * footprint.Footprint.RowPitch + 4 * 384;
      require(nose[0] < nose[1] && nose[1] < nose[2], "Low-light RGB channel differences were lost");
      require(tail[0] == tail[1] && tail[1] == tail[2], "Exposure invented colour in a neutral source");
      if (frame == 0)
        std::copy_n(nose, 3, night_day_pixel.begin());
      else {
        require(nose[0] > 0, "Night boost left a represented dim RGB channel black");
        for (unsigned channel = 0; channel < 3; ++channel)
          require(nose[channel] > night_day_pixel[channel], "Night boost did not brighten each existing RGB channel");
      }
      ++result.night_rgb_checks;
    }
    if (overlay_case && frame == 0) {
      const auto is_magenta = [&](UINT x, UINT y) {
        const auto* pixel = static_cast<const unsigned char*>(mapped) + footprint.Offset + UINT64(y) * footprint.Footprint.RowPitch + 4 * x;
        return pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255;
      };
      require(is_magenta(107, 122) && is_magenta(660, 122) && !is_magenta(107, 109) && !is_magenta(120, 122),
              "Nose reference square centres or outside edges are incorrect");
      unsigned nose_pixels = 0;
      for (UINT y = 100; y < 145; ++y)
        for (UINT x = 90; x < 678; ++x)
          if (is_magenta(x, y)) {
            if (!round_nose)
              require(((x >= 101 && x < 115) || (x >= 653 && x < 667)) && y >= 115 && y < 129,
                      "Nose marker exceeds its centred 14px square");
            ++nose_pixels;
          }
      if (round_nose)
        require(nose_pixels == 226 && !is_magenta(100, 114) && !is_magenta(115, 129),
                "A350 nose markers must be 12-pixel-diameter circles");
      else
        require(nose_pixels == 392, "Both A380 nose markers must be filled 14-by-14 squares");
      require(is_magenta(234, 637) && is_magenta(533, 637) && is_magenta(253, 582) && is_magenta(514, 582) && is_magenta(279, 641) &&
                  is_magenta(488, 641) && !is_magenta(245, 631),
              "Tail reference brackets missed the approved calibration landmarks");
    }
    reference_overlay_oracle::FontCoverage font_coverage;
    for (UINT y = 0; y < 1024; ++y) {
      const auto* row = static_cast<const unsigned char*>(mapped) + footprint.Offset + UINT64(y) * footprint.Footprint.RowPitch;
      for (UINT x = 0; x < 768; ++x) {
        const int font_cell = reference_overlay_oracle::font_cell(x, y, overlay_case && frame == 0, 11);
        if (font_cell >= 0) {
          require(font_coverage.observe(font_cell, row + x * 4), "GS font lost its opaque white-label/green-value colour contract");
          ++result.checked_pixels;
          continue;
        }
        std::array<unsigned char, 4> expected{0, 0, 0, 255};
        int tolerance = 0;
        if (y >= 763) {
          expected = SentinelBytes;
          ++result.lower_pixels;
        } else if (reference_overlay_oracle::pixel(x, y, expected, overlay_case && frame == 0, overlay_case && frame == 0, 11,
                                                   round_nose)) {
          if (y >= 251 && y < 263)
            ++result.divider_pixels;
          if (expected == std::array<unsigned char, 4>{255, 0, 255, 255})
            ++result.magenta_pixels;
        } else {
          const UINT source_index = y < 255 ? 0 : 1;
          const UINT source_y = source_index == 0 ? y : y - 259;
          const UINT region_height = source_index == 0 ? 255 : 504;
          if (source_descriptions[source_index].packed_float)
            ++result.packed_float_pixels;
          else if (source_descriptions[source_index].floating)
            ++result.float_pixels;
          for (UINT channel = 0; channel < 3; ++channel)
            expected[channel] = expected_channel(source_descriptions[source_index], x, source_y, region_height, channel, frame,
                                                 source_index, exposures[frame]);
          tolerance = 2;
        }
        for (UINT channel = 0; channel < 4; ++channel) {
          if (std::abs(int(row[x * 4 + channel]) - int(expected[channel])) > (channel == 3 ? 0 : tolerance)) {
            char message[256]{};
            std::snprintf(message, sizeof(message), "Composition mismatch frame=%u x=%u y=%u channel=%u got=%u expected=%u", frame, x, y,
                          channel, unsigned(row[x * 4 + channel]), unsigned(expected[channel]));
            throw std::runtime_error(message);
          }
        }
        ++result.checked_pixels;
      }
    }
    require(font_coverage.complete(overlay_case && frame == 0, 11), "GS glyphs are missing, filled rectangles or missing antialiasing");
    const D3D12_RANGE no_writes{0, 0};
    readbacks[frame]->Unmap(0, &no_writes);
    ++result.frames;
  }
}

Result run(bool force_warp) {
  Result result;
  Reference<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
    debug->EnableDebugLayer();
    result.debug_layer = true;
  }
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "CreateDXGIFactory2");
  Reference<ID3D12Device> device;
  HRESULT status = E_FAIL;
  if (!force_warp)
    status = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()));
  Reference<IDXGIAdapter> warp;
  if (FAILED(status)) {
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put())), "EnumWarpAdapter");
    check(D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "Create WARP device");
    result.warp = true;
  }
  Reference<ID3D12InfoQueue> messages;
  if (result.debug_layer && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(messages.put()))))
    messages->ClearStoredMessages();
  Compositor compositor;
  require(compositor.reference_guides(), "Reference guides must default on");
  require(compositor.display_exposure() == Compositor::DefaultExposureEv, "Unexpected default exposure");
  require(!compositor.set_display_exposure(std::numeric_limits<float>::quiet_NaN()) &&
              !compositor.set_display_exposure(std::numeric_limits<float>::infinity()) &&
              !compositor.set_display_exposure(-std::numeric_limits<float>::infinity()) &&
              compositor.display_exposure() == Compositor::DefaultExposureEv,
          "Nonfinite exposure was accepted or changed the previous exposure");
  require(compositor.set_display_exposure(-99) && compositor.display_exposure() == -16 && compositor.set_display_exposure(99) &&
              compositor.display_exposure() == 4,
          "Exposure clamp failed");
  result.exposure_checks += 3;
  require(FAILED(compositor.initialize(nullptr)), "Null device accepted");
  check(compositor.initialize(device.get()), compositor.last_error());
  require(compositor.initialize(device.get()) == S_FALSE, "Repeated initialization rebuilt the compositor");
  auto* output = compositor.output();
  GradientGenerator generator(device.get());
  const std::array<std::array<Source, 2>, 5> pairs{{
      {{{17, 11, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, false, D3D12_RESOURCE_STATE_RENDER_TARGET},
        {29, 19, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, false, D3D12_RESOURCE_STATE_RENDER_TARGET}}},
      {{{320, 181, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, true, false, D3D12_RESOURCE_STATE_COPY_SOURCE},
        {191, 127, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, false,
         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}}},
      {{{31, 15, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, true, D3D12_RESOURCE_STATE_COPY_DEST},
        {41, 21, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true, true, D3D12_RESOURCE_STATE_COMMON}}},
      {{{23, 17, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, D3D12_RESOURCE_STATE_COPY_DEST, true},
        {37, 29, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, D3D12_RESOURCE_STATE_COPY_SOURCE, true}}},
      {{{768, 255, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, false, false, D3D12_RESOURCE_STATE_COPY_DEST, true, true},
        {768, 504, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, false, false, D3D12_RESOURCE_STATE_COPY_SOURCE, true, true}}},
  }};
  for (std::size_t index = 0; index < pairs.size(); ++index) {
    pixel_case(device.get(), compositor, generator, pairs[index], index == 0, result);
    require(compositor.output() == output, "Output texture was reallocated between frames/input changes");
  }
  pixel_case(device.get(), compositor, generator, pairs.back(), false, result, {-16, 4});
  // Mixed formats prove the HDR flag is per feed, while exposure changes leave
  // the UNORM channel oracle and lower 261 rows unchanged.
  const std::array<Source, 2> mixed{pairs[0][0], pairs.back()[1]};
  pixel_case(device.get(), compositor, generator, mixed, false, result);
  // Record on/off states before one submit. Pixel oracle covers magenta
  // dots/bracket landmarks, mirror symmetry, divider, opaque GS11/'--' glyph coverage, and
  // every unmarked camera/lower-trim pixel. No descriptor or shader changes.
  pixel_case(device.get(), compositor, generator, pairs[0], false, result, {-8, -8}, true);
  pixel_case(device.get(), compositor, generator, pairs[0], false, result, {-8, -8}, true, true);
  auto night = pairs.back();
  night[0].night = night[1].night = true;
  pixel_case(device.get(), compositor, generator, night, false, result,
             {Compositor::DefaultExposureEv, Compositor::DefaultExposureEv + taxi_camera::DisplayExposureController::DefaultNightBoostEv});
  result.statistics = compositor.statistics();
  require(result.statistics.shader_compiles == 2 && result.statistics.descriptor_writes == 21 && result.statistics.input_changes == 10 &&
              result.statistics.recordings == 20 && result.night_rgb_checks == 2 && result.float_pixels > 1000000 &&
              result.packed_float_pixels > 2900000 && result.magenta_pixels > 400,
          "Unexpected compositor rebuild, descriptor update or recording count");
  compositor.release();
  if (messages.get()) {
    for (UINT64 index = 0; index < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
      SIZE_T bytes = 0;
      check(messages->GetMessage(index, nullptr, &bytes), "Get debug message size");
      std::vector<unsigned char> storage(bytes);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      check(messages->GetMessage(index, message, &bytes), "Get debug message");
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
        ++result.debug_errors;
        std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
      }
    }
  }
  require(result.debug_errors == 0, "D3D12 validation reported errors");
  return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--warp"), "Usage: compositor-validation.exe [--warp]");
    const auto result = run(argc == 2);
    std::printf(
        "{\"passed\":true,\"warp\":%s,\"debugLayer\":%s,\"debugErrors\":%llu,\"frames\":%llu,"
        "\"checkedPixels\":%llu,\"dividerPixels\":%llu,\"preservedLowerPixels\":%llu,\"rejectionChecks\":%llu,"
        "\"msaaRejectionChecks\":%llu,\"shaderCompiles\":%llu,\"descriptorWrites\":%llu,\"inputChanges\":%llu,\"floatPixels\":%llu,"
        "\"packedFloatPixels\":%llu,\"exposureChecks\":%llu,\"magentaPixels\":%llu,\"nightRgbChecks\":%llu,\"referenceOverlay\":true,"
        "\"hdrDisplayConversion\":true}"
        "\n",
        result.warp ? "true" : "false", result.debug_layer ? "true" : "false", static_cast<unsigned long long>(result.debug_errors),
        static_cast<unsigned long long>(result.frames), static_cast<unsigned long long>(result.checked_pixels),
        static_cast<unsigned long long>(result.divider_pixels), static_cast<unsigned long long>(result.lower_pixels),
        static_cast<unsigned long long>(result.rejection_checks), static_cast<unsigned long long>(result.msaa_rejection_checks),
        static_cast<unsigned long long>(result.statistics.shader_compiles),
        static_cast<unsigned long long>(result.statistics.descriptor_writes),
        static_cast<unsigned long long>(result.statistics.input_changes), static_cast<unsigned long long>(result.float_pixels),
        static_cast<unsigned long long>(result.packed_float_pixels), static_cast<unsigned long long>(result.exposure_checks),
        static_cast<unsigned long long>(result.magenta_pixels), static_cast<unsigned long long>(result.night_rgb_checks));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
