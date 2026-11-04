#include "../../src/app/bug_report.hpp"
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <random>

using namespace taxi_camera::standalone;

// An independent fixed-Huffman inflater (RFC 1951 3.2.6) for the ZIP checks.
struct BitReader {
  std::string_view in;
  std::size_t pos{};
  unsigned bit{};
  unsigned get(unsigned n) {
    unsigned value = 0;
    for (unsigned i = 0; i < n; ++i) {
      assert(pos < in.size());
      value |= ((static_cast<unsigned char>(in[pos]) >> bit) & 1u) << i;
      if (++bit == 8) {
        bit = 0;
        ++pos;
      }
    }
    return value;
  }
};
static unsigned fixed_symbol(BitReader& r) {
  unsigned code = 0;
  for (unsigned length = 1; length <= 9; ++length) {
    code = code << 1 | r.get(1);
    if (length == 7 && code <= 0x17)
      return 256 + code;
    if (length == 8 && code >= 0x30 && code <= 0xBF)
      return code - 0x30;
    if (length == 8 && code >= 0xC0 && code <= 0xC7)
      return 280 + code - 0xC0;
    if (length == 9 && code >= 0x190)
      return 144 + code - 0x190;
  }
  assert(false);
  return 0;
}
static std::string inflate(std::string_view in) {
  static constexpr unsigned length_base[]{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr unsigned length_extra[]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr unsigned distance_base[]{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  BitReader r{in};
  std::string out;
  for (bool last = false; !last;) {
    last = r.get(1);
    assert(r.get(2) == 1);
    for (;;) {
      const auto symbol = fixed_symbol(r);
      if (symbol < 256) {
        out += static_cast<char>(symbol);
        continue;
      }
      if (symbol == 256)
        break;
      assert(symbol <= 285);
      const auto length = length_base[symbol - 257] + r.get(length_extra[symbol - 257]);
      unsigned code = 0;
      for (int i = 0; i < 5; ++i)
        code = code << 1 | r.get(1);
      assert(code < 30);
      const auto distance = distance_base[code] + r.get(code < 4 ? 0 : code / 2 - 1);
      assert(distance >= 1 && distance <= 32768 && distance <= out.size());
      for (unsigned k = 0; k < length; ++k)
        out += out[out.size() - distance];
    }
  }
  assert(r.pos + (r.bit ? 1 : 0) == in.size());
  return out;
}
static std::uint32_t read32(std::string_view s, std::size_t at) {
  assert(at + 4 <= s.size());
  return static_cast<unsigned char>(s[at]) | static_cast<unsigned char>(s[at + 1]) << 8 | static_cast<unsigned char>(s[at + 2]) << 16 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24;
}
static unsigned read16(std::string_view s, std::size_t at) {
  assert(at + 2 <= s.size());
  return static_cast<unsigned char>(s[at]) | static_cast<unsigned char>(s[at + 1]) << 8;
}
// Name -> content, in central-directory order, checking every header field.
static std::vector<std::pair<std::string, std::string>> unzip(std::string_view zip) {
  assert(zip.size() >= 22);
  const auto end = zip.size() - 22;
  assert(read32(zip, end) == 0x06054B50 && read16(zip, end + 8) == read16(zip, end + 10));
  const unsigned count = read16(zip, end + 10);
  const auto directory_size = read32(zip, end + 12), directory = read32(zip, end + 16);
  assert(directory + directory_size == end);
  std::vector<std::pair<std::string, std::string>> out;
  std::size_t at = directory;
  for (unsigned i = 0; i < count; ++i) {
    assert(read32(zip, at) == 0x02014B50);
    const auto method = read16(zip, at + 10), crc = read32(zip, at + 16), packed = read32(zip, at + 20), size = read32(zip, at + 24);
    const auto name_size = read16(zip, at + 28), local = read32(zip, at + 42);
    assert(read16(zip, at + 30) == 0 && read16(zip, at + 32) == 0);
    const std::string name(zip.substr(at + 46, name_size));
    assert(read32(zip, local) == 0x04034B50 && read16(zip, local + 8) == method && read32(zip, local + 14) == crc &&
           read32(zip, local + 18) == packed && read32(zip, local + 22) == size && zip.substr(local + 30, name_size) == name);
    const auto body = zip.substr(local + 30 + name_size, packed);
    assert(method == 0 || method == 8);
    auto data = method == 8 ? inflate(body) : std::string(body);
    assert(data.size() == size && zip_crc32(data) == crc);
    out.push_back({name, std::move(data)});
    at += 46 + name_size;
  }
  assert(at == end);
  return out;
}
static void write_file(const std::wstring& path, std::string_view data) {
  const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  assert(file != INVALID_HANDLE_VALUE);
  DWORD written{};
  assert(WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size());
  CloseHandle(file);
}
static void remove_tree(const std::wstring& folder) {
  WIN32_FIND_DATAW entry{};
  const auto search = FindFirstFileW((folder + L"\\*").c_str(), &entry);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring path = folder + L"\\" + entry.cFileName;
      if (!std::wcscmp(entry.cFileName, L".") || !std::wcscmp(entry.cFileName, L".."))
        continue;
      if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        remove_tree(path);
      else
        DeleteFileW(path.c_str());
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  RemoveDirectoryW(folder.c_str());
}

static void zip_and_redaction() {
  assert(zip_crc32("123456789") == 0xCBF43926u && zip_crc32("") == 0);
  std::mt19937 random(7);
  std::string noise(100000, '\0');
  for (auto& c : noise)
    c = static_cast<char>(random());
  std::string log;
  for (int i = 0; i < 4000; ++i)
    log += "130109687 pid=38616 tid=10748 native=1 scene=1 mask=0 left=252 | Ready. | captured=" + std::to_string(i * 7) + "\n";
  // A repeat at the window's full distance, one just beyond it, and a run past
  // the longest match.
  std::string distant = noise.substr(0, 32768) + noise.substr(0, 300) + noise.substr(299, 600) + std::string(1000, 'x') + "ab";
  for (const std::string& input :
       {std::string(), std::string("a"), std::string("abcabcabcabc"), std::string(259, 'z'), noise, log, distant})
    assert(inflate(deflate_fixed(input)) == input);
  assert(deflate_fixed(log).size() * 8 < log.size());

  const auto zip = zip_archive({{"report-info.txt", "hello"}, {"logs/bridge.log", log}, {"crash/noise.bin", noise}, {"empty.txt", ""}},
                               zip_dos_time(23, 41, 5), zip_dos_date(2026, 11, 2));
  const auto files = unzip(zip);
  assert(files.size() == 4 && files[0] == std::make_pair(std::string("report-info.txt"), std::string("hello")));
  assert(files[1].second == log && files[2].second == noise && files[3].second.empty());
  assert(read16(zip, 10) == zip_dos_time(23, 41, 5) && read16(zip, 12) == (46 << 9 | 11 << 5 | 2));
  assert(zip.size() < log.size() / 4 + noise.size() + 1024);  // Noise is stored, the log deflated.

  const std::vector<Redaction> rules{{"C:\\Users\\Rob", "%USERPROFILE%", false}, {"Rob", "<user>", true}, {"", "never", false}};
  assert(redact("Path c:\\users\\rob\\AppData and C:/Users/ROB/x, Robert, rob_1, rob.", rules) ==
         "Path %USERPROFILE%\\AppData and C:/Users/<user>/x, Robert, rob_1, <user>.");
  assert(redact("rob", rules) == "<user>" && redact("", rules).empty() && redact("Ro", rules) == "Ro");
  assert(redact("\"C:\\\\Users\\\\rob\\\\Taxi\"", rules) == "\"C:\\\\Users\\\\<user>\\\\Taxi\"");
}

static void system_detail_text() {
  assert(report_text(std::string_view("caf\xc3\xa9\tA\r\nB\x01"), 64) == "caf?? A B?");
  assert(report_text(std::string_view("one\r\ntwo\n\n"), 64, true) == "one\ntwo");
  assert(report_text(std::string_view("abcdef"), 3) == "abc");
  assert(report_text(std::wstring_view(L"GeForce \u00ae"), 64) == "GeForce ?");
  assert(driver_version(0x10DE, 32ull << 48 | 0ull << 32 | 15ull << 16 | 6094) == "560.94 (32.0.15.6094)");
  assert(driver_version(0x10DE, 31ull << 48 | 0ull << 32 | 15ull << 16 | 5222) == "552.22 (31.0.15.5222)");
  assert(driver_version(0x1002, 32ull << 48 | 0ull << 32 | 12011ull << 16 | 1036) == "32.0.12011.1036");
  assert(simulator_store(L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\FlightSimulator2024.exe") ==
         "Microsoft Store/Xbox app");
  assert(simulator_store(L"D:\\SteamLibrary\\steamapps\\common\\MSFS2024\\FlightSimulator2024.exe") == "Steam");
  assert(simulator_store(L"") == "store unknown");
  const std::string_view config =
      "Version 66\r\n{Video\r\n\tAdapter \"GPU\"\r\n\tWindowed 1\r\n\tResolution 2558 1360\r\n"
      "\tAntiAliasing DLSS\r\n\tDLSSMode QUALITY\r\n\tFrameGeneration DLSSG\r\n\tPrimaryScaling 1.000000\r\n"
      "\tTargetFrameRate 30\r\n\tFrameGenerationVR NONE\r\n}\r\n{Graphics\r\n\tVersion 2.1.0\r\n\tPreset Custom\r\n"
      "\t{Texture\r\n\t\tQuality 0\r\n\t\tPreset Low\r\n\t}\r\n}\r\n{GraphicsVR\r\n\tPreset High\r\n}\r\n"
      "InstalledPackagesPath \"C:\\Users\\private\"\r\n";
  assert(user_config_summary(config) ==
         "Windowed 1; Resolution 2558 1360; AntiAliasing DLSS; DLSSMode QUALITY; FrameGeneration DLSSG; PrimaryScaling 1.000000; "
         "TargetFrameRate 30; Preset Custom");
  assert(user_config_summary("").empty() && user_config_summary("{Video\nWindowed").empty());
}

// Sets a file's last-write time to now plus `offset` (negative: earlier).
static void write_time(const std::wstring& path, long long offset_seconds) {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER t{};
  t.LowPart = now.dwLowDateTime;
  t.HighPart = now.dwHighDateTime;
  t.QuadPart += static_cast<std::uint64_t>(offset_seconds * 10000000);
  const FILETIME written{t.LowPart, t.HighPart};
  const auto file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  assert(file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &written));
  CloseHandle(file);
}
static bool exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static std::map<std::string, std::string> bundle_files(const ReportBundle& bundle) {
  const auto zip = read_file_tail(bundle.path, 1 << 20);
  const auto entries = unzip(zip);
  assert(!entries.empty() && entries.front().first == "report-info.txt");
  return {entries.begin(), entries.end()};
}

static void report_file() {
  wchar_t temp[MAX_PATH]{};
  assert(GetTempPathW(MAX_PATH, temp));
  const std::wstring data = std::wstring(temp) + L"taxi-cam-bug-report-test-" + std::to_wstring(GetCurrentProcessId());
  const auto install = data + L"\\install", reports = data + L"\\reports";
  remove_tree(data);
  assert(CreateDirectoryW(data.c_str(), nullptr));
  for (const auto* folder : {L"\\profiles", L"\\profiles\\discovered", L"\\install", L"\\reports"})
    CreateDirectoryW((data + folder).c_str(), nullptr);
  // An account name too short to replace on its own, renamed away from its
  // profile folder's name.
  SetEnvironmentVariableW(L"USERPROFILE", L"C:\\Users\\private-folder");
  SetEnvironmentVariableW(L"USERNAME", L"Al");
  SetEnvironmentVariableW(L"COMPUTERNAME", L"PRIVATE-PC");
  write_file(
      data + L"\\bridge.log",
      "100 | From C:\\Users\\private-folder\\AppData on private-pc; C:/Users/Private-Folder/x; D:\\Backup\\private-folder\\y; Al.\n");
  const std::string older(5000, 'o');
  write_file(data + L"\\bridge.log.1", older + "end");
  write_file(install + L"\\installation.json", "{\"destination\": \"C:\\\\Users\\\\private-folder\\\\AppData\\\\Local\"}");
  write_file(install + L"\\setup-diagnostics.log", "Setup ok\n");
  write_file(data + L"\\settings.ini", "[companion]\nlanguage=de\n");
  write_file(data + L"\\profiles\\discovered\\variant.ini", "[layout]\nsides=2\n");
  write_file(data + L"\\profiles\\notes.txt", "not settings");

  // Faulted records (state 2) newest first: one dated after now (clock set
  // back), two within 14 days, one older; plus a record that never faulted.
  std::string record(8192, '\0'), faulted;
  const std::uint32_t magic = 0x54434352, complete = 2;
  std::memcpy(record.data(), &magic, 4);
  faulted = record;
  std::memcpy(faulted.data() + 16, &complete, 4);
  const struct {
    const wchar_t* name;
    bool fault;
    long long offset;
  } records[]{{L"9-9", true, 86400}, {L"1-1", false, -1800}, {L"2-2", true, -3600}, {L"3-3", true, -7200}, {L"4-4", true, -20 * 86400}};
  for (const auto& r : records) {
    const auto path = data + L"\\renderer-fault-" + r.name + L".bin";
    write_file(path, r.fault ? faulted : record);
    write_time(path, r.offset);
  }
  // Five reports dated after now and one older: rotation keeps the five
  // newest by write time and never the report it just wrote.
  for (int i = 1; i <= 5; ++i) {
    const auto path = reports + L"\\taxi-cam-report-29990101-00000" + std::to_wstring(i) + L".zip";
    write_file(path, "old");
    write_time(path, i * 3600);
  }
  write_file(reports + L"\\taxi-cam-report-20000101-000000.zip", "old");
  write_time(reports + L"\\taxi-cam-report-20000101-000000.zip", -86400);

  SystemDetails details;
  details.user_config = data + L"\\missing-UserCfg.opt";
  auto bundle = write_report_bundle(data, install, details, "Report for private-folder", 4096);
  assert(bundle.error.empty() && bundle.name.starts_with("taxi-cam-report-") && bundle.name.ends_with(".zip"));
  assert(bundle.path == reports + L"\\" + std::wstring(bundle.name.begin(), bundle.name.end()) && exists(bundle.path));
  for (int i = 1; i <= 5; ++i)
    assert(exists(reports + L"\\taxi-cam-report-29990101-00000" + std::to_wstring(i) + L".zip"));
  assert(!exists(reports + L"\\taxi-cam-report-20000101-000000.zip"));

  auto files = bundle_files(bundle);
  assert(files.at("logs/bridge.log") == "100 | From %USERPROFILE%\\AppData on <computer>; %USERPROFILE%/x; D:\\Backup\\<user>\\y; Al.\n");
  assert(files.at("logs/bridge.log.1") == (older + "end").substr(5003 - 4096));
  assert(files.at("settings/installation.json") == "{\"destination\": \"%USERPROFILE%\\\\AppData\\\\Local\"}");
  assert(files.at("logs/setup-diagnostics.log") == "Setup ok\n");
  assert(files.at("settings/settings.ini") == "[companion]\nlanguage=de\n");
  assert(files.at("settings/profiles/discovered/variant.ini") == "[layout]\nsides=2\n");
  assert(files.at("crash/renderer-fault-9-9.bin") == faulted && files.at("crash/renderer-fault-2-2.bin") == faulted);
  for (const auto* absent :
       {"crash/renderer-fault-1-1.bin", "crash/renderer-fault-3-3.bin", "crash/renderer-fault-4-4.bin", "settings/profiles/notes.txt"})
    assert(!files.contains(absent));
  auto info = files.at("report-info.txt");
  assert(info.starts_with("Report for <user>\n"));
  assert(info.find("  logs/bridge.log\n") != std::string::npos &&
         info.find("  logs/bridge.log.1 (last 4096 bytes)\n") != std::string::npos);
  assert(info.find("Not found:\n") != std::string::npos && info.find("  msfs/UserCfg.opt\n") != std::string::npos);
  for (const auto& [name, content] : files) {
    std::string lower = content;
    for (auto& c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    assert(lower.find("private") == std::string::npos);
  }

  // With room for two records, the one older than 14 days still stays out.
  DeleteFileW((data + L"\\renderer-fault-9-9.bin").c_str());
  DeleteFileW((data + L"\\renderer-fault-2-2.bin").c_str());
  bundle = write_report_bundle(data, install, details, "Again");
  files = bundle_files(bundle);
  assert(files.contains("crash/renderer-fault-3-3.bin") && !files.contains("crash/renderer-fault-4-4.bin"));
  assert(files.at("logs/bridge.log.1") == older + "end" && files.at("report-info.txt").find("(last") == std::string::npos);
  assert(exists(bundle.path));
  remove_tree(data);

  ReportBundle unsaved = write_report_bundle({}, {}, details, "");
  assert(unsaved.path.empty() && !unsaved.error.empty());
}

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
  zip_and_redaction();
  system_detail_text();
  report_file();
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
  context.settings.day_brightness = -1.5f;
  context.settings.night_brightness = 0.25f;
  auto snapshot = bug_report_snapshot(context);
  assert(snapshot.find("not yet connected; status unavailable") != std::string::npos);
  assert(snapshot.find("captures=") == std::string::npos);
  assert(snapshot.find("unapplied text edits excluded); unsaved_edits=1") != std::string::npos);
  assert(snapshot.find("Profile=1/fbw-a380x") != std::string::npos);
  assert(snapshot.find("fps=30") != std::string::npos && snapshot.find("preview_mask=1") != std::string::npos);
  assert(snapshot.find("Brightness_EV day/night=-1.5/0.25") != std::string::npos);
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
  assert(snapshot.find("Panels: discovery=0; state=0; marked=0; tracked=0") != std::string::npos);
  context.status.panel_discovery = 1;
  context.status.panel_state = static_cast<std::uint32_t>(PanelMapState::marked);
  context.status.panel_count = 3;
  context.status.panels[0].id = 41;
  context.status.panels[2].id = 43;
  snapshot = bug_report_snapshot(context);
  assert(snapshot.find("Panels: discovery=1; state=1; marked=3; tracked=2") != std::string::npos);
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
  std::memset(context.status.panels[0].name, 'N', sizeof(context.status.panels[0].name));
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
  context.status.panel_count = context.status.panel_state = UINT32_MAX;
  for (auto& panel : context.status.panels)
    panel.id = UINT64_MAX;
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

  // The other form fields: sanitized, bounded, in form order before the
  // snapshot, which gets what is left and is shortened at a line.
  const auto parameters = [](const std::wstring& wide) {
    const std::string text(wide.begin(), wide.end());
    std::vector<std::pair<std::string, std::string>> out;
    for (auto at = text.find('&'); at != std::string::npos;) {
      const auto next = text.find('&', at + 1);
      const auto pair = text.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
      out.push_back({pair.substr(0, pair.find('=')), decode(pair.substr(pair.find('=') + 1))});
      at = next;
    }
    return out;
  };
  BugReportForm form{"MSFS 2024 1.9.21.0, Steam", "A380X profile; add-on version: ", "Windows: 11\nGPU: caf\xc3\xa9\r\n",
                     "Report file: taxi-cam-report-20261102-234105.zip"};
  auto fields = parameters(bug_report_url(context, form));
  assert(fields.size() == 6 && fields[0].first == "taxi_cam_version" &&
         fields[1] == std::make_pair(std::string("simulator_version"), form.simulator));
  assert(fields[2] == std::make_pair(std::string("aircraft"), std::string("A380X profile; add-on version:")));
  assert(fields[3] == std::make_pair(std::string("environment"), std::string("Windows: 11\nGPU: caf??")));
  assert(fields[4] == std::make_pair(std::string("logs"), form.logs) &&
         fields[5] == std::make_pair(std::string("runtime_snapshot"), snapshot));
  form = {std::string(4000, '%'), std::string(4000, '&'), std::string(4000, '#'), std::string(4000, '=')};
  const auto crowded = bug_report_url(context, form);
  assert(crowded.size() <= BugReportUrlLimit);
  for (const auto c : crowded)
    assert(c >= 33 && c <= 126);
  fields = parameters(crowded);
  assert(fields.size() == 6 && fields[1].second == std::string(120, '%') && fields[3].second == std::string(1000, '#'));
  const auto& shortened = fields[5].second;
  assert(shortened.ends_with("[Snapshot shortened; the full snapshot is in report-info.txt.]"));
  const auto kept = shortened.substr(0, shortened.find("[Snapshot shortened"));
  assert(!kept.empty() && kept.back() == '\n' && snapshot.starts_with(kept));
  assert(bug_report_fit(snapshot, 10).empty() && bug_report_fit(snapshot, 1 << 20) == snapshot);
  // Without a saved report file the snapshot does not point to one.
  fields = parameters(bug_report_url(context, form, false));
  assert(fields[5].second.ends_with("[Snapshot shortened to fit the form.]") && fields[5].second.find("report-info") == std::string::npos);

  // The aircraft line names the profile and a valid SimObject path only.
  BugReportContext aircraft;
  aircraft.settings.profile = 1;
  assert(bug_report_aircraft(aircraft) == "FlyByWire A380X profile; add-on version: ");
  aircraft.status.heartbeat = 1;
  std::strcpy(aircraft.status.aircraft_path, "C:\\Users\\private-account\\aircraft.cfg");
  std::strcpy(aircraft.status.aircraft_type, "A388");
  assert(bug_report_aircraft(aircraft) == "FlyByWire A380X profile; type A388; add-on version: ");
  std::strcpy(aircraft.status.aircraft_path, "SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_CFM_SL");
  assert(bug_report_aircraft(aircraft).find("SimObject SimObjects/Airplanes/FNX_32X/presets/fnx/FNX_320_CFM_SL") != std::string::npos);
  aircraft = {};
  aircraft.settings.profile = 0xFFFFFFFF;
  assert(bug_report_aircraft(aircraft) == "No aircraft identified; add-on version: ");
  std::printf("PASS bug reports: form fields, encoding, privacy, status freshness, bounds, report ZIP and redaction (URL %zu/%zu chars)\n",
              bounded_url.size(), BugReportUrlLimit);
}
