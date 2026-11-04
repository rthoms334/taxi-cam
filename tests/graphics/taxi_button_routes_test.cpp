#include <cassert>
#include <cstdio>
#include <limits>

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

  using Sides = taxi_camera::TaxiButtonRoutes::Sides;
  // Displays are routed by the names their panel marks give them.
  taxi_camera::TaxiButtonRoutes named;
  assert(!named.adopt_named({0, 0, 0}, 2) && named.active_mask(true, 3u) == 0);
  assert(named.adopt_named({280, 279, 0}, 2) && (named.targets == Sides{280, 279, 0}) && named.named(0) && named.named(1));
  assert(!named.adopt_named({280, 279, 0}, 2));  // Nothing changed.
  named.forget(280);
  assert((named.targets == Sides{0, 279, 0}) && !named.named(0) && named.active_mask(true, 3u) == 2);
  assert(named.adopt_named({281, 279, 0}, 2) && (named.targets == Sides{281, 279, 0}));  // A recreated texture takes its name.
  assert(named.adopt_named({279, 281, 0}, 2) && (named.targets == Sides{279, 281, 0}));  // Swapped names move both sides at once.
  // An explicit choice wins over a name, side by side.
  assert(named.select_explicit({500, 0}) && !named.named(0));
  assert(named.adopt_named({279, 281, 0}, 2) && (named.targets == Sides{500, 281, 0}));
  // A texture the user put on one side never also serves another by name.
  assert(named.select_explicit({281, 0}));
  assert(!named.adopt_named({279, 281, 0}, 2) && named.targets[1] == 0);
  assert(named.targets[0] == 281);
  assert(!named.select_explicit({904, 904}) && named.targets[0] == 281);
  assert(named.select_explicit({0, 0}) && named.adopt_named({279, 281, 0}, 2) && (named.targets == Sides{279, 281, 0}));
  assert(named.select_explicit({281, 279}) && named.matches(281, 1) && named.matches(279, 2));  // Explicit swap replaces both.
  assert(!named.adopt_named({279, 281, 0}, 2) && (named.targets == Sides{281, 279, 0}));
  named.reset();  // A profile change or reload releases the old sides.
  assert(named.active_mask(true, 3u) == 0 && named.adopt_named({480, 479, 0}, 2) && named.matches(480, 1));
  std::puts("Named routing: PASS; names adopted, recreated and swapped; explicit sides authoritative");
  // PMDG 777: both NDs share DUS; the lower DU is a separate EICASCDU texture.
  taxi_camera::TaxiButtonRoutes dus;
  assert(dus.adopt_named({229, 229, 228}, 3) && (dus.targets == Sides{229, 229, 228}) && dus.active_mask(true, 7u) == 7);
  assert(dus.matches(228, 4) && !dus.matches(228, 3) && dus.matches(229, 1) && !dus.matches(229, 4));
  assert(!dus.select_lower(229));  // The lower slot never takes the ND texture.
  assert(dus.select_lower(227) && dus.lower_explicit());
  assert(!dus.adopt_named({229, 229, 228}, 3) && dus.targets[2] == 227);  // Explicit lower kept.
  assert(!dus.select_single(227, true));                                  // Taken by the lower slot.
  assert(dus.select_single(230, true) && (dus.targets == Sides{230, 230, 227}));
  assert(!dus.adopt_named({229, 229, 228}, 3) && dus.targets[0] == 230);
  dus.forget(227);
  assert(dus.targets[2] == 0 && !dus.lower_explicit());
  assert(dus.adopt_named({229, 229, 228}, 3) && dus.targets[2] == 228);  // The lower DU takes its name again.
  assert(dus.select_single(0, true) && dus.adopt_named({229, 229, 228}, 3) && (dus.targets == Sides{229, 229, 228}));
  // Without a separate lower texture every side shares one texture.
  taxi_camera::TaxiButtonRoutes shared;
  assert(shared.select_single(55) && (shared.targets == Sides{55, 55, 55}) && shared.assigned_mask() == 7);
  assert(shared.select_single(0) && shared.assigned_mask() == 0);
  assert(shared.adopt_named({56, 56, 56}, 3) && (shared.targets == Sides{56, 56, 56}));
  std::puts("Lower DU routing: PASS; separate texture, explicit choice kept, names rebind");
  // A pilot pair's placed lower display, found by name; an explicit pair keeps it.
  taxi_camera::TaxiButtonRoutes placed;
  assert(placed.adopt_named({0, 0, 901}, 3) && placed.named(2) && (placed.targets == Sides{0, 0, 901}));
  assert(placed.select_explicit({702, 701}) && (placed.targets == Sides{702, 701, 901}));
  placed.forget(901);
  assert((placed.targets == Sides{702, 701, 0}) && placed.adopt_named({0, 0, 905}, 3) && (placed.targets == Sides{702, 701, 905}));
  std::puts("Placed lower display: PASS; named alone, kept by explicit choices");
  taxi_camera::TaxiButtonIntent sd;
  assert(sd.observe(10, true, 4u).buttons == 4 && sd.observe(20, true, 0xffu).buttons == 7);
  std::puts("Taxi button routes: PASS");
}
