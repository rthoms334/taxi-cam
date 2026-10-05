#include "../../src/camera/render_schedule.hpp"
#include "../../src/profiles/catalog.hpp"
#include "../../src/shared/camera_rate_policy.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using taxi_camera::EffectiveCameraRate;
using taxi_camera::ParkedRatePolicy;
using taxi_camera::native_camera::RenderSchedule;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void cadence(unsigned rate, unsigned feeds, std::uint64_t step) {
  RenderSchedule schedule;
  schedule.configure(rate, feeds);
  std::array<unsigned, 2> counts{};
  std::array<std::uint64_t, 2> last{};
  bool previous_on = false;
  unsigned closed_since = RenderSchedule::kIdleUpdatesAfterClose + 1;
  unsigned expected_feed = 0;
  for (std::uint64_t time = 0; time < 10000; time += step) {
    const auto active = schedule.tick(time);
    require(!(active[0] && active[1]), "Both cameras were scheduled in one interval");
    const bool on = active[0] || active[1];
    require(!(on && previous_on), "An active interval was not followed by an off interval");
    require(!on || closed_since > RenderSchedule::kIdleUpdatesAfterClose, "A closing update was not followed by an idle update");
    closed_since = on ? 0 : closed_since + 1;
    previous_on = on;
    for (unsigned i = 0; i < 2; ++i) {
      if (!active[i])
        continue;
      require(i == expected_feed, "Feeds did not alternate fairly");
      expected_feed = (expected_feed + 1) % feeds;
      if (counts[i])
        require(time - last[i] >= (1000 + rate - 1) / rate, "Per-camera maximum rate exceeded");
      last[i] = time;
      ++counts[i];
    }
  }
  require(counts[0] > 0 && (feeds == 1 ? counts[1] == 0 : counts[1] > 0), "A requested feed was starved or disabled feed ran");
  require(counts[0] <= rate * 10 && counts[1] <= rate * 10, "Ten-second budget exceeded");
}

void changes_and_stalls() {
  RenderSchedule schedule;
  require(schedule.rate() == taxi_camera::kDefaultCameraRate && schedule.feeds() == 2, "Defaults changed");
  schedule.configure(0, 0);
  require(schedule.rate() == taxi_camera::kMinimumParkedCameraRate && schedule.feeds() == 1, "Lower bounds were not applied");
  schedule.configure(999, 999);
  require(schedule.rate() == 60 && schedule.feeds() == 3, "Upper bounds were not applied");
  require(schedule.tick(100)[0], "Initial pulse missing");
  require(schedule.tick(10000) == std::array<bool, 3>{}, "A long stall left a camera on");
  require(schedule.tick(10000) == std::array<bool, 3>{}, "A closing update was not followed by an idle update");
  require(schedule.tick(10000)[1], "Stalled schedule did not resume with the other feed");
  require(schedule.tick(10000) == std::array<bool, 3>{}, "Duplicate timestamp did not close pulse");
  require(schedule.tick(10000) == std::array<bool, 3>{}, "Duplicate timestamp caused a catch-up burst");
  schedule.configure(15, 1);
  require(schedule.tick(10001) == std::array<bool, 3>{}, "UI change erased live deadlines");
  require(schedule.tick(10067)[0], "Single-feed configuration did not resume");
  require(schedule.tick(1) == std::array<bool, 3>{}, "Clock reversal did not close camera");
  require(schedule.tick(2) == std::array<bool, 3>{}, "Clock reversal caused an immediate burst");
  require(schedule.tick(68)[0], "Clock reversal recovery failed");
  schedule.reset();
  require(schedule.rate() == 15 && schedule.feeds() == 1, "Lifecycle reset erased configuration");
  require(schedule.tick(std::numeric_limits<std::uint64_t>::max() - 1)[0], "Large clock value failed");
  require(schedule.tick(std::numeric_limits<std::uint64_t>::max()) == std::array<bool, 3>{}, "Large timestamp did not close pulse");
}
void live_rate_changes() {
  RenderSchedule schedule;
  require(schedule.tick(0)[0], "Initial default-rate pulse");
  require(schedule.tick(1) == std::array<bool, 3>{}, "Default-rate pulse closes");
  schedule.configure(60);
  require(schedule.tick(8) == std::array<bool, 3>{}, "60fps aggregate interval rounded up");
  require(schedule.tick(9)[1], "60fps alternate feed permitted after9ms");
  require(schedule.tick(10) == std::array<bool, 3>{}, "60fps still requires full off interval");
  require(schedule.tick(17) == std::array<bool, 3>{}, "Aggregate limit prevents early next feed");
  require(schedule.tick(18)[0], "Higher rate retains previous feed timestamp");
  schedule.configure(15);
  require(schedule.tick(19) == std::array<bool, 3>{}, "Lowering rate still closes previous pulse");
  require(schedule.tick(52) == std::array<bool, 3>{}, "Lowered rate retains per-feed cooldown");
  require(schedule.tick(76)[1], "Lowered rate resumes without resetting history");
}
void suspend_and_resume() {
  for (unsigned rate : {5u, 10u, 15u, 60u}) {
    RenderSchedule schedule;
    schedule.configure(rate);
    require(schedule.tick(1000)[0], "Initial pulse before telemetry gap");
    for (std::uint64_t time = 1001; time < 7000; ++time)
      require(schedule.tick(time, true) == std::array<bool, 3>{}, "Suspension left a gate open");
    require(schedule.tick(7000)[1], "Resume must preserve the next camera without recreating the pair");
    require(schedule.tick(7001) == std::array<bool, 3>{}, "Resume must close the pulse without a catch-up burst");
    require(schedule.tick(7002) == std::array<bool, 3>{}, "Resume must respect the aggregate rate limit");
  }
}

std::array<unsigned, 2> opportunities(unsigned rate, unsigned feeds, const std::array<unsigned, 6>& intervals) {
  RenderSchedule schedule;
  schedule.configure(rate, feeds);
  std::array<unsigned, 2> count{};
  std::array<std::uint64_t, 2> last{};
  bool previous_on = false;
  unsigned next = 0, tick = 0;
  for (std::uint64_t now = 0; now < 10000; now += intervals[tick++ % intervals.size()]) {
    const auto active = schedule.tick(now);
    const bool on = active[0] || active[1];
    require(!(on && previous_on) && !(active[0] && active[1]), "Jitter skipped a mandatory closed interval");
    previous_on = on;
    for (unsigned feed = 0; feed < 2; ++feed) {
      if (!active[feed])
        continue;
      require(feed == next, "Jitter starved or reordered feeds");
      next = (next + 1) % feeds;
      if (count[feed])
        require(now - last[feed] >= (1000 + rate - 1) / rate, "Jitter exceeded the requested per-feed budget");
      last[feed] = now;
      ++count[feed];
    }
  }
  require(count[0] <= rate * 10 && count[1] <= rate * 10, "Jitter exceeded the ten-second budget");
  return count;
}

void effective_lower_budgets() {
  // At 50 manager updates/s 10, 15 and 30 all reach the idle-update cap: a gate
  // transition on two of every three updates. Lower rates must add more idle.
  const std::array<unsigned, 6> fifty_hz{20, 20, 20, 20, 20, 20};
  const auto five = opportunities(5, 2, fifty_hz);
  const auto ten = opportunities(10, 2, fifty_hz);
  const auto fifteen = opportunities(15, 2, fifty_hz);
  require(five == std::array<unsigned, 2>{50, 50}, "5fps did not reduce the 50Hz manager workload");
  require(ten == std::array<unsigned, 2>{84, 83}, "10fps did not reduce the 50Hz manager workload");
  require(fifteen == ten, "15fps exceeded the idle-update cap at 50Hz");
  require(opportunities(30, 2, fifty_hz) == fifteen, "50Hz manager saturation example changed");
  for (const auto& intervals : {std::array<unsigned, 6>{10, 10, 10, 10, 10, 10}, fifty_hz, std::array<unsigned, 6>{33, 33, 33, 33, 33, 33},
                                std::array<unsigned, 6>{8, 12, 17, 23, 10, 30}}) {
    for (unsigned feeds : {1u, 2u}) {
      const auto low = opportunities(5, feeds, intervals);
      const auto medium = opportunities(10, feeds, intervals);
      const auto original = opportunities(15, feeds, intervals);
      const auto total = [](const auto& count) { return count[0] + count[1]; };
      require(total(low) < total(medium) && total(medium) <= total(original), "Lower budgets did not reduce activation work");
      if (feeds == 1)
        require(!low[1] && !medium[1] && !original[1], "Single-feed mode activated the tail camera");
    }
  }
}

void lower_budget_changes() {
  RenderSchedule schedule;
  require(schedule.tick(1000)[0], "Initial opening before low-rate change");
  schedule.configure(5);
  require(schedule.tick(1001) == std::array<bool, 3>{}, "Lowering rate left the previous gate open");
  require(schedule.tick(1099) == std::array<bool, 3>{}, "Lowering rate erased aggregate history");
  require(schedule.tick(1100)[1], "5fps did not continue with the other feed");
  schedule.configure(10, 1);
  require(schedule.tick(1101) == std::array<bool, 3>{}, "Changing feeds left the tail open");
  require(schedule.tick(1199) == std::array<bool, 3>{}, "Feed-count change erased aggregate history");
  require(schedule.tick(1200)[0], "10fps single-feed schedule did not resume");
  schedule.configure(5, 2);
  require(schedule.tick(1201) == std::array<bool, 3>{}, "Restoring two feeds skipped closure");
  require(schedule.tick(1399) == std::array<bool, 3>{}, "Lowering rate erased the selected feed deadline");
  require(schedule.tick(1400)[0], "5fps pair did not preserve its pending feed");
  require(schedule.tick(20000) == std::array<bool, 3>{}, "Low-rate long stall did not close the active pulse");
  require(schedule.tick(20000) == std::array<bool, 3>{}, "Low-rate long stall skipped the idle update");
  require(schedule.tick(20000)[1], "Low-rate stalled pair did not resume fairly");
  require(schedule.tick(20000) == std::array<bool, 3>{}, "Low-rate duplicate timestamp left a gate open");
  require(schedule.tick(20000) == std::array<bool, 3>{}, "Low-rate stall caused a catch-up burst");
}

void parked_policy_hysteresis() {
  ParkedRatePolicy policy;
  require(!policy.parked(), "Policy starts moving");
  for (std::uint64_t now = 0; now < ParkedRatePolicy::kParkedSettleMs; now += 22)
    require(!policy.update(now, true, 0.0), "Zero speed parked before the settle time");
  require(policy.update(ParkedRatePolicy::kParkedSettleMs, true, 0.0), "Sustained zero speed did not park");
  require(policy.update(ParkedRatePolicy::kParkedSettleMs + 22, true, 0.09), "Standstill GROUND VELOCITY noise unparked");
  require(policy.update(ParkedRatePolicy::kParkedSettleMs + 44, true, 0.19), "Noise just below the threshold unparked");
  // 0.9.42 live: the earlier 0.5/1.0 kt band parked an aircraft still rolling
  // at 0.4 kt and held 5 fps until 1 kt. Any measurable motion now restores the
  // saved rate on the same update.
  require(!policy.update(ParkedRatePolicy::kParkedSettleMs + 66, true, 0.4), "A rolling aircraft stayed at the parked floor");
  require(!policy.update(ParkedRatePolicy::kParkedSettleMs + 88, true, 0.0), "A brief stop re-parked without settling");
  // Alternating stopped/creeping samples at taxi start must never park.
  ParkedRatePolicy creeping;
  for (std::uint64_t now = 0; now < 20000; now += 22)
    require(!creeping.update(now, true, (now / 22) % 2 ? 0.3 : 0.0), "Creeping taxi start parked the schedule");
  // Once parked, standstill noise below the threshold holds the parked state.
  ParkedRatePolicy holding;
  for (std::uint64_t now = 0; now <= ParkedRatePolicy::kParkedSettleMs; now += 20)
    holding.update(now, true, 0.0);
  require(holding.parked(), "Holding fixture did not park");
  for (std::uint64_t now = 4000; now < 10000; now += 22)
    require(holding.update(now, true, (now / 22) % 2 ? 0.15 : 0.0), "Standstill jitter flapped a parked schedule");
  // Rolling to a stop: the floor engages only after the aircraft has actually
  // stopped for the settle time, not while it is still coasting.
  ParkedRatePolicy coasting;
  for (std::uint64_t now = 0; now < 6000; now += 22)
    require(!coasting.update(now, true, 0.6 - 0.4 * (now / 6000.0)), "Coasting to a stop parked before the aircraft stopped");
  for (std::uint64_t now = 6000; now < 6000 + ParkedRatePolicy::kParkedSettleMs; now += 22)
    require(!coasting.update(now, true, 0.0), "Stopped aircraft parked before the settle time");
  require(coasting.update(6000 + ParkedRatePolicy::kParkedSettleMs, true, 0.0), "Stopped aircraft did not park after settling");
  // Missing or stale telemetry is moving, never a reason to lower the rate.
  require(!holding.update(10000, false, 0.0), "Invalid telemetry kept the parked floor");
  require(!holding.update(10022, true, 0.0), "Telemetry return re-parked without settling");
  require(!holding.update(10044, true, std::numeric_limits<double>::quiet_NaN()), "NaN speed parked");
  require(!holding.update(10066, true, -1.0), "Negative speed parked");
  ParkedRatePolicy reversal;
  for (std::uint64_t now = 5000; now < 6000; now += 22)
    reversal.update(now, true, 0.0);
  require(!reversal.update(100, true, 0.0), "Clock reversal parked early");
  require(!reversal.update(100 + ParkedRatePolicy::kParkedSettleMs - 22, true, 0.0), "Clock reversal did not restart the settle time");
  require(reversal.update(100 + ParkedRatePolicy::kParkedSettleMs, true, 0.0), "Settle after clock reversal failed");

  // 0.9.56 live: a parked A350 creeping around the 0.2 kt edge stepped the
  // inset 5<->10 fps twice in ~130 s. Band samples hold the parked floor; only
  // a sample at or above the moving threshold unparks, and it does so at once.
  ParkedRatePolicy creep;
  for (std::uint64_t now = 0; now <= ParkedRatePolicy::kParkedSettleMs; now += 20)
    creep.update(now, true, 0.14);
  require(creep.parked(), "Creep fixture did not park");
  for (std::uint64_t now = 4000; now < 14000; now += 22)
    require(creep.update(now, true, 0.2 + 0.14 * ((now / 22) % 2)), "Creep in the hysteresis band unparked the schedule");
  require(!creep.update(14000, true, ParkedRatePolicy::kMovingAboveKnots), "The moving threshold did not unpark at once");
  require(!creep.update(14022, true, 0.3), "A moving aircraft in the band re-parked without settling");
  require(!creep.update(14044, true, 0.0) && !creep.update(14044 + ParkedRatePolicy::kParkedSettleMs - 22, true, 0.0),
          "A stop after creep parked before the settle time");
  // Rolling to a stop through the band: below-threshold time only counts while
  // the aircraft is actually stopped; a band sample restarts the settle.
  ParkedRatePolicy stop;
  for (std::uint64_t now = 0; now < 2000; now += 22)
    require(!stop.update(now, true, 0.3), "Coasting in the band parked");
  for (std::uint64_t now = 2000; now < 2000 + ParkedRatePolicy::kParkedSettleMs - 22; now += 22)
    require(!stop.update(now, true, 0.1), "Nearly stopped aircraft parked before the settle time");
  require(!stop.update(2000 + ParkedRatePolicy::kParkedSettleMs, true, 0.25), "A band sample during the settle parked the schedule");
  for (std::uint64_t now = 5100; now < 5100 + ParkedRatePolicy::kParkedSettleMs; now += 22)
    require(!stop.update(now, true, 0.05), "The band sample did not restart the settle");
  require(stop.update(5100 + ParkedRatePolicy::kParkedSettleMs, true, 0.05), "Stopped aircraft did not park after the restarted settle");
  require(stop.update(5100 + ParkedRatePolicy::kParkedSettleMs + 22, true, 0.34), "Band noise unparked a freshly parked aircraft");
  require(!stop.update(5100 + ParkedRatePolicy::kParkedSettleMs + 44, true, 0.4), "A rolling aircraft stayed parked past the band");
}

void effective_rate_caps() {
  using namespace taxi_camera;
  const auto check = [](EffectiveCameraRate actual, unsigned rate, unsigned useful, unsigned reasons, const char* message) {
    require(actual.rate == rate && actual.useful_maximum == useful && actual.reasons == reasons, message);
  };
  check(effective_camera_rate(10, 0, false), 10, 15, kRateLimitNone, "Default rate on an unmeasured aircraft runs unchanged");
  check(effective_camera_rate(15, 0, false), 15, 15, kRateLimitNone, "The manager ceiling itself is honoured");
  check(effective_camera_rate(30, 0, false), 15, 15, kRateLimitManager, "30 is reported as manager-capped, not honoured");
  check(effective_camera_rate(60, 0, false), 15, 15, kRateLimitManager, "60 is reported as manager-capped, not honoured");
  check(effective_camera_rate(60, 80, false), 15, 15, kRateLimitManager, "A350 PFD refresh above the ceiling leaves the manager cap");
  check(effective_camera_rate(30, 16, false), 15, 15, kRateLimitManager, "ini A380 16 Hz refresh sits just above the manager ceiling");
  check(effective_camera_rate(30, 12, false), 12, 12, kRateLimitPfdRefresh, "A slower PFD caps below the manager ceiling");
  check(effective_camera_rate(10, 12, false), 10, 12, kRateLimitNone, "A rate under the PFD refresh is not capped");
  check(effective_camera_rate(30, 2, false), 5, 5, kRateLimitPfdRefresh, "PFD cap never goes below the schedule minimum");
  check(effective_camera_rate(3, 0, false), 5, 15, kRateLimitNone, "Out-of-range saved rate is clamped, not flagged");
  check(effective_camera_rate(10, 0, true), 2, 15, kRateLimitParked, "Parked default drops to the 2 per second floor");
  check(effective_camera_rate(5, 0, true, 5), 5, 15, kRateLimitNone, "Floor equal to the saved rate is not a limit");
  check(effective_camera_rate(5, 0, true, 1), 1, 15, kRateLimitParked, "A floor below the moving minimum applies while parked");
  check(effective_camera_rate(10, 0, true, 0), 10, 15, kRateLimitNone, "parked_rate 0 disables the floor");
  check(effective_camera_rate(10, 0, true, 20), 10, 15, kRateLimitNone, "A floor above the saved rate never raises it");
  check(effective_camera_rate(10, 0, true, 8), 8, 15, kRateLimitParked, "An adjusted floor applies while parked");
  check(effective_camera_rate(30, 0, true), 2, 15, kRateLimitParked | kRateLimitManager, "Parked and capped both reported");
  check(effective_camera_rate(30, 12, true), 2, 12, kRateLimitParked | kRateLimitPfdRefresh, "Parked and PFD-capped both reported");
  require(std::string_view(camera_rate_limit_name(kRateLimitNone)) == "user" &&
              std::string_view(camera_rate_limit_name(kRateLimitParked | kRateLimitManager)) == "parked" &&
              std::string_view(camera_rate_limit_name(kRateLimitPfdRefresh)) == "pfd_refresh" &&
              std::string_view(camera_rate_limit_name(kRateLimitManager)) == "manager_ceiling",
          "Rate limit names");
  require(camera_rate_limit_text(kRateLimitNone)[0] == L'\0' && camera_rate_limit_text(kRateLimitParked)[0] != L'\0',
          "Companion rate suffix");
  require(profiles::A380.pfd_refresh_hz == 0 && profiles::IniA380.pfd_refresh_hz == 16 && profiles::A359.pfd_refresh_hz == 80 &&
              profiles::A35K.pfd_refresh_hz == 80 && profiles::Pmdg777.pfd_refresh_hz == 0,
          "Catalog PFD refresh values changed");
  for (const auto* profile : profiles::Catalog)
    require(useful_camera_rate(profile->pfd_refresh_hz) == kManagerCeilingCameraRate,
            "Every catalogued PFD refresh currently sits at or above the manager ceiling");
  static_assert(effective_camera_rate(kDefaultCameraRate, 0, true).rate == kDefaultParkedCameraRate);
}

// Drives the schedule the way the bridge does: public ground speed selects
// the rate, configure() applies it to the live pair, and the gates keep
// pulsing while parked. Contracts are checked against the rate in force.
void adaptive_parked_schedule() {
  constexpr std::uint64_t step = 22;  // ~45 Hz manager, as measured live.
  constexpr unsigned user_rate = 10;
  const auto speed_at = [](std::uint64_t now) {
    if (now < 20000)
      return 0.0;  // Parked at the gate.
    if (now < 20500)
      return 0.4;  // Brake release: at the moving threshold, moving at once (0.2–0.34 kt holds; see parked_policy_hysteresis).
    if (now < 21000)
      return 0.6;  // Rolling.
    if (now < 40000)
      return 8.0;  // Taxiing.
    return 0.0;    // Holding.
  };
  RenderSchedule schedule;
  ParkedRatePolicy policy;
  std::array<unsigned, 2> parked_pulses{}, moving_pulses{};
  std::array<std::uint64_t, 2> last{};
  std::array<bool, 2> seen{};
  std::uint64_t last_any = 0, longest_parked_gap = 0;
  bool any = false, previous_on = false;
  unsigned expected_feed = 0;
  for (std::uint64_t now = 0; now < 60000; now += step) {
    const bool parked = policy.update(now, true, speed_at(now));
    // Settling completes on the first manager update at or after the settle
    // time, so allow one update of slack at each parked-window start.
    constexpr auto settle = ParkedRatePolicy::kParkedSettleMs;
    if (now < settle || (now >= 20000 && now < 40000 + settle))
      require(!parked, "Parked outside the expected windows");
    else if (now >= settle + step && (now < 20000 || now >= 40000 + settle + 2 * step))
      require(parked, "Not parked inside the expected windows");
    const auto effective = taxi_camera::effective_camera_rate(user_rate, 0, parked);
    require(effective.rate == (parked ? taxi_camera::kDefaultParkedCameraRate : user_rate), "Adaptive rate selection");
    schedule.configure(effective.rate, 2);
    const auto active = schedule.tick(now);
    require(!(active[0] && active[1]), "Adaptive path scheduled both cameras at once");
    const bool on = active[0] || active[1];
    require(!(on && previous_on), "Adaptive path skipped the mandatory closed interval");
    previous_on = on;
    if (!on)
      continue;
    const unsigned feed = active[0] ? 0 : 1;
    require(feed == expected_feed, "Adaptive path broke feed alternation");
    expected_feed = (expected_feed + 1) % 2;
    const auto per_feed_ms = (1000 + effective.rate - 1) / effective.rate;
    const auto between_ms = (1000 + 2 * effective.rate - 1) / (2 * effective.rate);
    if (any)
      require(now - last_any >= between_ms, "Adaptive rate change caused a catch-up burst");
    if (seen[feed])
      require(now - last[feed] >= per_feed_ms, "Adaptive rate change exceeded the per-camera budget");
    if (any && parked && policy.parked())
      longest_parked_gap = std::max(longest_parked_gap, now - last_any);
    any = true;
    seen[feed] = true;
    last_any = now;
    last[feed] = now;
    (parked ? parked_pulses : moving_pulses)[feed]++;
  }
  // Parked windows: 3–20 s and 43–60 s = 34 s; moving: 0–3 s and 20–43 s = 26 s (creep counts as moving).
  const auto total = [](const std::array<unsigned, 2>& count) { return count[0] + count[1]; };
  const double parked_per_second = total(parked_pulses) / 34.0, moving_per_second = total(moving_pulses) / 26.0;
  require(parked_pulses[0] > 0 && parked_pulses[1] > 0, "Parked floor closed a camera view");
  constexpr unsigned floor = taxi_camera::kDefaultParkedCameraRate;
  require(longest_parked_gap <= (1000 + floor - 1) / floor + step, "Parked schedule stopped pulsing");
  require(total(parked_pulses) <= floor * 2 * 34, "Parked window exceeded the floor budget");
  require(parked_per_second < 0.3 * moving_per_second, "Parked floor did not reduce activation work materially");
  require(moving_per_second > 12 && moving_per_second <= 20, "Moving window did not return to the saved rate");
  require(parked_per_second > 3 && parked_per_second <= 2 * floor, "Parked window did not run at the floor");
}

// OMDB with the iniBuilds A380 ran near 18 fps. Every pulse is followed by a
// closing and an idle update, so at that cadence two feeds pulse on every third
// update at 5 or 10 per second alike; only a parked floor below that lowers it.
void low_frame_rate_share() {
  const std::array<unsigned, 6> eighteen_fps{55, 55, 56, 55, 55, 56};
  const auto total = [](const std::array<unsigned, 2>& count) { return count[0] + count[1]; };
  const auto updates = 10000 / 55;
  const auto ten = total(opportunities(10, 2, eighteen_fps));
  require(total(opportunities(5, 2, eighteen_fps)) == ten && ten >= updates / 3 - 1 && ten <= updates / 3 + 1,
          "At 18 fps rates 5 and 10 no longer both pulse on every third update");
  const auto parked = total(opportunities(taxi_camera::kDefaultParkedCameraRate, 2, eighteen_fps));
  require(parked * 100 <= ten * 65 && parked * 100 >= ten * 55, "The parked floor did not cut the 18 fps render share to about 20%");
}

// Dynamic tail rate: every other turn of the non-nose feeds is an idle slot.
std::array<unsigned, 3> priority_counts(unsigned rate, unsigned feeds, std::uint64_t step, bool priority, std::uint64_t duration = 10000) {
  RenderSchedule schedule;
  schedule.configure(rate, feeds, priority);
  std::array<unsigned, 3> count{};
  std::array<std::uint64_t, 3> last{};
  bool previous_on = false;
  for (std::uint64_t now = 0; now < duration; now += step) {
    const auto active = schedule.tick(now);
    const unsigned open = unsigned(active[0]) + unsigned(active[1]) + unsigned(active[2]);
    require(open <= 1, "Nose priority opened two feeds in one update");
    require(!(open && previous_on), "Nose priority skipped a mandatory closed update");
    previous_on = open != 0;
    for (unsigned feed = 0; feed < feeds; ++feed)
      if (active[feed]) {
        if (count[feed])
          require(now - last[feed] >= (1000 + rate - 1) / rate, "Nose priority exceeded a per-feed budget");
        last[feed] = now;
        ++count[feed];
      }
  }
  return count;
}

// 2026-10-04 RJTT, PMDG 777, three feeds near 24 fps: rates 5 and 10 both
// changed a gate on 95-100 % of updates, each change a validated inspection.
// The idle update caps gate changes at two in three and extra views at one in three.
void three_feed_frame_budget() {
  for (unsigned rate : {5u, 10u, 15u}) {
    RenderSchedule schedule;
    schedule.configure(rate, 3);
    unsigned updates = 0, changes = 0, opens = 0;
    std::array<unsigned, 3> per_feed{};
    std::array<bool, 3> previous{};
    for (std::uint64_t now = 0; now < 10000; now += 42, ++updates) {
      const auto active = schedule.tick(now);
      changes += active != previous;
      previous = active;
      for (unsigned feed = 0; feed < 3; ++feed)
        if (active[feed]) {
          ++opens;
          ++per_feed[feed];
        }
    }
    require(changes * 3 <= updates * 2 + 3, "Three feeds at 24 fps changed a gate on more than two updates in three");
    require(opens * 3 <= updates + 3 && opens * 3 + 3 >= updates, "Three feeds at 24 fps did not open one update in three");
    require(per_feed[0] + 1 >= per_feed[2] && per_feed[2] + 1 >= per_feed[0], "Three feeds at 24 fps were not served fairly");
  }
}

void nose_priority_schedule() {
  // 18 fps: nose, tail, nose, idle, each followed by its closing and idle updates.
  RenderSchedule schedule;
  schedule.configure(10, 2, true);
  require(schedule.nose_priority(), "Nose priority was not configured");
  const std::array<int, 24> expected{0, -1, -1, 1, -1, -1, 0, -1, -1, -1, -1, -1, 0, -1, -1, 1, -1, -1, 0, -1, -1, -1, -1, -1};
  for (unsigned tick = 0; tick < expected.size(); ++tick) {
    const auto active = schedule.tick(tick * 55);
    const int opened = active[0] ? 0 : active[1] ? 1 : active[2] ? 2 : -1;
    require(opened == expected[tick], "Nose priority did not run nose, tail, nose, idle at 18 fps");
  }
  for (const std::uint64_t step : {22u, 33u, 55u, 56u}) {
    const auto equal = priority_counts(10, 2, step, false);
    const auto priority = priority_counts(10, 2, step, true);
    require(priority[0] + 1 >= equal[0] * 3 / 4, "Nose priority starved the nose");
    require(priority[1] * 2 <= equal[1] + 2 && priority[1] * 2 + 4 >= equal[1], "Nose priority did not halve the tail");
    require(priority[0] + priority[1] < equal[0] + equal[1], "Nose priority did not reduce the render share");
  }
  // At 18 fps the nose keeps its cadence exactly and the total drops by a quarter.
  const auto equal = priority_counts(10, 2, 55, false);
  const auto priority = priority_counts(10, 2, 55, true);
  require(priority[0] + 1 >= equal[0] && priority[0] <= equal[0] + 1, "At 18 fps nose priority changed the nose cadence");
  const auto total = [](const std::array<unsigned, 3>& c) { return c[0] + c[1] + c[2]; };
  require(total(priority) * 100 <= total(equal) * 77 && total(priority) * 100 >= total(equal) * 73,
          "At 18 fps nose priority did not remove about a quarter of the renders");
  // Three feeds: the nose keeps its turns, each wing gets every other one.
  const auto equal3 = priority_counts(10, 3, 55, false);
  const auto priority3 = priority_counts(10, 3, 55, true);
  require(priority3[0] + 1 >= equal3[0] && priority3[1] * 2 <= equal3[1] + 2 && priority3[2] * 2 <= equal3[2] + 2 && priority3[1] &&
              priority3[2],
          "Three-feed nose priority starved or failed to halve a wing");
  // Single feed: nothing to skip.
  require(priority_counts(10, 1, 55, true) == priority_counts(10, 1, 55, false), "Single-feed schedule changed under nose priority");
  // Switching priority on and off every few updates never bursts or opens two.
  RenderSchedule toggled;
  bool previous_on = false;
  std::uint64_t last_any = 0;
  bool any = false;
  for (std::uint64_t now = 0; now < 20000; now += 22) {
    toggled.configure(10, 2, (now / 330) % 2 == 0);
    const auto active = toggled.tick(now);
    const bool on = active[0] || active[1];
    require(!(active[0] && active[1]) && !(on && previous_on), "Priority switching broke the closed-update contract");
    if (on) {
      require(!any || now - last_any >= 50, "Priority switching caused a catch-up burst");
      any = true;
      last_any = now;
    }
    previous_on = on;
  }
  // Suspension still closes everything, including a pending idle slot.
  RenderSchedule suspended;
  suspended.configure(10, 2, true);
  require(suspended.tick(0)[0], "Priority schedule did not open the nose first");
  for (std::uint64_t now = 1; now < 3000; now += 55)
    require(suspended.tick(now, true) == std::array<bool, 3>{}, "Suspension left a priority gate open");
}

void nose_priority_policy() {
  using taxi_camera::heading_change_degrees;
  using taxi_camera::NosePriorityPolicy;
  constexpr double degrees = 3.14159265358979323846 / 180;
  const std::array<double, 3> up{0, 1, 0};
  const auto forward = [&](double heading, double pitch = 0) {
    return std::array<double, 3>{std::sin(heading * degrees) * std::cos(pitch * degrees), std::sin(pitch * degrees),
                                 std::cos(heading * degrees) * std::cos(pitch * degrees)};
  };
  require(std::abs(heading_change_degrees(forward(10), forward(15), up) - 5) < 1e-9 &&
              std::abs(heading_change_degrees(forward(15), forward(10), up) + 5) < 1e-9,
          "Heading change magnitude or sign");
  require(std::abs(heading_change_degrees(forward(359), forward(1), up) - 2) < 1e-9, "Heading change across north");
  require(std::abs(heading_change_degrees(forward(30, 0), forward(30, 4), up)) < 1e-9, "Pitch alone counted as turning");
  require(std::isnan(heading_change_degrees(up, forward(0), up)), "Degenerate forward vector produced a heading change");

  // Drives the policy with 5 ms control ticks so simulated frame rates stay exact.
  struct Drive {
    NosePriorityPolicy policy;
    std::uint64_t now = 0, frames = 0, pose_ms = 0;
    double heading = 0;
    bool run(std::uint64_t until, double turn_dps, double fps, bool enabled = true, bool moving = true, bool pose = true) {
      bool result = false;
      for (; now < until; now += 5) {
        const auto frame_ms = static_cast<std::uint64_t>(1000 / fps);
        if (now - pose_ms >= frame_ms) {
          heading += turn_dps * static_cast<double>(now - pose_ms) / 1000;
          pose_ms = now;
          ++frames;
        }
        NosePriorityPolicy::Input in;
        in.now_ms = now;
        in.enabled = enabled;
        in.moving = moving;
        in.pose_valid = pose;
        in.pose_sample_ms = pose_ms;
        const double h = heading * 3.14159265358979323846 / 180;
        in.forward = {std::sin(h), 0, std::cos(h)};
        in.up = {0, 1, 0};
        in.sim_frames = frames;
        result = policy.update(in);
      }
      return result;
    }
  };
  Drive straight;
  straight.now = straight.pose_ms = 1000;
  require(!straight.run(2000, 0, 18), "Priority before a frame-rate window and straight hold");
  require(straight.run(6000, 0, 18), "Straight taxi at 18 fps did not give the nose priority");
  require(std::abs(straight.policy.frame_rate() - 18) < 1.5 && straight.policy.turn_rate() < 0.5, "Measured rates");
  require(!straight.run(6300, 6, 18), "A 6 deg/s turn kept nose priority");
  require(!straight.run(8200, 0, 18), "Priority returned before the straight hold after a turn");
  require(straight.run(9000, 0, 18), "Priority did not return after the straight hold");
  require(straight.run(12000, 2, 18), "A gentle curve inside the band dropped priority on a straight");
  require(!straight.run(12500, 4, 18), "Entering a turn kept priority");
  require(!straight.run(16000, 2, 18), "A gentle curve inside the band ended a turn");
  require(straight.run(19000, 0, 18), "Straight rolling after a curve did not regain priority");
  require(!straight.run(19500, 0, 18, false), "Disabled setting kept priority");
  require(!straight.run(20000, 0, 18, true, false), "Parked or unknown speed kept priority");
  require(straight.run(20100, 0, 18), "Priority did not resume when moving again");
  require(!straight.run(20500, 0, 18, true, true, false), "Invalid pose kept priority");
  require(!straight.run(22000, 0, 18), "Priority returned before a fresh straight hold after a telemetry gap");
  require(straight.run(24000, 0, 18), "Priority did not recover after the telemetry gap");

  Drive fast;
  require(!fast.run(8000, 0, 30), "A 30 fps simulator used nose priority");
  Drive hysteresis;
  require(hysteresis.run(8000, 0, 20), "Nose priority missing at 20 fps");
  require(hysteresis.run(14000, 0, 25.6), "Nose priority dropped inside the frame-rate band");
  require(!hysteresis.run(20000, 0, 31), "Nose priority kept above the frame-rate band");
  require(!hysteresis.run(26000, 0, 25.6), "Nose priority returned inside the frame-rate band");
  require(hysteresis.run(32000, 0, 20), "Nose priority did not return below the band");

  // A stalled pose (no new samples) is stale after a second: back to equal.
  Drive stalled;
  require(stalled.run(8000, 0, 18), "Stall fixture did not reach priority");
  NosePriorityPolicy::Input frozen;
  frozen.now_ms = 8000 + NosePriorityPolicy::kHeadingStaleMs + 25;
  frozen.enabled = frozen.moving = frozen.pose_valid = true;
  frozen.pose_sample_ms = stalled.pose_ms;
  frozen.forward = {0, 0, 1};
  frozen.up = {0, 1, 0};
  frozen.sim_frames = stalled.frames;
  require(!stalled.policy.update(frozen), "A stale pose kept nose priority");
}

void adaptive_rate_switch_contract() {
  // Lowering to the floor right after an opening pulse keeps that pulse's
  // mandatory close and its aggregate deadline; raising back never bursts.
  RenderSchedule schedule;
  schedule.configure(10, 2);
  require(schedule.tick(1000)[0], "Opening pulse before parking");
  schedule.configure(taxi_camera::effective_camera_rate(10, 0, true).rate, 2);
  require(schedule.tick(1001) == std::array<bool, 3>{}, "Parking did not close the open pulse");
  require(schedule.tick(1249) == std::array<bool, 3>{}, "Parking erased the aggregate deadline");
  require(schedule.tick(1250)[1], "Parked floor did not continue with the other feed");
  schedule.configure(taxi_camera::effective_camera_rate(10, 0, false).rate, 2);
  require(schedule.tick(1251) == std::array<bool, 3>{}, "Unparking left the tail gate open");
  require(schedule.tick(1299) == std::array<bool, 3>{}, "Unparking burst ahead of the aggregate interval");
  require(schedule.tick(1300)[0], "Unparked schedule did not resume at the saved rate");
  require(schedule.rate() == 10, "Saved rate not restored after unparking");
}
}  // namespace

int main() {
  try {
    for (unsigned rate = taxi_camera::kMinimumParkedCameraRate; rate <= 60; ++rate)
      for (unsigned feeds : {1u, 2u})
        for (std::uint64_t step : {1, 5, 9, 10, 16, 20, 33, 50, 91, 250, 1000})
          cadence(rate, feeds, step);
    changes_and_stalls();
    live_rate_changes();
    suspend_and_resume();
    effective_lower_budgets();
    lower_budget_changes();
    parked_policy_hysteresis();
    effective_rate_caps();
    adaptive_parked_schedule();
    low_frame_rate_share();
    three_feed_frame_budget();
    nose_priority_schedule();
    nose_priority_policy();
    adaptive_rate_switch_contract();
    std::printf(
        "PASS: %u render-schedule checks; rate limits, alternating feeds, mandatory off and idle intervals, "
        "no catch-up bursts, parked floor, nose priority and rate caps.\n",
        checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
