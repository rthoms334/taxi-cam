#pragma once
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include "../shared/protocol.hpp"

namespace taxi_camera::standalone {
inline constexpr std::string_view BugReportPage = "https://github.com/rthoms334/taxi-cam/issues/new?template=bug_report.yml";
inline constexpr size_t BugReportSnapshotLimit = 2200, BugReportUrlLimit = 7500;
inline constexpr std::uint64_t BugReportFreshMs = 3000;

struct BugReportContext {
  Settings settings;
  Status status;
  std::uint64_t now{};
  bool simulator_running{}, bridge_seen{}, ui_preview{}, unsaved_edits{};
};

// Encode bytes (including UTF-8) as a single query value; '+' is not a space.
inline std::string bug_report_query_value(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
      result += static_cast<char>(c);
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

inline std::string bug_report_snapshot(const BugReportContext& context) {
  const auto& s = context.settings;
  const auto& sample = context.status;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(6);
  out << "Companion snapshot; protocol=" << ProtocolVersion << "; simulator_running=" << context.simulator_running
      << "; UI_preview=" << context.ui_preview << '\n';
  out << "Bridge status: ";
  if (!sample.heartbeat)
    out << (context.bridge_seen ? "disconnected; previous status unavailable" : "not yet connected; status unavailable");
  else if (context.now < sample.heartbeat)
    out << "stale; heartbeat time invalid";
  else
    out << (context.now - sample.heartbeat <= BugReportFreshMs ? "connected" : "stale; last known values below")
        << "; age_ms=" << context.now - sample.heartbeat;
  out << "\nCurrent companion settings (unapplied text edits excluded); unsaved_edits=" << context.unsaved_edits << '\n';
  const auto profile = [&](std::uint32_t id) {
    const auto* selected = profiles::find(id);
    out << id << '/' << (selected ? selected->key : "unknown");
  };
  out << "Profile=";
  profile(s.profile);
  out << "; service=" << s.enabled << "; fps=" << s.camera_rate << '\n';
  out << "TAXI_follow=" << s.follow_taxi << "; preview_mask=" << s.manual_mask << "; calibration_mask=" << s.calibration_mask
      << "; scene_test=" << s.scene_test << "; first_camera_only=" << s.single_camera << '\n';
  out << "Auto_detect=" << s.auto_detect << "; requested_PFDs=" << s.left_id << '/' << s.right_id
      << "; calibration_budget=" << s.calibration_budget << '\n';
  out << "Exposure_EV=" << s.exposure << "; auto_exposure=" << s.automatic_exposure << "; night_boost=" << s.night_boost << '\n';
  for (size_t i = 0; i < s.mounts.size(); ++i) {
    out << (i ? "Tail" : "Nose") << " mount (right/up/forward m; pitch/yaw deg; lens rad):";
    for (const double value : s.mounts[i])
      out << ' ' << value;
    out << '\n';
  }
  if (sample.heartbeat) {
    out << "Bridge: graphics_ready=" << sample.graphics_ready << "; scene_ready=" << sample.scene_ready << '\n';
    out << "TAXI_mask=" << sample.taxi_mask << "; speed_inhibited=" << sample.speed_inhibited << "; speed_knots=" << sample.speed
        << "; applied_EV=" << sample.exposure << '\n';
    out << "PFDs=" << sample.left_id << '/' << sample.right_id << "; candidates=" << sample.candidate_count
        << "; captures=" << sample.captures << "; compositions=" << sample.composed << "; stamps=" << sample.stamps
        << "; hook_failures=" << sample.hook_failures << '\n';
    out << "Probe CPU ms=" << sample.probe_cpu_ms << "; max=" << sample.probe_max_ms << '\n';
    out << "Stage ms (manager/pool/lifecycle/entries/view1/view2/handoff/pose/activation/publish):";
    for (const double value : sample.stage_ms)
      out << ' ' << value;
    out << '\n';
  }
  // Do not read message or connection text: even
  // bounded strings from the bridge may contain private paths or account names.
  auto result = out.str();
  if (result.size() > BugReportSnapshotLimit) {
    constexpr std::string_view suffix = "\n[Snapshot truncated; attach logs.]";
    result.resize(BugReportSnapshotLimit - suffix.size());
    result += suffix;
  }
  return result;
}

inline std::wstring bug_report_url(const BugReportContext& context) {
  const auto version = std::string(TAXI_CAM_VERSION) + "-build." + std::to_string(TAXI_CAM_BUILD_NUMBER);
  const auto url = std::string(BugReportPage) + "&taxi_cam_version=" + bug_report_query_value(std::string_view(version).substr(0, 64)) +
                   "&runtime_snapshot=" + bug_report_query_value(bug_report_snapshot(context));
  // Reserve space for the template and field names as well as worst-case query
  // encoding. Never hand the shell a partially encoded or truncated URL.
  static_assert(3 * (BugReportSnapshotLimit + 64) + BugReportPage.size() + 64 <= BugReportUrlLimit);
  return std::wstring(url.begin(), url.end());
}
}  // namespace taxi_camera::standalone
