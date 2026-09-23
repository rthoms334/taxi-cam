#include <cassert>
#include <cstdio>
#include <limits>

#include "../../src/graphics/pfd_target_detector.hpp"
#include "../../src/graphics/taxi_button_routes.hpp"

int main() {
  taxi_camera::TaxiButtonIntent intent;
  assert(intent.observe(1, false, 3u).buttons == 0);  // No invented initial ON.
  assert(intent.observe(10, true, 1u).buttons == 1);
  auto signal = intent.observe(100, false, 0u);
  assert(signal.buttons == 1 && signal.held && !signal.timed_out);
  assert(intent.observe(108, true, 1u).buttons == 1);  // Observed eight-ms gap.
  assert(intent.observe(200, false, 0u).buttons == 1);
  assert(intent.observe(206, true, 1u).buttons == 1);  // Observed six-ms gap.
  assert(intent.observe(300, false, 0u).held);
  signal = intent.observe(2299, false, 0u);
  assert(signal.buttons == 1 && signal.held && !signal.timed_out);
  signal = intent.observe(2300, false, 0u);
  assert(signal.buttons == 0 && !signal.held && signal.timed_out);
  assert(intent.observe(2400, true, 3u).buttons == 3);    // Freshness recovery is automatic.
  assert(intent.observe(2401, true, 0u).buttons == 0);  // Fresh OFF is immediate.
  assert(intent.observe(2500, false, 3u).buttons == 0);   // Invalid values cannot turn on.
  assert(intent.observe(2600, true, 2u).buttons == 2);
  assert(intent.observe(2700, false, 0u).buttons == 2);
  signal = intent.observe(2699, false, 0u);
  assert(signal.buttons == 0 && signal.timed_out);              // Clock regression closes.
  assert(intent.observe(2701, false, 0u).timed_out);  // Only a fresh response clears expiry.
  intent.reset();
  assert(intent.observe(2800, false, 3u).buttons == 0);  // Manual override clears old intent.

  taxi_camera::TaxiButtonRoutes routes;
  assert(routes.targets[0] == 0 && routes.targets[1] == 0);
  assert(routes.active_mask(true, 3u) == 0);
  assert(!routes.matches(0, 3) && !routes.matches(101, 3));
  assert(!routes.assign(0, 0) && !routes.assign(2, 101));
  assert(!routes.assign(std::numeric_limits<unsigned>::max(), 101));

  assert(routes.assign(0, 101));
  assert(routes.active_mask(true, 3u) == 1);
  assert(routes.active_mask(true, 2u) == 0);
  assert(routes.assign(0, 101));  // Reassigning the same side is harmless.
  assert(!routes.assign(1, 101));
  assert(routes.targets[0] == 101 && routes.targets[1] == 0);
  assert(routes.assign(1, 202));

  assert(routes.active_mask(true, 0u) == 0);
  assert(routes.active_mask(true, 1u) == 1);
  assert(routes.active_mask(true, 2u) == 2);
  assert(routes.active_mask(true, 3u) == 3);
  for (unsigned buttons = 0; buttons < 4; ++buttons)
    assert(routes.active_mask(false, buttons) == 0);
  assert(routes.matches(101, 1) && !routes.matches(202, 1));
  assert(routes.matches(202, 2) && !routes.matches(101, 2));
  assert(routes.matches(101, 3) && routes.matches(202, 3));
  assert(!routes.matches(0, 3) && !routes.matches(303, 3));
  assert(!routes.matches(101, 0) && !routes.matches(101, 4));

  assert(!routes.assign(0, 202));
  assert(routes.targets[0] == 101 && routes.targets[1] == 202);
  assert(routes.assign(0, 303));
  assert(!routes.matches(101, 3) && routes.matches(303, 1));
  routes.forget(101);  // A retired old incarnation cannot clear its replacement.
  routes.forget(999);
  routes.forget(0);
  assert(routes.targets[0] == 303 && routes.targets[1] == 202);
  routes.forget(303);
  assert(routes.active_mask(true, 3u) == 2);
  assert(!routes.matches(303, 3) && routes.matches(202, 2));
  assert(routes.assign(0, std::numeric_limits<std::uint64_t>::max()));
  assert(routes.matches(std::numeric_limits<std::uint64_t>::max(), 1));
  routes.forget(202);
  assert(routes.active_mask(true, 3u) == 1);
  routes.forget(std::numeric_limits<std::uint64_t>::max());
  assert(routes.active_mask(true, 3u) == 0);
  assert(routes.assign(1, 303));  // A forgotten ID may be explicitly assigned again.
  assert(routes.active_mask(true, 3u) == 2);

  taxi_camera::TaxiButtonRoutes replacements;
  assert(!replacements.adopt_detected({0, 1}));
  assert(!replacements.adopt_detected({1, 1}));
  taxi_camera::TaxiButtonRoutes single;
  assert(!single.adopt_detected({77, 0}));
  assert(!single.adopt_single(0));
  assert((single.adopt_single(77) && single.targets == taxi_camera::TaxiButtonRoutes::Sides{77, 77, 77}));
  assert(single.adopt_single(77) && !single.adopt_single(78));
  assert(single.active_mask(true, 7u) == 7 && single.matches(77, 4u) && !single.matches(78, 4u));
  taxi_camera::TaxiButtonIntent sd;
  assert(sd.observe(10, true, 4u).buttons == 4 && sd.observe(20, true, 0xffu).buttons == 7);
  taxi_camera::TaxiButtonRoutes chosen;
  assert((chosen.select_single(55) && chosen.targets == taxi_camera::TaxiButtonRoutes::Sides{55, 55, 55} && chosen.assigned_mask() == 7));
  assert(chosen.select_single(0) && chosen.assigned_mask() == 0);
  assert((chosen.select_explicit({5, 6}) && chosen.targets == taxi_camera::TaxiButtonRoutes::Sides{5, 6, 0} && chosen.assigned_mask() == 3));
  assert(!chosen.matches(5, 4u) && chosen.active_mask(true, 7u) == 3);
  assert(single.active_mask(true, 3u) == 3);
  assert(single.active_mask(true, 1u) == 1 && single.active_mask(true, 2u) == 2);
  assert(single.matches(77, 1) && single.matches(77, 2));
  single.forget(77);
  assert(single.targets[0] == 0 && single.targets[1] == 0);
  assert(single.adopt_single(77));  // Texture recreation must rebind without an Auto toggle.
  assert((single.targets == taxi_camera::TaxiButtonRoutes::Sides{77, 77, 77}));
  single.forget(77);
  assert(single.assign(0, 88));
  assert(!single.adopt_single(77));  // A surviving explicit side stays authoritative.
  assert(replacements.adopt_detected({11569, 11170}));
  replacements.forget(11170);
  assert(replacements.adopt_detected({43712, 11569}));  // New right ID is larger than surviving left.
  assert((replacements.targets == taxi_camera::TaxiButtonRoutes::Sides{11569, 43712, 0}));
  assert(replacements.matches(11569, 1) && replacements.matches(43712, 2));
  assert(replacements.adopt_detected({43712, 11569}));  // Reordered detections never swap live sides.
  assert(replacements.targets[0] == 11569);
  replacements.forget(11569);
  assert(!replacements.adopt_detected({60000, 50000}));  // A pair without the survivor cannot replace it.
  assert(replacements.targets[0] == 0 && replacements.targets[1] == 43712);
  assert(replacements.adopt_detected({60000, 43712}));  // Symmetric left-only replacement.
  assert(replacements.targets[0] == 60000 && replacements.targets[1] == 43712);
  replacements.forget(60000);
  replacements.forget(43712);
  assert(!replacements.adopt_detected({80000, 70000}));       // Both lost: no new creation-order guess.
  assert(replacements.adopt_detected({70000, 80000}, true));  // Exact names restore semantic identity.
  assert(replacements.targets[0] == 70000 && replacements.targets[1] == 80000);
  replacements.forget(70000);
  replacements.forget(80000);
  assert(replacements.assign(1, 90000));
  assert(replacements.adopt_detected({100000, 90000}));  // Explicit manual side remains authoritative.
  assert(replacements.targets[1] == 90000);

  // Existing live A350 textures stay eligible across the deliberate A35K ->
  // A359 profile change. Clearing only public IDs used to leave assigned_ set,
  // permanently refusing even a newly confirmed pair in the new profile.
  static taxi_camera::PfdTargetDetector profile_detector;
  taxi_camera::TaxiButtonRoutes profile_routes;
  std::array<taxi_camera::PfdTargetObservation, 2> live{{{280, 10000, 1644, 1024, 1, 27}, {279, 9000, 1644, 1024, 1, 27}}};
  std::uint64_t now = 10000;
  const auto confirm = [&](const taxi_camera::profiles::AircraftProfile& profile) {
    profile_detector.configure(profile);
    assert(!profile_detector.observe(live.data(), live.size(), now).valid);
    for (unsigned window = 1; window <= 3; ++window) {
      live[0].draws += 100;
      live[1].draws += 90;
      now += 1000;
      const auto& observed = profile_detector.observe(live.data(), live.size(), now);
      assert(observed.valid == (window == 3));
    }
    return profile_detector.snapshot().targets;
  };
  const auto old_pair = confirm(taxi_camera::profiles::A35K);
  assert(profile_routes.adopt_detected(old_pair));
  assert((old_pair == std::array<std::uint64_t, 2>{280, 279}));
  profile_routes.reset();  // Same operation used by the deliberate profile switch.
  assert(profile_routes.active_mask(true, 3u) == 0);
  const auto new_pair = confirm(taxi_camera::profiles::A359);
  assert(new_pair == old_pair);  // No new texture creation is required.
  assert(profile_routes.adopt_detected(new_pair));
  assert(profile_routes.active_mask(true, 3u) == 3);
  assert(profile_routes.matches(280, 1) && profile_routes.matches(279, 2));
  profile_routes.forget(280);
  profile_routes.forget(279);
  assert(!profile_routes.adopt_detected(confirm(taxi_camera::profiles::A359)));
  assert(profile_routes.active_mask(true, 3u) == 0);  // Ordinary both-lost remains ambiguous.
  profile_routes.reset();                                     // A deliberate reload of the same aircraft releases old ownership.
  assert(profile_routes.adopt_detected(confirm(taxi_camera::profiles::A359)));
  assert(profile_routes.matches(280, 1) && profile_routes.matches(279, 2));
  profile_routes.reset();
  live = {{{380, 0, 768, 1024, 5, 28}, {379, 0, 768, 1024, 5, 28}}};
  assert(profile_routes.adopt_detected(confirm(taxi_camera::profiles::A380)));
  assert(profile_routes.matches(380, 1) && profile_routes.matches(379, 2));
  profile_routes.reset();
  live = {{{480, 0, 1644, 1024, 1, 27}, {479, 0, 1644, 1024, 1, 27}}};
  assert(profile_routes.adopt_detected(confirm(taxi_camera::profiles::A359)));
  assert(profile_routes.matches(480, 1) && profile_routes.matches(479, 2));
  profile_routes.reset();
  assert(!profile_routes.adopt_detected({0, 279}));
  assert(!profile_routes.adopt_detected({280, 280}));
  assert(profile_routes.adopt_detected(confirm(taxi_camera::profiles::A35K)));
  std::puts("Aircraft session reacquisition: PASS; same-aircraft reload, A350/A380 round trip, ordinary both-lost still refused");
  // An explicit partial choice is authoritative for that side, independent of
  // detector ordering. Zero is Auto, never a resource identity.
  taxi_camera::TaxiButtonRoutes selections;
  assert(selections.select_explicit({280, 279}));
  assert(!selections.select_explicit({280, 280}));
  assert((selections.targets == taxi_camera::TaxiButtonRoutes::Sides{280, 279}));
  assert(selections.select_explicit({280, 0}));
  assert(selections.active_mask(true, 3u) == 1);
  assert(!selections.adopt_detected({800, 700}));  // No matching manual anchor.
  assert(selections.targets[0] == 280 && selections.targets[1] == 0);
  assert(selections.adopt_detected({900, 280}));
  assert(selections.targets[0] == 280 && selections.targets[1] == 900);
  assert(selections.select_explicit({0, 279}));
  assert(selections.active_mask(true, 3u) == 2);
  assert(selections.adopt_detected({279, 901}));
  assert(selections.targets[0] == 901 && selections.targets[1] == 279);
  selections.forget(901);
  selections.forget(279);
  assert(!selections.adopt_detected({902, 903}));  // Ordinary destruction still refuses.
  assert(selections.select_explicit({0, 0}));
  assert(selections.active_mask(true, 3u) == 0);
  assert(selections.adopt_detected({902, 903}));  // Explicit both-Auto releases old ownership.
  assert(selections.targets[0] == 902 && selections.targets[1] == 903);
  assert(!selections.select_explicit({904, 904}));
  assert(selections.targets[0] == 902 && selections.targets[1] == 903);
  assert(selections.select_explicit({903, 902}));  // Explicit swap replaces both atomically.
  assert(selections.matches(903, 1) && selections.matches(902, 2));
  // Losing the complete ini allocation group withdraws only heuristic sides.
  // It does not weaken manual anchors or the existing both-lost refusal.
  taxi_camera::TaxiButtonRoutes ranked;
  assert(ranked.adopt_detected({211, 209}));
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{0, 0}));
  assert(!ranked.adopt_detected({311, 309}));
  assert(ranked.select_explicit({0, 0}));
  assert(ranked.adopt_detected({311, 309}));
  assert(ranked.assign(0, 311));  // Making one side explicit preserves it.
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{311, 0}));
  assert(ranked.adopt_detected({411, 311}));
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{311, 411}));
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{311, 0}));
  assert(ranked.select_explicit({0, 509}));
  assert(ranked.adopt_detected({511, 509}));
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{0, 509}));
  assert(ranked.select_explicit({511, 509}));
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{511, 509}));
  ranked.reset();
  assert(ranked.adopt_detected({611, 609}, true));  // Semantic names are not rank evidence.
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{611, 609}));
  ranked.reset();
  assert(ranked.adopt_detected({711, 709}));
  assert(!ranked.select_explicit({711, 711}));
  ranked.forget_detected();
  assert((ranked.targets == taxi_camera::TaxiButtonRoutes::Sides{0, 0}));
  std::puts("Ranked auto selection invalidation: PASS; explicit and semantic sides preserved, both-lost still refused");
  taxi_camera::TaxiButtonRoutes recovered;
  assert(recovered.adopt_detected({20, 18}));
  recovered.forget(18);
  assert(!recovered.adopt_detected({30, 28}));
  assert(recovered.replace_detected({30, 28}));
  recovered.forget(28);
  assert(recovered.replace_detected({40, 38}));  // Repeated recovery keeps automatic provenance.
  assert(recovered.select_explicit({40, 0}));    // Same as old automatic still becomes explicit.
  assert(!recovered.replace_detected({50, 48}));
  recovered.forget_detected();
  assert((recovered.targets == taxi_camera::TaxiButtonRoutes::Sides{40, 0}));
  assert(recovered.select_explicit({0, 38}));
  assert(!recovered.replace_detected({50, 48}));
  assert((recovered.targets == taxi_camera::TaxiButtonRoutes::Sides{0, 38}));
  recovered.reset();
  assert(!recovered.replace_detected({0, 48}) && !recovered.replace_detected({50, 50}));
  assert(recovered.replace_detected({50, 48}));
  recovered.forget_detected();
  assert((recovered.targets == taxi_camera::TaxiButtonRoutes::Sides{0, 0}));
  assert(recovered.adopt_detected({60, 58}, true));
  recovered.forget(58);
  assert(!recovered.replace_detected({70, 68}));  // Semantic identity is also authoritative.
  std::puts("Ranked fallback recovery: PASS; automatic provenance retained, explicit single sides preserved");
  std::puts("Explicit manual/Auto routing: PASS; partial anchors preserved, duplicate pair unchanged");
  // PMDG 777: both NDs share DUS; the lower DU is a separate EICASCDU texture.
  using Sides = taxi_camera::TaxiButtonRoutes::Sides;
  taxi_camera::TaxiButtonRoutes lower;
  assert(lower.adopt_single(272, true) && (lower.targets == Sides{272, 272, 0}));
  assert(lower.adopt_single(272, true) && !lower.adopt_single(273, true));
  assert(lower.active_mask(true, 7u) == 3);
  assert(!lower.adopt_lower(0) && !lower.adopt_lower(272));  // Never the ND texture.
  assert(lower.adopt_lower(271) && lower.adopt_lower(271) && !lower.adopt_lower(270));
  assert((lower.targets == Sides{272, 272, 271}) && !lower.lower_explicit());
  assert(lower.active_mask(true, 4u) == 4 && lower.active_mask(true, 7u) == 7 && lower.active_mask(false, 7u) == 0);
  assert(lower.matches(271, 4) && !lower.matches(271, 3) && lower.matches(272, 1) && !lower.matches(272, 4));
  assert(!lower.select_lower(272));  // The lower slot never takes the ND texture.
  assert(lower.select_lower(270) && lower.lower_explicit() && lower.targets[2] == 270);
  assert(!lower.adopt_lower(271) && lower.targets[2] == 270);  // Explicit choice is kept.
  assert(!lower.select_single(270, true));                     // Taken by the lower slot.
  assert(lower.select_single(269, true) && (lower.targets == Sides{269, 269, 270}));
  lower.forget(270);
  assert(lower.targets[2] == 0 && !lower.lower_explicit() && lower.active_mask(true, 4u) == 0);
  assert(lower.adopt_lower(271));  // A lone lower texture rebinds automatically.
  lower.forget(269);
  assert(lower.adopt_single(268, true) && (lower.targets == Sides{268, 268, 271}));  // ND rebinds; lower kept.
  assert(lower.select_single(0, true) && (lower.targets == Sides{0, 0, 271}));
  // Without a separate lower texture every side still shares one texture.
  taxi_camera::TaxiButtonRoutes shared;
  assert(shared.adopt_single(55) && (shared.targets == Sides{55, 55, 55}));
  std::puts("Lower DU routing: PASS; separate texture, explicit choice kept, automatic rebind, shared profiles unchanged");
  std::puts("Taxi button routes: PASS");
}
