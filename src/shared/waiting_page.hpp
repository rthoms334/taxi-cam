#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include "../profiles/catalog.hpp"

namespace taxi_camera::standalone {
// A side that had finished its minimum page and is admitted again within this
// time resumes without it: its display texture was recreated (live PMDG 777,
// 2026-10-01: every 3-5 s in flight), not switched off and on by the pilot.
// The separate missing/stale camera image rule still shows the page.
inline constexpr std::uint64_t WaitingPageResumeMs = 3000;

// Camera-image age that brings the page back while a side stays on. A composed
// image needs a new frame from every feed, so one skipped frame delays it by a
// whole feed interval (live PMDG 777, 2026-10-03: a third of requested frames
// not drawn near 60 fps and the page flashed at random). Six intervals at the
// rate the cameras actually get, never under WaitingPageStaleMs.
inline constexpr std::uint64_t waiting_stale_ms(unsigned rate) noexcept {
  const std::uint64_t per_second = std::max(rate, 1u);
  const std::uint64_t interval = (1000 + per_second - 1) / per_second;
  return std::max<std::uint64_t>(profiles::WaitingPageStaleMs, 6 * interval);
}

// Per-side minimum PLEASE WAIT time. The clock starts when a side is first
// admitted for display writes and resets when that side stops drawing, so each
// OFF to TAXI change shows the page for at least WaitingPageMinimumMs, except
// a quick return of a side that was already past it and stayed requested
// (requested_mask) while it had no display texture (WaitingPageResumeMs).
class WaitingPageTimer {
 public:
  unsigned observe(std::uint64_t now, unsigned active_mask, unsigned requested_mask = 0) noexcept {
    unsigned waiting = 0;
    for (unsigned side = 0; side < since_.size(); ++side) {
      const unsigned bit = 1u << side;
      if (!(active_mask & bit)) {
        if (!(requested_mask & bit))
          dropped_[side] = 0;  // Switched off: the next ON shows the page.
        else if (since_[side] && (resumed_[side] || (now >= since_[side] && now - since_[side] >= profiles::WaitingPageMinimumMs)))
          dropped_[side] = now ? now : 1;
        since_[side] = 0;
        resumed_[side] = false;
        continue;
      }
      if (!since_[side]) {
        since_[side] = now ? now : 1;
        resumed_[side] = dropped_[side] && now >= dropped_[side] && now - dropped_[side] <= WaitingPageResumeMs;
        dropped_[side] = 0;
      }
      if (!resumed_[side] && (now < since_[side] || now - since_[side] < profiles::WaitingPageMinimumMs))
        waiting |= bit;
    }
    return waiting;
  }

 private:
  std::array<std::uint64_t, MaxDisplaySides> since_{}, dropped_{};
  std::array<bool, MaxDisplaySides> resumed_{};
};
}  // namespace taxi_camera::standalone
