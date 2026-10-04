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
  // Proven RT exits observed at queue submission; not native draw counts or
  // fence-confirmed GPU completion.
  // The caller excludes stale/aliased/unproven resource incarnations.
  std::uint64_t submission_activity = 0;
};

enum class PfdTargetConfidence { none, stabilizing, confirmed };

struct PfdTargetDetection {
  bool valid = false;
  bool invalidates_targets = false;
  // A confirmed texture was recreated and its replacement took over its role;
  // the caller adopts it even where an earlier invalidation dropped targets.
  bool replaced = false;
  std::array<std::uint64_t, 2> targets{};  // Left, right.
  unsigned stable_windows = 0;
  PfdTargetConfidence confidence = PfdTargetConfidence::none;
  const char* status = "warming_up";
};

// Empirical profile-specific heuristics, not material-name or aircraft identity
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
    submission_mode_ = false;
    confirmed_count_ = 0;
    confirmed_targets_ = {};
    roles_from_replacement_ = false;
    clear("warming_up");
  }

  const PfdTargetDetection& observe(const PfdTargetObservation* observations,
                                    std::size_t count,
                                    std::uint64_t now_ms,
                                    bool inventory_complete = true) noexcept {
    if (count > capacity || (count != 0 && observations == nullptr)) {
      reset();
      clear(count > capacity ? "capacity_exceeded" : "invalid_input", true);
      return detection_;
    }
    if ((profile_->pfd_detection == profiles::PfdDetectionPolicy::ini_a380_allocation_group ||
         profile_->pfd_detection == profiles::PfdDetectionPolicy::single_display) &&
        !inventory_complete) {
      reset();
      clear("incomplete_inventory", true);
      return detection_;
    }
    bool has_draws = false, has_submission_activity = false;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& value = observations[i];
      if (profiles::matches_detection(*profile_, value.width, value.height, value.levels, value.format)) {
        has_draws |= value.draws != 0;
        has_submission_activity |= value.submission_activity != 0;
      }
    }
    // Keep ordinary zero-to-first-draw startup on its existing baseline. Once
    // submitted exits become the only evidence, retain that counter source
    // through an all-zero rollback so the existing reset guard can detect it.
    const bool submission_mode = !has_draws && (has_submission_activity || submission_mode_);
    std::size_t current_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& value = observations[i];
      if (!profiles::matches_detection(*profile_, value.width, value.height, value.levels, value.format))
        continue;
      if (value.id == 0) {
        reset();
        clear("invalid_input", true);
        return detection_;
      }
      current_[current_count++] = {value.id, submission_mode ? value.submission_activity : value.draws, value.format, value.levels};
    }
    std::sort(current_.begin(), current_.begin() + current_count, [](const Counter& a, const Counter& b) { return a.id < b.id; });
    for (std::size_t i = 1; i < current_count; ++i) {
      if (current_[i - 1].id == current_[i].id) {
        reset();
        clear("duplicate_id", true);
        return detection_;
      }
    }
    const bool changed_source = baseline_valid_ && submission_mode != submission_mode_;
    submission_mode_ = submission_mode;
    if (submission_mode && (profile_->id == profiles::A359.id || profile_->id == profiles::A35K.id)) {
      // A350 native draws and submitted RT exits rank different surfaces.
      // The observed five-mip UNORM auxiliaries complete more passes than
      // either EFIS. Only the complete three-surface one-mip typeless group
      // is qualified for this fallback; retain all ordinary draw candidates.
      const auto reject = [&](const char* status) -> const PfdTargetDetection& {
        clear(status);
        baseline_valid_ = false;
        previous_count_ = 0;
        return detection_;
      };
      if (!inventory_complete)
        return reject("incomplete_inventory");
      std::size_t group_count = 0;
      for (std::size_t i = 0; i < current_count; ++i) {
        const auto value = current_[i];
        if (value.levels == 1 && value.format == 27)
          current_[group_count++] = value;
        else if (value.levels != 5 || value.format != 28)
          return reject("a350_group_format");
      }
      if (group_count != 3)
        return reject(group_count < 3 ? "a350_group_incomplete" : "a350_group_ambiguous");
      current_count = group_count;
      // A recreated group member is a clean replacement, not a changed group.
      if (replace_recreated(current_count, now_ms))
        return detection_;
      // Even replacement of the third (unselected) surface starts a new
      // complete group baseline, never an artificially uncontested pair.
      if (baseline_valid_ && !changed_source) {
        bool same = previous_count_ == current_count;
        for (std::size_t i = 0; same && i < current_count; ++i)
          same = previous_[i].id == current_[i].id;
        if (!same) {
          clear("a350_group_changed");
          seed(current_count, now_ms);
          return detection_;
        }
      }
    }
    if (replace_recreated(current_count, now_ms))
      return detection_;
    return remember(dispatch(current_count, now_ms, changed_source), current_count);
  }

 private:
  const PfdTargetDetection& dispatch(std::size_t current_count, std::uint64_t now_ms, bool changed_source) noexcept {
    if (changed_source) {
      clear("activity_source_changed", profile_->pfd_detection == profiles::PfdDetectionPolicy::ini_a380_allocation_group);
      seed(current_count, now_ms);
      return detection_;
    }
    if (profile_->pfd_detection == profiles::PfdDetectionPolicy::ini_a380_allocation_group)
      return observe_allocation_group(current_count, now_ms);
    if (profile_->pfd_detection == profiles::PfdDetectionPolicy::single_display)
      return observe_single_display(current_count, now_ms);
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
    // A350 power-up has a third active display close behind both PFDs. Its
    // measured activity breaks the FBW 50% lead despite a stable leading pair.
    // Require a 25% lead for A350, retaining three stable windows and ID sides.
    const unsigned lead_divisor = profile_->id == profiles::A359.id || profile_->id == profiles::A35K.id ? 4u : 2u;
    if (busiest[1].draws <= busiest[2].draws || busiest[1].draws - busiest[2].draws <= busiest[2].draws / lead_divisor) {
      clear("ambiguous_activity");
      return detection_;
    }
    // In the verified sessions the higher of the two resource IDs is LEFT;
    // which PFD happened to draw more often does not determine its side.
    std::array<std::uint64_t, 2> pair{std::max(busiest[0].id, busiest[1].id), std::min(busiest[0].id, busiest[1].id)};
    if (!profile_->higher_id_left)
      std::swap(pair[0], pair[1]);
    return confirm(pair);
  }

 private:
  const profiles::AircraftProfile* profile_ = &profiles::A380;
  struct Counter {
    std::uint64_t id = 0;
    std::uint64_t draws = 0;
    std::uint32_t format = 0;
    std::uint32_t levels = 0;
  };

  // A valid detection records its targets and the matching inventory it was
  // made from, for replace_recreated.
  const PfdTargetDetection& remember(const PfdTargetDetection& result, std::size_t count) noexcept {
    if (result.valid) {
      confirmed_targets_ = result.targets;
      for (std::size_t i = 0; i < count; ++i)
        confirmed_ids_[i] = current_[i].id;
      confirmed_count_ = count;
    }
    return result;
  }

  // An aircraft can recreate a display texture mid-flight (live PMDG 777,
  // 2026-10-01: the display every 3-5 s, and the lower texture once). When,
  // against the inventory of the last valid detection, exactly one texture is
  // gone and exactly one texture of the same shape is new, the new one takes
  // over the gone one's role (left/right, display/lower) at once; a gone
  // texture that was no target leaves the targets as they were. The baseline
  // and confirmation windows are for a first detection, and the id-order and
  // allocation-order rules would misplace a new incarnation (it has the
  // highest id). Incarnation ids are not reused. Anything less clear-cut
  // (none, two or more changes) takes the policy's own path.
  bool replace_recreated(std::size_t count, std::uint64_t now_ms) noexcept {
    if (!confirmed_count_ || !confirmed_targets_[0])
      return false;
    std::uint64_t gone = 0, created = 0;
    unsigned gone_count = 0, created_count = 0;
    std::size_t i = 0, j = 0;
    while (i < confirmed_count_ || j < count) {
      if (j == count || (i < confirmed_count_ && confirmed_ids_[i] < current_[j].id)) {
        gone = confirmed_ids_[i++];
        ++gone_count;
      } else if (i == confirmed_count_ || current_[j].id < confirmed_ids_[i]) {
        created = current_[j++].id;
        ++created_count;
      } else {
        ++i;
        ++j;
      }
    }
    if (gone_count != 1 || created_count != 1)
      return false;
    auto targets = confirmed_targets_;
    for (auto& target : targets)
      if (target == gone)
        target = created;
    clear("replaced");
    seed(count, now_ms);
    const bool single = profile_->pfd_detection == profiles::PfdDetectionPolicy::single_display;
    pending_ = single ? std::array<std::uint64_t, 2>{targets[0], 0} : targets;
    roles_from_replacement_ = true;
    detection_.valid = true;
    detection_.replaced = true;
    detection_.stable_windows = required_windows;
    detection_.confidence = PfdTargetConfidence::confirmed;
    detection_.targets = targets;
    detection_.status = "replaced";
    remember(detection_, count);
    return true;
  }

  const PfdTargetDetection& confirm(const std::array<std::uint64_t, 2>& pair) noexcept {
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

  const PfdTargetDetection& observe_allocation_group(std::size_t count, std::uint64_t now_ms) noexcept {
    // Two live ini A380 sessions exposed eight active, one-mip RGBA8 typeless
    // displays. In the complete allocation-ordered group the last is LEFT and
    // third-last is RIGHT. IDs are incarnation IDs shared with other objects,
    // so gaps are valid. Never generalize this to a partial list or to another
    // format/profile, even when two textures happen to dominate activity.
    constexpr std::size_t group_size = 8;
    const auto reject = [&](const char* status, bool invalidates = true) -> const PfdTargetDetection& {
      clear(status, invalidates);
      seed(count, now_ms);
      return detection_;
    };
    if (count != group_size)
      return reject(count < group_size ? "ini_group_incomplete" : "ini_group_ambiguous");
    for (std::size_t i = 0; i < count; ++i) {
      if (current_[i].format != 27)
        return reject("ini_group_format");
      if (current_[i].draws == UINT64_MAX)
        return reject("counter_saturated");
    }
    if (!baseline_valid_)
      return reject("warming_up", false);
    if (previous_count_ != count) {
      roles_from_replacement_ = false;
      return reject("ini_group_changed");
    }
    bool active = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (previous_[i].id != current_[i].id || previous_[i].format != current_[i].format) {
        roles_from_replacement_ = false;
        return reject("ini_group_changed");
      }
      if (current_[i].draws < previous_[i].draws)
        return reject("counter_reset");
      active &= current_[i].draws > previous_[i].draws;
    }
    if (now_ms < baseline_ms_ || now_ms - baseline_ms_ > maximum_window_ms)
      return reject(now_ms < baseline_ms_ ? "clock_reset" : "stale_window", false);
    if (now_ms - baseline_ms_ < window_ms)
      return detection_;
    seed(count, now_ms);
    if (!active) {
      clear("ini_group_inactive");
      return detection_;
    }
    // A replacement keeps the roles it took over: the new incarnation's id
    // no longer reflects the allocation order.
    return confirm(roles_from_replacement_ ? confirmed_targets_
                                           : std::array<std::uint64_t, 2>{current_[count - 1].id, current_[count - 3].id});
  }

  // One destination texture. observe() already ordered matches by resource id,
  // which is the routing dropdown order. The last entry is the navigation
  // display for this profile; the first entry is not. The confirmed pair is
  // {id, 0}; routing copies that id onto both inboard rectangles.
  // A changed last id starts a new baseline; a recreated confirmed texture is
  // handled before this (replace_recreated). Draw count does not rank them.
  const PfdTargetDetection& observe_single_display(std::size_t count, std::uint64_t now_ms) noexcept {
    if (count == 0) {
      clear("no_candidates");
      seed(count, now_ms);
      return detection_;
    }
    const auto chosen = current_[count - 1].id;
    if (!baseline_valid_ || now_ms < baseline_ms_ || now_ms - baseline_ms_ > maximum_window_ms) {
      clear(!baseline_valid_ ? "warming_up" : now_ms < baseline_ms_ ? "clock_reset" : "stale_window");
      seed(count, now_ms);
      return detection_;
    }
    if (previous_count_ == 0 || previous_[previous_count_ - 1].id != chosen) {
      clear("candidate_disappeared");
      seed(count, now_ms);
      return detection_;
    }
    if (now_ms - baseline_ms_ < window_ms)
      return detection_;
    seed(count, now_ms);
    confirm({chosen, 0});
    return single_display_confirmed(count);
  }

  const PfdTargetDetection& single_display_confirmed(std::size_t count) noexcept {
    if (!detection_.valid)
      return detection_;
    // Unverified lower-texture guess for a profile whose side 2 is a separate
    // texture, reported beside the confirmed display without affecting its
    // stability: the next-highest id. One PMDG 777 session log listed five
    // 2048 x 2048 textures; the busiest non-DUS texture, consistent with
    // EICASCDU, was the next-highest id. Routing can override it.
    if (profiles::separate_lower_texture(*profile_) && count >= 2)
      detection_.targets[1] = current_[count - 2].id;
    return detection_;
  }

  bool contains(std::size_t count, std::uint64_t id) const noexcept {
    const auto found = std::lower_bound(current_.begin(), current_.begin() + count, id,
                                        [](const Counter& value, std::uint64_t key) { return value.id < key; });
    return found != current_.begin() + count && found->id == id;
  }
  void clear(const char* status, bool invalidates = false) noexcept {
    detection_ = {};
    detection_.invalidates_targets = invalidates;
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
  bool submission_mode_ = false;
  std::array<std::uint64_t, 2> pending_{};
  // The last valid detection's targets and the sorted matching inventory it was
  // made from (replace_recreated). reset() forgets them.
  std::array<std::uint64_t, 2> confirmed_targets_{};
  std::array<std::uint64_t, capacity> confirmed_ids_{};
  std::size_t confirmed_count_ = 0;
  // Allocation group: targets keep the roles a replacement gave them until the
  // group changes in a way that is not a clean replacement.
  bool roles_from_replacement_ = false;
  PfdTargetDetection detection_{};
};

}  // namespace taxi_camera
