#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include "../profiles/catalog.hpp"

namespace taxi_camera {

struct PfdTargetObservation {
  std::uint64_t id = 0;
  std::uint64_t draws = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t levels = 0;
  std::uint32_t format = 0;
};

enum class PfdTargetConfidence { none, stabilizing, confirmed };

struct PfdTargetDetection {
  bool valid = false;
  std::array<std::uint64_t, 2> targets{};  // Left, right.
  unsigned stable_windows = 0;
  PfdTargetConfidence confidence = PfdTargetConfidence::none;
  const char* status = "warming_up";
};

// Empirically validated A380 heuristic, not material-name or aircraft identity
// proof. IDs must identify resource incarnations and must not be reused. Supply
// the complete current inventory on each call; reset on device/session changes.
// The caller serializes access. This class never reads GPU resources or allocates.
class PfdTargetDetector {
 public:
  static constexpr std::size_t capacity = 16384;
  static constexpr std::uint64_t window_ms = 1000;
  static constexpr std::uint64_t maximum_window_ms = 5000;
  static constexpr unsigned required_windows = 3;

  const PfdTargetDetection& snapshot() const noexcept { return detection_; }

  void configure(const profiles::AircraftProfile& profile) noexcept {
    profile_ = &profile;
    reset();
  }

  void reset() noexcept {
    previous_count_ = 0;
    baseline_valid_ = false;
    clear("warming_up");
  }

  const PfdTargetDetection& observe(const PfdTargetObservation* observations, std::size_t count, std::uint64_t now_ms) noexcept {
    if (count > capacity || (count != 0 && observations == nullptr)) {
      reset();
      clear(count > capacity ? "capacity_exceeded" : "invalid_input");
      return detection_;
    }
    std::size_t current_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& value = observations[i];
      if (!profiles::matches_display(*profile_, value.width, value.height, value.levels, value.format))
        continue;
      if (value.id == 0) {
        reset();
        clear("invalid_input");
        return detection_;
      }
      current_[current_count++] = {value.id, value.draws};
    }
    std::sort(current_.begin(), current_.begin() + current_count, [](const Counter& a, const Counter& b) { return a.id < b.id; });
    for (std::size_t i = 1; i < current_count; ++i) {
      if (current_[i - 1].id == current_[i].id) {
        reset();
        clear("duplicate_id");
        return detection_;
      }
    }
    if (current_count < 2) {
      clear(current_count == 0 ? "no_candidates" : "insufficient_candidates");
      seed(current_count, now_ms);
      return detection_;
    }
    if (pending_[0] != 0 && (!contains(current_count, pending_[0]) || !contains(current_count, pending_[1])))
      clear("candidate_disappeared");
    if (!baseline_valid_ || now_ms < baseline_ms_ || now_ms - baseline_ms_ > maximum_window_ms) {
      clear(!baseline_valid_ ? "warming_up" : now_ms < baseline_ms_ ? "clock_reset" : "stale_window");
      seed(current_count, now_ms);
      return detection_;
    }

    std::array<Counter, 3> busiest{};
    std::size_t previous_index = 0;
    for (std::size_t i = 0; i < current_count; ++i) {
      const auto& value = current_[i];
      while (previous_index < previous_count_ && previous_[previous_index].id < value.id)
        ++previous_index;
      if (previous_index == previous_count_ || previous_[previous_index].id != value.id)
        continue;  // A newly observed incarnation needs a full baseline window.
      if (value.draws < previous_[previous_index].draws) {
        clear("counter_reset");
        seed(current_count, now_ms);
        return detection_;
      }
      Counter ranked{value.id, value.draws - previous_[previous_index].draws};
      for (auto& slot : busiest) {
        if (ranked.draws > slot.draws)
          std::swap(ranked, slot);
      }
    }
    if (now_ms - baseline_ms_ < window_ms)
      return detection_;
    seed(current_count, now_ms);
    if (busiest[1].draws == 0) {
      clear("no_activity");
      return detection_;
    }
    // Integer comparisons avoid overflow even for near-UINT64_MAX counters.
    const auto quotient = busiest[0].draws / busiest[1].draws;
    if (quotient > 3 || (quotient == 3 && busiest[0].draws % busiest[1].draws != 0)) {
      clear("incomparable_rates");
      return detection_;
    }
    if (busiest[1].draws <= busiest[2].draws || busiest[1].draws - busiest[2].draws <= busiest[2].draws / 2) {
      clear("ambiguous_activity");
      return detection_;
    }
    // In the verified sessions the higher of the two resource IDs is LEFT;
    // which PFD happened to draw more often does not determine its side.
    std::array<std::uint64_t, 2> pair{std::max(busiest[0].id, busiest[1].id), std::min(busiest[0].id, busiest[1].id)};
    if (!profile_->higher_id_left)
      std::swap(pair[0], pair[1]);
    if (pair != pending_) {
      clear("stabilizing");
      pending_ = pair;
    }
    detection_.stable_windows = std::min(required_windows, detection_.stable_windows + 1);
    detection_.valid = detection_.stable_windows == required_windows;
    detection_.confidence = detection_.valid ? PfdTargetConfidence::confirmed : PfdTargetConfidence::stabilizing;
    detection_.targets = detection_.valid ? pair : std::array<std::uint64_t, 2>{};
    detection_.status = detection_.valid ? "detected" : "stabilizing";
    return detection_;
  }

 private:
  const profiles::AircraftProfile* profile_ = &profiles::A380;
  struct Counter {
    std::uint64_t id = 0;
    std::uint64_t draws = 0;
  };

  bool contains(std::size_t count, std::uint64_t id) const noexcept {
    const auto found = std::lower_bound(current_.begin(), current_.begin() + count, id,
                                        [](const Counter& value, std::uint64_t key) { return value.id < key; });
    return found != current_.begin() + count && found->id == id;
  }

  void clear(const char* status) noexcept {
    detection_ = {};
    detection_.status = status;
    pending_ = {};
  }

  void seed(std::size_t count, std::uint64_t now_ms) noexcept {
    std::copy_n(current_.begin(), count, previous_.begin());
    previous_count_ = count;
    baseline_ms_ = now_ms;
    baseline_valid_ = true;
  }

  std::array<Counter, capacity> previous_{};
  std::array<Counter, capacity> current_{};
  std::size_t previous_count_ = 0;
  std::uint64_t baseline_ms_ = 0;
  bool baseline_valid_ = false;
  std::array<std::uint64_t, 2> pending_{};
  PfdTargetDetection detection_{};
};

}  // namespace taxi_camera
