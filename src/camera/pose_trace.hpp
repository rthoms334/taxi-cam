#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "aircraft_mounts.hpp"

namespace taxi_camera::native_camera {

// Diagnostics only (0.9.62): the cameras still jumped a couple of metres back
// and forth with a velocity x frame-time lead. To see how the aircraft model's
// transform advances, the observer reads it on every camera-manager update
// while the aircraft moves and records two windows: one from taxi speed and
// one from take-off speed. The bridge writes each completed window to a CSV
// beside bridge.log. Nothing here changes camera placement.
struct PoseTraceEntry {
  std::uint64_t update = 0;
  double time_s = 0, frame_s = 0;
  Vector3 read{};     // Model origin read on this update (zero when the read failed).
  Vector3 applied{};  // Led origin the cameras were placed on this update.
  std::uint32_t flags = 0;
  double lead_m = 0;
};
inline constexpr std::uint32_t kPoseTraceRead = 1, kPoseTraceApplied = 2;

class PoseTrace {
 public:
  static constexpr std::size_t kFrames = 1500;
  static constexpr unsigned kWindows = 2;
  // Smoothed model speed (m/s) that starts each window: about 4 kt, 60 kt.
  static constexpr std::array<double, kWindows> kStartSpeed{2, 30};

  // Observer thread. Starts the first unstarted window whose speed is reached,
  // appends one entry per update, and completes it when full.
  void record(const PoseTraceEntry& entry, double speed_mps) noexcept {
    if (active_ < 0) {
      for (unsigned w = 0; w < kWindows; ++w)
        if (!started_[w] && speed_mps >= kStartSpeed[w]) {
          started_[w] = true;
          active_ = static_cast<int>(w);
          count_ = 0;
          break;
        }
      if (active_ < 0)
        return;
    }
    auto& window = windows_[static_cast<unsigned>(active_)];
    window[count_++] = entry;
    if (count_ == kFrames) {
      complete_[static_cast<unsigned>(active_)].store(true, std::memory_order_release);
      active_ = -1;
    }
  }
  // Whether this update would be recorded: a window is open, or an unstarted
  // one's speed is reached. Lets the caller skip the transform read otherwise.
  bool wants(double speed_mps) const noexcept {
    if (active_ >= 0)
      return true;
    for (unsigned w = 0; w < kWindows; ++w)
      if (!started_[w] && speed_mps >= kStartSpeed[w])
        return true;
    return false;
  }
  // Bridge worker: copies a completed window not yet taken. The observer never
  // writes a window again once it is complete.
  template <typename Sink>
  bool take(Sink&& sink) noexcept {
    for (unsigned w = 0; w < kWindows; ++w)
      if (complete_[w].load(std::memory_order_acquire) && !taken_[w].exchange(true, std::memory_order_acq_rel)) {
        sink(w, windows_[w].data(), kFrames);
        return true;
      }
    return false;
  }

 private:
  std::array<std::array<PoseTraceEntry, kFrames>, kWindows> windows_{};
  std::array<std::atomic<bool>, kWindows> complete_{}, taken_{};
  std::array<bool, kWindows> started_{};
  std::size_t count_ = 0;
  int active_ = -1;
};

}  // namespace taxi_camera::native_camera
