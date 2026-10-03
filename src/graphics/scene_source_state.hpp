#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace taxi_camera::source_state {

enum class Model { unknown, legacy_rt, enhanced_rt, other };
inline bool render_target_model(Model model) noexcept {
  return model == Model::legacy_rt || model == Model::enhanced_rt;
}
struct Key {
  std::uint64_t handle = 0, generation = 0;
  bool operator==(const Key&) const = default;
};
struct Effect {
  // other: leave RT and clear retained history (barrier/alias/reset/observer-disabled).
  // pass_other: leave the live model without clearing retained_rt (PassState/PassBegin/
  // unsupported-work reports that named a published source). Encoded in Kind so apply()
  // does not guess from ambient state.
  enum class Kind { legacy_rt, enhanced_rt, other, pass_other, draw };
  Key key;
  Kind kind = Kind::other;
  // Native draws this draw evidence stands for (append counts collapsed
  // repeats); 0 for state effects. It occupies the struct's tail padding.
  std::uint32_t draws = 0;
  bool operator==(const Effect&) const = default;
};
static_assert(sizeof(Effect) == 24, "The draw count must not grow the fixed recording storage");

// CPU recording evidence only. Apply immutable recordings in actual native
// Execute order, including every replay. Invalid recordings retain their prior
// effects/count so callers can preserve separate source-touched diagnostics.
// No initial resource state is assumed. Callers own synchronization and classify
// verified absolute RT evidence; split/alias/unknown/pass/truncation calls must
// invalidate their recording. This does not infer state from a draw or format.
struct Recording {
  static constexpr std::size_t capacity = 256;
  std::array<Effect, capacity> effects{};
  std::size_t count = 0;
  bool invalid = false;
  bool overflowed = false;

  // Repeated draw evidence collapses across independent keys until this key's
  // next state effect and is counted on the evidence it collapses into.
  // Transitions only collapse when consecutively identical.
  bool append(Effect effect) noexcept;
  void invalidate() noexcept { invalid = true; }
  // Old storage is ignored, not traversed/cleared; subsequent appends overwrite.
  void reset() noexcept {
    count = 0;
    invalid = false;
    overflowed = false;
  }
};

struct State {
  Model model = Model::unknown;
  bool drawn = false;
  // Native draws in a render-target model since the batch began or the model
  // was last set, saturating; nonzero exactly when drawn.
  std::uint32_t draws = 0;
};

class Tracker {
 public:
  static constexpr std::size_t capacity = 128;
  // Duplicate exact registration preserves state. A new generation uses only
  // the positively observed native creation model, otherwise unknown. Initial
  // creation state precedes GPU recordings; it is never reseeded on replay or
  // Reset. An overflowing new handle or invalid model refuses registration.
  bool register_source(Key key, Model initial = Model::unknown) noexcept;
  void unregister_source(Key key) noexcept;
  void clear() noexcept;
  // Submission-batch boundary: clear draw evidence and counts, preserve known final states.
  void begin_batch() noexcept;
  // Stale/unregistered keys cannot affect a current generation. Invalid logs
  // invalidate every tracked state and return false; later valid absolute RT
  // evidence can establish state anew. No recording-order mutation is allowed.
  bool apply(const Recording& recording) noexcept;
  State state(Key key) const noexcept;
  void invalidate_all() noexcept;
  // Retire every live model to other and keep retained_rt: the treatment a
  // named-source pass-state report gets, applied to a whole tracker when a
  // batch escaped ordering. rearm_retained_rt() then restores the RT models
  // in place, so the cost is one missed draw instead of an unknown state.
  void retire_live_models() noexcept;
  // After an unknown-list / invalid-log wipe, restore the last positively
  // observed RT model for the same generation. Does not infer RT from a draw
  // and does not revive a source that already left RT. Returns how many
  // sources were rearmed.
  unsigned rearm_retained_rt() noexcept;

 private:
  struct Slot {
    Key key;
    State state;
    Model retained_rt = Model::unknown;
  };
  std::array<Slot, capacity> sources_{};
};

}  // namespace taxi_camera::source_state
