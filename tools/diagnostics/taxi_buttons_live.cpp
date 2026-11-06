#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../src/camera/body_pose_provider.hpp"

constexpr unsigned long kDefaultDurationSeconds = 30;
constexpr unsigned long kMaximumDurationSeconds = 120;

int main(int argc, char** argv) {
  unsigned long duration_seconds = kDefaultDurationSeconds;
  if (argc > 2)
    return 2;
  if (argc == 2) {
    char* end = nullptr;
    duration_seconds = std::strtoul(argv[1], &end, 10);
    if (!end || *end || duration_seconds < 1 || duration_seconds > kMaximumDurationSeconds)
      return 2;
  }
  using namespace taxi_camera::native_camera;
  if (!initialize_body_pose_provider()) {
    std::puts("{\"passed\":false,\"error\":\"provider_initialization\"}");
    return 1;
  }
  const auto start = GetTickCount64();
  std::uint64_t printed_at = 0;
  TaxiButtonSample previous;
  unsigned valid_samples = 0, changes = 0;
  do {
    const auto now = GetTickCount64();
    const auto sample = get_taxi_buttons();
    const bool changed = sample.valid != previous.valid || sample.left_on != previous.left_on || sample.right_on != previous.right_on ||
                         std::strcmp(sample.error, previous.error) != 0;
    if (sample.valid && sample.sample_ms != previous.sample_ms)
      ++valid_samples;
    if (previous.valid && sample.valid && (sample.left_on != previous.left_on || sample.right_on != previous.right_on))
      ++changes;
    if (!printed_at || changed || now - printed_at >= 1000) {
      const auto ground = get_ground_speed();
      const auto body = sample_body_pose(now);
      std::printf(
          "{\"elapsed_ms\":%llu,\"valid\":%s,\"left_on\":%s,\"right_on\":%s,\"sample_ms\":%llu,\"age_ms\":%llu,"
          "\"error\":\"%s\",\"ground_valid\":%s,\"body_error\":\"%s\"}\n",
          static_cast<unsigned long long>(now - start), sample.valid ? "true" : "false", sample.left_on ? "true" : "false",
          sample.right_on ? "true" : "false", static_cast<unsigned long long>(sample.sample_ms),
          static_cast<unsigned long long>(sample.sample_ms && now >= sample.sample_ms ? now - sample.sample_ms : 0), sample.error,
          ground.valid ? "true" : "false", body.error);
      std::fflush(stdout);
      printed_at = now;
    }
    previous = sample;
    Sleep(20);
  } while (GetTickCount64() - start < duration_seconds * 1000ull);
  shutdown_body_pose_provider();
  std::printf("{\"passed\":%s,\"valid_samples\":%u,\"changes\":%u,\"shutdown_cleared\":%s}\n", valid_samples ? "true" : "false",
              valid_samples, changes, !get_taxi_buttons().valid ? "true" : "false");
  return valid_samples ? 0 : 1;
}
