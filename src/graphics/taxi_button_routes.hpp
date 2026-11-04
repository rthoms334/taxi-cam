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
// A pilot pair routes the captain and first-officer sides; a shared-texture
// profile routes one texture to every side, except that a separate lower
// texture (PMDG 777 lower DU) keeps its own side-2 identity.
// Which display texture each side draws on. Displays are named by their
// panel marks (bridge_main.cpp service_mark_identity); the user can choose
// any texture instead, and a choice always wins over a name.
class TaxiButtonRoutes {
 public:
  using Pair = std::array<std::uint64_t, 2>;
  using Sides = std::array<std::uint64_t, MaxDisplaySides>;
  Sides targets{};

  // Explicit device/profile session change only. Resource destruction uses forget().
  void reset() noexcept { *this = {}; }

  // The caller first validates supplied nonzero resource IDs. Zero leaves
  // that side to its name. Side 2 is not part of the pair: a lower display
  // the user placed (named) stays.
  bool select_explicit(const Pair& selection) noexcept {
    if (selection[0] && selection[0] == selection[1])
      return false;
    targets = {selection[0], selection[1], targets[2]};
    return true;
  }
  // Single-display profiles: an explicit texture serves every side, because all
  // display rectangles live on it. Zero leaves them to their names.
  // separate_lower leaves side 2 on its own texture, which it cannot share.
  bool select_single(std::uint64_t id, bool separate_lower = false) noexcept {
    if (separate_lower) {
      if (id && id == targets[2])
        return false;
      targets[0] = targets[1] = id;
      return true;
    }
    targets = {id, id, id};
    lower_explicit_ = false;
    return true;
  }
  // Separate lower texture: explicit choice; zero leaves it to its name.
  bool select_lower(std::uint64_t id) noexcept {
    if (id && (id == targets[0] || id == targets[1]))
      return false;
    targets[2] = id;
    lower_explicit_ = id != 0;
    return true;
  }
  bool lower_explicit() const noexcept { return lower_explicit_; }
  bool assign(unsigned side, std::uint64_t id) noexcept {
    if (side >= 2 || id == 0 || targets[1 - side] == id)
      return false;
    targets[side] = id;
    return true;
  }

  // A side the user did not choose takes the texture its name resolves to.
  // Single-display profiles may name one texture for several sides.
  bool adopt_named(const Sides& named, unsigned sides) noexcept {
    // A side the user chose: not its name's texture. Single-display choices
    // also set side 2 this way; the 777 lower DU has its own flag.
    const auto chosen = [&](unsigned side) {
      return (side == 2 && lower_explicit_) || (targets[side] && targets[side] != named_targets_[side]);
    };
    std::array<bool, MaxDisplaySides> user{};
    for (unsigned side = 0; side < targets.size(); ++side)
      user[side] = chosen(side);
    bool changed = false;
    for (unsigned side = 0; side < sides && side < targets.size(); ++side) {
      const auto id = named[side];
      if (!id || targets[side] == id || user[side])
        continue;
      // A texture the user put on another side (the right PFD's texture moved
      // to the left) never serves this side as well. Named sides do not
      // block: swapped names move both sides in this one pass.
      bool held = false;
      for (unsigned other = 0; other < targets.size(); ++other)
        held = held || (other != side && user[other] && targets[other] == id && named[other] != id);
      if (held)
        continue;
      targets[side] = named_targets_[side] = id;
      changed = true;
    }
    return changed;
  }
  bool named(unsigned side) const noexcept { return side < targets.size() && targets[side] && targets[side] == named_targets_[side]; }

  void forget(std::uint64_t id) noexcept {
    for (unsigned side = 0; side < targets.size(); ++side)
      if (targets[side] == id) {
        targets[side] = 0;
        named_targets_[side] = 0;
        if (side == 2)
          lower_explicit_ = false;
      }
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
  bool lower_explicit_ = false;
  Sides named_targets_{};
};

}  // namespace taxi_camera
