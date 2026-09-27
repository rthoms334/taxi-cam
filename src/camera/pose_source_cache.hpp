#pragma once
#include <cstdint>

namespace taxi_camera::native_camera {

// The aircraft controller ("user") whose scene transform places the cameras.
// Proving it walks the whole world/user/controller/component chain: about 70
// reads and 30 memory queries on the simulator main thread every pulse. That
// chain changes only with the aircraft session, so a proven controller is
// reused within one session epoch and reset generation for less than
// kMaximumAgeMs, then proven again. Every pulse still reads the scene
// transform from it with all of that read's checks: controller vtable,
// generation-checked model node and alias, attached model identity, a complete
// reread and a match with the public aircraft pose. The caller forgets it on
// any failure. Observer thread only.
class PoseSourceCache {
 public:
  static constexpr std::uint64_t kMaximumAgeMs = 1000;

  // The proven controller, or zero when there is none for this epoch and reset
  // generation, or it is due to be proven again. Epoch zero (no readiness yet)
  // never reuses one.
  std::uint64_t reuse(std::uint64_t now_ms, std::uint64_t epoch, std::uint64_t resets) const noexcept {
    return user_ && epoch && epoch == epoch_ && resets == resets_ && now_ms >= proven_ms_ && now_ms - proven_ms_ < kMaximumAgeMs ? user_
                                                                                                                                 : 0;
  }
  void prove(std::uint64_t user, std::uint64_t now_ms, std::uint64_t epoch, std::uint64_t resets) noexcept {
    user_ = user;
    proven_ms_ = now_ms;
    epoch_ = epoch;
    resets_ = resets;
  }
  void forget() noexcept { user_ = 0; }

 private:
  std::uint64_t user_ = 0, proven_ms_ = 0, epoch_ = 0, resets_ = 0;
};

}  // namespace taxi_camera::native_camera
