#pragma once

namespace taxi_camera {

inline constexpr unsigned kMinimumCameraRate = 5;
inline constexpr unsigned kMaximumCameraRate = 60;
inline constexpr unsigned kDefaultCameraRate = 10;
// Schedule floor used while the aircraft is parked; 0 in settings disables it.
// A parked image barely changes, so the floor may go below the moving minimum.
// It has to: every pulse is followed by a closed tick, so below about 20 fps
// two feeds already pulse on every other frame at any rate of 5 or more, and
// only a lower floor reduces the share of frames that render an extra view.
inline constexpr unsigned kMinimumParkedCameraRate = 1;
inline constexpr unsigned kDefaultParkedCameraRate = 2;
// Profiles saved before this revision hold the previous shipped floor of 5,
// which is loaded as the new default once; a later saved choice is kept.
inline constexpr unsigned kParkedRateRevision = 1;
inline constexpr unsigned kPreviousDefaultParkedCameraRate = 5;
// Live camera-manager cadence was 42–47 updates/s (0.9.35 rate sweep). From
// this setting on, nearly every manager update is already a gate transition,
// so a higher request cannot add useful pulses and is reported as capped.
inline constexpr unsigned kManagerCeilingCameraRate = 15;

}  // namespace taxi_camera
