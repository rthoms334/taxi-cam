#pragma once
#include <windows.h>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "../profiles/aircraft_variant.hpp"
#include "../shared/camera_rate_policy.hpp"
#include "../shared/protocol.hpp"
#include "report_bundle.hpp"
#include "system_details.hpp"

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
  out << "; service=" << s.enabled << "; camera_mode=" << s.camera_mode << "; fps=" << s.camera_rate << '\n';
  out << "TAXI_follow=" << s.follow_taxi << "; preview_mask=" << s.manual_mask << "; calibration_mask=" << s.calibration_mask
      << "; scene_test=" << s.scene_test << '\n';
  out << "Auto_detect=" << s.auto_detect << "; requested_PFDs=" << s.left_id << '/' << s.right_id << '/' << s.lower_id
      << "; calibration_budget=" << s.calibration_budget << '\n';
  out << "Brightness_EV day/night=" << s.day_brightness << '/' << s.night_brightness << '\n';
  // Settings::mounts: nose, tail (or bottom left), bottom right, door.
  constexpr const char* mount_names[]{"Nose", "Tail", "Bottom-right", "Door"};
  for (size_t i = 0; i < s.mounts.size(); ++i) {
    out << (i < std::size(mount_names) ? mount_names[i] : "Other") << " mount (right/up/forward m; pitch/yaw deg; lens rad):";
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
    // Counts only: panel names are in bridge.log.
    std::uint32_t tracked = 0;
    for (std::uint32_t i = 0; i < sample.panel_count && i < MaxPanelTextures; ++i)
      tracked += sample.panels[i].id != 0;
    out << "Panels: discovery=" << sample.panel_discovery << "; state=" << sample.panel_state << "; clean=" << sample.panel_clean
        << "; shift=" << sample.panel_shift << "; paired=" << sample.panel_count << "; tracked=" << tracked << '\n';
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

// The aircraft as the bridge identified it: the matched profile, the
// SimObject path (a package path, never a disk path) and the ATC type.
inline std::string bug_report_aircraft(const BugReportContext& context) {
  const auto& sample = context.status;
  const auto id = sample.heartbeat && sample.active_profile ? sample.active_profile : context.settings.profile;
  std::string out;
  if (const auto* profile = profiles::find(id))
    out = report_text(std::wstring_view(profile->name), 80) + " profile";
  const std::string_view path(sample.aircraft_path, strnlen(sample.aircraft_path, sizeof(sample.aircraft_path)));
  profiles::VariantKey key{};
  if (profiles::variant_key(path, key))
    out += (out.empty() ? "" : "; ") + std::string("SimObject ") + report_text(std::string_view(key.data()), 160);
  const auto type = report_text(std::string_view(sample.aircraft_type, strnlen(sample.aircraft_type, sizeof(sample.aircraft_type))), 60);
  if (!type.empty())
    out += (out.empty() ? "" : "; ") + std::string("type ") + type;
  if (out.empty())
    out = "No aircraft identified";
  return out + "; add-on version: ";
}

// Local wall-clock time with its UTC offset, and the tick count bridge.log
// lines start with, so a reported time can be found in the log.
inline std::string bug_report_created(std::uint64_t tick) {
  SYSTEMTIME utc{}, local{};
  GetSystemTime(&utc);
  if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
    local = utc;
  FILETIME a{}, b{};
  SystemTimeToFileTime(&utc, &a);
  SystemTimeToFileTime(&local, &b);
  const auto ticks = [](const FILETIME& t) {
    return static_cast<long long>(static_cast<std::uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime);
  };
  const long long minutes = (ticks(b) - ticks(a)) / 600000000, offset = minutes < 0 ? -minutes : minutes;
  char text[128];
  std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u local, UTC%c%02lld:%02lld; bridge.log tick %llu", local.wYear,
                local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond, minutes < 0 ? '-' : '+', offset / 60, offset % 60,
                static_cast<unsigned long long>(tick));
  return text;
}

inline std::string bug_report_environment(const SystemDetails& d, std::string_view created) {
  return "Windows: " + d.windows + "\nCPU: " + d.cpu + "\nRAM: " + d.memory + "\nGPU: " + d.gpus + "\nMSFS graphics: " + d.graphics +
         "\nGraphics hooks in MSFS: " + d.hooks + "\nOther tools running: " + d.tools + "\nReport created: " + std::string(created);
}

// Text for the form's other fields; English like the rest of the report.
struct BugReportForm {
  std::string simulator, aircraft, environment, logs;
};

// The snapshot's leading whole lines whose query encoding fits `budget`. The
// report file keeps the full snapshot.
inline std::string bug_report_fit(const std::string& snapshot, std::size_t budget) {
  if (bug_report_query_value(snapshot).size() <= budget)
    return snapshot;
  constexpr std::string_view suffix = "[Snapshot shortened; the full snapshot is in report-info.txt.]";
  const auto suffix_bytes = bug_report_query_value(suffix).size();
  if (suffix_bytes > budget)
    return {};
  std::size_t kept = 0, used = 0;
  for (std::size_t begin = 0; begin < snapshot.size();) {
    auto end = snapshot.find('\n', begin);
    end = end == std::string::npos ? snapshot.size() : end + 1;
    const auto bytes = bug_report_query_value(std::string_view(snapshot).substr(begin, end - begin)).size();
    if (used + bytes + suffix_bytes > budget)
      break;
    used += bytes;
    kept = begin = end;
  }
  return snapshot.substr(0, kept) + std::string(suffix);
}

inline std::wstring bug_report_url(const BugReportContext& context, const BugReportForm& form = {}) {
  const auto version = std::string(TAXI_CAM_VERSION) + "-build." + std::to_string(TAXI_CAM_BUILD_NUMBER);
  auto url = std::string(BugReportPage) + "&taxi_cam_version=" + bug_report_query_value(std::string_view(version).substr(0, 64));
  const struct {
    std::string_view id;
    const std::string& value;
    std::size_t limit;
    bool lines;
  } fields[]{{"simulator_version", form.simulator, 120, false},
             {"aircraft", form.aircraft, 240, false},
             {"environment", form.environment, 1000, true},
             {"logs", form.logs, 300, true}};
  for (const auto& field : fields)
    if (!field.value.empty())
      url += "&" + std::string(field.id) + "=" + bug_report_query_value(report_text(field.value, field.limit, field.lines));
  // The snapshot goes last and gets what is left, so the URL never exceeds the
  // limit and is never cut inside an escape. Worst-case encoding of the other
  // fields still leaves room for most of a snapshot.
  constexpr std::string_view snapshot_field = "&runtime_snapshot=";
  static_assert(BugReportPage.size() + 3 * (64 + 120 + 240 + 1000 + 300) + 128 < BugReportUrlLimit - 1500);
  const auto budget = BugReportUrlLimit - url.size() - snapshot_field.size();
  url += std::string(snapshot_field) + bug_report_query_value(bug_report_fit(bug_report_snapshot(context), budget));
  return std::wstring(url.begin(), url.end());
}

// The ZIP the Report a bug button saves for the user to attach.
inline constexpr std::uint64_t BugReportFileLimit = 16ull << 20;  // Tail kept of each file.
inline constexpr unsigned BugReportKeep = 5, BugReportCrashRecords = 2, BugReportCrashDays = 14;

struct ReportBundle {
  std::wstring path;  // Empty when nothing was written.
  std::string name, error;
};

inline std::string report_utf8(std::wstring_view text) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// The Windows profile folder, account name and computer name in shared files.
inline std::vector<Redaction> report_redactions() {
  const auto variable = [](const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(name, value.data(), 32768);
    return n && n < 32768 ? report_utf8(std::wstring_view(value.data(), n)) : std::string{};
  };
  std::vector<Redaction> rules{{variable(L"USERPROFILE"), "%USERPROFILE%", false}};
  // A very short name would also match ordinary words in the logs.
  if (auto user = variable(L"USERNAME"); user.size() >= 3)
    rules.push_back({std::move(user), "<user>", true});
  if (auto computer = variable(L"COMPUTERNAME"); computer.size() >= 3)
    rules.push_back({std::move(computer), "<computer>", true});
  return rules;
}

namespace report_detail {
struct Found {
  std::wstring path, name;
  FILETIME written{};
};
// Files matching `pattern`, newest first.
inline std::vector<Found> files(const std::wstring& folder, const wchar_t* pattern) {
  std::vector<Found> out;
  WIN32_FIND_DATAW entry{};
  const auto search = FindFirstFileW((folder + L"\\" + pattern).c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE)
    return out;
  do
    if (!(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
      out.push_back({folder + L"\\" + entry.cFileName, entry.cFileName, entry.ftLastWriteTime});
  while (out.size() < 1000 && FindNextFileW(search, &entry));
  FindClose(search);
  std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return CompareFileTime(&a.written, &b.written) > 0; });
  return out;
}
// Saved aircraft settings: the *.ini files below profiles.
inline void profile_files(const std::wstring& folder,
                          const std::string& name,
                          unsigned depth,
                          std::vector<std::pair<std::wstring, std::string>>& out) {
  for (const auto& file : files(folder, L"*.ini"))
    if (out.size() < 200)
      out.push_back({file.path, name + "/" + report_utf8(file.name)});
  if (depth >= 8)
    return;
  WIN32_FIND_DATAW entry{};
  const auto search = FindFirstFileW((folder + L"\\*").c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE)
    return;
  do
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        std::wcscmp(entry.cFileName, L".") && std::wcscmp(entry.cFileName, L".."))
      profile_files(folder + L"\\" + entry.cFileName, name + "/" + report_utf8(entry.cFileName), depth + 1, out);
  while (FindNextFileW(search, &entry));
  FindClose(search);
}
inline std::uint64_t file_time(const FILETIME& t) {
  return static_cast<std::uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime;
}
}  // namespace report_detail

// Writes <data>\reports\taxi-cam-report-<local time>.zip: `info`, the logs,
// saved settings, MSFS graphics options and recent renderer-fault records,
// with the account details above replaced in every text file. Keeps the newest
// few reports.
inline ReportBundle write_report_bundle(const std::wstring& data, const SystemDetails& details, const std::string& info) {
  using namespace report_detail;
  ReportBundle result;
  if (data.empty()) {
    result.error = "no settings folder";
    return result;
  }
  const auto rules = report_redactions();
  std::vector<ZipEntry> entries;
  std::string included, missing;
  const auto add = [&](const std::wstring& path, const std::string& name) {
    bool cut = false;
    auto content = read_file_tail(path, BugReportFileLimit, &cut);
    if (content.empty()) {
      missing += "  " + name + "\n";
      return;
    }
    included += "  " + name + (cut ? " (last 16 MiB)" : "") + "\n";
    entries.push_back({name, redact(std::move(content), rules)});
  };
  add(data + L"\\bridge.log", "logs/bridge.log");
  add(data + L"\\bridge.log.1", "logs/bridge.log.1");
  add(data + L"\\launcher.log", "logs/launcher.log");
  add(data + L"\\app\\setup-diagnostics.log", "logs/setup-diagnostics.log");
  add(data + L"\\app\\installation.json", "settings/installation.json");
  for (const auto* name : {L"settings.ini", L"hotkeys.ini", L"buttons.ini"})
    add(data + L"\\" + name, "settings/" + report_utf8(name));
  std::vector<std::pair<std::wstring, std::string>> saved_profiles;
  profile_files(data + L"\\profiles", "settings/profiles", 0, saved_profiles);
  for (const auto& [path, name] : saved_profiles)
    add(path, name);
  if (!details.user_config.empty())
    add(details.user_config, "msfs/UserCfg.opt");
  // Only records of a fault that happened: Record::state 2 at offset 16. They
  // hold registers and engine bytes, no text, so they are copied unchanged.
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const std::uint64_t age_limit = std::uint64_t{BugReportCrashDays} * 24 * 3600 * 10000000;
  unsigned crashes = 0;
  for (const auto& file : files(data, L"renderer-fault-*.bin")) {
    if (crashes >= BugReportCrashRecords || file_time(now) - file_time(file.written) > age_limit)
      break;
    auto record = read_file_tail(file.path, 64 << 10);
    std::uint32_t magic{}, state{};
    if (record.size() < 20)
      continue;
    std::memcpy(&magic, record.data(), 4);
    std::memcpy(&state, record.data() + 16, 4);
    if (magic != 0x54434352 || state != 2)
      continue;
    included += "  crash/" + report_utf8(file.name) + "\n";
    entries.push_back({"crash/" + report_utf8(file.name), std::move(record)});
    ++crashes;
  }
  auto text = info + "\n\nFiles in this report:\n" + included + (missing.empty() ? "" : "Not found:\n" + missing) +
              "\nThe Windows profile folder, user name and computer name are replaced with %USERPROFILE%, <user> and <computer>.\n";
  entries.insert(entries.begin(), {"report-info.txt", redact(std::move(text), rules)});

  SYSTEMTIME local{};
  GetLocalTime(&local);
  char name[64];
  std::snprintf(name, sizeof(name), "taxi-cam-report-%04u%02u%02u-%02u%02u%02u.zip", local.wYear, local.wMonth, local.wDay, local.wHour,
                local.wMinute, local.wSecond);
  const auto archive =
      zip_archive(entries, zip_dos_time(local.wHour, local.wMinute, local.wSecond), zip_dos_date(local.wYear, local.wMonth, local.wDay));
  if (archive.empty()) {
    result.error = "report too large";
    return result;
  }
  const auto folder = data + L"\\reports";
  CreateDirectoryW(folder.c_str(), nullptr);
  const auto path = folder + L"\\" + std::wstring(name, name + std::strlen(name));
  const auto partial = path + L".partial";
  const HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    result.error = "could not create the file (error " + std::to_string(GetLastError()) + ")";
    return result;
  }
  DWORD written = 0;
  const bool ok = WriteFile(file, archive.data(), static_cast<DWORD>(archive.size()), &written, nullptr) && written == archive.size();
  CloseHandle(file);
  if (!ok || !MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    result.error = "could not write the file (error " + std::to_string(GetLastError()) + ")";
    DeleteFileW(partial.c_str());
    return result;
  }
  result.path = path;
  result.name = name;
  const auto saved = files(folder, L"taxi-cam-report-*.zip");
  for (std::size_t i = BugReportKeep; i < saved.size(); ++i)
    DeleteFileW(saved[i].path.c_str());
  return result;
}

// What the bug report button shares: the form link and the saved file's text.
inline std::string bug_report_info(const BugReportForm& form, const std::string& snapshot) {
  return "Taxi Cam bug report\nTaxi Cam: " + std::string(TAXI_CAM_VERSION) + "-build." + std::to_string(TAXI_CAM_BUILD_NUMBER) +
         "\nSimulator: " + form.simulator + "\nAircraft: " + form.aircraft + "\n" + form.environment + "\n\n" + snapshot;
}
}  // namespace taxi_camera::standalone
