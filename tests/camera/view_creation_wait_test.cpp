#include "../../src/camera/view_creation_wait.hpp"
#include "../../src/shared/scene_demand.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
namespace ec = taxi_camera::engine_camera;
using taxi_camera::native_camera::ViewCreationWait;
constexpr ec::ManagerToken manager{17, 3};
constexpr std::uint64_t revision = 42;
unsigned checks = 0;

void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

// Pool refusal precedes a native call. The separate counters distinguish a
// refused callback from a real create that returned no ID after doing work.
struct Engine {
  enum class Mode {
    ready,
    first_initialize,
    first_create,
    second_initialize,
    second_create,
    bad_descriptor,
    native_failure,
    partial_failure
  };
  ec::PairController pair;
  ViewCreationWait wait;
  Mode mode = Mode::ready;
  bool pool_pending = true, allow_erase = false, try_resume_in_callback = false;
  bool reentrant_resume_refused = false;
  unsigned initializers = 0, create_callbacks = 0, native_initializers = 0, native_creates = 0, erases = 0;
  unsigned attempt_initializers = 0, attempt_creates = 0;
  std::array<ec::EntryId, 4> live{};
  ec::EntryId next_id = 1001;

  bool refuse_pool() noexcept {
    wait.defer(revision);
    if (try_resume_in_callback) {
      reentrant_resume_refused = !wait.resume(pair, revision, true) && wait.pending();
    }
    return false;
  }
  static bool initialize(void* opaque, ec::DescriptorStorage& descriptor) noexcept {
    auto& e = *static_cast<Engine*>(opaque);
    ++e.initializers;
    ++e.attempt_initializers;
    if (e.pool_pending && ((e.mode == Mode::first_initialize && e.attempt_initializers == 1) ||
                           (e.mode == Mode::second_initialize && e.attempt_initializers == 2)))
      return e.refuse_pool();
    ++e.native_initializers;
    descriptor.bytes.fill(0);
    if (e.mode == Mode::bad_descriptor)
      descriptor.bytes[0] = 1;
    return true;
  }
  static ec::EntryId create(void* opaque, ec::ManagerToken owner, const ec::DescriptorStorage&) noexcept {
    auto& e = *static_cast<Engine*>(opaque);
    require(owner == manager, "Create changed manager identity");
    ++e.create_callbacks;
    ++e.attempt_creates;
    if (e.pool_pending &&
        ((e.mode == Mode::first_create && e.attempt_creates == 1) || (e.mode == Mode::second_create && e.attempt_creates == 2))) {
      e.refuse_pool();
      return 0;
    }
    ++e.native_creates;
    if (e.mode == Mode::native_failure || (e.mode == Mode::partial_failure && e.attempt_creates == 2))
      return 0;
    for (unsigned i = 0; i < 2; ++i) {
      if (!e.live[i])
        return e.live[i] = e.next_id++;
    }
    require(false, "A third view was created before confirmed pair cleanup");
    return 0;
  }
  static bool erase(void* opaque, ec::ManagerToken owner, ec::EntryId id) noexcept {
    auto& e = *static_cast<Engine*>(opaque);
    require(owner == manager && id, "Erase lost exact ownership");
    ++e.erases;
    if (!e.allow_erase)
      return false;
    for (auto& owned : e.live) {
      if (owned == id) {
        owned = 0;
        return true;
      }
    }
    require(false, "Confirmed-absent ID was erased again");
    return false;
  }
  ec::EngineCallbacks callbacks() noexcept { return {this, initialize, create, erase}; }
  void update() { require(pair.process_update(manager, callbacks()), "Observer update was refused"); }
  void start() {
    attempt_initializers = attempt_creates = 0;
    pair.request_independent_pose();
    update();
  }
};

void initial_callback_wait() {
  for (const auto mode : {Engine::Mode::first_initialize, Engine::Mode::first_create}) {
    Engine e;
    e.mode = mode;
    e.try_resume_in_callback = true;
    e.start();
    require(e.wait.pending() && e.pair.snapshot().state == ec::State::failed && e.native_creates == 0 && e.erases == 0,
            "Initial pool refusal was not distinguishable from a native allocation");
    require(e.reentrant_resume_refused, "Deferral reset an in-flight controller whose published snapshot appeared empty");

    // A successful external preflight cannot authorize the later callback:
    // repeated callbacks may still observe pending retirement. The original
    // Start remains retryable without a new user request or native allocation.
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
      e.pair.request_disable();
      e.update();
      const auto initializers = e.initializers, callbacks = e.create_callbacks;
      require(e.wait.resume(e.pair, revision, true), "Empty pool refusal lost the current Start request");
      require(!e.wait.pending() && e.pair.snapshot().state == ec::State::disabled && e.pair.snapshot().failure == ec::Failure::none &&
                  e.initializers == initializers && e.create_callbacks == callbacks && e.native_creates == 0 && e.erases == 0,
              "Deferral reset invoked native callbacks or retained a terminal failure");
      e.start();
      require(e.wait.pending() && e.native_creates == 0, "Pending pool was bypassed on a retry");
    }
    e.pair.request_disable();
    e.update();
    for (unsigned update = 0; update < 8; ++update) {
      require(!e.wait.resume(e.pair, revision, true, false) && e.wait.pending() && e.pair.snapshot().state == ec::State::disabled &&
                  e.native_creates == 0,
              "Temporary suspension or public unready state consumed the pending Start");
    }
    require(e.wait.resume(e.pair, revision, true), "Drained pool could not resume the same Start");
    e.pool_pending = false;
    e.start();
    require(e.pair.snapshot().state == ec::State::active && e.native_creates == 2 && e.erases == 0 && !e.wait.pending(),
            "Same-request retry failed to create exactly one pair after pool admission");
    const auto ids = e.pair.snapshot().owned_ids;
    require(!e.wait.resume(e.pair, revision, true) && e.pair.snapshot().owned_ids == ids && e.native_creates == 2,
            "Consumed deferral reset an active pair");
  }
}

void partial_callback_wait() {
  for (const auto mode : {Engine::Mode::second_initialize, Engine::Mode::second_create}) {
    Engine e;
    e.mode = mode;
    e.start();
    const auto owned = e.pair.snapshot().owned_ids;
    require(
        e.wait.pending() && owned[0] == 1001 && !owned[1] && e.native_creates == 1 && e.pair.snapshot().state == ec::State::cleanup_pending,
        "Second-stage pool refusal forgot the first native allocation");
    e.pair.request_disable();
    for (unsigned update = 0; update < 8; ++update) {
      const auto erases = e.erases;
      e.update();
      require(e.erases == erases + 1 && e.pair.snapshot().owned_ids == owned && e.native_creates == 1,
              "Pending pool blocked cleanup or recreated a partial pair");
      require(!e.wait.resume(e.pair, revision, true) && e.wait.pending() && e.pair.snapshot().owned_ids == owned,
              "Deferral was lost or forgot ownership before cleanup confirmation");
    }
    e.allow_erase = true;
    e.update();
    require(e.pair.snapshot().state == ec::State::disabled && e.live == std::array<ec::EntryId, 4>{},
            "Later cleanup did not confirm the first allocation absent");
    const auto erases = e.erases;
    require(e.wait.resume(e.pair, revision, true) && !e.wait.pending(), "Confirmed partial cleanup lost the pending Start");
    e.pool_pending = false;
    e.start();
    require(e.pair.snapshot().state == ec::State::active && e.native_creates == 3 && e.erases == erases &&
                e.pair.snapshot().owned_ids == std::array<ec::EntryId, 4>{1002, 1003, 0},
            "Partial cleanup retry reused a retired ID or failed to produce one replacement pair");
  }
}

void immediate_prewarm_publication() {
  using taxi_camera::standalone::ScenePrewarm;
  for (const auto mode : {Engine::Mode::first_initialize, Engine::Mode::first_create, Engine::Mode::second_create}) {
    for (const bool cleanup_completed : {false, true}) {
      Engine e;
      e.mode = mode;
      e.allow_erase = cleanup_completed;
      ScenePrewarm prewarm;
      require(prewarm.observe(100, true, false, {}, false), "Prewarm did not begin");
      e.start();
      const auto creates = e.native_creates, erases = e.erases;

      // This is the probe's same-observer settlement, before it publishes its
      // snapshot to the control worker. Deferring it until a later update can
      // permanently park prewarm on the transient State::failed publication.
      e.pair.request_disable();
      const auto cancelled = e.pair.cancel_uncreated_request();
      const auto published = e.pair.snapshot();
      const bool partial = mode == Engine::Mode::second_create && !cleanup_completed;
      require(cancelled == (partial ? ec::EmptyPairCancel::owned : ec::EmptyPairCancel::cancelled) &&
                  published.state == (partial ? ec::State::cleanup_pending : ec::State::disabled) && e.wait.pending() &&
                  e.native_creates == creates && e.erases == erases,
              "Immediate settlement lost partial ownership or called native cleanup");
      require(!e.wait.resume(e.pair, revision, true, false) && e.wait.pending(), "Temporary suspension consumed the settled request");
      require(prewarm.observe(110, true, false, {}, published.state == ec::State::failed || published.state == ec::State::blocked) &&
                  prewarm.pending() && prewarm.phase() == ScenePrewarm::Phase::preparing,
              "One transient pool-refusal publication permanently failed background preparation");
      if (partial) {
        require(!e.wait.resume(e.pair, revision, true), "Publication allowed partial ownership to be forgotten");
        e.allow_erase = true;
        e.update();
      }
      require(e.wait.resume(e.pair, revision, true), "Settled pool refusal lost its later same-request resume");
      e.pool_pending = false;
      e.start();
      require(e.pair.snapshot().state == ec::State::active && e.native_creates == creates + 2,
              "Settled publication could not resume camera creation");
      require(!prewarm.observe(120, true, false, {true, true, 3, 3}, false) && prewarm.phase() == ScenePrewarm::Phase::ready,
              "Background preparation failed to finish after the deferred pool became available");
    }
  }
}

void cancellation() {
  for (const bool partial : {false, true}) {
    for (const bool newer_start : {false, true}) {
      Engine e;
      e.mode = partial ? Engine::Mode::second_create : Engine::Mode::first_initialize;
      e.start();
      const auto before = e.pair.snapshot();
      const auto creates = e.native_creates, erases = e.erases;
      if (newer_start)
        e.pair.request_independent_pose();
      else
        e.pair.request_disable();
      require(!e.wait.resume(e.pair, newer_start ? revision + 1 : revision, newer_start) && !e.wait.pending(),
              "Stop or a newer Start resurrected the deferred request");
      require(e.pair.snapshot().request_pending && e.pair.snapshot().owned_ids == before.owned_ids && e.native_creates == creates &&
                  e.erases == erases,
              "Cancelled deferral consumed the newer mailbox request or touched native ownership");
      e.allow_erase = true;
      e.pool_pending = false;
      e.update();
      require(e.pair.snapshot().state == (newer_start ? ec::State::active : ec::State::disabled) &&
                  e.native_creates == creates + (newer_start ? 2u : 0u),
              "The newer request failed to own subsequent creation or cleanup");
      require(!e.wait.resume(e.pair, revision, true), "Cancelled old revision could be resumed later");
    }
  }
}

void native_failures_remain_terminal() {
  for (const auto mode : {Engine::Mode::bad_descriptor, Engine::Mode::native_failure, Engine::Mode::partial_failure}) {
    Engine e;
    e.mode = mode;
    e.allow_erase = true;
    e.start();
    const auto failed = e.pair.snapshot();
    const auto initializers = e.initializers, creates = e.native_creates, erases = e.erases;
    require(!e.wait.pending() && failed.state == ec::State::failed && failed.failure != ec::Failure::none,
            "A real native or descriptor failure was misclassified as pool deferral");
    for (unsigned update = 0; update < 8; ++update) {
      require(!e.wait.resume(e.pair, revision, true), "An unmarked native failure automatically rearmed");
      e.update();
      require(e.pair.snapshot().failure == failed.failure && e.initializers == initializers && e.native_creates == creates &&
                  e.erases == erases,
              "Pool wait policy weakened a terminal native failure");
    }
  }
}
}  // namespace

int main() {
  initial_callback_wait();
  partial_callback_wait();
  immediate_prewarm_publication();
  cancellation();
  native_failures_remain_terminal();
  std::printf("PASS: view creation wait (%u checks)\n", checks);
}
