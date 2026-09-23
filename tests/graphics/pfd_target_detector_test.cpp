#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "../../src/graphics/pfd_target_detector.hpp"

using taxi_camera::PfdTargetConfidence;
using taxi_camera::PfdTargetDetector;
using taxi_camera::PfdTargetObservation;

namespace {
void submission_activity() {
  static PfdTargetDetector detector;
  std::array<PfdTargetObservation, 4> values{};
  const auto initialize = [&] {
    detector.configure(taxi_camera::profiles::A380);
    values = {{{10, 0, 768, 1024, 5, 28, 100},
               {20, 0, 768, 1024, 5, 28, 100},
               {30, 0, 768, 1024, 5, 28, 0},
               {40, 900000, 768, 1024, 1, 28, 900000}}};
    assert(!detector.observe(values.data(), values.size(), 0).valid);
  };
  const auto add = [&](std::uint64_t left, std::uint64_t right, std::uint64_t other) {
    for (auto& value : values)
      value.submission_activity += value.id == 20 ? left : value.id == 10 ? right : value.id == 30 ? other : 0;
  };
  const auto observe = [&](std::uint64_t now) -> const auto& { return detector.observe(values.data(), values.size(), now); };
  initialize();
  for (unsigned window = 1; window <= 3; ++window) {
    add(window % 2 ? 10 : 9, window % 2 ? 9 : 10, 0);
    std::reverse(values.begin(), values.end());
    const auto& result = observe(window * 1000);
    assert(result.valid == (window == 3) && result.stable_windows == window);
  }
  assert((detector.snapshot().targets == std::array<std::uint64_t, 2>{20, 10}));
  for (const auto& value : values)
    if (value.levels == 5)
      assert(value.draws == 0);  // Never manufacture native draw counters.

  // The FBW 50% lead and 3:1 bounds apply unchanged to submitted exits.
  for (const auto rates : {std::array<std::uint64_t, 3>{90, 100, 60}, {91, 100, 60}, {30, 91, 1}, {30, 90, 1}, {0, 100, 0}}) {
    initialize();
    const bool acceptable = rates == std::array<std::uint64_t, 3>{91, 100, 60} || rates == std::array<std::uint64_t, 3>{30, 90, 1};
    for (unsigned window = 1; window <= 3; ++window) {
      add(rates[0], rates[1], rates[2]);
      assert(observe(window * 1000).valid == (acceptable && window == 3));
    }
  }
  initialize();
  for (unsigned window = 1; window <= 3; ++window) {
    add(10, 9, 0);
    observe(window * 1000);
  }
  values[0].draws = 1;
  assert(!observe(3100).valid && std::strcmp(detector.snapshot().status, "activity_source_changed") == 0);
  assert(detector.snapshot().stable_windows == 0);
  add(1000, 1000, 0);
  ++values[0].draws;
  assert(!observe(4100).valid && std::strcmp(detector.snapshot().status, "no_activity") == 0);
  for (unsigned window = 1; window <= 3; ++window) {
    values[0].draws += 9;
    values[1].draws += 10;
    assert(observe(4100 + window * 1000).valid == (window == 3));
  }
  // Losing all native draw history cannot subtract submission counters from
  // a draw baseline. The new source requires another full three windows.
  for (auto& value : values)
    value.draws = 0;
  assert(!observe(7200).valid && std::strcmp(detector.snapshot().status, "activity_source_changed") == 0);
  for (unsigned window = 1; window <= 3; ++window) {
    add(10, 9, 0);
    assert(observe(7200 + window * 1000).valid == (window == 3));
  }
  for (auto& value : values)
    value.submission_activity = 0;
  assert(!observe(10300).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);

  initialize();
  for (auto& value : values)
    value.submission_activity = 0;
  detector.reset();
  observe(0);
  add(10, 9, 0);
  assert(!observe(1000).valid && std::strcmp(detector.snapshot().status, "activity_source_changed") == 0);
  for (unsigned window = 1; window <= 3; ++window) {
    add(10, 9, 0);
    assert(observe(1000 + window * 1000).valid == (window == 3));
  }
  values[1].id = 21;  // Replacement is a new incarnation, never inherited activity.
  add(10, 9, 0);
  assert(!observe(5000).valid && detector.snapshot().stable_windows == 0);
  values[1].id = 10;
  assert(!observe(5100).valid && std::strcmp(detector.snapshot().status, "duplicate_id") == 0);

  detector.configure(taxi_camera::profiles::IniA380);
  std::array<PfdTargetObservation, 8> ini{};
  for (unsigned i = 0; i < ini.size(); ++i)
    ini[i] = {101 + i, 0, 768, 1024, 1, 27, 10};
  detector.observe(ini.data(), ini.size(), 0);
  for (unsigned window = 1; window <= 3; ++window) {
    for (auto& value : ini)
      value.submission_activity += 10;
    assert(detector.observe(ini.data(), ini.size(), window * 1000).valid == (window == 3));
  }
  assert((detector.snapshot().targets == std::array<std::uint64_t, 2>{108, 106}));
  ini[0].draws = 1;
  assert(!detector.observe(ini.data(), ini.size(), 3100).valid);
  assert(detector.snapshot().invalidates_targets && std::strcmp(detector.snapshot().status, "activity_source_changed") == 0);
  for (auto& value : ini)
    value.submission_activity += 1000;
  assert(!detector.observe(ini.data(), ini.size(), 4100).valid);
  assert(std::strcmp(detector.snapshot().status, "ini_group_inactive") == 0);
  std::puts("Submitted PFD exit activity: PASS; FBW ranking, unchanged guards, counter-source reseeding and ini ordering");
}
void a350_power_up() {
  // Captured A350-900 ULR activity after AC power was applied at the gate.
  // Three quiet five-mip surfaces remain alongside three active one-mip
  // surfaces. 1089 (left) and 1088 (right) were the manually selected working
  // pair in the reported session. A350-1000 coverage below is fixture-only.
  constexpr std::array<std::uint64_t, 8> times{89548890, 89550062, 89551171, 89552296, 89553421, 89554546, 89555671, 89556812};
  constexpr std::array<std::uint64_t, 8> pfds{686562, 689737, 692531, 695198, 698373, 700532, 702691, 705866};
  constexpr std::array<std::uint64_t, 8> other{483285, 485170, 487345, 489085, 490970, 492855, 494740, 496625};
  constexpr std::array<std::uint64_t, 8> quiet{7870, 7896, 7924, 7952, 7978, 8006, 8036, 8064};
  static PfdTargetDetector detector;
  for (const auto* profile : {&taxi_camera::profiles::A359, &taxi_camera::profiles::A35K}) {
    detector.configure(*profile);
    std::array<PfdTargetObservation, 6> values{};
    for (unsigned i = 0; i < values.size(); ++i)
      values[i] = {1084 + i, i < 3 ? quiet[0] : i == 3 ? other[0] : pfds[0], 1644, 1024, i < 3 ? 5u : 1u, i < 3 ? 28u : 27u};
    assert(!detector.observe(nullptr, 0, times[0] - 2000).valid);
    assert(!detector.observe(values.data(), 3, times[0] - 1000).valid);
    assert(detector.snapshot().targets[0] == 0 && detector.snapshot().targets[1] == 0);
    for (unsigned sample = 0; sample < times.size(); ++sample) {
      for (auto& value : values)
        value.draws = value.id < 1087 ? quiet[sample] : value.id == 1087 ? other[sample] : pfds[sample];
      std::reverse(values.begin(), values.end());
      const auto& result = detector.observe(values.data(), values.size(), times[sample]);
      // The first powered sample establishes the new resources' baseline;
      // three real windows confirm the pair. Later low-lead windows still
      // refuse new confirmation, retaining the existing ambiguity safeguard.
      const bool confirmed = sample == 3 || sample == 4;
      assert(result.valid == confirmed);
      if (confirmed)
        assert((result.targets == std::array<std::uint64_t, 2>{1089, 1088}));
      if (sample < 4)
        assert(result.stable_windows == sample);
      if (sample == 5 || sample == 6)
        assert(std::strcmp(result.status, "ambiguous_activity") == 0 && result.stable_windows == 0);
    }

    // Use different IDs and make the unrelated active surface the highest ID:
    // selection must follow draw activity, then the existing side-order rule.
    const auto initialize = [&] {
      detector.configure(*profile);
      values = {{{91, 900000, 1644, 1024, 5, 28},
                 {92, 900000, 1644, 1024, 5, 28},
                 {93, 900000, 1644, 1024, 5, 28},
                 {900, 900000, 1644, 1024, 1, 27},
                 {100, 0, 1644, 1024, 1, 27},
                 {200, 0, 1644, 1024, 1, 27}}};
      detector.observe(values.data(), values.size(), 1000);
    };
    const auto add = [&](std::uint64_t left, std::uint64_t right, std::uint64_t third) {
      for (auto& value : values)
        value.draws += value.id == 200 ? left : value.id == 100 ? right : value.id == 900 ? third : 0;
    };
    initialize();
    for (unsigned window = 1; window <= 3; ++window) {
      add(window % 2 ? 110 : 100, window % 2 ? 100 : 110, 75);
      std::reverse(values.begin(), values.end());
      const auto& result = detector.observe(values.data(), values.size(), 1000 + window * 1000);
      assert(result.valid == (window == 3));
    }
    assert((detector.snapshot().targets == std::array<std::uint64_t, 2>{200, 100}));

    for (const auto rates : {std::array<std::uint64_t, 3>{100, 100, 100}, {125, 125, 100}, {124, 124, 100}, {126, 126, 101}, {0, 100, 0}}) {
      initialize();
      for (unsigned window = 1; window <= 3; ++window) {
        add(rates[0], rates[1], rates[2]);
        const auto& result = detector.observe(values.data(), values.size(), 1000 + window * 1000);
        assert(!result.valid && result.stable_windows == 0);
        assert(std::strcmp(result.status, rates[0] ? "ambiguous_activity" : "no_activity") == 0);
      }
    }
    for (const auto rates : {std::array<std::uint64_t, 3>{126, 126, 100}, {127, 127, 101}}) {
      initialize();
      for (unsigned window = 1; window <= 3; ++window) {
        // One draw above the integral boundary, then the first integer above
        // a fractional boundary (126.25), must both confirm normally.
        add(rates[0], rates[1], rates[2]);
        assert(detector.observe(values.data(), values.size(), 1000 + window * 1000).valid == (window == 3));
      }
    }
    values[5].draws = 0;
    assert(!detector.observe(values.data(), values.size(), 4100).valid);
    assert(std::strcmp(detector.snapshot().status, "counter_reset") == 0);

    initialize();
    for (unsigned window = 1; window <= 3; ++window) {
      add(126, 126, 100);
      detector.observe(values.data(), values.size(), 1000 + window * 1000);
    }
    values[5].id = 201;
    values[5].draws = 9000000;  // A new incarnation cannot inherit past confirmation.
    assert(!detector.observe(values.data(), values.size(), 4100).valid);
    assert(std::strcmp(detector.snapshot().status, "candidate_disappeared") == 0 && detector.snapshot().stable_windows == 0);
    // The first whole window after replacement establishes the new baseline.
    detector.observe(values.data(), values.size(), 5000);
    for (unsigned window = 1; window <= 3; ++window) {
      for (auto& value : values)
        value.draws += value.id == 201 || value.id == 100 ? 126 : value.id == 900 ? 100 : 0;
      assert(detector.observe(values.data(), values.size(), 5000 + window * 1000).valid == (window == 3));
    }
    assert((detector.snapshot().targets == std::array<std::uint64_t, 2>{201, 100}));

    // Near-UINT64_MAX activity must retain exact lead/comparability decisions
    // without multiplying large counters or overflowing a scaled threshold.
    const auto wide_delta = [&](std::uint64_t first, std::uint64_t second, std::uint64_t third) -> const auto& {
      initialize();
      for (auto& value : values)
        value.draws = 0;
      detector.reset();
      detector.observe(values.data(), values.size(), 1000);
      add(first, second, third);
      return detector.observe(values.data(), values.size(), 2000);
    };
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto exact_third = (maximum / 5) * 4;
    assert(wide_delta(maximum, maximum, exact_third).stable_windows == 0);
    assert(std::strcmp(detector.snapshot().status, "ambiguous_activity") == 0);
    assert(wide_delta(maximum, maximum, exact_third - 1).stable_windows == 1);
    assert(wide_delta(maximum, maximum / 4, 1).stable_windows == 0);
    assert(std::strcmp(detector.snapshot().status, "incomparable_rates") == 0);
  }
  std::puts("A350 cold/dark power-up activity and retained rejection guards: PASS");
}

void a350_submitted_exits() {
  // A350-1000 PID44364, cold/dark power-up on2026-09-18: native draws
  // were unavailable. Five-mip auxiliaries43/44/46 led RT-exit counts;
  // one-mip47/45 led the EFIS group, with unrelated75 (highest ID) third.
  // The user confirmed47 as left;45/right follows the existing side rule.
  // Replaying this layout for A359 below is fixture coverage, not live proof.
  constexpr std::array<std::uint64_t, 6> times{145065109, 145066265, 145071265, 145076265, 145081265, 145086265};
  constexpr std::array<std::uint64_t, 6> auxiliary{88, 111, 385, 683, 978, 1259};
  constexpr std::array<std::uint64_t, 6> left{51, 65, 172, 277, 395, 522};
  constexpr std::array<std::uint64_t, 6> right{51, 65, 176, 290, 399, 526};
  constexpr std::array<std::uint64_t, 6> other{34, 43, 113, 180, 249, 317};
  static PfdTargetDetector detector;
  for (const auto* profile : {&taxi_camera::profiles::A35K, &taxi_camera::profiles::A359}) {
    std::array<PfdTargetObservation, 8> values{};
    std::size_t count = 6;
    const auto initialize = [&] {
      detector.configure(*profile);
      count = 6;
      values = {{{43, 0, 1644, 1024, 5, 28, 88},
                 {44, 0, 1644, 1024, 5, 28, 88},
                 {45, 0, 1644, 1024, 1, 27, 51},
                 {46, 0, 1644, 1024, 5, 28, 88},
                 {47, 0, 1644, 1024, 1, 27, 51},
                 {75, 0, 1644, 1024, 1, 27, 34}}};
    };
    const auto observe = [&](std::uint64_t now, bool complete = true) -> const auto& {
      return detector.observe(values.data(), count, now, complete);
    };
    const auto add = [&](std::uint64_t l = 100, std::uint64_t r = 100, std::uint64_t third = 60) {
      for (std::size_t i = 0; i < count; ++i) {
        auto& value = values[i];
        value.submission_activity += value.levels == 5 ? 200 : value.id == 45 ? r : value.id == 47 || value.id == 48 ? l : third;
      }
    };
    const auto confirms = [&](std::uint64_t baseline, std::array<std::uint64_t, 2> pair = {47, 45}) {
      for (unsigned window = 1; window <= 3; ++window) {
        add();
        const auto& result = observe(baseline + window * 1000);
        assert(result.valid == (window == 3) && result.stable_windows == window);
      }
      assert(detector.snapshot().targets == pair);
    };
    initialize();
    assert(!detector.observe(nullptr, 0, times[0] - 2000).valid);
    for (auto& value : values)
      value.submission_activity = 0;
    assert(!observe(times[0] - 1000).valid);
    for (std::size_t sample = 0; sample < times.size(); ++sample) {
      for (std::size_t i = 0; i < count; ++i) {
        auto& value = values[i];
        value.submission_activity = value.levels == 5 ? auxiliary[sample]
                                    : value.id == 47  ? left[sample]
                                    : value.id == 45  ? right[sample]
                                                      : other[sample];
      }
      std::reverse(values.begin(), values.begin() + count);
      const auto& result = observe(times[sample]);
      assert(result.valid == (sample >= 3));
      if (sample >= 3)
        assert((result.targets == std::array<std::uint64_t, 2>{47, 45}));
    }
    for (const auto& value : values)
      assert(!value.draws);

    // Two active EFIS surfaces cannot win during partial cold boot, even
    // with arbitrarily high counts. A complete third incarnation is required.
    initialize();
    count = 5;
    for (unsigned window = 0; window < 5; ++window) {
      add();
      assert(!observe(window * 1000).valid);
      assert(std::strcmp(detector.snapshot().status, "a350_group_incomplete") == 0);
    }
    count = 6;
    assert(!observe(5000).valid && detector.snapshot().stable_windows == 0);
    confirms(5000);
    assert(!observe(8100, false).valid && std::strcmp(detector.snapshot().status, "incomplete_inventory") == 0);
    assert(!observe(8200).valid && detector.snapshot().stable_windows == 0);
    confirms(8200);

    // Auxiliary count is not a side-order signal. The complete three-EFIS
    // group also works when auxiliaries are absent or one more is observed.
    initialize();
    values[0] = values[2];
    values[1] = values[4];
    values[2] = values[5];
    count = 3;
    observe(0);
    confirms(0);
    initialize();
    values[count++] = {99, 0, 1644, 1024, 5, 28, 900000};
    observe(0);
    confirms(0);

    for (const auto extra :
         {PfdTargetObservation{76, 0, 1644, 1024, 1, 27, 100}, {76, 0, 1644, 1024, 1, 28, 100}, {76, 0, 1644, 1024, 5, 27, 100}}) {
      initialize();
      values[count++] = extra;
      assert(!observe(0).valid);
      assert(std::strcmp(detector.snapshot().status,
                         extra.levels == 1 && extra.format == 27 ? "a350_group_ambiguous" : "a350_group_format") == 0);
    }
    initialize();
    values[0].id = 0;  // Excluded auxiliaries still need valid, unique IDs.
    assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "invalid_input") == 0);
    initialize();
    values[1].id = values[0].id;
    assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "duplicate_id") == 0);
    initialize();
    values[5].height = 1023;
    assert(!observe(0).valid && std::strcmp(detector.snapshot().status, "a350_group_incomplete") == 0);

    initialize();
    observe(0);
    confirms(0);
    values[4].submission_activity = 0;
    assert(!observe(3100).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);
    confirms(3100);
    for (auto& value : values)
      value.submission_activity = 0;
    assert(!observe(6200).valid && std::strcmp(detector.snapshot().status, "counter_reset") == 0);
    confirms(6200);

    for (const auto replacement : {std::pair{4u, 48ull}, std::pair{5u, 76ull}}) {
      initialize();
      observe(0);
      confirms(0);
      values[replacement.first].id = replacement.second;
      values[replacement.first].submission_activity = 100000;
      assert(!observe(3100).valid && std::strcmp(detector.snapshot().status, "a350_group_changed") == 0);
      confirms(3100, {replacement.first == 4 ? 48u : 47u, 45});
    }
    initialize();
    observe(1000);
    confirms(1000);
    assert(!observe(3999).valid && std::strcmp(detector.snapshot().status, "clock_reset") == 0);
    confirms(3999);
    assert(!observe(12000).valid && std::strcmp(detector.snapshot().status, "stale_window") == 0);
    confirms(12000);

    for (const auto rates : {std::array<std::uint64_t, 3>{100, 100, 100}, {125, 125, 100}, {301, 100, 20}, {0, 100, 0}}) {
      initialize();
      observe(0);
      for (unsigned window = 1; window <= 3; ++window) {
        add(rates[0], rates[1], rates[2]);
        assert(!observe(window * 1000).valid && detector.snapshot().stable_windows == 0);
      }
    }
    // Native draw evidence anywhere in the eligible inventory retains its
    // original precedence, including auxiliaries. Counter sources never mix.
    initialize();
    observe(0);
    confirms(0);
    values[0].draws = 1;
    assert(!observe(3100).valid && std::strcmp(detector.snapshot().status, "activity_source_changed") == 0);
    for (unsigned window = 1; window <= 3; ++window) {
      add();
      for (auto& value : values)
        value.draws += value.levels == 5 ? 200 : 0;
      assert(!observe(3100 + window * 1000).valid && std::strcmp(detector.snapshot().status, "ambiguous_activity") == 0);
    }
    for (auto& value : values)
      value.draws = 0;
    assert(!observe(6200).valid && std::strcmp(detector.snapshot().status, "activity_source_changed") == 0);
    confirms(6200);
  }
  std::puts("A350 submitted-exit EFIS group: PASS; captured A35K layout, A359 fixture, complete inventory and unchanged activity guards");
}

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
  submission_activity();
  a350_submitted_exits();
  a350_power_up();
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

  static PfdTargetDetector inboard;
  inboard.configure(taxi_camera::profiles::Pmdg777);
  const auto& pmdg = taxi_camera::profiles::Pmdg777;
  assert(pmdg.formats[0] == 0 && pmdg.mips == 0 && !pmdg.reference_guides && !pmdg.ground_speed);
  const unsigned levels = pmdg.mips ? pmdg.mips : 1u;
  // 28 is an observation format, not a scanned PMDG DXGI format.
  PfdTargetObservation one{42, 10, pmdg.width, pmdg.height, levels, 28};
  assert(!inboard.observe(&one, 1, 0, false).valid && std::strcmp(inboard.snapshot().status, "incomplete_inventory") == 0);
  inboard.reset();
  assert(!inboard.observe(&one, 1, 0).valid);
  assert(!inboard.observe(&one, 1, 1000).valid && inboard.snapshot().stable_windows == 1);
  assert(!inboard.observe(&one, 1, 2000).valid && inboard.snapshot().stable_windows == 2);
  const auto& confirmed = inboard.observe(&one, 1, 3000);
  assert(confirmed.valid && confirmed.targets[0] == 42 && confirmed.targets[1] == 0);
  std::array<PfdTargetObservation, 2> pair{one, one};
  pair[1].id = 43;
  pair[1].draws = 1000;
  inboard.reset();
  assert(!inboard.observe(pair.data(), pair.size(), 0).valid);
  assert(!inboard.observe(pair.data(), pair.size(), 1000).valid && inboard.snapshot().stable_windows == 1);
  assert(!inboard.observe(pair.data(), pair.size(), 2000).valid && inboard.snapshot().stable_windows == 2);
  const auto& last = inboard.observe(pair.data(), pair.size(), 3000);
  // The next-highest texture is the unverified lower DU (EICASCDU) guess.
  assert(last.valid && last.targets[0] == 43 && last.targets[1] == 42);
  PfdTargetObservation kept = pair[1];
  const auto& still_last = inboard.observe(&kept, 1, 4000);
  assert(still_last.valid && still_last.targets[0] == 43 && still_last.targets[1] == 0);
  std::array<PfdTargetObservation, 3> reversed{one, one, one};
  reversed[0].id = 90;
  reversed[0].draws = 1;
  reversed[1].id = 10;
  reversed[1].draws = 5000;
  reversed[2].id = 50;
  inboard.reset();
  assert(!inboard.observe(reversed.data(), reversed.size(), 0).valid);
  assert(!inboard.observe(reversed.data(), reversed.size(), 1000).valid);
  assert(!inboard.observe(reversed.data(), reversed.size(), 2000).valid);
  const auto& tail = inboard.observe(reversed.data(), reversed.size(), 3000);
  assert(tail.valid && tail.targets[0] == 90 && tail.targets[1] == 50);
  reversed[0].id = 91;
  assert(!inboard.observe(reversed.data(), reversed.size(), 4000).valid);
  assert(std::strcmp(inboard.snapshot().status, "candidate_disappeared") == 0 && inboard.snapshot().targets[0] == 0);

  // Aerosoft A346: both NDs are rectangles on one 4096 $GAUGES_UNIFIED texture.
  // A 2048 PMDG-sized texture is not a candidate for this profile.
  static PfdTargetDetector unified;
  const auto& a346 = taxi_camera::profiles::AerosoftA346;
  unified.configure(a346);
  std::array<PfdTargetObservation, 2> gauges{{{7, 50, 2048, 2048, 1, 28}, {12, 5, a346.width, a346.height, 1, 28}}};
  assert(!unified.observe(gauges.data(), gauges.size(), 0).valid);
  assert(!unified.observe(gauges.data(), gauges.size(), 1000).valid);
  assert(!unified.observe(gauges.data(), gauges.size(), 2000).valid);
  const auto& gauges_found = unified.observe(gauges.data(), gauges.size(), 3000);
  assert(gauges_found.valid && gauges_found.targets[0] == 12 && gauges_found.targets[1] == 0);

  // iniBuilds A340-300: the 2026-09-23 cockpit group was one-mip typeless
  // (format 27) textures, one of them 1560 x 2340. A five-mip 2340 x 2340
  // texture and the smaller group members are not candidates.
  static PfdTargetDetector ini_a343;
  ini_a343.configure(taxi_camera::profiles::IniA343);
  std::array<PfdTargetObservation, 4> group{
      {{30, 9, 2340, 2340, 5, 28}, {31, 9, 1024, 1024, 1, 27}, {32, 9, 1560, 2340, 1, 27}, {33, 9, 450, 400, 1, 27}}};
  for (std::uint64_t now = 0; now < 3000; now += 1000)
    assert(!ini_a343.observe(group.data(), group.size(), now).valid);
  const auto& grid = ini_a343.observe(group.data(), group.size(), 3000);
  assert(grid.valid && grid.targets[0] == 32 && grid.targets[1] == 0);

  std::puts("PFD target detector: PASS");
}
