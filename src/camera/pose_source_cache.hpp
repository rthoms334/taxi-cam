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
// reread and, until the session is proven, a match with the public aircraft
// pose. The caller forgets it on any failure. Observer thread only.
//
// The first public match also proves the controller for the whole session
// epoch and reset generation. The public match needs the local altitude
// calibration, which is valid only within 10 km and cannot be re-latched in
// flight, so once proven the same controller (found again by the walk, or
// reused) is trusted without it. A new aircraft session, a session reset or a
// different controller needs a fresh public match.
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
  // The controller publicly matched in this session epoch and reset
  // generation, or zero. Not aged and not cleared by forget().
  std::uint64_t session_proven(std::uint64_t epoch, std::uint64_t resets) const noexcept {
    return session_user_ && epoch && epoch == session_epoch_ && resets == session_resets_ ? session_user_ : 0;
  }
  // A public match: starts the one-second reuse and proves the session.
  void prove(std::uint64_t user, std::uint64_t now_ms, std::uint64_t epoch, std::uint64_t resets) noexcept {
    remember(user, now_ms, epoch, resets);
    session_user_ = user;
    session_epoch_ = epoch;
    session_resets_ = resets;
  }
  // The session-proven controller found again by the walk: restarts the
  // one-second reuse without a public match. Any other controller, epoch or
  // reset generation is refused and changes nothing.
  bool refresh(std::uint64_t user, std::uint64_t now_ms, std::uint64_t epoch, std::uint64_t resets) noexcept {
    if (!user || user != session_proven(epoch, resets))
      return false;
    remember(user, now_ms, epoch, resets);
    return true;
  }
  // A failed scene read only ends the one-second reuse; a transient read of a
  // matrix the engine is writing must not cost the session proof.
  void forget() noexcept { user_ = 0; }

 private:
  void remember(std::uint64_t user, std::uint64_t now_ms, std::uint64_t epoch, std::uint64_t resets) noexcept {
    user_ = user;
    proven_ms_ = now_ms;
    epoch_ = epoch;
    resets_ = resets;
  }
  std::uint64_t user_ = 0, proven_ms_ = 0, epoch_ = 0, resets_ = 0;
  std::uint64_t session_user_ = 0, session_epoch_ = 0, session_resets_ = 0;
};

}  // namespace taxi_camera::native_camera
