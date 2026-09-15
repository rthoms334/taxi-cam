#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace taxi_camera::source_state {

enum class Model { unknown, legacy_rt, enhanced_rt, other };
struct Key {
  std::uint64_t handle = 0, generation = 0;
  bool operator==(const Key&) const = default;
};
struct Effect {
  enum class Kind { legacy_rt, enhanced_rt, other, draw };
  Key key;
  Kind kind = Kind::other;
  bool operator==(const Effect&) const = default;
};

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
  // next state effect. Transitions only collapse when consecutively identical.
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
  // Submission-batch boundary: clear draw evidence, preserve known final states.
  void begin_batch() noexcept;
  // Stale/unregistered keys cannot affect a current generation. Invalid logs
  // invalidate every tracked state and return false; later valid absolute RT
  // evidence can establish state anew. No recording-order mutation is allowed.
  bool apply(const Recording& recording) noexcept;
  State state(Key key) const noexcept;
  void invalidate_all() noexcept;

 private:
  struct Slot {
    Key key;
    State state;
  };
  std::array<Slot, capacity> sources_{};
};

}  // namespace taxi_camera::source_state
