#include "../../src/graphics/scene_source_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace {
using namespace taxi_camera::source_state;
using Kind = Effect::Kind;
constexpr Key nose{0x1000, 11}, tail{0x2000, 12};
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
Recording record(std::initializer_list<Effect> effects) {
  Recording result;
  for (const auto effect : effects)
    require(result.append(effect), "fixture effect refused");
  return result;
}
void state_is(const Tracker& tracker, Key key, Model model, bool drawn) {
  const auto value = tracker.state(key);
  require(value.model == model && value.drawn == drawn, "submitted final state/draw evidence differs");
}

void actual_submission_order() {
  Tracker tracker;
  require(tracker.register_source(nose) && tracker.register_source(tail), "pair registration refused");
  // These immutable lists may be recorded on the CPU in any order. Merely
  // building them changes nothing; only the order passed to apply matters.
  const auto draw = record({{nose, Kind::draw}});
  const auto to_rt = record({{nose, Kind::legacy_rt}});
  const auto to_copy = record({{nose, Kind::other}});
  state_is(tracker, nose, Model::unknown, false);
  tracker.begin_batch();
  require(tracker.apply(draw), "draw-only initial list refused structurally");
  state_is(tracker, nose, Model::unknown, false);
  require(tracker.apply(to_rt), "positive RT evidence refused");
  state_is(tracker, nose, Model::legacy_rt, false);
  require(tracker.apply(draw), "following native draw refused");
  state_is(tracker, nose, Model::legacy_rt, true);
  require(tracker.apply(to_copy), "copy-state transition refused");
  state_is(tracker, nose, Model::other, false);
  require(tracker.apply(draw), "non-RT draw log refused structurally");
  state_is(tracker, nose, Model::other, false);

  // A later actual batch retains the known RT model but requires fresh drawing.
  require(tracker.apply(to_rt) && tracker.apply(draw), "RT restoration refused");
  tracker.begin_batch();
  state_is(tracker, nose, Model::legacy_rt, false);
  require(tracker.apply(draw), "state did not persist across command lists/batches");
  state_is(tracker, nose, Model::legacy_rt, true);
  state_is(tracker, tail, Model::unknown, false);

  // Replaying a transition-only recording must clear a prior draw. Replaying a
  // draw-only recording after leaving RT cannot revive an obsolete RT model.
  require(tracker.apply(to_rt), "transition replay refused");
  state_is(tracker, nose, Model::legacy_rt, false);
  require(tracker.apply(to_copy) && tracker.apply(draw), "replay after non-RT transition refused structurally");
  state_is(tracker, nose, Model::other, false);
  require(tracker.apply(record({{nose, Kind::legacy_rt}, {nose, Kind::draw}, {tail, Kind::enhanced_rt}, {tail, Kind::draw}})),
          "two independent state models refused");
  state_is(tracker, nose, Model::legacy_rt, true);
  state_is(tracker, tail, Model::enhanced_rt, true);
  require(tracker.apply(record({{tail, Kind::legacy_rt}})), "explicit model replacement refused");
  state_is(tracker, tail, Model::legacy_rt, false);
  state_is(tracker, nose, Model::legacy_rt, true);
}

void native_creation_state() {
  for (const auto initial : {Model::unknown, Model::legacy_rt, Model::enhanced_rt, Model::other}) {
    Tracker tracker;
    require(tracker.register_source(nose, initial), "explicit native creation model refused");
    state_is(tracker, nose, initial, false);
    const auto draw = record({{nose, Kind::draw}});
    tracker.begin_batch();
    require(tracker.apply(draw), "first draw-only recording refused");
    state_is(tracker, nose, initial, initial == Model::legacy_rt || initial == Model::enhanced_rt);
    tracker.begin_batch();
    require(tracker.apply(draw), "creation model did not persist for draw-only replay");
    require(tracker.apply(record({{nose, Kind::other}})), "leaving creation state refused");
    require(tracker.register_source(nose, initial), "duplicate source registration refused");
    state_is(tracker, nose, Model::other, false);
    require(tracker.apply(draw), "replay after leaving RT refused structurally");
    state_is(tracker, nose, Model::other, false);
    tracker.invalidate_all();
    require(tracker.register_source(nose, Model::legacy_rt), "duplicate after unknown refused structurally");
    state_is(tracker, nose, Model::unknown, false);
    const Key replacement{nose.handle, nose.generation + 1};
    require(tracker.register_source(replacement, initial), "new incarnation creation model refused");
    state_is(tracker, replacement, initial, false);
    require(tracker.apply(draw), "stale-generation draw refused structurally");
    state_is(tracker, replacement, initial, false);
  }
  Tracker tracker;
  require(!tracker.register_source(nose, static_cast<Model>(99)), "invalid native creation model admitted");
  state_is(tracker, nose, Model::unknown, false);
}

void ordering_permutations() {
  // The same CPU-recorded effects give different safe results when the queue
  // actually executes them in a different order. No recording timestamp wins.
  const Recording lists[]{record({{nose, Kind::legacy_rt}}), record({{nose, Kind::draw}}), record({{nose, Kind::other}})};
  constexpr unsigned permutations[][3]{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (const auto& order : permutations) {
    Tracker tracker;
    require(tracker.register_source(nose), "permutation registration refused");
    for (const auto index : order)
      require(tracker.apply(lists[index]), "permutation submission refused");
    const auto last = order[2];
    const bool drawn = order[0] == 2 && order[1] == 0 && last == 1;
    state_is(tracker, nose, last == 2 || (last == 1 && order[1] == 2) ? Model::other : Model::legacy_rt, drawn);
  }
}

void registration_generations() {
  Tracker tracker;
  require(!tracker.register_source({}) && !tracker.register_source({0, 3}) && !tracker.register_source({4, 0}),
          "incomplete key registered");
  require(tracker.register_source(nose), "source registration failed");
  const auto old = record({{nose, Kind::enhanced_rt}, {nose, Kind::draw}});
  require(tracker.apply(old) && tracker.register_source(nose), "exact repeated registration failed");
  state_is(tracker, nose, Model::enhanced_rt, true);
  const Key replacement{nose.handle, nose.generation + 1};
  require(tracker.register_source(replacement), "replacement generation refused");
  state_is(tracker, nose, Model::unknown, false);
  state_is(tracker, replacement, Model::unknown, false);
  require(tracker.apply(old), "stale log caused an unrelated failure");
  state_is(tracker, replacement, Model::unknown, false);
  require(tracker.apply(record({{replacement, Kind::legacy_rt}, {replacement, Kind::draw}})), "replacement state failed");
  tracker.unregister_source(nose);
  state_is(tracker, replacement, Model::legacy_rt, true);
  tracker.unregister_source(replacement);
  require(tracker.apply(old), "unregistered replay refused structurally");
  state_is(tracker, replacement, Model::unknown, false);
  require(tracker.register_source(nose), "source re-registration failed");
  state_is(tracker, nose, Model::unknown, false);
  tracker.clear();
  require(tracker.apply(old), "cleared registry accepted malformed log state");
  state_is(tracker, nose, Model::unknown, false);
}

void invalidation() {
  const auto ready = record({{nose, Kind::legacy_rt}, {nose, Kind::draw}, {tail, Kind::enhanced_rt}, {tail, Kind::draw}});
  for (unsigned scenario = 0; scenario < 5; ++scenario) {
    Tracker tracker;
    require(tracker.register_source(nose) && tracker.register_source(tail) && tracker.apply(ready), "invalidation setup failed");
    auto broken = ready;
    switch (scenario) {
      case 0:
        broken.invalidate();
        break;  // split, alias, pass or unknown evidence
      case 1:
        broken.count = Recording::capacity + 1;
        break;
      case 2:
        broken.effects[2].kind = static_cast<Kind>(99);
        break;
      case 3:
        broken.effects[2].key = {};
        break;
      case 4:
        broken.effects[2].key.generation = 0;
        break;
    }
    require(!tracker.apply(broken), "invalid/truncated log was accepted");
    state_is(tracker, nose, Model::unknown, false);
    state_is(tracker, tail, Model::unknown, false);
    require(tracker.apply(record({{nose, Kind::draw}})), "draw after unknown refused structurally");
    state_is(tracker, nose, Model::unknown, false);
    require(tracker.apply(record({{nose, Kind::enhanced_rt}, {nose, Kind::draw}})), "new positive evidence could not recover");
    state_is(tracker, nose, Model::enhanced_rt, true);
    state_is(tracker, tail, Model::unknown, false);
    tracker.invalidate_all();
    state_is(tracker, nose, Model::unknown, false);
    require(tracker.rearm_retained_rt() == 2, "invalidation lost the last observed RT models");
    state_is(tracker, nose, Model::enhanced_rt, false);
    state_is(tracker, tail, Model::enhanced_rt, false);
  }
}

void retained_rt_rearm_after_aa_wipe() {
  Tracker tracker;
  require(tracker.register_source(nose, Model::legacy_rt), "creation RT refused");
  require(tracker.apply(record({{nose, Kind::draw}})), "creation draw refused");
  state_is(tracker, nose, Model::legacy_rt, true);
  tracker.invalidate_all();
  state_is(tracker, nose, Model::unknown, false);
  require(tracker.register_source(nose, Model::legacy_rt), "duplicate after wipe refused");
  state_is(tracker, nose, Model::unknown, false);
  require(tracker.apply(record({{nose, Kind::draw}})), "draw after wipe refused");
  state_is(tracker, nose, Model::unknown, false);
  require(tracker.rearm_retained_rt() == 1, "live allocation did not restore retained RT");
  state_is(tracker, nose, Model::legacy_rt, false);
  require(tracker.apply(record({{nose, Kind::draw}})), "draw after rearm refused");
  state_is(tracker, nose, Model::legacy_rt, true);

  require(tracker.register_source(tail, Model::enhanced_rt), "tail creation RT refused");
  require(tracker.apply(record({{tail, Kind::enhanced_rt}, {tail, Kind::draw}})), "tail RT+draw refused");
  require(tracker.apply(record({{tail, Kind::other}})), "explicit leave-RT refused");
  tracker.invalidate_all();
  require(tracker.rearm_retained_rt() == 1, "left-RT source was resurrected or nose was not rearmed");
  state_is(tracker, nose, Model::legacy_rt, false);
  state_is(tracker, tail, Model::unknown, false);

  const Key replacement{nose.handle, nose.generation + 1};
  require(tracker.register_source(replacement, Model::unknown), "replacement generation refused");
  state_is(tracker, replacement, Model::unknown, false);
  require(tracker.rearm_retained_rt() == 0, "stale generation retained RT leaked");
}

void pass_other_keeps_retained_rt() {
  Tracker tracker;
  require(tracker.register_source(nose, Model::legacy_rt) && tracker.register_source(tail, Model::enhanced_rt),
          "pass_other pair registration refused");
  require(tracker.apply(record({{nose, Kind::legacy_rt}, {nose, Kind::draw}, {tail, Kind::enhanced_rt}, {tail, Kind::draw}})),
          "pass_other RT setup refused");
  require(tracker.apply(record({{nose, Kind::pass_other}})), "PassState retirement refused");
  state_is(tracker, nose, Model::other, false);
  state_is(tracker, tail, Model::enhanced_rt, true);
  require(tracker.rearm_retained_rt() == 1, "PassState other cleared retained_rt");
  state_is(tracker, nose, Model::legacy_rt, false);
  state_is(tracker, tail, Model::enhanced_rt, true);
  require(tracker.apply(record({{nose, Kind::draw}})), "draw after PassState rearm refused");
  state_is(tracker, nose, Model::legacy_rt, true);
}

void retire_live_models_keeps_retained_rt() {
  // An escaped batch: every live model retires to other (never unknown, so no
  // CaptureProgress stall), retained_rt survives, and the in-place rearm puts
  // the RT models back with only the draw evidence lost.
  Tracker tracker;
  require(tracker.register_source(nose, Model::legacy_rt) && tracker.register_source(tail, Model::enhanced_rt),
          "retire pair registration refused");
  require(tracker.apply(record({{nose, Kind::legacy_rt}, {nose, Kind::draw}, {tail, Kind::enhanced_rt}, {tail, Kind::draw}})),
          "retire RT setup refused");
  tracker.retire_live_models();
  state_is(tracker, nose, Model::other, false);
  state_is(tracker, tail, Model::other, false);
  require(tracker.rearm_retained_rt() == 2, "retire_live_models cleared retained_rt");
  state_is(tracker, nose, Model::legacy_rt, false);
  state_is(tracker, tail, Model::enhanced_rt, false);
  require(tracker.apply(record({{nose, Kind::draw}})), "draw after in-place rearm refused");
  state_is(tracker, nose, Model::legacy_rt, true);
  // A source that had genuinely left RT stays other: nothing is resurrected.
  require(tracker.apply(record({{tail, Kind::other}})), "explicit leave-RT refused");
  tracker.retire_live_models();
  require(tracker.rearm_retained_rt() == 1, "retire rearm resurrected a source that left RT");
  state_is(tracker, nose, Model::legacy_rt, false);
  state_is(tracker, tail, Model::other, false);
  // Unregistered slots stay empty: a later registration starts from its own model.
  Tracker empty;
  empty.retire_live_models();
  require(empty.rearm_retained_rt() == 0 && empty.register_source(nose, Model::unknown), "retire touched an empty slot");
  state_is(empty, nose, Model::unknown, false);
}

void barrier_other_clears_retained_rt() {
  Tracker tracker;
  require(tracker.register_source(nose, Model::legacy_rt), "barrier other registration refused");
  require(tracker.apply(record({{nose, Kind::legacy_rt}, {nose, Kind::draw}})), "barrier other RT setup refused");
  require(tracker.apply(record({{nose, Kind::other}})), "barrier-style leave-RT refused");
  state_is(tracker, nose, Model::other, false);
  require(tracker.rearm_retained_rt() == 0, "barrier/alias/reset other kept retained_rt");
  state_is(tracker, nose, Model::other, false);
}

void recording_bounds() {
  Recording log;
  for (unsigned i = 0; i < 10000; ++i)
    require(log.append({nose, Kind::draw}), "adjacent duplicate draw did not collapse");
  require(log.count == 1 && !log.invalid, "duplicate draws exhausted fixed effect storage");
  require(log.append({nose, Kind::legacy_rt}) && log.append({nose, Kind::legacy_rt}) && log.count == 2,
          "adjacent identical transition did not collapse");
  require(log.append({tail, Kind::legacy_rt}) && log.append({nose, Kind::legacy_rt}) && log.count == 4,
          "different-key effects collapsed across ordering evidence");
  log.reset();
  for (std::size_t i = 0; i < Recording::capacity; ++i)
    require(log.append({nose, i % 2 ? Kind::draw : Kind::legacy_rt}), "bounded distinct effect refused early");
  const auto count = log.count;
  const auto first = log.effects[0];
  require(log.append(log.effects[count - 1]) && log.count == count && !log.invalid, "full log could not collapse its exact last effect");
  require(!log.append({nose, Kind::other}) && log.invalid && log.count == count && log.effects[0] == first,
          "overflow did not invalidate while retaining recorded keys/count");
  require(!log.append({tail, Kind::other}) && log.count == count, "invalid log resumed appending");
  log.reset();
  require(!log.invalid && log.count == 0 && log.append({nose, Kind::enhanced_rt}), "recording reset did not start a fresh log");
  Tracker tracker;
  require(tracker.register_source(nose) && tracker.apply(log), "reset recording retained stale effects");
  state_is(tracker, nose, Model::enhanced_rt, false);
  for (const auto bad : {Effect{{}, Kind::draw}, Effect{nose, static_cast<Kind>(-1)}}) {
    log.reset();
    require(!log.append(bad) && log.invalid && log.count == 0, "invalid effect entered a recording");
  }
}

void interleaved_draw_compression() {
  Recording log;
  Tracker tracker;
  for (std::uint64_t i = 1; i <= 8; ++i) {
    require(tracker.register_source({i, 7}), "MRT source registration refused");
    require(log.append({{i, 7}, Kind::legacy_rt}), "MRT initial state refused");
  }
  for (unsigned draw = 0; draw < 1000; ++draw)
    for (std::uint64_t i = 1; i <= 8; ++i)
      require(log.append({{i, 7}, Kind::draw}), "unchanged MRT drawing exhausted recording");
  require(log.count == 16 && !log.invalid && tracker.apply(log), "MRT draw evidence was not bounded by independent keys");
  for (std::uint64_t i = 1; i <= 8; ++i)
    state_is(tracker, {i, 7}, Model::legacy_rt, true);
  const auto previous = log.count;
  require(log.append({{1, 8}, Kind::draw}) && log.count == previous + 1, "different generation draw was incorrectly collapsed");
  require(log.append({{1, 7}, Kind::other}) && log.append({{2, 7}, Kind::draw}) && log.append({{1, 7}, Kind::draw}) &&
              log.count == previous + 3,
          "same-key non-RT transition failed to stop draw collapse");
  require(tracker.apply(log), "compressed MRT recording refused");
  state_is(tracker, {1, 7}, Model::other, false);
  require(log.append({{1, 7}, Kind::enhanced_rt}) && log.append({{2, 7}, Kind::draw}) && log.append({{1, 7}, Kind::draw}),
          "same-key model transition refused");
  require(tracker.apply(log), "MRT state restoration refused");
  state_is(tracker, {1, 7}, Model::enhanced_rt, true);

  // Compare every prefix against the exact uncompressed stream. Deliberately
  // vary initial state, stale generations, model changes and native replay.
  std::uint32_t random = 0x719381;
  for (unsigned scenario = 0; scenario < 512; ++scenario) {
    Recording raw, compressed;
    for (unsigned step = 0; step < 64; ++step) {
      random = random * 1664525u + 1013904223u;
      const Effect effect{{(random >> 16) % 4 + 1, (random >> 8) % 7 == 0 ? 8u : 7u}, static_cast<Kind>((random >> 24) % 5)};
      raw.effects[raw.count++] = effect;
      require(compressed.append(effect), "bounded random recording refused");
      Tracker original, deduplicated;
      for (std::uint64_t key = 1; key <= 4; ++key) {
        require(original.register_source({key, 7}) && deduplicated.register_source({key, 7}), "equivalence source refused");
        const auto initial = record({{{key, 7}, static_cast<Kind>(scenario % 5)}});
        require(original.apply(initial) && deduplicated.apply(initial), "initial equivalence state refused");
      }
      for (unsigned replay = 0; replay < 2; ++replay) {
        original.begin_batch();
        deduplicated.begin_batch();
        require(original.apply(raw) && deduplicated.apply(compressed), "equivalence replay refused");
        for (std::uint64_t key = 1; key <= 4; ++key) {
          const auto expected = original.state({key, 7});
          state_is(deduplicated, {key, 7}, expected.model, expected.drawn);
          require(deduplicated.state({key, 7}).draws == expected.draws, "compressed draw count differs from the raw stream");
        }
      }
    }
  }
}

void draw_counts() {
  // One camera render in two submissions: the deferred list draws once, the
  // forward list fifty times. Each batch counts only its own draws.
  Tracker tracker;
  require(tracker.register_source(nose, Model::legacy_rt) && tracker.register_source(tail, Model::legacy_rt), "count registration refused");
  Recording deferred, forward;
  require(deferred.append({nose, Kind::draw}) && deferred.count == 1 && deferred.effects[0].draws == 1, "single draw not counted");
  for (unsigned i = 0; i < 50; ++i)
    require(forward.append({nose, Kind::draw}) && forward.append({tail, Kind::draw}), "forward draw refused");
  require(forward.count == 2 && forward.effects[0].draws == 50 && forward.effects[1].draws == 50,
          "interleaved draws were not counted on their own collapsed evidence");
  tracker.begin_batch();
  require(tracker.apply(deferred) && tracker.state(nose).draws == 1 && tracker.state(tail).draws == 0, "deferred batch count");
  tracker.begin_batch();
  require(tracker.state(nose).draws == 0 && !tracker.state(nose).drawn, "batch boundary kept a draw count");
  require(tracker.apply(forward) && tracker.state(nose).draws == 50 && tracker.state(tail).draws == 50, "forward batch count");
  // Both lists in one batch, and a replay of the same immutable recording.
  tracker.begin_batch();
  require(tracker.apply(deferred) && tracker.apply(forward) && tracker.state(nose).draws == 51, "two-list batch count");
  require(tracker.apply(deferred) && tracker.state(nose).draws == 52, "replayed recording was not counted again");
  // A state effect stops the collapse; entering RT restarts the count.
  Recording entry;
  require(entry.append({nose, Kind::draw}) && entry.append({nose, Kind::legacy_rt}) && entry.append({nose, Kind::draw}) &&
              entry.append({nose, Kind::draw}) && entry.count == 3 && entry.effects[2].draws == 2,
          "draws after a transition joined the earlier evidence");
  require(tracker.apply(entry) && tracker.state(nose).draws == 2 && tracker.state(nose).drawn, "RT entry did not restart the count");
  // Draws outside a render-target model are not counted.
  require(tracker.apply(record({{nose, Kind::other}, {nose, Kind::draw}})), "non-RT draw refused structurally");
  state_is(tracker, nose, Model::other, false);
  require(tracker.state(nose).draws == 0, "a non-RT draw was counted");
  // Transitions carry no count and still collapse when consecutively identical.
  Recording states;
  require(states.append({nose, Kind::legacy_rt, 9}) && states.append({nose, Kind::legacy_rt}) && states.count == 1 &&
              states.effects[0].draws == 0,
          "a state effect carried a draw count");
  // Saturation: neither the evidence nor the batch total wraps.
  Recording many;
  require(many.append({nose, Kind::draw}), "saturation setup refused");
  many.effects[0].draws = UINT32_MAX - 1;
  require(many.append({nose, Kind::draw}) && many.append({nose, Kind::draw}) && many.effects[0].draws == UINT32_MAX,
          "collapsed draw count wrapped");
  require(tracker.apply(record({{nose, Kind::legacy_rt}})) && tracker.apply(many) && tracker.apply(many) &&
              tracker.state(nose).draws == UINT32_MAX,
          "batch draw count wrapped");
  // Wipes and retirements clear the count with the draw evidence.
  tracker.retire_live_models();
  require(tracker.state(nose).draws == 0 && tracker.rearm_retained_rt() == 2 && tracker.state(nose).draws == 0, "retire kept a count");
  require(tracker.apply(deferred) && tracker.state(nose).draws == 1, "count after rearm");
  tracker.invalidate_all();
  require(tracker.state(nose).draws == 0, "wipe kept a count");
}

void source_bounds() {
  Tracker tracker;
  for (std::size_t i = 0; i < Tracker::capacity; ++i)
    require(tracker.register_source({i + 1, 7}), "bounded source registration refused early");
  require(!tracker.register_source({Tracker::capacity + 1, 7}), "source capacity overflow was accepted");
  for (std::size_t i = 0; i < Tracker::capacity; ++i) {
    const Key old{i + 1, 7}, fresh{i + 1, 8};
    require(tracker.register_source(old) && tracker.register_source(fresh), "full registry could not keep/replace an existing handle");
    require(tracker.apply(record({{fresh, Kind::legacy_rt}, {fresh, Kind::draw}})), "full registry application failed");
    tracker.unregister_source(old);
    state_is(tracker, fresh, Model::legacy_rt, true);
  }
  tracker.begin_batch();
  for (std::size_t i = 0; i < Tracker::capacity; ++i)
    state_is(tracker, {i + 1, 8}, Model::legacy_rt, false);
  tracker.unregister_source({64, 8});
  require(tracker.register_source({Tracker::capacity + 1, 9}), "released source slot was not reusable");
  state_is(tracker, {Tracker::capacity + 1, 9}, Model::unknown, false);
  tracker.clear();
  for (std::size_t i = 0; i < Tracker::capacity; ++i)
    require(tracker.register_source({i + 1, UINT64_MAX}), "clear did not release all slots/full-width generation refused");
}
}  // namespace

int main() {
  native_creation_state();
  actual_submission_order();
  ordering_permutations();
  registration_generations();
  invalidation();
  retained_rt_rearm_after_aa_wipe();
  pass_other_keeps_retained_rt();
  retire_live_models_keeps_retained_rt();
  barrier_other_clears_retained_rt();
  recording_bounds();
  interleaved_draw_compression();
  draw_counts();
  source_bounds();
  std::printf("PASS: %u source-state checks; pure submitted-order evidence, no graphics/process APIs.\n", checks);
}
