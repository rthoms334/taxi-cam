#pragma once

#include <dxgi1_4.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace taxi_camera {

// The simulator process's GPU memory use against the budget Windows gives it
// (IDXGIAdapter3::QueryVideoMemoryInfo). Local is the card's own memory;
// non-local is system memory the card reaches over PCIe. Usage above the local
// budget is what Windows demotes to non-local, so a stall while over budget
// points at memory pressure. The figures cover the whole process: MSFS, its
// add-ons and Taxi Cam together, not Taxi Cam alone.
//
// Bridge worker thread only: the adapter is opened once by LUID and queried at
// most once a second. Never call from a simulator or watchdog thread.
struct VideoMemorySample {
  bool valid = false;
  std::uint64_t local_usage = 0, local_budget = 0, nonlocal_usage = 0, nonlocal_budget = 0;
};

class VideoMemoryProbe {
 public:
  VideoMemoryProbe() = default;
  VideoMemoryProbe(const VideoMemoryProbe&) = delete;
  VideoMemoryProbe& operator=(const VideoMemoryProbe&) = delete;
  ~VideoMemoryProbe() { reset(); }

  // One attempt per adapter: a missing interface is not retried every second.
  bool open(LUID luid) noexcept {
    if (adapter_ && luid.LowPart == luid_.LowPart && luid.HighPart == luid_.HighPart)
      return true;
    if (attempted_ && luid.LowPart == luid_.LowPart && luid.HighPart == luid_.HighPart)
      return false;
    reset();
    attempted_ = true;
    luid_ = luid;
    IDXGIFactory4* factory{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
      return fail("dxgi_factory_unavailable");
    const HRESULT found = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_));
    factory->Release();
    if (FAILED(found)) {
      adapter_ = nullptr;
      return fail("adapter_not_found");
    }
    error_ = "";
    return true;
  }

  VideoMemorySample sample() noexcept {
    VideoMemorySample out;
    if (!adapter_)
      return out;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
      error_ = "query_failed";
      return out;
    }
    out = {true, local.CurrentUsage, local.Budget, nonlocal.CurrentUsage, nonlocal.Budget};
    return out;
  }

  bool ready() const noexcept { return adapter_ != nullptr; }
  const char* error() const noexcept { return error_; }

 private:
  bool fail(const char* error) noexcept {
    error_ = error;
    return false;
  }
  void reset() noexcept {
    if (adapter_)
      adapter_->Release();
    adapter_ = nullptr;
    attempted_ = false;
    error_ = "not_opened";
  }
  IDXGIAdapter3* adapter_ = nullptr;
  LUID luid_{};
  bool attempted_ = false;
  const char* error_ = "not_opened";
};

// Latest sample plus the peaks since the last log record.
class VideoMemoryRecord {
 public:
  void add(const VideoMemorySample& sample) noexcept {
    if (!sample.valid)
      return;
    latest_ = sample;
    peak_local_ = std::max(peak_local_, sample.local_usage);
    peak_nonlocal_ = std::max(peak_nonlocal_, sample.nonlocal_usage);
  }
  // "Video memory: ..." detail; the peaks restart after each record.
  int format(char* out, std::size_t size, const char* error) noexcept {
    constexpr std::uint64_t MiB = 1024 * 1024;
    const int written =
        !latest_.valid
            ? std::snprintf(out, size, "Video memory: unavailable reason=%s", error && *error ? error : "no_sample")
            : std::snprintf(out, size,
                            "Video memory: local_mb=%llu/%llu nonlocal_mb=%llu/%llu over_budget=%u peak_local_mb=%llu "
                            "peak_nonlocal_mb=%llu",
                            static_cast<unsigned long long>(latest_.local_usage / MiB),
                            static_cast<unsigned long long>(latest_.local_budget / MiB),
                            static_cast<unsigned long long>(latest_.nonlocal_usage / MiB),
                            static_cast<unsigned long long>(latest_.nonlocal_budget / MiB),
                            latest_.local_budget && latest_.local_usage > latest_.local_budget,
                            static_cast<unsigned long long>(peak_local_ / MiB), static_cast<unsigned long long>(peak_nonlocal_ / MiB));
    peak_local_ = latest_.local_usage;
    peak_nonlocal_ = latest_.nonlocal_usage;
    return written;
  }
  const VideoMemorySample& latest() const noexcept { return latest_; }

 private:
  VideoMemorySample latest_{};
  std::uint64_t peak_local_ = 0, peak_nonlocal_ = 0;
};

}  // namespace taxi_camera
