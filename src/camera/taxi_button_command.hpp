#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include "../shared/display_sides.hpp"

namespace taxi_camera::native_camera {
struct TaxiButtonRequest {
  std::uint64_t serial{}, session_epoch{};
  std::uint32_t profile{}, selected_mask{}, desired_mask{};
  bool operator==(const TaxiButtonRequest&) const = default;
};
struct TaxiButtonRequestStatus {
  std::uint64_t serial{};
  unsigned pending_mask{};
  bool failed{};
  const char* error = "";
};
struct TaxiButtonCommandContext {
  std::uint64_t now{}, session_epoch{}, button_sample_ms{};
  std::uint32_t profile{};
  unsigned actual_mask{};
  bool identity_valid{}, buttons_valid{}, aircraft_buttons = true;
};
struct TaxiButtonCommandDecision {
  unsigned send_mask{}, desired_mask{};
  std::uint64_t serial{}, button_sample_ms{};
};

// One dispatcher owns user TAXI requests. Accepted toggle sends survive
// request cancellation until a later observed latch acknowledges them; a
// timeout is not permission to repeat an uncertain toggle.
class TaxiButtonCommand {
 public:
  static constexpr std::uint64_t PermissionAgeMs = 500, RequestLifetimeMs = 3000;
  const TaxiButtonRequestStatus& status() const noexcept { return status_; }
  void update(const TaxiButtonRequest& request,
              bool permitted,
              std::uint64_t now,
              std::uint64_t epoch,
              std::uint32_t profile,
              bool aircraft_buttons) noexcept {
    permission_ = permitted;
    permission_ms_ = now;
    if (!permitted)
      fail("taxi_request_not_permitted");
    if (!request.serial) {
      fail("taxi_request_cancelled");
      return;
    }
    if (request.serial <= latest_.serial) {
      if (request.serial == latest_.serial && request != latest_)
        fail("taxi_request_changed");
      return;
    }
    latest_ = request;  // Even refused requests are consumed, never replayed later.
    status_ = {request.serial, request.selected_mask, false, ""};
    started_ms_ = now;
    if ((request.selected_mask & ~AllDisplaySides) || (request.desired_mask & ~request.selected_mask))
      reject("taxi_request_invalid");
    else if (!request.selected_mask)
      return;
    else if (!permitted)
      reject("taxi_request_not_permitted");
    else if (!aircraft_buttons)
      reject("taxi_buttons_unavailable_use_manual_control");
    else if (!epoch || request.session_epoch != epoch || request.profile != profile)
      reject("taxi_request_session_changed");
  }
  // Preserve the consumed request key across worker restarts/reselections.
  void reset_session() noexcept {
    fail("taxi_request_session_changed");
    permission_ = false;
    flights_ = {};
  }
  TaxiButtonCommandDecision step(const TaxiButtonCommandContext& c) noexcept {
    TaxiButtonCommandDecision result{0, 0, status_.serial, c.button_sample_ms};
    const bool buttons =
        c.buttons_valid && c.button_sample_ms && c.now >= c.button_sample_ms && c.now - c.button_sample_ms <= PermissionAgeMs;
    if (!permission_ || c.now < permission_ms_ || c.now - permission_ms_ > PermissionAgeMs)
      fail("taxi_request_permission_expired");
    if (status_.pending_mask && (latest_.session_epoch != c.session_epoch || latest_.profile != c.profile || !c.aircraft_buttons))
      fail("taxi_request_session_changed");
    if (status_.pending_mask && (c.now < started_ms_ || c.now - started_ms_ >= RequestLifetimeMs))
      fail("taxi_request_ack_timeout");
    if (!c.identity_valid || !buttons || !c.aircraft_buttons)
      return result;
    for (unsigned side = 0; side < MaxDisplaySides; ++side) {
      const unsigned bit = 1u << side;
      auto& flight = flights_[side];
      const bool actual = (c.actual_mask & bit) != 0;
      if (flight.active && c.button_sample_ms > flight.sample_ms && c.button_sample_ms > flight.sent_ms && actual == flight.desired)
        flight = {};
      if (flight.active || !(status_.pending_mask & bit))
        continue;
      const bool desired = (latest_.desired_mask & bit) != 0;
      if (actual == desired) {
        status_.pending_mask &= ~bit;
      } else {
        result.send_mask |= bit;
        if (desired)
          result.desired_mask |= bit;
      }
    }
    return result;
  }
  // Recheck after releasing the cache lock to prepare the SimConnect call.
  bool current(const TaxiButtonCommandDecision& decision, unsigned side, std::uint64_t now) const noexcept {
    if (side >= MaxDisplaySides || !(decision.send_mask & (1u << side)) || flights_[side].active)
      return false;
    return permission_ && now >= permission_ms_ && now - permission_ms_ <= PermissionAgeMs && decision.serial == status_.serial &&
           (status_.pending_mask & (1u << side));
  }
  void sent(const TaxiButtonCommandDecision& decision, unsigned side, bool accepted, std::uint64_t now) noexcept {
    if (side >= MaxDisplaySides || !(decision.send_mask & (1u << side)))
      return;
    if (accepted)
      flights_[side] = {true, (decision.desired_mask & (1u << side)) != 0, decision.button_sample_ms, now, decision.serial};
    else if (decision.serial == status_.serial)
      fail("taxi_button_command_failed");
  }
  // Only a correlated SimConnect rejection proves an accepted send did not run.
  void rejected(unsigned side) noexcept {
    if (side < MaxDisplaySides && flights_[side].active) {
      const auto serial = flights_[side].serial;
      flights_[side] = {};
      if (serial == status_.serial)
        fail("taxi_button_command_rejected");
    }
  }

 private:
  void reject(const char* error) noexcept {
    status_.pending_mask = 0;
    status_.failed = true;
    status_.error = error;
  }
  void fail(const char* error) noexcept {
    if (status_.pending_mask)
      reject(error);
  }
  struct Flight {
    bool active{}, desired{};
    std::uint64_t sample_ms{}, sent_ms{}, serial{};
  };
  TaxiButtonRequest latest_{};
  TaxiButtonRequestStatus status_{};
  std::array<Flight, MaxDisplaySides> flights_{};
  std::uint64_t permission_ms_{}, started_ms_{};
  bool permission_{};
};
}  // namespace taxi_camera::native_camera
