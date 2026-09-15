#include "../../src/camera/probe.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
namespace nc = taxi_camera::native_camera;
namespace ec = taxi_camera::engine_camera;
unsigned checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void require_wrong_host() {
  wchar_t path[32768]{};
  const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  require(length > 0 && length < std::size(path), "The test executable identity could not be read exactly");
  const auto* separator = std::wcsrchr(path, L'\\');
  const auto* basename = separator ? separator + 1 : path;
  require(_wcsicmp(basename, L"FlightSimulator2024.exe") != 0,
          "This integration test must run as an ordinary executable, never as FlightSimulator2024.exe");
}

void require_inert(const nc::ProbeSnapshot& snapshot) {
  require(!snapshot.hook_installed, "Wrong host installed an engine observer hook");
  require(!snapshot.accepting_requests, "Wrong host accepted a native scene request");
  require(!snapshot.restart_pending && !snapshot.recovery_pending, "Wrong host queued native camera recovery");
  require(!snapshot.pose_captured, "Wrong host reported a captured private-engine pose");
  for (const auto& pose : snapshot.mounted_poses)
    require(pose.position == nc::Vector3{} && pose.target == nc::Vector3{} && pose.up == nc::Vector3{} && pose.fov == 0,
            "Wrong host published an applied mount pose");
  require(snapshot.updates == 0 && snapshot.thread_id == 0, "Wrong host ran an engine update observer");
  require(snapshot.inspection_count == 0 && snapshot.created_total == 0, "Wrong host serviced or created private engine views");
  require(snapshot.observer_last_ms == 0 && snapshot.observer_max_ms == 0, "Wrong host reported timed observer work");
  const auto& performance = snapshot.performance;
  require(performance.stage_ms == decltype(performance.stage_ms){} && performance.query_calls == 0 && performance.read_calls == 0 &&
              performance.requested_bytes == 0 && performance.query_ms == 0 && performance.read_ms == 0 && performance.entry_count == 0 &&
              performance.bucket_count == 0,
          "Wrong host performed or timed diagnostic memory work");
  require(snapshot.gates == std::array<bool, 2>{} && snapshot.activation_counts == std::array<std::uint64_t, 2>{},
          "Wrong host applied a private activation pulse");
  require(snapshot.free_views == 0 && snapshot.ready == std::array<bool, 2>{} && snapshot.resource_present == std::array<bool, 2>{},
          "Wrong host published discovered views or output resources");
  require(snapshot.dimensions == decltype(snapshot.dimensions){} && snapshot.flags == decltype(snapshot.flags){},
          "Wrong host published private view dimension or flag metadata");
  require(snapshot.pair.owned_ids == std::array<ec::EntryId, 2>{} && !snapshot.pair.owner.valid(),
          "Wrong host acquired an engine-manager lifetime or owned entry IDs");
  require(snapshot.pair.state == ec::State::disabled && snapshot.pair.failure == ec::Failure::none &&
              snapshot.pair.blocked == ec::Blocked::none && !snapshot.pair.creation_pending,
          "Wrong host advanced camera-pair lifecycle state");
}

void require_refused() {
  const auto snapshot = nc::scene_snapshot();
  require_inert(snapshot);
  require(snapshot.message == "Native camera access requires the FlightSimulator2024.exe process.",
          "The public start request did not refuse at the actual executable-name guard");
}

bool same_mounts(const nc::MountPair& left, const nc::MountPair& right) {
  for (unsigned i = 0; i < left.size(); ++i)
    if (left[i].position_m != right[i].position_m || left[i].pitch_degrees != right[i].pitch_degrees ||
        left[i].yaw_degrees != right[i].yaw_degrees || left[i].fov_radians != right[i].fov_radians)
      return false;
  return true;
}

void mount_mailbox() {
  const auto defaults = nc::default_mounts();
  require(same_mounts(nc::scene_snapshot().mounts, defaults), "Initial mount settings do not match evidenced defaults");
  auto first = defaults, second = defaults;
  first[0].position_m = {-2, -3, 25};
  first[1].pitch_degrees = -25;
  first[1].fov_radians = 0.7f;
  second[0].position_m = {3, -2, 28};
  second[0].yaw_degrees = 5;
  second[1].pitch_degrees = -15;
  second[1].fov_radians = 1.1f;
  require(nc::request_scene_mounts(first), "A valid mount pair was refused");
  require(same_mounts(nc::scene_snapshot().mounts, first), "A valid mount pair was not published to settings");
  auto invalid = second;
  invalid[1].fov_radians = std::numeric_limits<float>::quiet_NaN();
  require(!nc::request_scene_mounts(invalid), "Nonfinite mount configuration was accepted");
  require(same_mounts(nc::scene_snapshot().mounts, first), "Invalid configuration replaced the accepted pair");
  invalid = second;
  invalid[0].position_m[0] = nc::kMountMaximumOffset + 1;
  require(!nc::request_scene_mounts(invalid), "Out-of-bounds mount configuration was accepted");
  require(same_mounts(nc::scene_snapshot().mounts, first), "Out-of-bounds configuration replaced the accepted pair");
  require_inert(nc::scene_snapshot());
  require(!nc::scene_snapshot().pair.request_pending, "A mount configuration queued creation on the wrong host");

  std::atomic<bool> coherent{true};
  std::array<std::thread, 4> workers;
  for (unsigned worker = 0; worker < workers.size(); ++worker)
    workers[worker] = std::thread([&, worker] {
      for (unsigned i = 0; i < 1000; ++i) {
        if (!nc::request_scene_mounts((i + worker) % 2 ? first : second))
          coherent.store(false);
        const auto snapshot = nc::scene_snapshot();
        if ((!same_mounts(snapshot.mounts, first) && !same_mounts(snapshot.mounts, second)) || snapshot.hook_installed ||
            snapshot.pose_captured || snapshot.updates != 0 || snapshot.pair.request_pending)
          coherent.store(false);
      }
    });
  for (auto& worker : workers)
    worker.join();
  require(coherent.load(), "Concurrent mount requests split the pair or triggered native work");
  require_inert(nc::scene_snapshot());
  require(nc::request_scene_mounts(defaults), "Restoring default mounts failed");
}

void profile_transition_mailbox() {
  require_wrong_host();
  const auto first = nc::request_scene_profile_transition(1);
  const auto first_status = nc::scene_snapshot();
  require(first && first_status.profile_transition_token == first && first_status.profile_transition_id == 1 &&
              first_status.profile_transition_ready && !first_status.profile_transition_pending,
          "Initial empty-pair transition did not acknowledge its exact request");
  require_inert(first_status);
  const auto repeat = nc::request_scene_profile_transition(1);
  require(repeat > first && nc::scene_snapshot().profile_transition_token == repeat,
          "Same-profile reload reused the previous transition token");
  const auto next = nc::request_scene_profile_transition(2);
  const auto next_status = nc::scene_snapshot();
  require(next > repeat && next_status.profile_transition_id == 2 && next_status.profile_transition_token == next,
          "Superseding profile failed to publish its own acknowledgement");
  require(nc::request_scene_profile_transition(9999) == 0 && nc::scene_snapshot().profile_transition_token == next,
          "Invalid profile changed the accepted transition");
  require_inert(next_status);
}
void wrong_host_public_flow() {
  require_wrong_host();
  const auto initial = nc::scene_snapshot();
  require_inert(initial);
  require(!initial.pair.request_pending, "A fresh runtime unexpectedly contains a queued pair request");
  require(initial.requested_rate == 15 && initial.requested_feeds == 2, "A fresh runtime has unexpected pulse configuration");
  mount_mailbox();

  // This API is a pure atomic settings mailbox even on the wrong host. Editing
  // it cannot bypass the executable guard, install a hook or queue creation.
  const std::array<std::array<unsigned, 4>, 9> settings{{{0, 0, 15, 1},
                                                         {14, 1, 15, 1},
                                                         {17, 2, 17, 2},
                                                         {20, 3, 20, 2},
                                                         {21, 2, 21, 2},
                                                         {30, 2, 30, 2},
                                                         {60, 1, 60, 1},
                                                         {61, 3, 60, 2},
                                                         {0xffffffffu, 0xffffffffu, 60, 2}}};
  for (const auto& values : settings) {
    nc::request_scene_rate(values[0], values[1]);
    const auto snapshot = nc::scene_snapshot();
    require_inert(snapshot);
    require(snapshot.requested_rate == values[2] && snapshot.requested_feeds == values[3],
            "Pulse settings were not clamped to the supported range");
    require(!snapshot.pair.request_pending, "A pulse settings update queued engine work");
  }
  nc::request_scene_rate(15);

  // Exact production public entry point. No forged PE image, profile overrides,
  // observer invocation or mock replacement for any private native function.
  nc::request_scene_test();
  require_refused();
  require(!nc::scene_snapshot().pair.request_pending, "Refused start queued a creation request before host validation");

  nc::request_scene_stop();
  require_refused();
  require(nc::scene_snapshot().pair.request_pending, "Stop did not queue its non-executing disable request");

  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    nc::request_scene_test();
    require_refused();
    nc::request_scene_stop();
    require_refused();
  }

  // UI requests and snapshots may arrive on different threads. Wrong-host
  // starts must remain refused while stop only changes the pair mailbox.
  std::array<std::thread, 4> workers;
  for (unsigned worker = 0; worker < workers.size(); ++worker) {
    workers[worker] = std::thread([worker] {
      for (unsigned attempt = 0; attempt < 32; ++attempt) {
        nc::request_scene_rate(worker % 2 ? 60 : 15, worker % 2 ? 1 : 2);
        if ((worker + attempt) % 2 == 0)
          nc::request_scene_test();
        else
          nc::request_scene_stop();
        const auto snapshot = nc::scene_snapshot();
        (void)snapshot;
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  require_refused();
  const auto concurrent = nc::scene_snapshot();
  require((concurrent.requested_rate == 15 && concurrent.requested_feeds == 2) ||
              (concurrent.requested_rate == 60 && concurrent.requested_feeds == 1),
          "Concurrent configuration split the atomic rate/feed pair");
  nc::request_scene_stop();
  require_refused();
}

}  // namespace

int main() {
  try {
    profile_transition_mailbox();
    wrong_host_public_flow();
    std::printf("PASS: %u production runtime wrong-host checks; no hook, owned entries, pose calls or engine updates.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
