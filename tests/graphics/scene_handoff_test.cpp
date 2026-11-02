#include "../../src/graphics/scene_handoff.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
using namespace taxi_camera;
constexpr std::uint64_t kDevice = 11;
constexpr std::uint64_t kNose = 101;
constexpr std::uint64_t kTail = 202;
constexpr SceneManagerIdentity kManager{301, 7};
constexpr std::array<std::uint64_t, 4> kIds{401, 402, 0, 0};
constexpr std::array<std::uint64_t, 4> kHandles{kNose, kTail, 0, 0};
unsigned checks = 0;

void require(bool value, const char* error) {
  ++checks;
  if (!value)
    throw std::runtime_error(error);
}

struct Fixture {
  SceneHandoff handoff;
  std::uint64_t device_epoch = 0;
  Fixture() {
    device_epoch = handoff.register_device(kDevice);
    require(device_epoch != 0, "Device registration failed");
    require(handoff.register_resource(kDevice, kNose, 1), "Nose registration failed");
    require(handoff.register_resource(kDevice, kTail, 2), "Tail registration failed");
  }
  void start() { require(handoff.begin_scene() != 0, "Authorized scene start failed"); }
  bool publish() { return handoff.publish(handoff.begin_capture(), kManager, kIds, kHandles); }
  SceneCopyMatch nose() { return handoff.observe_copy(kDevice, kNose, 0).source; }
};

void basic_matching() {
  Fixture test;
  require(!test.handoff.may_match_resource(0) && !test.handoff.may_match_resource(kNose), "Inactive atomic hint matched");
  require(!test.publish() && !test.nose().matched, "Unrequested scene allowed publication");
  test.start();
  require(test.publish(), "Complete pair was not published");
  require(test.handoff.may_match_resource(kNose) && test.handoff.may_match_resource(kTail) && !test.handoff.may_match_resource(999) &&
              !test.handoff.may_match_resource(0),
          "Atomic hint did not reject unrelated resources");
  const auto observation = test.handoff.observe_copy(kDevice, kNose, kTail);
  require(
      observation.source.matched && observation.source.feed == 0 && observation.destination.matched && observation.destination.feed == 1,
      "Copy source/destination did not preserve ordered feed identities");
  require(observation.source.manager == kManager && observation.source.entry_id == kIds[0] && observation.destination.entry_id == kIds[1] &&
              observation.source.resource.device_epoch == test.device_epoch && observation.source.resource.resource_id == 1 &&
              observation.destination.resource.resource_id == 2 &&
              observation.source.resource.generation != observation.destination.resource.generation,
          "Manager, entry or live resource identity was lost");
  require(test.handoff.is_current(observation.source) && test.handoff.is_current(observation.destination), "Fresh identity was stale");
  require(
      !test.handoff.observe_copy(kDevice + 1, kNose, kTail).source.matched && !test.handoff.observe_copy(kDevice, 999, 0).source.matched,
      "An unregistered device/resource matched");
  const auto reversed = test.handoff.observe_copy(kDevice, kTail, kNose);
  require(reversed.source.feed == 1 && reversed.destination.feed == 0, "Feed identity depends on copy direction");
  const auto pending = test.handoff.begin_capture();
  require(test.handoff.may_match_resource(kNose) && test.handoff.may_match_resource(kTail), "Refresh lost completed pair hints");
  require(test.nose().matched && test.handoff.is_current(observation.source) &&
              test.nose().capture_sequence == observation.source.capture_sequence,
          "Refresh revoked or prematurely replaced a completed publication");
  require(test.handoff.publish(pending, kManager, kIds, kHandles), "Same-pair refresh failed");
  require(test.handoff.is_current(observation.source), "A completed owned frame was invalidated solely by a later same-pair observation");
  require(!test.handoff.publish(pending, kManager, kIds, kHandles), "One capture ticket was published twice");
  require(test.nose().matched, "Duplicate stale publication revoked a valid publication");
}

void lifecycle_and_aba() {
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    Fixture test;
    test.start();
    require(test.publish(), "Lifecycle fixture failed");
    const auto old = test.nose();
    const auto ticket = test.handoff.begin_capture();
    if (mutation == 0)
      test.handoff.unregister_resource(kDevice, kNose);
    if (mutation == 1) {
      test.handoff.unregister_resource(kDevice, kNose);
      require(test.handoff.register_resource(kDevice, kNose, 1), "Same-address registration failed");
    }
    if (mutation == 2)
      require(test.handoff.register_resource(kDevice, kNose, 1), "Repeated init failed");
    if (mutation == 3)
      test.handoff.unregister_device(kDevice);
    if (mutation == 4) {
      test.handoff.unregister_device(kDevice);
      require(test.handoff.register_device(kDevice) != test.device_epoch, "Reused device key retained epoch");
      require(test.handoff.register_resource(kDevice, kNose, 1) && test.handoff.register_resource(kDevice, kTail, 2),
              "Recreated device resource registration failed");
    }
    if (mutation == 5)
      require(test.handoff.register_resource(kDevice, 999, 3), "Unrelated registration failed");
    if (mutation == 6)
      test.handoff.unregister_resource(kDevice, 998);
    require(!test.handoff.publish(ticket, kManager, kIds, kHandles), "Lifecycle change did not refuse an open capture ticket");
    if (mutation < 5) {
      require(!test.handoff.is_current(old) && !test.nose().matched, "Target/device lifecycle left an old match usable");
      require(!test.handoff.may_match_resource(kNose) && !test.handoff.may_match_resource(kTail), "Target/device lifecycle retained hint");
    } else {
      require(test.handoff.is_current(old) && test.nose().matched, "Unrelated lifecycle revoked a completed publication");
      require(test.handoff.may_match_resource(kNose) && test.handoff.may_match_resource(kTail), "Unrelated lifecycle lost completed hints");
    }
    if (mutation == 1 || mutation == 2 || mutation == 4) {
      require(test.publish(), "Fresh graph capture could not observe recreated resources");
      require(test.nose().resource.generation != old.resource.generation && !test.handoff.is_current(old),
              "Same-address ABA reused an old resource generation");
    }
  }
}

void scene_and_owner_changes() {
  Fixture test;
  test.start();
  require(test.publish(), "Owner fixture failed");
  const auto old = test.nose();
  const auto ticket = test.handoff.begin_capture();
  test.handoff.stop_scene();
  require(!test.handoff.may_match_resource(kNose) && !test.handoff.may_match_resource(kTail), "Stop retained atomic hints");
  require(!test.handoff.publish(ticket, kManager, kIds, kHandles) && !test.handoff.is_current(old),
          "Stop did not invalidate in-flight work");
  test.start();
  require(test.publish() && !test.handoff.is_current(old) && test.nose().scene_epoch != old.scene_epoch,
          "Restart accepted previous scene's GPU capture");
  for (unsigned change = 0; change < 4; ++change) {
    test.start();
    require(test.publish(), "Initial owner binding failed");
    auto manager = kManager;
    auto ids = kIds;
    if (change == 0)
      ++manager.identity;
    if (change == 1)
      ++manager.generation;
    if (change == 2)
      ++ids[1];
    if (change == 3)
      ids = {kIds[1], kIds[0]};
    require(!test.handoff.publish(test.handoff.begin_capture(), manager, ids, kHandles) && !test.nose().matched,
            "Manager generation or ordered pair changed within an authorized scene");
    require(!test.publish(), "Owner mismatch allowed automatic reauthorization");
  }
  test.start();
  const auto older = test.handoff.begin_capture();
  const auto newer = test.handoff.begin_capture();
  require(test.handoff.publish(newer, kManager, kIds, kHandles), "Newer capture failed");
  require(!test.handoff.publish(older, {}, {}, {}) && test.nose().matched, "Older completion revoked or replaced newer publication");
}

void streaming_lifecycle() {
  Fixture test;
  require(test.handoff.register_device(22) != 0, "Second device setup failed");
  test.start();
  require(test.publish(), "Streaming fixture publication failed");
  const auto original = test.nose();
  const auto initial_unrelated = test.handoff.diagnostics().unrelated_events;
  for (unsigned i = 0; i < 200; ++i) {
    const std::uint64_t unrelated = 1000 + i;
    require(test.handoff.register_resource(kDevice, unrelated, 3 + i), "Streaming resource init failed");
    test.handoff.unregister_resource(kDevice, unrelated);
    require(!test.handoff.register_resource(22, unrelated, 0), "Untracked buffer was registered");
    test.handoff.unregister_resource(22, unrelated);
    require(test.handoff.is_current(original) && test.nose().matched && test.handoff.may_match_resource(kNose),
            "Unrelated streaming invalidated a completed camera publication");
  }
  require(test.handoff.diagnostics().published && test.handoff.diagnostics().unrelated_events - initial_unrelated == 800,
          "Streaming diagnostics did not preserve publication");
  const auto ticket = test.handoff.begin_capture();
  require(test.handoff.observed_feed(kNose) == 0 && test.handoff.observed_feed(kTail) == 1 && test.handoff.observed_feed(0) == -1 &&
              test.handoff.observed_feed(999) == -1,
          "Read-only observation hints were lost during the next inspection");
  require(test.nose().matched && test.handoff.may_match_resource(kNose) && test.handoff.is_current(original),
          "Opening streaming refresh revoked completed capture authority");
  test.handoff.unregister_resource(22, 888);
  require(!test.handoff.publish(ticket, kManager, kIds, kHandles) && test.handoff.diagnostics().ticket_invalidations == 1,
          "Unrelated lifecycle during inspection did not invalidate its ticket");
  require(test.handoff.diagnostics().published && test.handoff.is_current(original) && test.nose().matched,
          "Rejected refresh ticket revoked the previously completed pair");
  require(test.publish(), "Publication did not recover after fresh inspection");
  require(test.handoff.register_resource(22, kNose, 5), "Ambiguous address registration failed");
  require(!test.handoff.is_current(original) && !test.nose().matched && test.handoff.observed_feed(kNose) == -1,
          "Same-address registration on another device preserved publication");
  require(!test.publish(), "Ambiguous resource address was published");
  test.handoff.unregister_resource(22, kNose);
  require(test.publish(), "Publication did not recover after ambiguity ended");
  test.handoff.unregister_resource(kDevice, kTail);
  require(!test.nose().matched && test.handoff.observed_feed(kNose) == -1 && test.handoff.observed_feed(kTail) == -1,
          "Target destruction failed to invalidate both feed hints");
  require(test.handoff.register_resource(kDevice, kTail, 2) && test.publish(), "Fresh target incarnation did not recover");
  test.handoff.stop_scene();
  require(test.handoff.observed_feed(kNose) == -1 && !test.handoff.diagnostics().published, "Stop retained an observed source");
}

void invalid_requests_and_multiple_devices() {
  for (unsigned invalid = 0; invalid < 12; ++invalid) {
    Fixture test;
    test.start();
    require(test.publish(), "Invalid-refresh prior publication failed");
    auto manager = kManager;
    auto ids = kIds;
    auto handles = kHandles;
    if (invalid == 0)
      manager.identity = 0;
    if (invalid == 1)
      manager.generation = 0;
    if (invalid == 2)
      ids[0] = 0;
    if (invalid == 3)
      ids[1] = ids[0];
    if (invalid == 4)
      handles[0] = 0;
    if (invalid == 5)
      handles[1] = handles[0];
    if (invalid == 6)
      handles[1] = 999;
    if (invalid == 7 || invalid == 8) {
      require(test.handoff.register_device(22) != 0 && test.handoff.register_resource(22, kTail, 2), "Secondary device setup failed");
      if (invalid == 8)
        test.handoff.unregister_resource(kDevice, kTail);
    }
    // The door feed (index 3) follows the third feed's rules: an id without
    // a handle, the nose's handle again, a handle without an id.
    if (invalid == 9)
      ids[3] = 403;
    if (invalid == 10) {
      ids[3] = 403;
      handles[3] = kNose;
    }
    if (invalid == 11)
      handles[3] = kTail;
    require(!test.handoff.publish(test.handoff.begin_capture(), manager, ids, handles) && !test.nose().matched,
            "Malformed/unknown/ambiguous/mixed-device resources published");
    require(!test.handoff.may_match_resource(kNose) && !test.handoff.may_match_resource(kTail),
            "Invalid latest refresh retained previous candidate authorization hints");
  }
  Fixture test;
  test.start();
  require(test.publish(), "Match validation fixture failed");
  const auto good = test.nose();
  for (unsigned invalid = 0; invalid < 9; ++invalid) {
    auto changed = good;
    if (invalid == 0)
      changed.matched = false;
    if (invalid == 1)
      changed.feed = 2;
    if (invalid == 2)
      ++changed.scene_epoch;
    if (invalid == 3)
      ++changed.manager.generation;
    if (invalid == 4)
      ++changed.entry_id;
    if (invalid == 5)
      ++changed.resource.device_epoch;
    if (invalid == 6)
      ++changed.resource.resource_id;
    if (invalid == 7)
      ++changed.resource.generation;
    if (invalid == 8)
      changed.capture_sequence = 0;
    require(!test.handoff.is_current(changed), "A mismatched completed-frame token remained current");
  }
}

void completed_refresh_retention() {
  Fixture test;
  test.start();
  require(test.publish(), "Refresh retention fixture failed");
  const auto completed = test.nose();
  const auto publications = test.handoff.diagnostics().publications;
  for (unsigned iteration = 0; iteration < 330; ++iteration) {
    const auto refresh = test.handoff.begin_capture();
    require(test.handoff.is_current(completed) && test.nose().capture_sequence == completed.capture_sequence,
            "Open refresh replaced or revoked the last completed frame");
    test.handoff.unregister_resource(kDevice, 1000 + iteration);
    require(!test.handoff.publish(refresh, kManager, kIds, kHandles), "Unrelated churn did not reject the pending ticket");
    require(test.handoff.is_current(completed) && test.handoff.diagnostics().published && test.nose().matched,
            "Repeated rejected refresh created a publication gap");
  }
  require(test.handoff.diagnostics().ticket_invalidations == 330 && test.handoff.diagnostics().publications == publications,
          "Retained publication was counted as a successful refresh");

  constexpr std::array<std::uint64_t, 4> future{501, 502, 0, 0};
  require(test.handoff.register_resource(kDevice, future[0], 7) && test.handoff.register_resource(kDevice, future[1], 8),
          "Future outputs could not be registered");
  const auto older = test.handoff.begin_capture();
  const auto latest = test.handoff.begin_capture();
  require(!test.handoff.publish(older, {}, {}, {}) && test.handoff.is_current(completed),
          "Superseded malformed refresh revoked completed publication");
  require(!test.handoff.may_match_resource(future[0]) && !test.handoff.observe_copy(kDevice, future[0], 0).source.matched,
          "Registered but unpublished future output gained capture authority");
  require(test.handoff.publish(latest, kManager, kIds, future), "Latest complete replacement output pair refused");
  require(!test.handoff.is_current(completed) && !test.nose().matched && !test.handoff.may_match_resource(kNose) &&
              test.handoff.may_match_resource(future[0]) && test.handoff.may_match_resource(future[1]),
          "Completed replacement did not switch resource identity and hints");
  const auto replacement = test.handoff.observe_copy(kDevice, future[0], 0).source;
  require(replacement.matched && replacement.capture_sequence == latest.sequence,
          "Replacement published an unvalidated or superseded sequence");
  require(!test.handoff.publish(older, kManager, kIds, kHandles) && test.handoff.is_current(replacement),
          "Older completion replaced a newer completed pair");

  Fixture concurrent;
  concurrent.start();
  require(concurrent.publish(), "Concurrent refresh fixture failed");
  const auto current = concurrent.nose();
  std::atomic<bool> bad{false};
  std::thread refresh([&] {
    for (unsigned i = 0; i < 2000; ++i) {
      const auto ticket = concurrent.handoff.begin_capture();
      concurrent.handoff.unregister_resource(kDevice, 10000 + i);
      if (concurrent.handoff.publish(ticket, kManager, kIds, kHandles))
        bad.store(true);
    }
  });
  std::thread consume([&] {
    for (unsigned i = 0; i < 3000; ++i)
      if (!concurrent.handoff.is_current(current) || !concurrent.nose().matched || !concurrent.handoff.may_match_resource(kNose) ||
          !concurrent.handoff.diagnostics().published)
        bad.store(true);
  });
  refresh.join();
  consume.join();
  require(!bad.load(), "Concurrent unrelated refreshes exposed a completed-publication gap");
}

void caps_and_concurrency() {
  Fixture invalid_registration;
  invalid_registration.start();
  require(invalid_registration.publish(), "Invalid-registration fixture failed");
  require(!invalid_registration.handoff.register_resource(kDevice, kNose, 0) && !invalid_registration.publish(),
          "An untracked replacement registration retained a previous live incarnation");
  SceneHandoff bounded;
  require(bounded.register_device(0) == 0 && !bounded.register_resource(0, 1, 1), "Zero device key was accepted");
  for (std::uint64_t device = 1; device <= SceneHandoff::maximum_devices; ++device)
    require(bounded.register_device(device) != 0, "Supported device count refused");
  require(bounded.register_device(SceneHandoff::maximum_devices + 1) == 0, "Device capacity exceeded");
  for (std::uint64_t resource = 1; resource <= SceneHandoff::maximum_resources_per_device; ++resource)
    require(bounded.register_resource(1, resource, resource), "Supported resource count refused");
  require(!bounded.register_resource(1, SceneHandoff::maximum_resources_per_device + 1, 1), "Resource capacity exceeded");
  require(bounded.register_resource(1, 1, 88), "Existing handle could not get a fresh generation at capacity");
  bounded.unregister_device(1);
  require(bounded.register_device(5) != 0 && bounded.register_resource(5, 1, 1), "Device cleanup did not reclaim bounded capacity");

  Fixture test;
  test.start();
  std::atomic<bool> bad{false};
  std::thread observer([&]() {
    for (unsigned i = 0; i < 3000; ++i) {
      test.handoff.publish(test.handoff.begin_capture(), kManager, kIds, kHandles);
      const auto copy = test.handoff.observe_copy(kDevice, kNose, kTail);
      if ((copy.source.matched && (copy.source.feed != 0 || copy.source.entry_id != kIds[0])) ||
          (copy.destination.matched && (copy.destination.feed != 1 || copy.destination.entry_id != kIds[1])))
        bad.store(true);
    }
  });
  std::thread lifecycle([&]() {
    for (unsigned i = 0; i < 3000; ++i) {
      test.handoff.unregister_resource(kDevice, kNose);
      test.handoff.register_resource(kDevice, kNose, 1);
    }
  });
  observer.join();
  lifecycle.join();
  require(!bad.load() && test.publish(), "Concurrent lifecycle/observer calls corrupted matching or failed to recover");
}
}  // namespace

int main() {
  try {
    basic_matching();
    lifecycle_and_aba();
    scene_and_owner_changes();
    streaming_lifecycle();
    invalid_requests_and_multiple_devices();
    completed_refresh_retention();
    caps_and_concurrency();
    std::printf("PASS: %u scene handoff checks; lifecycle/scene ABA, generations, caps and concurrent callbacks.\n", checks);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
