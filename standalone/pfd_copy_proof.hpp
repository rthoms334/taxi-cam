#pragma once
#include <array>
#include <cstdint>

namespace taxi_camera::standalone {
// Comparison-only per-recording evidence. Pre-forward barrier metadata is never
// usable directly: only a later completed external native Draw promotes it.
// The boundary observer bypasses nested Draw notifications throughout its
// original ResourceBarrier/Barrier call, so driver reentry cannot promote it.
class PfdCopyProof {
 public:
  enum class Mode { unknown, legacy_rt, enhanced_rt };
  struct Key {
    std::uint64_t resource{}, generation{};
    bool operator==(const Key&) const = default;
  };
  void reset(bool observed) noexcept {
    slots_ = {};
    known_ = observed;
  }
  void invalidate() noexcept { known_ = false; }
  // ClearState does not change resource states, but we conservatively discard
  // this optional proof; only a fully observed new recording renews admission.
  void clear_state() noexcept { invalidate(); }
  void forget_resource(std::uint64_t resource) noexcept {
    for (auto& slot : slots_)
      if (slot.key.resource == resource)
        slot = {};
  }
  void observe_transition(Key key, Mode after, const char* refused = "not_rt_entry") noexcept {
    if (!known_ || !key.resource || !key.generation)
      return;
    auto* slot = find(key);
    if (!slot) {
      for (auto& item : slots_) {
        if (!item.key.resource || item.key.resource == key.resource) {
          item = {};
          item.key = key;
          slot = &item;
          break;
        }
      }
    }
    if (!slot) {
      invalidate();
      return;
    }
    slot->ready = Mode::unknown;
    // A second matching transition before a completed draw is deliberately
    // ambiguous here (including duplicate/split sequences), never last-wins.
    if (slot->pending_seen) {
      slot->pending = Mode::unknown;
      slot->reason = "multiple_transitions_before_draw";
    } else {
      slot->pending = after;
      slot->reason = after == Mode::unknown ? refused : "awaiting_forwarded_draw";
    }
    slot->pending_seen = true;
  }
  void after_draw(Key key) noexcept {
    if (known_)
      if (auto* slot = find(key); slot && slot->pending_seen) {
        slot->ready = slot->pending;
        slot->pending = Mode::unknown;
        slot->pending_seen = false;
        if (slot->ready != Mode::unknown)
          slot->reason = "ready";
      }
  }
  Mode mode(Key key) const noexcept {
    if (known_)
      for (const auto& slot : slots_)
        if (slot.key == key)
          return slot.ready;
    return Mode::unknown;
  }
  const char* reason(Key key) const noexcept {
    if (!known_)
      return "recording_unknown_or_invalidated";
    for (const auto& slot : slots_)
      if (slot.key == key)
        return slot.reason;
    return "no_rt_entry_in_recording";
  }

 private:
  struct Slot {
    Key key{};
    Mode pending = Mode::unknown, ready = Mode::unknown;
    bool pending_seen = false;
    const char* reason = "no_rt_entry_in_recording";
  };
  Slot* find(Key key) noexcept {
    for (auto& slot : slots_)
      if (slot.key == key)
        return &slot;
    return nullptr;
  }
  std::array<Slot, 2> slots_{};
  bool known_ = false;
};
}  // namespace taxi_camera::standalone
