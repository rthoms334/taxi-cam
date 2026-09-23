#include "../../src/shared/companion_control.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
namespace {
unsigned checks{};
void require(bool ok, const char* label) {
  ++checks;
  if (!ok)
    throw std::runtime_error(label);
}
using namespace taxi_camera::standalone;
using taxi_camera::NotificationReader;
using taxi_camera::SimEvent;
using taxi_camera::SimEventLog;
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
    require(valid_settings(settings), "Default settings are valid");
    settings.camera_rate = 60;
    settings.exposure = -7.3f;
    settings.follow_taxi = 0;
    settings.manual_mask = 3;
    settings.mounts[0][2] = 27.25;
    settings.nose_dot = {0.125f, 0.375f};
    settings.tail_upper = {0.25f, 0.625f};
    settings.tail_corner = {0.1875f, 0.75f};
    settings.tail_inner = {0.375f, 0.875f};
    settings.guide_color = {0.125f, 0.5f, 0.875f};
    settings.speed_color = {0.25f, 0.75f, 0.375f};
    settings.parked_rate = 8;
    require(valid_settings(settings), "Adjusted parked floor is valid");
    settings.parked_rate = 3;
    require(!valid_settings(settings), "Parked floor below the schedule minimum is rejected");
    settings.parked_rate = 0;
    require(valid_settings(settings), "Parked floor 0 disables the floor and stays valid");
    {
      // Bit 2 is the lower ECAM: valid only for a profile with that side.
      auto sd = settings;
      sd.manual_mask = 4;
      require(!valid_settings(sd), "A two-display profile rejects the SD side");
      sd.profile = taxi_camera::profiles::AerosoftA346.id;
      require(valid_settings(sd), "The A340-600 accepts its SD side");
      sd.manual_mask = 8;
      require(!valid_settings(sd), "Bits beyond the lower ECAM stay invalid");
      sd.manual_mask = 0;
      sd.taxi_request = 1;
      sd.taxi_selected_mask = sd.taxi_desired_mask = 4;
      require(valid_settings(sd), "An SD cockpit request is valid on the A340-600");
    }
    settings.parked_rate = 8;
    settings.notifications = 0;
    require(valid_settings(settings), "Notifications off is valid");
    settings.notifications = 2;
    require(!valid_settings(settings), "Notifications is a 0/1 flag");
    settings.notifications = 0;
    settings.route_request = 12;
    settings.taxi_request = 7;
    settings.taxi_selected_mask = 3;
    settings.taxi_desired_mask = 2;
    settings.left_id = 149;
    settings.right_id = 148;
    publish(owner, 10000, settings);
    control.refresh(reader);
    require(control.connected(10000), "Fresh companion accepted");
    require(control.owner_pid() == GetCurrentProcessId(), "Complete message retains the current companion owner");
    CompanionSetupSession setup;
    auto change = setup.observe(control.connected(10000), control.settings().enabled, control.owner_pid(), 1);
    require(change.started && !change.stopped && change.generation == 1, "First Connect starts bridge setup");
    change = setup.observe(true, true, control.owner_pid(), 1);
    require(!change.started && !change.stopped && change.generation == 1, "Same live request does not repeat setup");
    change = setup.observe(true, true, control.owner_pid(), 2);
    require(change.started && change.generation == 2, "Rapid Disconnect/Connect serial forces setup even between bridge polls");
    change = setup.observe(false, false, control.owner_pid(), 2);
    require(change.stopped && !change.started, "Disconnect closes the setup session");
    change = setup.observe(false, false, control.owner_pid(), 2);
    require(!change.stopped && !change.started, "Disconnected polling does not repeat shutdown");
    change = setup.observe(true, true, control.owner_pid(), 2);
    require(change.started && change.generation == 3, "Heartbeat reconnection restarts unchanged-profile setup");
    change = setup.observe(true, true, control.owner_pid() + 1, 2);
    require(change.started && change.generation == 4, "New companion owner restarts setup despite reused serial");
    change = setup.observe(true, false, control.owner_pid() + 1, 2);
    require(change.stopped, "Disabled connection closes output without waiting for heartbeat timeout");
    require(ProtocolVersion == 15 && control.settings().nose_dot == settings.nose_dot &&
                control.settings().tail_upper == settings.tail_upper && control.settings().tail_corner == settings.tail_corner &&
                control.settings().tail_inner == settings.tail_inner,
            "Protocol11 guide coordinates roundtrip");
    require(control.settings().parked_rate == settings.parked_rate, "Protocol11 parked floor roundtrip");
    require(control.settings().notifications == 0, "Protocol12 notification preference roundtrip");
    {
      // Protocol 12 status: the bridge's notification log travels in Status
      // and the companion reader orders it by serial.
      SimEventLog log;
      log.publish(SimEvent::bridge_connected, 20000);
      log.publish(SimEvent::cameras_ready, 20100);
      require(owner.lock(1000), "Status lock");
      log.snapshot(owner.data()->status.notifications);
      owner.unlock();
      require(reader.lock(1000), "Status read lock");
      const auto wire = reader.data()->status.notifications;
      reader.unlock();
      NotificationReader companion;
      std::array<SimEvent, 4> events{};
      require(companion.take(wire, 20200, events.data(), events.size()) == 2 && events[0] == SimEvent::bridge_connected &&
                  events[1] == SimEvent::cameras_ready,
              "Protocol12 notification log roundtrip");
    }
    require(control.settings().guide_color == settings.guide_color && control.settings().speed_color == settings.speed_color,
            "Independent marking and ground-speed colours roundtrip");
    require(control.settings().taxi_request == 7 && control.settings().taxi_selected_mask == 3 && control.settings().taxi_desired_mask == 2,
            "Protocol9 scoped aircraft TAXI request roundtrip");
    auto invalid_request = settings;
    invalid_request.taxi_selected_mask = 1;
    require(!valid_settings(invalid_request), "Aircraft request cannot change an unselected side");
    invalid_request = settings;
    invalid_request.taxi_request = 0;
    require(!valid_settings(invalid_request), "Aircraft request must have a nonzero serial");
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
    for (auto member : {&Settings::guide_color, &Settings::speed_color}) {
      for (unsigned channel = 0; channel < 3; ++channel) {
        for (float value : {-0.001f, 1.001f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
          auto invalid = settings;
          (invalid.*member)[channel] = value;
          publish(owner, 10000, invalid);
          control.refresh(reader);
          require(!control.connected(10000) && !control.settings().enabled, "Invalid RGB IPC cannot enable cameras");
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
        require(control.owner_pid() == GetCurrentProcessId(), "Contention does not invent an owner change");
        const auto& held = control.settings();
        require(held.manual_mask == 3 && held.camera_rate == 60 && held.exposure == settings.exposure && held.mounts == settings.mounts &&
                    held.route_request == 12 && held.left_id == 149 && held.right_id == 148 && held.nose_dot == settings.nose_dot &&
                    held.tail_upper == settings.tail_upper && held.tail_corner == settings.tail_corner &&
                    held.tail_inner == settings.tail_inner && held.guide_color == settings.guide_color &&
                    held.speed_color == settings.speed_color,
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
    for (const std::uint32_t old_version : {9u, 10u, 11u}) {
      require(owner.lock(1000), "Protocol mutation lock");
      owner.data()->version = old_version;
      owner.unlock();
      control.refresh(reader);
      require(!control.connected(17000), "Old protocol 9/10/11 is rejected by the protocol12 layout");
    }
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
