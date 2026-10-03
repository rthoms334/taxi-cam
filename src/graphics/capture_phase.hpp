#pragma once

#include <cstddef>
#include <cstdint>

namespace taxi_camera::capture_phase {

// The simulator renders a camera view into its output in two submissions with
// a queue Wait between them (1.8.16.0, PIX 2026-10-02): one full-screen
// deferred-lighting draw, then the forward list (sky, macro lights, cloud
// merge, billboard lights, particles). A queue-tail capture taken after the
// first holds none of the lights drawn over the ground.
//
// Live on the PMDG 777 (2026-10-03) every capture had followed that
// deferred-only batch; capturing the complete render made the night lights
// look like the main view's.
//
// A capture that is due after a batch that drew the camera image exactly once
// waits for the next batch that draws it, whatever its draw count. A feed holds
// at most one batch between two captures, and a forgotten hold stays spent, so
// the next drawing batch is never held: a view whose image is written once per
// render is captured one render later, never stalled. A hold covers only the
// drawing batch directly after it; the caller forgets it when that batch
// records no capture.
enum class Kind : std::uint8_t { after_multi, after_hold, forced };
inline constexpr std::size_t KindCount = 3;
constexpr const char* kind_name(Kind kind) noexcept {
  constexpr const char* names[]{"after_multi", "after_hold", "forced"};
  return static_cast<std::size_t>(kind) < KindCount ? names[static_cast<std::size_t>(kind)] : "invalid_kind";
}
struct Feed {
  bool held = false;   // A one-draw batch was held; the next drawing batch is this render's rest.
  bool spent = false;  // A batch was held since this feed's last capture.
};
struct Decision {
  bool capture = true;
  Kind kind = Kind::after_multi;  // Of a capture; a held batch has none.
};
// Called once the feed's capture interval has passed, for a batch that drew
// its image `draws` times (at least one) in a render-target state.
inline Decision decide(Feed& feed, std::uint32_t draws) noexcept {
  if (feed.held)
    return {true, Kind::after_hold};
  if (draws > 1)
    return {true, Kind::after_multi};
  if (feed.spent)
    return {true, Kind::forced};
  feed.held = feed.spent = true;
  return {false};
}
// The capture was recorded: the next interval may hold again.
inline void captured(Feed& feed) noexcept {
  feed = {};
}
// Ordering evidence was lost (a wipe, a retirement, a rearm or the source
// leaving its render-target state), or the batch after a hold recorded no
// capture (no lease, the interval, no packet): the next drawing batch is not
// known to be this render's rest. The hold stays spent.
inline void forget(Feed& feed) noexcept {
  feed.held = false;
}

}  // namespace taxi_camera::capture_phase
