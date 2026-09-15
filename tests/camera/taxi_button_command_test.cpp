#include "../../src/camera/taxi_button_command.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../../src/camera/taxi_speed_cutoff.hpp"

namespace {
using namespace taxi_camera::native_camera;
unsigned checks{};
void require(bool value, const char* text) {
  ++checks;
  if (!value)
    throw std::runtime_error(text);
}
TaxiButtonCommandContext context(std::uint64_t now, std::uint64_t sample, unsigned actual) {
  TaxiButtonCommandContext c;
  c.now = now;
  c.session_epoch = 10;
  c.profile = 1;
  c.button_sample_ms = sample;
  c.actual_mask = actual;
  c.identity_valid = c.buttons_valid = c.speed_valid = true;
  c.speed_knots = 10;
  return c;
}
TaxiButtonRequest request(std::uint64_t serial, unsigned selected, unsigned desired) {
  return {serial, 10, 1, selected, desired};
}
void update(TaxiButtonCommand& p, const TaxiButtonRequest& r, std::uint64_t now, bool allowed = true) {
  p.update(r, allowed, now, 10, 1, true);
}
void reversal() {
  TaxiButtonCommand p;
  update(p, request(1, 1, 1), 100);
  auto d = p.step(context(101, 100, 0));
  require(d.send_mask == 1 && d.desired_mask == 1 && !d.cutoff_mask, "Fresh OFF becomes one ON press");
  p.sent(d, 0, true, 102);
  require(!p.step(context(103, 100, 0)).send_mask, "Accepted toggle is not repeated on its pre-send sample");
  update(p, request(2, 1, 0), 110);
  require(!p.step(context(111, 110, 0)).send_mask && p.status().pending_mask == 1,
          "Rapid OFF reversal waits for accepted ON, even when the current lamp is still OFF");
  d = p.step(context(130, 129, 1));
  require(d.send_mask == 1 && !d.desired_mask && d.serial == 2, "After ON acknowledgement send the latest OFF request");
  p.sent(d, 0, true, 131);
  require(!p.step(context(140, 129, 1)).send_mask, "Reversed OFF is also sent only once");
  p.step(context(150, 149, 0));
  require(!p.status().pending_mask && !p.status().failed && p.status().serial == 2, "Later OFF telemetry retires latest request");
  update(p, request(2, 1, 0), 160);
  require(!p.step(context(161, 160, 1)).send_mask, "Retired serial does not replay after a physical cockpit press");
  TaxiButtonCommand rejected;
  update(rejected, request(1, 1, 1), 100);
  d = rejected.step(context(101, 100, 0));
  rejected.sent(d, 0, true, 102);
  update(rejected, request(2, 1, 0), 110);
  rejected.rejected(0);
  require(!rejected.status().failed && rejected.status().pending_mask == 1,
          "Late rejection of superseded ON does not fail the newer OFF request");
  require(!rejected.step(context(120, 119, 0)).send_mask && !rejected.status().pending_mask,
          "Newer OFF can complete from observed OFF after prior ON was rejected");
}
void sides() {
  TaxiButtonCommand p;
  update(p, request(1, 3, 3), 100);
  auto d = p.step(context(101, 100, 1));
  require(d.send_mask == 2 && d.desired_mask == 2 && p.status().pending_mask == 2,
          "Both ON preserves the already-ON side and sends only the other side");
  p.sent(d, 1, true, 102);
  p.step(context(120, 119, 3));
  require(!p.status().pending_mask, "Both ON completes when the second lamp acknowledges");
  update(p, request(2, 3, 0), 130);
  d = p.step(context(131, 130, 3));
  require(d.send_mask == 3 && !d.desired_mask, "Both OFF independently requests two sides");
  p.sent(d, 0, true, 132);
  p.sent(d, 1, true, 133);
  require(!p.step(context(140, 139, 2)).send_mask && p.status().pending_mask == 2,
          "One side acknowledgement cannot resend or finish the other side");
  p.step(context(150, 149, 0));
  require(!p.status().pending_mask && !p.status().failed, "Both OFF retires only after both lamps agree");
}
void permission_and_cutoff() {
  TaxiButtonCommand p;
  update(p, request(1, 1, 1), 100);
  auto d = p.step(context(101, 100, 0));
  p.sent(d, 0, true, 102);
  update(p, request(1, 1, 1), 110, false);
  require(!p.status().pending_mask && p.status().failed, "Permission loss cancels pending user intent");
  auto c = context(120, 119, 0);
  c.cutoff_inhibited = true;
  c.cutoff_commands = 1;
  c.speed_knots = 61;
  require(!p.step(c).send_mask, "Cutoff does not toggle a still-OFF lamp while accepted ON is in flight");
  c = context(130, 129, 1);
  c.cutoff_inhibited = true;
  c.cutoff_commands = 1;
  c.speed_knots = 61;
  d = p.step(c);
  require(d.send_mask == 1 && d.cutoff_mask == 1 && !d.desired_mask, "Cutoff OFF wins after ON acknowledgement despite permission loss");
  p.sent(d, 0, true, 131);
  require(!p.step(c).send_mask, "User/cutoff share the same accepted-send guard");
  update(p, request(2, 1, 1), 140);
  c.now = 141;
  require(!p.step(c).send_mask && p.status().failed && !p.status().pending_mask, "New ON is refused while cutoff is inhibited");
  update(p, request(2, 1, 1), 150);
  c = context(151, 150, 0);
  require(!p.step(c).send_mask, "Previously refused ON does not replay when speed falls");

  TaxiButtonCommand watchdog;
  update(watchdog, request(1, 1, 1), 100);
  require(!watchdog.step(context(601, 600, 0)).send_mask && watchdog.status().failed, "Bridge permission expires after 500 ms");
  update(watchdog, request(1, 1, 1), 610);
  require(!watchdog.step(context(611, 610, 0)).send_mask, "Refreshed permission does not revive expired intent");
}
void unavailable_and_timeout() {
  for (unsigned missing = 0; missing < 3; ++missing) {
    TaxiButtonCommand p;
    update(p, request(1, 1, 1), 100);
    auto c = context(101, 100, 0);
    if (missing == 0)
      c.identity_valid = false;
    if (missing == 1)
      c.buttons_valid = false;
    if (missing == 2)
      c.speed_valid = false;
    require(!p.step(c).send_mask && p.status().pending_mask == 1, "Missing identity/lamp/speed does not emit an aircraft request");
    update(p, request(1, 1, 1), 3099);
    c.now = 3100;
    require(!p.step(c).send_mask && p.status().failed && !p.status().pending_mask,
            "Request lifetime is bounded despite permission refresh");
  }
  TaxiButtonCommand p;
  update(p, request(1, 1, 1), 1000);
  require(!p.step(context(1001, 499, 0)).send_mask, "Stale lamp telemetry is not an OFF state");
  require(!p.step(context(1001, 1002, 0)).send_mask, "Future lamp telemetry is not authoritative");
  auto d = p.step(context(1010, 1009, 0));
  require(d.send_mask == 1, "Fresh telemetry permits an unexpired waiting request");
  p.sent(d, 0, true, 1011);
  update(p, request(1, 1, 1), 3999);
  require(!p.step(context(4000, 3999, 0)).send_mask && p.status().failed, "Accepted-send timeout never repeats the toggle");
  update(p, request(2, 1, 0), 4010);
  require(!p.step(context(4011, 4010, 0)).send_mask && p.status().pending_mask == 1,
          "Timeout retains uncertain accepted send across later requests");
}
void lifecycle_and_failures() {
  TaxiButtonCommand p;
  update(p, request(1, 1, 1), 100, false);
  require(p.status().serial == 1 && p.status().failed && !p.status().pending_mask, "Refused serial is consumed");
  update(p, request(1, 1, 1), 110);
  require(!p.step(context(111, 110, 0)).send_mask, "Enabling service cannot replay a refused request");
  update(p, request(2, 1, 1), 120);
  auto d = p.step(context(121, 120, 0));
  update(p, request(3, 0, 0), 122);
  require(!p.current(d, 0, 123) && p.status().serial == 3 && !p.status().pending_mask && !p.status().failed,
          "Empty mask consumes cancellation and cancels a prepared but unissued command");
  update(p, request(4, 1, 1), 130);
  d = p.step(context(131, 130, 0));
  p.sent(d, 0, false, 132);
  require(p.status().failed && !p.status().pending_mask, "Synchronous failure retires user request visibly");
  update(p, request(5, 1, 1), 140);
  d = p.step(context(141, 140, 0));
  p.sent(d, 0, true, 142);
  p.rejected(0);
  require(p.status().failed && !p.status().pending_mask, "Correlated async rejection retires user request");
  update(p, request(6, 1, 1), 150);
  require(p.step(context(151, 150, 0)).send_mask == 1, "Known rejected send does not block an explicit new request");
  p.reset_session();
  update(p, request(6, 1, 1), 160);
  require(!p.step(context(161, 160, 0)).send_mask, "Worker restart consumes old intent rather than replaying it");
  auto wrong = request(7, 1, 1);
  wrong.session_epoch = 9;
  update(p, wrong, 170);
  require(p.status().failed && !p.status().pending_mask, "Prior aircraft epoch is refused");
  wrong = request(8, 1, 1);
  wrong.profile = 2;
  update(p, wrong, 180);
  require(p.status().failed && !p.status().pending_mask, "Different aircraft profile is refused");
  p.update(request(9, 1, 1), true, 190, 10, 1, false);
  require(p.status().failed && !p.status().pending_mask, "INOP manual-only profile emits no aircraft request");
  update(p, request(10, 0, 1), 200);
  require(p.status().failed && !p.status().pending_mask, "Desired bits outside selected mask are refused");
  update(p, request(11, 1, 1), 210);
  d = p.step(context(211, 210, 0));
  p.sent(d, 0, true, 212);
  update(p, {}, 220, false);
  update(p, request(12, 1, 0), 230);
  require(!p.step(context(231, 230, 0)).send_mask && p.status().pending_mask == 1,
          "Serial-zero disconnect cancels intent without forgetting an accepted toggle");

  TaxiSpeedCutoff cutoff;
  require(cutoff.update(100, true, 61, true, 1, 100) == 1, "Cutoff first OFF");
  cutoff.sent(0, true);
  cutoff.rejected(0);
  require(!cutoff.update(1099, true, 55, true, 1, 1099) && cutoff.inhibited(), "Rejected cutoff retains inhibition and retry delay");
  require(cutoff.update(1100, true, 55, true, 1, 1100) == 1, "Rejected cutoff permits bounded OFF retry");
}
}  // namespace
int main() {
  try {
    reversal();
    sides();
    permission_and_cutoff();
    unavailable_and_timeout();
    lifecycle_and_failures();
    std::printf("PASS TAXI button commands: %u acknowledgement, reversal, cutoff, lifecycle and failure checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL TAXI button commands: %s\n", error.what());
    return 1;
  }
}
