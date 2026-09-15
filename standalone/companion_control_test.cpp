#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#include "companion_control.hpp"
namespace {
unsigned checks{};
void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}
using namespace taxi_camera::standalone;
void publish(Mailbox& owner, std::uint64_t beat, const Settings& settings) {
  require(owner.lock(1000), "Writer lock");
  owner.data()->owner_pid = GetCurrentProcessId();
  owner.data()->owner_heartbeat = beat;
  owner.data()->settings = settings;
  owner.unlock();
}
struct BusyWriter {
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool locked{};
  std::thread worker;
  explicit BusyWriter(Mailbox& owner)
      : worker([&] {
          locked = owner.lock(1000);
          SetEvent(ready);
          WaitForSingleObject(release, 10000);
          if (locked)
            owner.unlock();
        }) {}
  ~BusyWriter() {
    SetEvent(release);
    worker.join();
    CloseHandle(ready);
    CloseHandle(release);
  }
  void wait() { require(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && locked, "Hold mutex from another thread"); }
};
}  // namespace
int main() {
  try {
    Mailbox owner, reader;
    require(owner.open(GetCurrentProcessId(), true), "Create isolated IPC");
    require(reader.open(GetCurrentProcessId(), false), "Open isolated IPC");
    CompanionControl control;
    require(!control.connected(1000) && !control.settings().enabled, "No enable before first message");
    Settings settings;
    require(settings.graphics_state_test == 0 && valid_settings(settings), "Graphics state test defaults off");
    settings.graphics_state_test = 1;
    settings.camera_rate = 60;
    settings.exposure = -7.3f;
    settings.follow_taxi = 0;
    settings.manual_mask = 3;
    settings.mounts[0][2] = 27.25;
    settings.nose_dot = {0.125f, 0.375f};
    settings.tail_upper = {0.25f, 0.625f};
    settings.tail_corner = {0.1875f, 0.75f};
    settings.tail_inner = {0.375f, 0.875f};
    settings.route_request = 12;
    settings.left_id = 149;
    settings.right_id = 148;
    publish(owner, 10000, settings);
    control.refresh(reader);
    require(control.connected(10000), "Fresh companion accepted");
    require(ProtocolVersion == 6 && control.settings().nose_dot == settings.nose_dot &&
                control.settings().tail_upper == settings.tail_upper && control.settings().tail_corner == settings.tail_corner &&
                control.settings().tail_inner == settings.tail_inner,
            "Protocol6 guide coordinates roundtrip");
    require(control.settings().graphics_state_test == 1, "Diagnostic flag roundtrips through IPC");
    constexpr const char* expected_modes[]{
        "off", "all", "targets_only", "shader_state_only", "pipeline_only", "root_bindings_only", "raster_state_only"};
    require(MaximumGraphicsStateTestMode == 6, "Diagnostic range preserves existing values and adds exactly three modes");
    for (unsigned mode = 0; mode <= MaximumGraphicsStateTestMode; ++mode) {
      auto diagnostic = settings;
      diagnostic.graphics_state_test = mode;
      publish(owner, 10000, diagnostic);
      control.refresh(reader);
      require(control.connected(10000) && control.settings().graphics_state_test == mode,
              "Every graphics diagnostic mode roundtrips through IPC");
      require(std::strcmp(graphics_state_mode_name(mode), expected_modes[mode]) == 0, "Diagnostic mode has stable log name");
    }
    for (const unsigned bad_mode : {7u, std::numeric_limits<unsigned>::max()}) {
      auto invalid = settings;
      invalid.graphics_state_test = bad_mode;
      publish(owner, 10000, invalid);
      control.refresh(reader);
      require(!control.connected(10000) && !control.settings().enabled, "Malformed diagnostic flag refuses IPC");
      require(std::strcmp(graphics_state_mode_name(bad_mode), "invalid") == 0, "Invalid diagnostic log name is bounded");
    }
    for (auto member : {&Settings::nose_dot, &Settings::tail_upper, &Settings::tail_corner, &Settings::tail_inner}) {
      for (unsigned axis = 0; axis < 2; ++axis) {
        for (float value :
             {-0.001f, axis ? 1.001f : 0.501f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
          auto invalid = settings;
          (invalid.*member)[axis] = value;
          publish(owner, 10000, invalid);
          control.refresh(reader);
          require(!control.connected(10000) && !control.settings().enabled, "Invalid guide IPC cannot enable cameras");
        }
      }
    }
    publish(owner, 10000, settings);
    control.refresh(reader);
    require(control.connected(10000), "Valid guides restore IPC after malformed packets");
    {
      BusyWriter writer(owner);
      writer.wait();
      // The former bridge immediately stopped its scenes when this returned false.
      require(!reader.lock(), "Reproduce former false-disconnect trigger");
      for (std::uint64_t now = 10025; now <= 15000; now += 25) {
        require(control.refresh(reader) == Mailbox::LockResult::busy, "Identify contention");
        require(control.connected(now), "Both PFDs stay requested during live heartbeat");
        const auto& held = control.settings();
        require(held.manual_mask == 3 && held.camera_rate == 60 && held.exposure == settings.exposure && held.mounts == settings.mounts &&
                    held.route_request == 12 && held.left_id == 149 && held.right_id == 148 && held.nose_dot == settings.nose_dot &&
                    held.tail_upper == settings.tail_upper && held.tail_corner == settings.tail_corner &&
                    held.tail_inner == settings.tail_inner && held.graphics_state_test == 1,
                "Preserve exact settings during contention");
      }
      require(!control.connected(15001), "Contention cannot extend heartbeat deadline");
    }
    require(control.busy_reads() == 200, "Contention counter");
    control.refresh(reader);
    require(!control.connected(15001), "Unchanged heartbeat cannot revive expired enable");
    publish(owner, 15002, settings);
    control.refresh(reader);
    require(control.connected(15002), "Fresh heartbeat restores connection");
    settings.enabled = 0;
    publish(owner, 15003, settings);
    control.refresh(reader);
    require(control.connected(15003) && !control.settings().enabled, "Fresh service OFF immediate");
    settings.enabled = 1;
    settings.manual_mask = 0;
    publish(owner, 15004, settings);
    control.refresh(reader);
    require(control.settings().manual_mask == 0, "Fresh display OFF immediate");
    publish(owner, 0, settings);
    control.refresh(reader);
    require(!control.connected(15005), "Companion exit immediate");
    publish(owner, 16001, settings);
    control.refresh(reader);
    require(!control.connected(16000) && control.connected(16001), "Post-read timestamp avoids false future heartbeat");
    settings.camera_rate = 999;
    publish(owner, 16002, settings);
    control.refresh(reader);
    require(!control.connected(16002) && !control.settings().enabled, "Malformed settings invalidate cache");
    settings.camera_rate = 60;
    publish(owner, 17000, settings);
    control.refresh(reader);
    require(control.connected(17000), "Valid message restores connection");
    require(owner.lock(1000), "Protocol mutation lock");
    owner.data()->version = 5;
    owner.unlock();
    control.refresh(reader);
    require(!control.connected(17000), "Old protocol5 invalidates the protocol6 diagnostic layout");
    require(owner.lock(1000), "Restore protocol lock");
    owner.data()->version = ProtocolVersion;
    owner.unlock();
    control.refresh(reader);
    require(control.connected(17000), "Restored protocol accepted");
    bool abandoned{};
    std::thread dead_writer([&] {
      abandoned = owner.lock(1000);
      if (abandoned)
        owner.data()->settings.camera_rate = 30;
      // Normal thread exit deliberately abandons an incomplete message.
    });
    dead_writer.join();
    require(abandoned, "Dead writer acquired mutex");
    require(control.refresh(reader) == Mailbox::LockResult::invalid, "Abandonment differs from contention");
    require(!control.connected(17001) && !control.settings().enabled, "Abandonment invalidates immediately");
    control.refresh(reader);
    require(!control.connected(17001), "Abandoned payload cannot resurrect enable");
    publish(owner, 18000, settings);
    control.refresh(reader);
    require(control.connected(18000), "New message recovers after abandonment");
    reader.close();
    control.refresh(reader);
    require(!control.connected(18000), "Closed channel invalidates");
    std::printf("PASS companion control: %u contention, watchdog, settings and abandonment checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL companion control: %s\n", error.what());
    return 1;
  }
}
