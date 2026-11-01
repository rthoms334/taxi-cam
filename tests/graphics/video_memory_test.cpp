#include "../../src/graphics/video_memory.hpp"
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void record_format() {
  taxi_camera::VideoMemoryRecord record;
  char line[256];
  require(record.format(line, sizeof(line), "adapter_not_found") > 0 && std::strstr(line, "unavailable reason=adapter_not_found"),
          "Missing sample did not report its reason");
  constexpr std::uint64_t MiB = 1024 * 1024;
  record.add({true, 15000 * MiB, 14800 * MiB, 4000 * MiB, 15600 * MiB});
  record.add({true, 14500 * MiB, 14800 * MiB, 3000 * MiB, 15600 * MiB});
  require(record.format(line, sizeof(line), "") > 0, "Formatting failed");
  require(std::strstr(line, "local_mb=14500/14800 nonlocal_mb=3000/15600 over_budget=0 peak_local_mb=15000 peak_nonlocal_mb=4000"),
          "Latest, budget and peaks not reported");
  require(record.format(line, sizeof(line), "") > 0 && std::strstr(line, "peak_local_mb=14500 peak_nonlocal_mb=3000"),
          "Peaks did not restart after a record");
  record.add({true, 15000 * MiB, 14800 * MiB, 0, 15600 * MiB});
  require(record.format(line, sizeof(line), "") > 0 && std::strstr(line, "over_budget=1"), "Over-budget usage not flagged");
  // A failed query is reported with its reason; the last good figures are not
  // repeated as current, and the watchdog's copies read zero.
  record.add({});
  require(record.format(line, sizeof(line), "query_failed") > 0 && std::strstr(line, "unavailable reason=query_failed"),
          "Failed query repeated the last good sample");
  require(!record.latest().valid && record.latest().local_usage == 0, "Failed query left the latest figures in place");
}

// The probe opens the adapter behind a real D3D12 device by LUID, as the
// bridge does, and reads a budget. WARP keeps this independent of the GPU.
void warp_probe() {
  IDXGIFactory4* factory{};
  require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
  IDXGIAdapter* warp{};
  require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
  ID3D12Device* device{};
  require(SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
  taxi_camera::VideoMemoryProbe probe;
  require(!probe.ready() && !probe.sample().valid, "Unopened probe sampled");
  require(probe.open(device->GetAdapterLuid()) && probe.ready(), "Probe did not open the device's adapter");
  require(probe.open(device->GetAdapterLuid()) && probe.attempts() == 1, "Reopening the same adapter opened it again");
  const auto sample = probe.sample();
  require(sample.valid && sample.local_budget > 0, "No video-memory budget reported");
  taxi_camera::VideoMemoryProbe missing;
  require(!missing.open(LUID{0x7fffffff, 0x7fffffff}) && std::strcmp(missing.error(), "adapter_not_found") == 0,
          "Unknown adapter was not refused");
  require(missing.attempts() == 1, "Unknown adapter attempt not counted");
  require(!missing.open(LUID{0x7fffffff, 0x7fffffff}) && missing.attempts() == 1, "Failed adapter was retried");
  device->Release();
  warp->Release();
  factory->Release();
}
}  // namespace

int main() {
  try {
    record_format();
    warp_probe();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "video memory: %s\n", error.what());
    return 1;
  }
  std::printf("video memory: %u checks passed\n", checks);
  return 0;
}
