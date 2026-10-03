#include "../../src/graphics/capture_phase.hpp"

#include <cstdio>

namespace {
using namespace taxi_camera::capture_phase;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
bool is(Decision decision, Kind kind) {
  return decision.capture && decision.kind == kind;
}
}  // namespace

int main() {
  bool ok = true;
  // A two-submission render: the deferred batch is held, the forward batch captures.
  Feed feed;
  ok &= require(!decide(feed, 1).capture, "The deferred-only batch was captured");
  ok &= require(is(decide(feed, 52), Kind::after_hold), "The batch after a hold was not captured");
  captured(feed);
  ok &= require(!decide(feed, 1).capture, "A capture did not let the next interval hold");
  // The forward pass may draw once (sky only): it is still this render's rest.
  ok &= require(is(decide(feed, 1), Kind::after_hold), "A one-draw batch after a hold was held again");
  captured(feed);

  // Both phases in one batch: captured at once.
  ok &= require(is(decide(feed, 3), Kind::after_multi), "A complete batch was held");
  captured(feed);

  // Ordering lost between the hold and the next batch: that batch still captures.
  ok &= require(!decide(feed, 1).capture, "Setup hold refused");
  forget(feed);
  ok &= require(is(decide(feed, 1), Kind::forced), "A forgotten hold let the feed hold twice");
  captured(feed);
  ok &= require(!decide(feed, 1).capture, "Setup hold refused");
  forget(feed);
  ok &= require(is(decide(feed, 7), Kind::after_multi), "A several-draw batch after a forgotten hold was not captured");
  captured(feed);

  // A capture that could not be recorded (no lease, the interval, no packet)
  // ends the hold: the next render's deferred batch is not this render's rest
  // and is counted as forced, never after_hold.
  ok &= require(!decide(feed, 1).capture, "Setup hold refused");
  ok &= require(is(decide(feed, 9), Kind::after_hold), "Hold not resolved");
  forget(feed);
  ok &= require(is(decide(feed, 1), Kind::forced), "A batch after an unrecorded capture was counted as after_hold");
  captured(feed);

  // Every sequence of up to 10 events (one draw, several draws, lost ordering,
  // a due capture that was not recorded and is forgotten, as the queue tail
  // does): a feed never holds two drawing batches in a row, holds only one-draw
  // batches, and counts after_hold only for the drawing batch directly after a
  // hold.
  unsigned sequences = 0;
  for (unsigned length = 1; length <= 10; ++length) {
    unsigned total = 1;
    for (unsigned i = 0; i < length; ++i)
      total *= 4;
    for (unsigned code = 0; code < total; ++code) {
      Feed state;
      bool previous_held = false;
      unsigned value = code;
      for (unsigned step = 0; step < length; ++step, value /= 4) {
        const unsigned event = value % 4;
        if (event == 2) {
          forget(state);
          continue;
        }
        const std::uint32_t draws = event == 0 ? 1 : 40;
        const auto decision = decide(state, draws);
        if (!decision.capture) {
          ok &= require(draws == 1 && !previous_held, "A feed held a batch it must capture");
          previous_held = true;
          continue;
        }
        ok &= require(decision.kind != Kind::after_hold || previous_held, "after_hold did not follow a hold");
        previous_held = false;
        if (event != 3)
          captured(state);
        else
          forget(state);  // 3: due but not recorded (no lease, the interval, no packet)
      }
      ++sequences;
    }
  }
  ok &= require(kind_name(Kind::forced)[0] == 'f' && kind_name(static_cast<Kind>(9))[0] == 'i', "Kind names");
  if (!ok)
    return 1;
  std::printf("PASS: %u capture-phase checks over %u event sequences.\n", checks, sequences);
  return 0;
}
