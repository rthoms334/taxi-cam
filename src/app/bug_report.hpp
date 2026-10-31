#pragma once
#include <cwchar>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include "../shared/camera_rate_policy.hpp"
#include "../shared/protocol.hpp"
#include "launcher.hpp"

namespace taxi_camera::standalone {
inline constexpr std::string_view BugReportPage = "https://github.com/rthoms334/taxi-cam/issues/new?template=bug_report.yml";
inline constexpr size_t BugReportSnapshotLimit = 2200, BugReportUrlLimit = 7500;
inline constexpr std::uint64_t BugReportFreshMs = 3000;

// Why the bridge has not reported: the companion-side facts that separate
// "never attached", "attached but the load failed" and "loaded but silent".
// Fixed vocabulary and numbers only; no paths, names or message text.
struct BridgeLinkReport {
  bool auto_connect{}, connection_requested{}, connection_disconnected{}, mailbox_open{};
  bool load_attempted{}, load_ok{};
  unsigned long load_error{};
  const char* load_phase = "none";   // Static string from this file's vocabulary.
  const char* install = "unknown";   // store, steam, other or unknown.
};
// The install family of the attached simulator image, from its path alone.
inline const char* simulator_install_kind(const std::wstring& path) {
  if (path.empty())
    return "unknown";
  const auto normalized = normalize_path_separators(path);
  if (contains_ci(normalized, L"\\steamapps\\"))
    return "steam";
  if (contains_ci(normalized, L"\\XboxGames\\") || contains_ci(normalized, L"\\WindowsApps\\"))
    return "store";
  return "other";
}
// Launch phases are wide literals in the launcher; report only those names so
// arbitrary text never reaches the report.
inline const char* load_phase_name(const wchar_t* phase) {
  struct Phase {
    const wchar_t* wide;
    const char* narrow;
  };
  static constexpr Phase phases[] = {{L"preflight", "preflight"},
                                     {L"windows_loader_create", "windows_loader_create"},
                                     {L"windows_loader", "windows_loader"},
                                     {L"after_load_module_scan", "after_load_module_scan"},
                                     {L"bridge_export", "bridge_export"},
                                     {L"bridge_start_create", "bridge_start_create"},
                                     {L"bridge_start", "bridge_start"},
                                     {L"complete", "complete"}};
  if (!phase)
    return "none";
  for (const auto& known : phases)
    if (std::wcscmp(phase, known.wide) == 0)
      return known.narrow;
  return "other";
}
struct BugReportContext {
  Settings settings;
  Status status;
  std::uint64_t now{};
  bool simulator_running{}, bridge_seen{}, ui_preview{}, unsaved_edits{};
  BridgeLinkReport link;
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
  out << "\nBridge link: sim_attached=" << context.simulator_running << "; install=" << context.link.install
      << "; auto_connect=" << context.link.auto_connect << "; connect_requested=" << context.link.connection_requested
      << "; disconnected_by_user=" << context.link.connection_disconnected << "; mailbox_open=" << context.link.mailbox_open
      << "; load_attempted=" << context.link.load_attempted << "; load_ok=" << context.link.load_ok
      << "; load_error=" << context.link.load_error << "; load_phase=" << context.link.load_phase
      << "\nCurrent companion settings (unapplied text edits excluded); unsaved_edits=" << context.unsaved_edits << '\n';
  const auto profile = [&](std::uint32_t id) {
    const auto* selected = profiles::find(id);
    out << id << '/' << (selected ? selected->key : "unknown");
  };
  out << "Profile=";
  profile(s.profile);
  out << "; service=" << s.enabled << "; camera_mode=" << s.camera_mode << "; fps=" << s.camera_rate << '\n';
  out << "TAXI_follow=" << s.follow_taxi << "; preview_mask=" << s.manual_mask << "; calibration_mask=" << s.calibration_mask
      << "; scene_test=" << s.scene_test << "; first_camera_only=" << s.single_camera << '\n';
  out << "Auto_detect=" << s.auto_detect << "; requested_PFDs=" << s.left_id << '/' << s.right_id << '/' << s.lower_id
      << "; calibration_budget=" << s.calibration_budget << '\n';
  out << "Brightness_EV day/night=" << s.day_brightness << '/' << s.night_brightness << '\n';
  for (size_t i = 0; i < s.mounts.size(); ++i) {
    out << (i ? "Tail" : "Nose") << " mount (right/up/forward m; pitch/yaw deg; lens rad):";
    for (const double value : s.mounts[i])
      out << ' ' << value;
    out << '\n';
  }
  if (sample.heartbeat) {
    out << "Bridge: graphics_ready=" << sample.graphics_ready << "; scene_ready=" << sample.scene_ready << '\n';
    out << "TAXI_mask=" << sample.taxi_mask << "; speed_knots=" << sample.speed << "; applied_EV=" << sample.exposure << '\n';
    out << "Rate_in_use=" << sample.effective_rate << "; useful_rate=" << sample.useful_rate << "; rate_limits=" << sample.rate_limits
        << "; update_hz=" << sample.update_hz << "; reachable_rate=" << sample.reachable_rate << "; auto_level=" << sample.auto_level
        << "; auto_floor=" << sample.auto_floor << '\n';
    out << "PFDs=" << sample.left_id << '/' << sample.right_id << '/' << sample.lower_id << "; candidates=" << sample.candidate_count
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
