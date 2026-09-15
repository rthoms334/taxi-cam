#include "../../src/graphics/pfd_copy_proof.hpp"
#include <cstdio>
#include <stdexcept>
namespace {
using taxi_camera::standalone::PfdCopyProof;
using Mode = PfdCopyProof::Mode;
unsigned checks{};
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
constexpr PfdCopyProof::Key a{123, 1}, b{456, 2}, stale{123, 0}, replaced{123, 3};
void blocked(const PfdCopyProof& p, PfdCopyProof::Key k = a) {
  require(p.mode(k) == Mode::unknown, "Unproven copy admitted");
}
void modes() {
  for (const auto mode : {Mode::legacy_rt, Mode::enhanced_rt}) {
    PfdCopyProof p;
    p.observe_transition(a, mode);
    p.after_draw(a);
    blocked(p);
    p.reset(true);
    p.observe_transition(a, mode);
    blocked(p);
    p.after_draw(stale);
    blocked(p);
    p.after_draw(b);
    blocked(p);
    p.after_draw(a);
    require(p.mode(a) == mode, "Post-forward draw did not promote exact target");
    p.after_draw(a);
    require(p.mode(a) == mode, "Repeated draw lost unchanged proof");
    p.observe_transition(a, Mode::unknown);
    blocked(p);
    p.after_draw(a);
    blocked(p);
    p.observe_transition(a, mode);
    blocked(p);
    p.after_draw(a);
    require(p.mode(a) == mode, "Fresh exact RT entry did not recover");
    p.reset(true);
    blocked(p);
    p.after_draw(a);
    blocked(p);
    p.observe_transition(a, mode);
    p.reset(false);
    p.after_draw(a);
    blocked(p);
  }
}
void ambiguity() {
  PfdCopyProof p;
  p.reset(true);
  p.observe_transition(a, Mode::legacy_rt);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  blocked(p);
  p.reset(true);
  p.observe_transition(a, Mode::unknown, "split_or_partial_transition");
  p.after_draw(a);
  blocked(p);
  p.observe_transition(a, Mode::enhanced_rt);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  blocked(p);
  for (const bool before_draw : {true, false}) {
    p.reset(true);
    p.observe_transition(a, Mode::legacy_rt);
    if (!before_draw)
      p.after_draw(a);
    p.invalidate();  // Alias/discard/pass/unknown recording evidence.
    p.after_draw(a);
    blocked(p);
    p.observe_transition(a, Mode::legacy_rt);
    p.after_draw(a);
    blocked(p);
  }
}
void clear_state() {
  PfdCopyProof p;
  p.reset(true);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  require(p.mode(a) == Mode::legacy_rt, "ClearState fixture has no prior proof");
  p.clear_state();
  blocked(p);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  blocked(p);
  p.reset(true);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  require(p.mode(a) == Mode::legacy_rt, "Observed Reset did not renew post-ClearState proof");
}
void selection_changes() {
  PfdCopyProof p;
  p.reset(true);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  require(p.mode(a) == Mode::legacy_rt, "Selection fixture has no ready proof");
  // A barrier recorded while deselected must invalidate the previous proof.
  p.forget_resource(a.resource);
  p.after_draw(a);  // Reselect and draw without a new state transition.
  blocked(p);
  p.observe_transition(a, Mode::enhanced_rt);
  p.after_draw(a);
  require(p.mode(a) == Mode::enhanced_rt, "Fresh selected state entry failed");
  p.observe_transition(a, Mode::legacy_rt);
  p.forget_resource(a.resource);
  p.after_draw(a);
  blocked(p);
}
void identity_and_capacity() {
  PfdCopyProof p;
  p.reset(true);
  p.observe_transition(a, Mode::legacy_rt);
  p.observe_transition(b, Mode::enhanced_rt);
  p.after_draw(a);
  require(p.mode(a) == Mode::legacy_rt, "First independent resource lost proof");
  blocked(p, b);
  p.after_draw(b);
  require(p.mode(b) == Mode::enhanced_rt, "Second independent resource lost proof");
  p.observe_transition(replaced, Mode::enhanced_rt);
  blocked(p, a);
  blocked(p, replaced);
  p.after_draw(a);
  blocked(p, replaced);
  p.after_draw(replaced);
  require(p.mode(replaced) == Mode::enhanced_rt, "Fresh incarnation not promoted");
  p.observe_transition({789, 4}, Mode::legacy_rt);
  blocked(p, b);
  blocked(p, replaced);
  p.reset(true);
  p.observe_transition({0, 1}, Mode::legacy_rt);
  p.observe_transition({1, 0}, Mode::legacy_rt);
  p.observe_transition(a, Mode::legacy_rt);
  p.after_draw(a);
  require(p.mode(a) == Mode::legacy_rt, "Invalid keys consumed capacity");
}
}  // namespace
int main() {
  try {
    modes();
    ambiguity();
    identity_and_capacity();
    clear_state();
    selection_changes();
    std::printf("{\"passed\":true,\"checks\":%u}\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
