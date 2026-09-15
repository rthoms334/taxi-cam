#include "../../src/app/bug_report.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace taxi_camera::standalone;

static std::string decode(std::string_view input) {
  std::string result;
  const auto digit = [](char c) {
    assert((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    return c <= '9' ? c - '0' : c - 'A' + 10;
  };
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%') {
      assert(i + 2 < input.size());
      result += static_cast<char>(digit(input[i + 1]) * 16 + digit(input[i + 2]));
      i += 2;
    } else
      result += input[i];
  }
  return result;
}

int main() {
  const std::string special = "space +&#?=%\r\n\"\\\xc3\xa9";
  const auto encoded = bug_report_query_value(special);
  assert(encoded == "space%20%2B%26%23%3F%3D%25%0D%0A%22%5C%C3%A9");
  assert(decode(encoded) == special);
  assert(bug_report_query_value("azAZ09-_.~") == "azAZ09-_.~");
  assert(bug_report_query_value(std::string_view("a\0b", 3)) == "a%00b");

  BugReportContext context;
  context.now = 10000;
  context.simulator_running = true;
  context.unsaved_edits = true;
  context.settings.profile = 1;
  context.settings.manual_mask = 1;
  context.settings.camera_rate = 30;
  auto snapshot = bug_report_snapshot(context);
  assert(snapshot.find("not yet connected; status unavailable") != std::string::npos);
  assert(snapshot.find("captures=") == std::string::npos);
  assert(snapshot.find("unapplied text edits excluded); unsaved_edits=1") != std::string::npos);
  assert(snapshot.find("Profile=1/fbw-a380x") != std::string::npos);
  assert(snapshot.find("fps=30") != std::string::npos && snapshot.find("preview_mask=1") != std::string::npos);
  context.bridge_seen = true;
  assert(bug_report_snapshot(context).find("disconnected; previous status unavailable") != std::string::npos);

  context.status.heartbeat = context.now - BugReportFreshMs;
  context.status.graphics_ready = 1;
  context.status.captures = 123456;
  context.status.composed = 789;
  context.status.stamps = 234;
  context.status.hook_failures = 3;
  snapshot = bug_report_snapshot(context);
  assert(snapshot.find("Bridge status: connected; age_ms=3000") != std::string::npos);
  assert(snapshot.find("captures=123456; compositions=789; stamps=234; hook_failures=3") != std::string::npos);
  assert(snapshot.find("Stage ms (manager/pool/lifecycle/entries/view1/view2/handoff/pose/activation/publish)") != std::string::npos);
  assert(snapshot.find("truncated") == std::string::npos);
  --context.status.heartbeat;
  assert(bug_report_snapshot(context).find("stale; last known values below; age_ms=3001") != std::string::npos);
  context.status.heartbeat = context.now + 1;
  assert(bug_report_snapshot(context).find("stale; heartbeat time invalid") != std::string::npos);
  context.status.heartbeat = context.now;

  // Changing all arbitrary bridge text must leave the report unchanged. Include
  // both realistic private paths and buffers without a terminating NUL.
  const auto private_baseline = bug_report_url(context);
  std::strcpy(context.status.message, "Token=secret-value; C:\\Users\\private-account\\logs");
  assert(bug_report_url(context) == private_baseline);
  std::memset(context.status.message, 'M', sizeof(context.status.message));
  assert(bug_report_url(context) == private_baseline);

  const auto wide_url = bug_report_url(context);
  const std::string url(wide_url.begin(), wide_url.end());
  assert(url.starts_with(BugReportPage));
  const std::string version_marker = "&taxi_cam_version=", snapshot_marker = "&runtime_snapshot=";
  const auto version_start = url.find(version_marker);
  const auto snapshot_start = url.find(snapshot_marker);
  assert(version_start == BugReportPage.size() && snapshot_start > version_start);
  assert(url.find('&', snapshot_start + 1) == std::string::npos && url.find('#') == std::string::npos);
  const auto version =
      decode(std::string_view(url).substr(version_start + version_marker.size(), snapshot_start - version_start - version_marker.size()));
  assert(version == std::string(TAXI_CAM_VERSION) + "-build." + std::to_string(TAXI_CAM_BUILD_NUMBER));
  assert(decode(std::string_view(url).substr(snapshot_start + snapshot_marker.size())) == bug_report_snapshot(context));

  // Hostile numeric status cannot cause oversized URLs or iterate candidate_count.
  context.now = UINT64_MAX;
  context.status.heartbeat = 1;
  context.status.captures = context.status.composed = context.status.stamps = context.status.hook_failures = UINT64_MAX;
  context.status.left_id = context.status.right_id = UINT64_MAX;
  context.settings.left_id = context.settings.right_id = UINT64_MAX;
  context.status.graphics_ready = context.status.scene_ready = context.status.candidate_count = UINT32_MAX;
  context.settings.profile = UINT32_MAX;
  context.status.speed = std::numeric_limits<float>::quiet_NaN();
  context.status.exposure = std::numeric_limits<float>::infinity();
  context.status.probe_cpu_ms = context.status.probe_max_ms = std::numeric_limits<double>::max();
  context.status.stage_ms.fill(-std::numeric_limits<double>::max());
  for (auto& mount : context.settings.mounts)
    mount.fill(-std::numeric_limits<double>::max());
  snapshot = bug_report_snapshot(context);
  assert(snapshot.size() <= BugReportSnapshotLimit);
  const auto bounded_url = bug_report_url(context);
  assert(bounded_url.size() <= BugReportUrlLimit);
  for (const auto c : bounded_url)
    assert(c >= 33 && c <= 126);
  const std::string bounded(bounded_url.begin(), bounded_url.end());
  const auto bounded_start = bounded.find(snapshot_marker) + snapshot_marker.size();
  assert(decode(std::string_view(bounded).substr(bounded_start)) == snapshot);
  std::printf("PASS bug reports: form fields, encoding, privacy, status freshness and bounds (URL %zu/%zu chars)\n", bounded_url.size(),
              BugReportUrlLimit);
}
