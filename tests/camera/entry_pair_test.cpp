#include "../../src/camera/entry_pair.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

namespace {
using namespace taxi_camera::engine_camera;

unsigned checks = 0;
void require(bool value, const char* message) noexcept {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

constexpr ManagerToken manager{11, 1};
constexpr ManagerToken other_manager{11, 2};

PairKeys keys(std::uint8_t seed = 1) {
  PairKeys result{};
  for (std::size_t camera = 0; camera < result.size(); ++camera)
    for (std::size_t byte = 0; byte < result[camera].size(); ++byte)
      result[camera][byte] = static_cast<std::uint8_t>(seed + camera * 31 + byte);
  return result;
}

void mock_initialize(DescriptorStorage& descriptor) noexcept {
  // Deliberately leave unobserved bytes dirty: packing may touch only its fields.
  descriptor.bytes.fill(0xa5);
  descriptor.bytes[0] = descriptor.bytes[64] = 0;
  for (const auto offset : {32u, 40u, 192u})
    std::fill_n(descriptor.bytes.begin() + offset, 4, 0);
  descriptor.bytes[44] = 1;
}

struct MockEngine {
  std::thread::id update_thread = std::this_thread::get_id();
  unsigned initializations = 0;
  unsigned creations = 0;
  unsigned initialization_failure_at = 0;
  unsigned bad_initializer_at = 0;
  unsigned creation_failure_at = 0;
  unsigned duplicate_at = 0;
  unsigned refused_erases = 0;
  EntryId next_id = 100;
  std::set<EntryId> live;
  std::vector<EntryId> erased;
  std::vector<PairKeys::value_type> created_keys;
  std::vector<DescriptorStorage> descriptors;
  PairController* controller = nullptr;
  bool exercise_reentry = false;
  bool reentry_tested = false;
  bool request_disable_during_init = false;
  std::mutex coordination;
  std::condition_variable condition;
  bool initializer_entered = false;
  bool ui_finished = false;

  void check_thread() noexcept { require(std::this_thread::get_id() == update_thread, "An engine callback ran on the UI thread"); }

  EngineCallbacks callbacks() noexcept { return {this, initialize, create, erase}; }

  static bool initialize(void* context, DescriptorStorage& descriptor) noexcept {
    auto& self = *static_cast<MockEngine*>(context);
    self.check_thread();
    ++self.initializations;
    require(std::all_of(descriptor.bytes.begin(), descriptor.bytes.end(), [](auto byte) { return byte == 0; }),
            "Initializer did not receive fresh zeroed padded storage");
    if (self.exercise_reentry && !self.reentry_tested) {
      self.reentry_tested = true;
      require(!self.controller->process_update(manager, self.callbacks()), "Reentrant update was not refused");
      require(!self.controller->acknowledge_manager_destroyed(manager), "Reentrant lifetime acknowledgement was not refused");
      (void)self.controller->snapshot();  // Must not deadlock: callback holds no mailbox lock.
    }
    if (self.request_disable_during_init && self.initializations == 1) {
      std::unique_lock lock(self.coordination);
      self.initializer_entered = true;
      self.condition.notify_all();
      self.condition.wait(lock, [&] { return self.ui_finished; });
    }
    if (self.initializations == self.initialization_failure_at)
      return false;
    mock_initialize(descriptor);
    if (self.initializations == self.bad_initializer_at)
      descriptor.bytes[192] = 1;
    return true;
  }

  static EntryId create(void* context, ManagerToken token, const DescriptorStorage& descriptor) noexcept {
    auto& self = *static_cast<MockEngine*>(context);
    self.check_thread();
    require(token == manager || token == other_manager, "Create received an unexpected manager token");
    ++self.creations;
    self.descriptors.push_back(descriptor);
    PoseKey key{};
    std::copy_n(descriptor.bytes.begin() + 48, key.size(), key.begin());
    self.created_keys.push_back(key);
    if (self.creations == self.creation_failure_at)
      return 0;
    if (self.creations == self.duplicate_at)
      return self.next_id - 1;
    const auto id = self.next_id++;
    self.live.insert(id);
    return id;
  }

  static bool erase(void* context, ManagerToken token, EntryId id) noexcept {
    auto& self = *static_cast<MockEngine*>(context);
    self.check_thread();
    require(token == manager || token == other_manager, "Erase received an unexpected manager token");
    require(id != 0, "Controller tried to erase a zero ID");
    self.erased.push_back(id);
    if (self.refused_erases != 0) {
      --self.refused_erases;
      return false;
    }
    // An adapter may confirm absence after a previous unconfirmed erase.
    self.live.erase(id);
    return true;
  }
};

void packing() {
  require(sizeof(DescriptorStorage) == 256 && alignof(DescriptorStorage) == 16 && DescriptorStorage::minimum_observed_extent == 196,
          "Local padded descriptor storage changed");
  DescriptorStorage descriptor;
  mock_initialize(descriptor);
  const auto before = descriptor.bytes;
  const auto key = keys()[0];
  require(pack_mode_zero(descriptor, key), "Initialized empty-name descriptor rejected");
  for (std::size_t offset = 0; offset < descriptor.bytes.size(); ++offset) {
    const auto expected = offset >= 40 && offset < 44   ? 0
                          : offset == 44                ? 1
                          : offset >= 48 && offset < 64 ? key[offset - 48]
                                                        : before[offset];
    require(descriptor.bytes[offset] == expected, "Packing modified an unrelated descriptor byte");
  }
  for (const auto offset : {0u, 32u, 33u, 35u, 64u, 192u, 193u, 195u}) {
    mock_initialize(descriptor);
    descriptor.bytes[offset] = 1;
    const auto bad = descriptor.bytes;
    require(!pack_mode_zero(descriptor, key) && descriptor.bytes == bad, "Malformed initialized name was overwritten or accepted");
  }
}

void success_and_requests() {
  PairController controller;
  MockEngine engine;
  controller.request_enable(keys());
  require(engine.creations == 0 && engine.initializations == 0 && controller.snapshot().request_pending,
          "Request publication performed engine work");
  require(controller.process_update(manager, engine.callbacks()), "Update unexpectedly refused");
  auto snapshot = controller.snapshot();
  require(snapshot.state == State::active && snapshot.owner == manager && snapshot.owned_ids == std::array<EntryId, 4>{100, 101, 0} &&
              !snapshot.request_pending && !snapshot.creation_pending && engine.created_keys == std::vector<PoseKey>{keys()[0], keys()[1]},
          "Successful pair did not preserve order, keys or ownership");
  for (unsigned i = 0; i < 50; ++i)
    controller.process_update(manager, engine.callbacks());
  controller.request_enable(keys());
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 2 && engine.erased.empty(), "Repeated updates or identical active enable created duplicates");
  controller.request_disable();
  require(engine.erased.empty(), "Disable request erased on its publisher thread");
  controller.process_update(manager, engine.callbacks());
  require(engine.erased == std::vector<EntryId>{101, 100} && engine.live.empty() && controller.snapshot().state == State::disabled,
          "Pair did not clean up in reverse order exactly once");
  for (unsigned i = 0; i < 50; ++i)
    controller.process_update(manager, engine.callbacks());
  require(engine.erased.size() == 2, "Confirmed absent IDs were erased repeatedly");

  controller.request_enable(keys());
  controller.request_disable();
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 2, "Last disable request did not cancel queued enable");
  controller.request_enable(keys());
  controller.request_enable(keys(7));
  controller.process_update(manager, engine.callbacks());
  require(engine.created_keys[2] == keys(7)[0] && engine.created_keys[3] == keys(7)[1], "Mailbox did not retain latest opaque keys");
  controller.request_enable(keys(9));
  controller.process_update(manager, engine.callbacks());
  require(
      engine.erased == std::vector<EntryId>{101, 100, 103, 102} && controller.snapshot().owned_ids == std::array<EntryId, 4>{104, 105, 0},
      "Replacing an active pair failed to clean up old ownership first");
}

void failures() {
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    PairController controller;
    MockEngine engine;
    Failure expected = Failure::none;
    if (scenario < 2) {
      engine.initialization_failure_at = scenario + 1;
      expected = Failure::initializer_failed;
    } else if (scenario < 4) {
      engine.bad_initializer_at = scenario - 1;
      expected = Failure::initializer_contract;
    } else if (scenario < 6) {
      engine.creation_failure_at = scenario - 3;
      expected = scenario == 4 ? Failure::first_create_failed : Failure::second_create_failed;
    } else {
      engine.duplicate_at = 2;
      expected = Failure::duplicate_id;
    }
    controller.request_enable(keys());
    controller.process_update(manager, engine.callbacks());
    const auto snapshot = controller.snapshot();
    require(snapshot.state == State::failed && snapshot.failure == expected && snapshot.owned_ids == std::array<EntryId, 4>{} &&
                engine.live.empty() && !snapshot.creation_pending,
            "Failed pair retained successful state or leaked confirmed owned IDs");
    require(engine.erased.size() <= 1 && (engine.erased.empty() || engine.erased[0] == 100),
            "Failure erased an unowned ID or erased the duplicate twice");
    const auto creations = engine.creations;
    const auto initializations = engine.initializations;
    const auto erases = engine.erased.size();
    for (unsigned i = 0; i < 50; ++i)
      controller.process_update(manager, engine.callbacks());
    require(engine.creations == creations && engine.initializations == initializations && engine.erased.size() == erases,
            "A failed creation automatically retried without an explicit request");
    engine.initialization_failure_at = engine.bad_initializer_at = engine.creation_failure_at = engine.duplicate_at = 0;
    controller.request_enable(keys());
    controller.process_update(manager, engine.callbacks());
    require(controller.snapshot().state == State::active && engine.creations == creations + 2,
            "Explicit enable could not retry after a consumed failure");
  }
}

void pending_cleanup() {
  PairController controller;
  MockEngine engine;
  engine.creation_failure_at = 2;
  engine.refused_erases = 2;
  controller.request_enable(keys());
  controller.process_update(manager, engine.callbacks());
  require(controller.snapshot().state == State::cleanup_pending && controller.snapshot().owned_ids[0] == 100 &&
              controller.snapshot().failure == Failure::second_create_failed,
          "Unconfirmed rollback forgot its ID");
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 2 && controller.snapshot().owned_ids[0] == 100, "Cleanup retry recreated entries or lost ownership");
  controller.process_update(manager, engine.callbacks());
  require(controller.snapshot().state == State::failed && engine.live.empty() && engine.erased == std::vector<EntryId>{100, 100, 100},
          "Successful later rollback did not retain the creation failure");
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 2 && engine.erased.size() == 3, "Completed rollback was repeated");

  controller.request_enable(keys());
  controller.process_update(manager, engine.callbacks());
  engine.refused_erases = 2;
  controller.request_disable();
  controller.process_update(manager, engine.callbacks());
  const auto old_ids = controller.snapshot().owned_ids;
  require(controller.snapshot().state == State::cleanup_pending && old_ids[0] != 0 && old_ids[1] != 0,
          "Unconfirmed disable forgot ownership");
  // Even if both IDs remain tracked, a prior unconfirmed erase may have deleted
  // them. An identical enable must finish cleanup and create, not reactivate.
  controller.request_enable(keys());
  controller.process_update(manager, engine.callbacks());
  require(controller.snapshot().state == State::active && controller.snapshot().owned_ids != old_ids && engine.creations == 6,
          "Enable reused IDs after unconfirmed deletion");

  engine.refused_erases = 1;
  controller.request_disable();
  controller.process_update(manager, engine.callbacks());
  const auto partial = controller.snapshot().owned_ids;
  require(partial[0] == 0 && partial[1] != 0, "Partial cleanup did not forget only the confirmed ID");
  const auto erased_count = engine.erased.size();
  controller.process_update(manager, engine.callbacks());
  require(engine.erased.size() == erased_count + 1 && engine.erased.back() == partial[1] && controller.snapshot().state == State::disabled,
          "Cleanup retried a previously confirmed ID");
}

void context_and_lifetime() {
  PairController controller;
  MockEngine engine;
  controller.request_enable(keys());
  controller.process_update({}, engine.callbacks());
  require(controller.snapshot().blocked == Blocked::invalid_manager && engine.creations == 0 && controller.snapshot().creation_pending,
          "Invalid manager consumed creation or called the engine");
  for (unsigned missing = 0; missing < 3; ++missing) {
    auto callbacks = engine.callbacks();
    if (missing == 0)
      callbacks.initialize = nullptr;
    if (missing == 1)
      callbacks.create = nullptr;
    if (missing == 2)
      callbacks.erase = nullptr;
    controller.process_update(manager, callbacks);
    require(controller.snapshot().blocked == Blocked::missing_callbacks && engine.creations == 0, "Incomplete callback set was used");
  }
  controller.process_update(manager, engine.callbacks());
  controller.request_enable(keys(2));
  controller.process_update(other_manager, engine.callbacks());
  require(controller.snapshot().blocked == Blocked::manager_mismatch && controller.snapshot().owner == manager && engine.erased.empty() &&
              engine.creations == 2,
          "Manager generation switch reused or erased old IDs");
  require(!controller.acknowledge_manager_destroyed(other_manager), "Wrong destroyed manager forgot live ownership");
  controller.request_enable(keys(3));
  require(controller.acknowledge_manager_destroyed(manager), "Exact destroyed manager was not acknowledged");
  require(controller.snapshot().state == State::failed && controller.snapshot().failure == Failure::manager_destroyed &&
              controller.snapshot().owned_ids == std::array<EntryId, 4>{} && !controller.snapshot().request_pending &&
              engine.erased.empty(),
          "Destruction event called erase or kept queued automatic recreation");
  engine.live.clear();  // The simulated engine owner destroyed this manager.
  controller.process_update(other_manager, engine.callbacks());
  require(engine.creations == 2, "Manager destruction automatically recreated a pair");
  controller.request_enable(keys(4));
  controller.process_update(other_manager, engine.callbacks());
  require(controller.snapshot().owner == other_manager && engine.creations == 4,
          "Explicit enable could not use the new manager generation");
}

void independent_pose() {
  DescriptorStorage descriptor;
  mock_initialize(descriptor);
  const auto original = descriptor.bytes;
  require(pack_independent_pose(descriptor), "Independent descriptor refused initialized names");
  for (std::size_t index = 0; index < descriptor.bytes.size(); ++index) {
    const auto expected = index == 40                                                 ? 2
                          : (index > 40 && index < 44) || (index >= 48 && index < 64) ? 0
                          : index == 44                                               ? 1
                                                                                      : original[index];
    require(descriptor.bytes[index] == expected, "Independent packing changed an unrelated byte");
  }
  PairController controller;
  MockEngine engine;
  controller.request_independent_pose();
  require(engine.creations == 0, "Independent request called the engine from the UI");
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 2 && controller.snapshot().state == State::active, "Independent pair was not created");
  for (const auto& created : engine.descriptors)
    require(
        created.bytes[40] == 2 && std::all_of(created.bytes.begin() + 48, created.bytes.begin() + 64, [](auto byte) { return byte == 0; }),
        "Independent mode used an opaque key");
  controller.request_independent_pose();
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 2 && engine.erased.empty(), "Same independent request was not idempotent");
  controller.request_enable({});
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 4 && engine.erased.size() == 2 && engine.descriptors.back().bytes[40] == 0,
          "Switching to configured zero keys did not replace independent cameras");
  controller.request_independent_pose();
  controller.process_update(manager, engine.callbacks());
  require(engine.creations == 6 && engine.descriptors.back().bytes[40] == 2, "Independent replacement did not restore mode two");
  controller.request_disable();
  controller.process_update(manager, engine.callbacks());
  require(engine.live.empty(), "Independent cameras were not removed");
}

void threading() {
  PairController controller;
  MockEngine engine;
  engine.controller = &controller;
  engine.exercise_reentry = true;
  engine.request_disable_during_init = true;
  controller.request_enable(keys());
  std::thread ui([&] {
    std::unique_lock lock(engine.coordination);
    engine.condition.wait(lock, [&] { return engine.initializer_entered; });
    lock.unlock();
    controller.request_disable();
    const auto snapshot = controller.snapshot();
    // Avoid shared test-counter writes while the update thread runs callbacks.
    if (!snapshot.request_pending)
      std::abort();
    lock.lock();
    engine.ui_finished = true;
    engine.condition.notify_all();
  });
  controller.process_update(manager, engine.callbacks());
  ui.join();
  require(engine.reentry_tested && controller.snapshot().request_pending && controller.snapshot().state == State::active,
          "An in-flight UI request was lost or reentrant processing ran");
  controller.process_update(manager, engine.callbacks());
  require(controller.snapshot().state == State::disabled && engine.live.empty(),
          "The next update did not consume the concurrent UI request");
}

}  // namespace

int main() {
  packing();
  success_and_requests();
  failures();
  pending_cleanup();
  context_and_lifetime();
  independent_pose();
  threading();
  std::printf("PASS: %u descriptor, pair ownership, rollback, manager lifetime and mailbox checks. Mock engine only.\n", checks);
  return 0;
}
