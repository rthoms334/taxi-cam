#pragma once

#include <array>
#include <cstdint>
#include "../shared/display_sides.hpp"

namespace taxi_camera {

struct TaxiButtonIntentSnapshot {
  unsigned buttons = 0;
  bool held = false;
  bool timed_out = false;
};

// A missing response is not an OFF event. Hold the last accepted intent for a
// bounded interval after the first observed gap; pose freshness is enforced
// independently before any render gate opens. Fresh OFF is always immediate.
class TaxiButtonIntent {
 public:
  static constexpr std::uint64_t gap_grace_ms = 2000;
  void reset() noexcept { *this = {}; }
  TaxiButtonIntentSnapshot observe(std::uint64_t now, bool valid, unsigned buttons) noexcept {
    if (valid) {
      known_ = true;
      gap_ = false;
      expired_ = false;
      buttons_ = buttons & AllDisplaySides;
      return {buttons_, false, false};
    }
    if (!known_)
      return {};
    if (!gap_) {
      gap_ = true;
      gap_started_ = now;
    }
    if (expired_ || now < gap_started_ || now - gap_started_ >= gap_grace_ms) {
      expired_ = true;
      return {0, false, true};
    }
    return {buttons_, true, false};
  }

 private:
  bool known_ = false;
  bool gap_ = false;
  bool expired_ = false;
  unsigned buttons_ = 0;
  std::uint64_t gap_started_ = 0;
};

// Session-only resource identities. The caller serializes access and forgets
// destroyed resources; no simulator state or resource lifetime is owned here.
// Pair detection routes the captain and first-officer sides; a single-display
// profile routes one texture to every side, except that a separate lower
// texture (PMDG 777 lower DU) keeps its own side-2 identity.
class TaxiButtonRoutes {
 public:
  using Pair = std::array<std::uint64_t, 2>;
  using Sides = std::array<std::uint64_t, MaxDisplaySides>;
  Sides targets{};

  // Explicit device/profile session change only. Resource destruction uses forget().
  void reset() noexcept { *this = {}; }

  // The caller first validates supplied nonzero resource IDs. Zero requests
  // automatic detection for that side; both zero starts a fresh explicit request.
  bool select_explicit(const Pair& selection) noexcept {
    if (selection[0] && selection[0] == selection[1])
      return false;
    targets = {selection[0], selection[1], 0};
    detected_targets_ = {};
    assigned_ = selection[0] != 0 || selection[1] != 0;
    return true;
  }
  // Single-display profiles: an explicit texture serves every side, because all
  // display rectangles live on it. Zero requests automatic detection again.
  // separate_lower leaves side 2 on its own texture, which it cannot share.
  bool select_single(std::uint64_t id, bool separate_lower = false) noexcept {
    if (separate_lower) {
      if (id && id == targets[2])
        return false;
      targets[0] = targets[1] = id;
      detected_targets_[0] = detected_targets_[1] = 0;
      assigned_ = id != 0;
      return true;
    }
    targets = {id, id, id};
    detected_targets_ = {};
    lower_explicit_ = false;
    assigned_ = id != 0;
    return true;
  }
  // Separate lower texture: explicit choice; zero returns it to automatic.
  bool select_lower(std::uint64_t id) noexcept {
    if (id && (id == targets[0] || id == targets[1]))
      return false;
    targets[2] = id;
    detected_targets_[2] = 0;
    lower_explicit_ = id != 0;
    return true;
  }
  // Separate lower texture: automatic guess. An existing choice is kept until
  // its texture is destroyed or the user selects another.
  bool adopt_lower(std::uint64_t id) noexcept {
    if (!id || id == targets[0] || id == targets[1])
      return false;
    if (targets[2] == id)
      return true;
    if (targets[2])
      return false;
    targets[2] = detected_targets_[2] = id;
    lower_explicit_ = false;
    return true;
  }
  bool lower_explicit() const noexcept { return lower_explicit_; }
  bool assign(unsigned side, std::uint64_t id) noexcept {
    if (side >= 2 || id == 0 || targets[1 - side] == id)
      return false;
    targets[side] = id;
    detected_targets_[side] = 0;
    assigned_ = true;
    return true;
  }

  // Detector ordering is only an initial-session hint. Once a side is known,
  // retain every surviving identity through replacement. With both lost,
  // semantic labels or an explicit assignment are required to recover sides.
  bool adopt_detected(const Pair& detected, bool exact_names = false) noexcept {
    if (!detected[0] || !detected[1] || detected[0] == detected[1])
      return false;
    if (exact_names || (!assigned_ && !targets[0] && !targets[1])) {
      targets = {detected[0], detected[1], 0};
      detected_targets_ = exact_names ? Sides{} : Sides{detected[0], detected[1], 0};
      assigned_ = true;
      return true;
    }
    if (targets[0] && targets[1])
      return (targets[0] == detected[0] && targets[1] == detected[1]) || (targets[0] == detected[1] && targets[1] == detected[0]);
    if (!targets[0] && !targets[1])
      return false;
    const unsigned survivor = targets[0] ? 0u : 1u;
    if (targets[survivor] != detected[0] && targets[survivor] != detected[1])
      return false;
    targets[1 - survivor] = detected[0] == targets[survivor] ? detected[1] : detected[0];
    detected_targets_[1 - survivor] = targets[1 - survivor];
    assigned_ = true;
    return true;
  }

  // Single-display profiles confirm one texture. Every display rectangle lives
  // on it, so every side receives that id. Pair adoption still rejects a
  // repeated id and is not used here.
  // Both-zero after forget() can leave assigned_ set (same sticky ownership as
  // dual-PFD both-lost). Unlike pair adoption, a lone navigation texture may
  // be re-bound automatically: there is no left/right ambiguity to resolve.
  bool adopt_single(std::uint64_t id, bool separate_lower = false) noexcept {
    if (!id)
      return false;
    if (separate_lower) {
      if (targets[0] == id && targets[1] == id)
        return true;
      if (targets[0] || targets[1] || targets[2] == id)
        return false;
      targets[0] = targets[1] = detected_targets_[0] = detected_targets_[1] = id;
      assigned_ = true;
      return true;
    }
    if (targets[0] == id && targets[1] == id && targets[2] == id)
      return true;
    if (targets[0] || targets[1] || targets[2])
      return false;
    targets = {id, id, id};
    detected_targets_ = {id, id, id};
    assigned_ = true;
    return true;
  }

  // A complete allocation group may replace stale automatic ranks, but never
  // a surviving explicit/semantic side. Keep the replacement marked detected
  // so a later group change can withdraw or replace it in the same way.
  bool replace_detected(const Pair& detected) noexcept {
    if (!detected[0] || !detected[1] || detected[0] == detected[1])
      return false;
    for (unsigned side = 0; side < targets.size(); ++side)
      if (targets[side] && targets[side] != detected_targets_[side])
        return false;
    targets = detected_targets_ = Sides{detected[0], detected[1], 0};
    assigned_ = true;
    return true;
  }

  void forget(std::uint64_t id) noexcept {
    assigned_ = assigned_ || targets[0] != 0 || targets[1] != 0 || targets[2] != 0;
    for (unsigned side = 0; side < targets.size(); ++side)
      if (targets[side] == id) {
        targets[side] = 0;
        detected_targets_[side] = 0;
        if (side == 2)
          lower_explicit_ = false;
      }
  }

  // An allocation-rank policy must be withdrawn when its complete-group
  // evidence changes. Explicit sides and semantic-name assignments survive;
  // the existing both-lost guard still requires an explicit session retry.
  void forget_detected() noexcept {
    const auto previous = detected_targets_;
    for (auto id : previous)
      if (id)
        forget(id);
  }

  unsigned active_mask(bool valid, unsigned buttons) const noexcept { return valid ? buttons & assigned_mask() : 0; }
  unsigned assigned_mask() const noexcept {
    unsigned mask = 0;
    for (unsigned side = 0; side < targets.size(); ++side)
      mask |= targets[side] ? 1u << side : 0u;
    return mask;
  }

  bool matches(std::uint64_t id, unsigned mask) const noexcept {
    for (unsigned side = 0; id && side < targets.size(); ++side)
      if ((mask & (1u << side)) && targets[side] == id)
        return true;
    return false;
  }

 private:
  bool assigned_ = false;
  bool lower_explicit_ = false;
  Sides detected_targets_{};
};

}  // namespace taxi_camera
