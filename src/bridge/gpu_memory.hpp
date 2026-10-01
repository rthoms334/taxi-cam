#pragma once

#include <dxgi1_4.h>
#include <windows.h>

#include <cstdint>

namespace taxi_camera::standalone {

// Diagnostics only: this process's (the simulator's) video memory use against
// the budget Windows gives it, on the hardware adapter with the most dedicated
// memory. The bridge does not know the simulator's adapter LUID, so a system
// with several hardware GPUs may report the wrong one. Read-only DXGI queries
// on the bridge worker thread: no device, swap chain or simulator object.
struct GpuMemorySample {
  bool valid = false;
  std::uint64_t local_usage = 0, local_budget = 0, nonlocal_usage = 0, nonlocal_budget = 0;
};

class GpuMemoryProbe {
 public:
  GpuMemoryProbe() = default;
  GpuMemoryProbe(const GpuMemoryProbe&) = delete;
  GpuMemoryProbe& operator=(const GpuMemoryProbe&) = delete;
  ~GpuMemoryProbe() {
    if (adapter_)
      adapter_->Release();
  }
  GpuMemorySample sample() noexcept {
    GpuMemorySample out;
    if (!adapter_ && !tried_) {
      tried_ = true;
      adapter_ = largest_hardware_adapter();
    }
    if (!adapter_)
      return out;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
      return out;
    out = {true, local.CurrentUsage, local.Budget, nonlocal.CurrentUsage, nonlocal.Budget};
    return out;
  }

 private:
  static IDXGIAdapter3* largest_hardware_adapter() noexcept {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
      return nullptr;
    IDXGIAdapter3* best = nullptr;
    SIZE_T best_memory = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      IDXGIAdapter3* candidate = nullptr;
      if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > best_memory &&
          SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&candidate)))) {
        if (best)
          best->Release();
        best = candidate;
        best_memory = desc.DedicatedVideoMemory;
      }
      adapter->Release();
    }
    factory->Release();
    return best;
  }
  IDXGIAdapter3* adapter_ = nullptr;
  bool tried_ = false;
};

}  // namespace taxi_camera::standalone
