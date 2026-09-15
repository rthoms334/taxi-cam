#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../../src/graphics/pfd_target_detector.hpp"

using taxi_camera::PfdTargetConfidence;
using taxi_camera::PfdTargetDetector;
using taxi_camera::PfdTargetObservation;

namespace {
void ini_a380_group() {
  static PfdTargetDetector detector;
  detector.configure(taxi_camera::profiles::IniA380);
  // Captured incarnation ordering from two different simulator sessions.
  // Session two has an ID gap because other object types share the ID counter.
  for (const auto ids : {std::array<std::uint64_t, 8>{229, 230, 231, 232, 233, 234, 235, 236},
                         std::array<std::uint64_t, 8>{203, 205, 206, 207, 208, 209, 210, 211}}) {
    std::array<PfdTargetObservation, 8> values{};
    for (unsigned i = 0; i < values.size(); ++i)
      values[i] = {ids[i], 10000 + i, 768, 1024, 1, 27};
    detector.reset();
    assert(!detector.observe(values.data(), values.size(), 0).valid);
    for (unsigned window = 1; window <= 3; ++window) {
      for (auto& value : values)
        value.draws += value.id == ids[2] ? 140 : 100;  // A different display is busiest.
      std::reverse(values.begin(), values.end());
      const auto& result = detector.observe(values.data(), values.size(), window * 1000);
      assert(result.valid == (window == 3) && result.stable_windows == window);
    }
    assert((detector.snapshot().targets == std::array<std::uint64_t, 2>{ids[7], ids[5]}));
  }

  std::array<PfdTargetObservation, 9> values{};
  const auto initialize = [&] {
    detector.reset();
    for (unsigned i = 0; i < values.size(); ++i)
      values[i] = {100 + i * 2, 1000, 768, 1024, 1, 27};
  };
  const auto add = [&] {
    for (auto& value : values)
      value.draws += 100;
  };
  const auto observe = [&](std::uint64_t now, std::size_t count = 8, bool complete = true) -> const auto& {
    return detector.observe(values.data(), count, now, complete);
  };
  const auto reason = [&](const char* expected, bool invalidates = true) {
    assert(!detector.snapshot().valid && detector.snapshot().targets[0] == 0 && detector.snapshot().targets[1] == 0);
    assert(detector.snapshot().stable_windows == 0 && std::strcmp(detector.snapshot().status, expected) == 0);
    assert(detector.snapshot().invalidates_targets == invalidates);
  };
  const auto confirm = [&] {
    initialize();
    observe(0);
    for (unsigned window = 1; window <= 3; ++window) {
      add();
      observe(window * 1000);
    }
    assert(detector.snapshot().valid);
  };

  // Never rank a partial group, even when only two candidates are very busy.
  for (unsigned count = 0; count < 8; ++count) {
    initialize();
    observe(0, count);
    for (unsigned window = 1; window <= 4; ++window) {
      add();
      observe(window * 1000, count);
      reason("ini_group_incomplete");
    }
  }
  confirm();
  observe(3100, 9);  // A late extra invalidates immediately, before a full window.
  reason("ini_group_ambiguous");
  observe(3200, 8);
  reason("ini_group_changed");
  for (unsigned window = 1; window <= 3; ++window) {
    add();
    assert(observe(3200 + window * 1000).valid == (window == 3));
  }
  observe(6300, 7);
  reason("ini_group_incomplete");

  confirm();
  values[0].id = 99;  // Unselected-member replacement also invalidates the pair.
  observe(3100);
  reason("ini_group_changed");
  for (unsigned window = 1; window <= 3; ++window) {
    add();
    assert(observe(3100 + window * 1000).valid == (window == 3));
  }
  confirm();
  values[0].draws = 0;
  observe(3100);
  reason("counter_reset");
  confirm();
  values[0].draws = UINT64_MAX;
  observe(3100);
  reason("counter_saturated");
  confirm();
  observe(2999);
  reason("clock_reset", false);
  confirm();
  observe(8001);
  reason("stale_window", false);
  confirm();
  values[0].id = 99;  // A stale clock must not hide membership loss.
  observe(8001);
  reason("ini_group_changed");
  confirm();
  observe(3100, 8, false);  // Registry saturation/lost observation is not a complete group.
  reason("incomplete_inventory");
  observe(3200);
  reason("warming_up", false);

  for (unsigned changed = 0; changed < 8; ++changed) {
    confirm();
    add();
    values[changed].draws -= 100;
    observe(4000);
    reason("ini_group_inactive", false);
  }
  for (unsigned format : {28u, 29u, 87u, 90u, 91u}) {
    confirm();
    values[0].format = format;  // Still admitted manually, but not this empirical rule.
    observe(3100);
    reason("ini_group_format");
  }
  for (unsigned field = 0; field < 4; ++field) {
    confirm();
    if (field == 0)
      values[0].width = 767;
    if (field == 1)
      values[0].height = 1023;
    if (field == 2)
      values[0].levels = 5;
    if (field == 3)
      values[0].format = 26;
    observe(3100);
    reason("ini_group_incomplete");
  }
  confirm();
  values[0].id = values[1].id;
  observe(3100);
  reason("duplicate_id");
  confirm();
  values[0].id = 0;
  observe(3100);
  reason("invalid_input");
  confirm();
  detector.observe(values.data(), PfdTargetDetector::capacity + 1, 3100);
  reason("capacity_exceeded");

  // Ignored five-mip static resources do not fill a missing active slot or
  // create false ambiguity when the full one-mip group is present.
  initialize();
  values[8].levels = 5;
  observe(0, 9);
  for (unsigned window = 1; window <= 3; ++window) {
    add();
    assert(observe(window * 1000, 9).valid == (window == 3));
  }
  assert((detector.snapshot().targets == std::array<std::uint64_t, 2>{114, 110}));
  std::puts("ini A380 complete allocation group: PASS");
}
}  // namespace

int main() {
  ini_a380_group();
  static PfdTargetDetector detector;
  std::array<PfdTargetObservation, 3> values{
      {{10, 100000, 768, 1024, 5, 28}, {20, 200000, 768, 1024, 5, 28}, {30, 9000000, 768, 1024, 5, 28}}};
  auto observe = [&](std::uint64_t now) -> const auto& { return detector.observe(values.data(), values.size(), now); };
  auto add = [&](std::uint64_t left, std::uint64_t right, std::uint64_t other) {
    for (auto& value : values)
      value.draws += value.id == 20 ? left : value.id == 10 ? right : other;
  };
  assert(!observe(0).valid && detector.snapshot().stable_windows == 0);
  add(100, 90, 10);
  assert(!observe(999).valid && detector.snapshot().stable_windows == 0);
  assert(!observe(1000).valid && detector.snapshot().stable_windows == 1);
  assert(detector.snapshot().targets[0] == 0 && detector.snapshot().confidence == PfdTargetConfidence::stabilizing);
  // Reverse both enumeration order and busiest-PFD rank. Side mapping is by ID.
  std::swap(values[0], values[2]);
  add(90, 100, 10);
  assert(!observe(2000).valid && detector.snapshot().stable_windows == 2);
  add(100, 100, 10);
  assert(observe(3000).valid);
  assert(detector.snapshot().targets[0] == 20 && detector.snapshot().targets[1] == 10);
  assert(detector.snapshot().confidence == PfdTargetConfidence::confirmed);
  assert(!observe(4000).valid && std::strcmp(detector.snapshot().status, "no_activity") == 0);
  assert(detector.snapshot().targets[0] == 0 && detector.snapshot().stable_windows == 0);

  add(90, 100, 60);  // Exactly 1.5 times third place is not a clear lead.
  assert(!observe(5000).valid && std::strcmp(detector.snapshot().status, "ambiguous_activity") == 0);
  add(91, 100, 60);
  assert(!observe(6000).valid && detector.snapshot().stable_windows == 1);
  add(30, 91, 1);  // Just over the 3:1 comparability limit.
  assert(!observe(7000).valid && std::strcmp(detector.snapshot().status, "incomparable_rates") == 0);
  add(30, 90, 1);
  assert(!observe(8000).valid && detector.snapshot().stable_windows == 1);

  // Counter rollback is detected even before the next complete time window.
  values[0].draws = 0;
  assert(!observe(8100).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);
  assert(detector.snapshot().stable_windows == 0);
  add(100, 90, 10);
  assert(!observe(9100).valid && detector.snapshot().stable_windows == 1);
  assert(!observe(9000).valid && std::strcmp(detector.snapshot().status, "clock_reset") == 0);
  add(100, 90, 10);
  assert(!observe(15001).valid && std::strcmp(detector.snapshot().status, "stale_window") == 0);
  assert(detector.snapshot().stable_windows == 0);

  detector.reset();
  observe(0);
  for (unsigned window = 1; window <= 3; ++window) {
    add(100, 90, 10);
    observe(window * 1000);
  }
  assert(detector.snapshot().valid);
  for (auto& value : values)
    if (value.id == 20)
      value.width = 767;
  assert(!observe(3100).valid && std::strcmp(detector.snapshot().status, "candidate_disappeared") == 0);
  assert(detector.snapshot().targets[0] == 0);
  assert(!detector.observe(nullptr, 0, 3200).valid && std::strcmp(detector.snapshot().status, "no_candidates") == 0);
  assert(!detector.observe(nullptr, 1, 3300).valid && std::strcmp(detector.snapshot().status, "invalid_input") == 0);
  assert(!detector.observe(values.data(), PfdTargetDetector::capacity + 1, 3400).valid);
  assert(std::strcmp(detector.snapshot().status, "capacity_exceeded") == 0);

  values = {{{10, 0, 768, 1024, 5, 28}, {20, 0, 768, 1024, 5, 28}, {30, 0, 768, 1024, 5, 28}}};
  values[1].id = 10;
  assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "duplicate_id") == 0);
  values[1].id = 0;
  assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "invalid_input") == 0);
  values[1].id = 20;
  for (unsigned field = 0; field < 4; ++field) {
    detector.reset();
    auto wrong = values;
    for (auto& value : wrong) {
      if (field == 0)
        value.width = 1024;
      if (field == 1)
        value.height = 768;
      if (field == 2)
        value.levels = 1;
      if (field == 3)
        value.format = 26;
    }
    assert(!detector.observe(wrong.data(), wrong.size(), 0).valid);
    assert(std::strcmp(detector.snapshot().status, "no_candidates") == 0);
  }

  // UINT64_MAX deltas exercise ratio checks without multiplication overflow.
  detector.reset();
  observe(0);
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  add(maximum, maximum / 2, maximum / 4);
  assert(!observe(1000).valid && detector.snapshot().stable_windows == 1);
  values[1].draws = 0;
  assert(!observe(2000).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);

  static std::array<PfdTargetObservation, PfdTargetDetector::capacity> full;
  for (std::size_t index = 0; index < full.size(); ++index)
    full[index] = {index + 1, 0, 768, 1024, 5, 28};
  detector.reset();
  assert(!detector.observe(full.data(), full.size(), 0).valid);
  for (unsigned window = 1; window <= 3; ++window) {
    full[100].draws += 100;
    full.back().draws += 90;
    detector.observe(full.data(), full.size(), window * 1000);
  }
  assert(detector.snapshot().valid && detector.snapshot().targets[0] == full.size() && detector.snapshot().targets[1] == 101);
  detector.reset();
  assert(!detector.snapshot().valid && detector.snapshot().targets[0] == 0 && detector.snapshot().stable_windows == 0);

  std::puts("PFD target detector: PASS");
}
