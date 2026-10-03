#include "setup_commands.hpp"
#include <windows.h>

#include <objbase.h>
#include <cwchar>
#include <string>
#include <vector>
#include "../shared/version.hpp"
#include "json_writer.hpp"
#include "prerequisites.hpp"
#include "processes.hpp"
#include "saved_settings.hpp"
#include "setup_common.hpp"
#include "startup_xml.hpp"

namespace taxi_camera::setup {
namespace {
struct Options {
  std::wstring command, destination, state, simulator, exe_xml, startup = L"automatic";
  DWORD update_from_pid{};
  bool reset_settings{}, remove_settings{}, settings_closed{};
};
Options parse(int argc, wchar_t** argv) {
  Options options;
  if (argc < 1)
    fail(L"A setup command is required.");
  options.command = argv[0];
  for (int i = 1; i < argc; ++i) {
    const std::wstring name = argv[i];
    const auto value = [&]() -> std::wstring {
      if (i + 1 >= argc)
        fail(L"Missing value for " + name);
      return argv[++i];
    };
    if (name == L"--destination")
      options.destination = value();
    else if (name == L"--state")
      options.state = value();
    else if (name == L"--simulator")
      options.simulator = value();
    else if (name == L"--exe-xml")
      options.exe_xml = value();
    else if (name == L"--startup")
      options.startup = lower(value());
    else if (name == L"--update-from-pid") {
      const auto text = value();
      wchar_t* end{};
      const auto pid = std::wcstoul(text.c_str(), &end, 10);
      if (text.empty() || *end)
        fail(L"Invalid --update-from-pid value.");
      options.update_from_pid = static_cast<DWORD>(pid);
    } else if (name == L"--reset-settings")
      options.reset_settings = true;
    else if (name == L"--remove-settings")
      options.remove_settings = true;
    else if (name == L"--settings-closed")
      options.settings_closed = true;
    else
      fail(L"Unknown setup option: " + name);
  }
  if (options.destination.empty() || options.state.empty())
    fail(L"--destination and --state are required.");
  if (options.startup != L"automatic" && options.startup != L"manual")
    fail(L"--startup must be automatic or manual.");
  options.destination = trim_separators(full_path(options.destination));
  options.state = full_path(options.state);
  return options;
}

void write_utf8(const std::wstring& path, const std::wstring& text) {
  write_file(path, narrow(text));
}
// Replaces a record through a sibling temporary file so a failure keeps the old one.
void write_record(const std::wstring& path, const std::string& bytes) {
  const auto temporary = path + L"." + new_guid() + L".tmp";
  write_file(temporary, bytes);
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = GetLastError();
    DeleteFileW(temporary.c_str());
    fail_win32(L"Could not write " + path, error);
  }
}

struct Record {
  bool present{};
  std::wstring simulator, exe_xml, startup_requested, legacy_backup;
  std::vector<std::wstring> startup_paths;
};
std::wstring member_text(const json_out::Value& object, const char* key) {
  const auto* value = object.find(key);
  return value && value->is_string() ? widen(value->text) : std::wstring{};
}
Record read_record(const std::wstring& destination) {
  Record record;
  const auto path = join(destination, L"installation.json");
  if (!is_file(path))
    return record;
  json_out::Value value;
  if (!standalone::json::parse(read_file(path), value) || !value.is_object())
    fail(L"Cannot read the previous installation settings. Check installation.json.");
  record.present = true;
  record.simulator = member_text(value, "simulator");
  record.exe_xml = member_text(value, "exeXml");
  record.startup_requested = member_text(value, "startupRequested");
  record.legacy_backup = member_text(value, "legacyBackup");
  // Older records predate optional startup and always owned their exe.xml entry.
  if (const auto* paths = value.find("startupPaths"); paths && paths->is_array()) {
    for (const auto& item : paths->items)
      if (item.is_string() && !item.text.empty())
        record.startup_paths.push_back(widen(item.text));
  } else if (!record.exe_xml.empty()) {
    record.startup_paths.push_back(record.exe_xml);
  }
  return record;
}
std::vector<std::wstring> standard_launch_files() {
  return {join(environment(L"LOCALAPPDATA"), L"Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\exe.xml"),
          join(environment(L"APPDATA"), L"Microsoft Flight Simulator 2024\\exe.xml")};
}

void discover(const Options& options) {
  std::wstring simulator, exe_xml, startup = L"automatic";
  if (const auto record = read_record(options.destination); record.present) {
    simulator = parent_path(record.simulator);
    exe_xml = record.exe_xml;
    if (!record.startup_requested.empty())
      startup = lower(record.startup_requested);
  } else {
    std::vector<std::wstring> found;
    for (const auto* candidate :
         {L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content", L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Limitless"})
      if (is_file(join(candidate, L"FlightSimulator2024.exe")))
        found.push_back(candidate);
    if (found.size() == 1)
      simulator = found[0];
    std::vector<std::wstring> launch;
    for (const auto& candidate : standard_launch_files())
      if (exists(candidate))
        launch.push_back(candidate);
    if (launch.size() == 1)
      exe_xml = launch[0];
  }
  // Setup reads this with the Windows profile API, which expects UTF-16.
  const std::wstring text = L"\xFEFF[Paths]\r\nSimulator=" + simulator + L"\r\nExeXml=" + exe_xml + L"\r\nStartup=" + startup + L"\r\n";
  write_file(join(options.state, L"choices.ini"),
             std::string_view(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)));
}

void check(const Options& options) {
  if (options.update_from_pid)
    wait_for_updating_companion(options.update_from_pid, options.destination);
  if (simulator_running())
    fail(L"Close MSFS 2024 before installing or uninstalling Taxi Cam.");
  if (companion_running_from(options.destination))
    fail(L"Exit the taxi camera app from its tray menu, then retry.");
  if (options.settings_closed)
    assert_settings_closed();
  if (options.simulator.empty())
    return;
  const auto simulator = trim_separators(full_path(options.simulator));
  if (!is_file(join(simulator, L"FlightSimulator2024.exe")))
    fail(L"Select the MSFS 2024 Content directory containing FlightSimulator2024.exe.");
  if (equal_insensitive(options.destination, trim_separators(path_root(options.destination))) ||
      equal_insensitive(options.destination, simulator))
    fail(L"Use a dedicated companion installation directory.");
  if (options.startup == L"automatic" && !options.exe_xml.empty() && !equal_insensitive(file_name(options.exe_xml), L"exe.xml"))
    fail(L"Select the simulator launch configuration named exe.xml.");
  assert_prerequisites(simulator);
  // A packaged host can redirect AppData writes into its own cache, where MSFS
  // cannot see the companion. Prove the destination before Setup copies files.
  create_directories(options.destination);
  const auto probe = join(options.destination, L".taxi-cam-probe-" + new_guid() + L".tmp");
  write_file(probe, {});
  try {
    assert_visible_install_path(probe);
  } catch (...) {
    DeleteFileW(probe.c_str());
    throw;
  }
  DeleteFileW(probe.c_str());
}

// Removed settings stay recoverable until configure has finished.
struct SettingsReset {
  std::wstring backup;
  std::vector<SettingsEntry> snapshot;
};
// Fills reset before removing anything, so a partial removal can be restored.
void reset_settings(const std::wstring& destination, SettingsReset& reset) {
  assert_settings_closed();
  reset.backup = join(temp_directory(), L"taxi-cam-settings-" + new_guid());
  create_directories(reset.backup);
  // The installation's camera mounts are replaced by Setup's bundled defaults.
  reset.snapshot = snapshot_settings(settings_targets(destination, false), reset.backup);
  remove_settings(reset.snapshot);
}

void configure(const Options& options) {
  if (options.simulator.empty())
    fail(L"--simulator is required.");
  const auto destination = options.destination;
  const auto simulator = trim_separators(full_path(options.simulator));
  const auto simulator_exe = join(simulator, L"FlightSimulator2024.exe");
  const auto executable = join(destination, L"taxi-cam.exe");
  if (!is_file(simulator_exe))
    fail(L"Select the MSFS 2024 Content directory containing FlightSimulator2024.exe.");
  create_directories(destination);
  const auto previous = read_record(destination);
  // Every change below is undone if a later step fails, so the uninstaller
  // never meets a startup entry or disabled add-on that no record describes.
  SettingsReset reset;
  std::wstring legacy, legacy_backup, exe_xml = options.exe_xml, backup, written;
  bool legacy_moved = false, updated = false;
  try {
    if (options.reset_settings)
      reset_settings(destination, reset);
    auto startup_paths = previous.startup_paths;
    // A remembered exe.xml belongs to the simulator it was chosen with. Never
    // carry it to another simulator, such as Steam after a Store installation.
    bool same_simulator = false;
    if (path_rooted(previous.simulator)) {
      try {
        same_simulator = equal_insensitive(full_path(previous.simulator), simulator_exe);
      } catch (const Failure&) {
      }
    }
    if (exe_xml.empty() && same_simulator)
      exe_xml = previous.exe_xml;
    // Disable the former in-simulator add-on; keep the file for recovery.
    if (!previous.legacy_backup.empty() && is_file(previous.legacy_backup) &&
        equal_insensitive(parent_path(previous.legacy_backup), simulator) &&
        starts_with_insensitive(file_name(previous.legacy_backup), L"taxi-camera-native.addon64.disabled-native-"))
      legacy_backup = previous.legacy_backup;
    legacy = join(simulator, L"taxi-camera-native.addon64");
    if (exists(legacy)) {
      legacy_backup = legacy + L".disabled-native-" + utc_file_tag();
      move_file(legacy, legacy_backup);
      legacy_moved = true;
    }
    std::wstring status = startup_paths.empty() ? L"manual" : L"unchanged", error, warning;
    bool unsafe = false;
    if (options.startup == L"automatic") {
      try {
        if (exe_xml.empty()) {
          std::vector<std::wstring> found;
          for (const auto& candidate : standard_launch_files())
            if (exists(candidate))
              found.push_back(candidate);
          if (found.size() != 1)
            fail(L"Select the simulator exe.xml in Setup to configure automatic startup.");
          exe_xml = found[0];
        }
        exe_xml = full_path(exe_xml);
        if (!equal_insensitive(file_name(exe_xml), L"exe.xml"))
          fail(L"Startup target must be named exe.xml.");
        const auto hash = file_hash_or_empty(exe_xml);
        auto document = read_launch_xml(exe_xml, true);
        if (startup_disabled_globally(document))
          fail(L"The simulator launch document disables automatic startup globally.");
        set_startup_entry(document, executable, simulator_exe);
        backup = save_launch_xml(document, exe_xml, hash, written, true);
        updated = true;
        status = L"configured";
        bool known = false;
        for (const auto& path : startup_paths)
          known = known || equal_insensitive(path, exe_xml);
        if (!known)
          startup_paths.push_back(exe_xml);
      } catch (const Failure& failure) {
        error = L"Startup file: " + exe_xml + L"\r\n" + failure.detail();
        unsafe = failure.unsafe;
      }
    }
    if (!updated) {
      warning = options.startup == L"manual" ? L"Automatic startup was not changed."
                                             : L"Taxi Cam was installed, but automatic startup could not be configured.";
      warning += L" Open Taxi Cam from the Start menu when you use MSFS. Existing startup entries were left unchanged.";
      if (unsafe)
        warning += L" Another program may have changed exe.xml while Setup ran; check it before starting MSFS.";
      if (!error.empty())
        warning += L" Details are saved in installation.json in the installation folder. Run Setup again to retry automatic startup.";
    }
    using namespace json_out;
    auto record = object();
    set(record, "version", text(TAXI_CAM_VERSION_WIDE));
    set(record, "buildNumber", number(TAXI_CAM_BUILD_NUMBER));
    set(record, "installedUtc", text(utc_iso_timestamp()));
    set(record, "destination", text(destination));
    set(record, "simulator", text(simulator_exe));
    set(record, "exeXml", text_or_null(exe_xml));
    set(record, "exeXmlBackup", text_or_null(backup));
    set(record, "legacyBackup", text_or_null(legacy_backup));
    set(record, "startupRequested", text(options.startup));
    set(record, "startupStatus", text(status));
    auto paths = array();
    for (const auto& path : startup_paths)
      paths.items.push_back(text(path));
    set(record, "startupPaths", std::move(paths));
    set(record, "startupWarning", text(warning));
    set(record, "startupError", text(error));
    set(record, "startupUpdated", boolean(updated));
    set(record, "exeXmlInstalledHash", text(written));
    write_record(join(destination, L"installation.json"), serialize(record));
    // The record is the commit point; diagnostics and notices are best effort.
    try {
      write_utf8(join(destination, L"setup-diagnostics.log"),
                 L"Taxi Cam Setup " TAXI_CAM_VERSION_WIDE L" build " + std::to_wstring(TAXI_CAM_BUILD_NUMBER) + L"\r\nUTC: " +
                     utc_iso_timestamp() + L"\r\nDestination: " + destination + L"\r\nStartup requested: " + options.startup +
                     L"\r\nStartup status: " + status + L"\r\nStartup file: " + exe_xml + L"\r\n" + warning + L"\r\n" + error + L"\r\n");
      const auto warning_path = join(options.state, L"warning.txt");
      if (!warning.empty())
        write_utf8(warning_path, warning);
      else
        DeleteFileW(warning_path.c_str());
    } catch (const Failure&) {
    }
  } catch (const Failure& failure) {
    std::wstring rollback;
    if (updated) {
      try {
        if (!equal_insensitive(file_hash_or_empty(exe_xml), written))
          fail(L"exe.xml changed after Setup wrote it; the newer contents were preserved: " + exe_xml);
        if (backup.empty())
          delete_file(exe_xml);
        else
          copy_file(backup, exe_xml, true);
      } catch (const Failure& restore) {
        rollback += L" " + restore.message;
      }
    }
    if (legacy_moved) {
      try {
        if (exists(legacy))
          fail(L"An active legacy add-on appeared; the disabled copy was kept: " + legacy_backup);
        move_file(legacy_backup, legacy);
      } catch (const Failure& restore) {
        rollback += L" " + restore.message;
      }
    }
    if (!reset.snapshot.empty()) {
      try {
        restore_settings(reset.snapshot);
      } catch (const Failure& restore) {
        rollback += L" " + restore.message;
      }
    }
    if (!rollback.empty())
      fail(L"Setup could not finish configuring Taxi Cam and its rollback needs attention." +
           (reset.backup.empty() ? std::wstring{} : L" Settings recovery files: " + reset.backup + L".") + L" " + failure.message +
           rollback);
    if (!reset.backup.empty())
      remove_directory_tree(reset.backup);
    throw;
  }
  if (!reset.backup.empty())
    remove_directory_tree(reset.backup);
}

struct XmlWrite {
  std::wstring path, backup, prior_hash, installed_hash;
};
void uninstall(const Options& options) {
  if (simulator_running())
    fail(L"Close MSFS 2024 before installing or uninstalling Taxi Cam.");
  const auto record = read_record(options.destination);
  std::wstring settings_backup;
  std::vector<SettingsEntry> snapshot;
  if (options.remove_settings) {
    assert_settings_closed();
    settings_backup = join(temp_directory(), L"taxi-cam-settings-" + new_guid());
    create_directories(settings_backup);
    snapshot = snapshot_settings(settings_targets(options.destination, true), settings_backup);
  }
  const auto owned = [&](const std::wstring& path) {
    return equal_insensitive(path, join(options.destination, L"taxi-cam.exe")) ||
           equal_insensitive(path, join(options.destination, L"380-taxi-cam.exe"));
  };
  std::vector<XmlWrite> writes;
  try {
    for (const auto& path : record.startup_paths) {
      if (!is_file(path))
        continue;
      const auto hash = sha256_file(path);
      auto document = read_launch_xml(path);
      // Remove only entries still pointing to this installation, including the former name.
      if (!remove_startup_entries(document, owned))
        continue;
      std::wstring written;
      const auto backup = save_launch_xml(document, path, hash, written);
      writes.push_back({path, backup, hash, written});
    }
    if (options.remove_settings)
      remove_settings(snapshot);
  } catch (const Failure& failure) {
    std::wstring rollback;
    if (options.remove_settings) {
      try {
        restore_settings(snapshot);
      } catch (const Failure& restore) {
        rollback += L" " + restore.message;
      }
    }
    // Every successful XML change is undone, newest first, unless another
    // writer changed the file afterwards.
    for (auto write = writes.rbegin(); write != writes.rend(); ++write) {
      try {
        if (!equal_insensitive(file_hash_or_empty(write->path), write->installed_hash))
          fail(L"Startup file changed during uninstall; newer contents were preserved: " + write->path);
        if (!equal_insensitive(file_hash_or_empty(write->backup), write->prior_hash))
          fail(L"Startup recovery backup is missing or changed: " + write->backup);
        copy_file(write->backup, write->path, true);
      } catch (const Failure& restore) {
        rollback += L" " + restore.message;
      }
    }
    if (!rollback.empty()) {
      std::wstring recovery = settings_backup;
      for (const auto& write : writes)
        recovery += (recovery.empty() ? L"" : L", ") + write.backup;
      fail(L"Uninstall rollback needs attention. Recovery files: " + recovery + L"." + rollback);
    }
    if (!settings_backup.empty())
      remove_directory_tree(settings_backup);
    throw;
  }
  if (!settings_backup.empty())
    remove_directory_tree(settings_backup);
}
}  // namespace

int run_setup_command(int argc, wchar_t** argv) {
  std::wstring state;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::wstring(argv[i]) == L"--state")
      state = argv[i + 1];
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int code = 0;
  try {
    const auto options = parse(argc, argv);
    create_directories(options.state);
    DeleteFileW(join(options.state, L"error.txt").c_str());
    if (options.command == L"discover")
      discover(options);
    else if (options.command == L"check")
      check(options);
    else if (options.command == L"configure")
      configure(options);
    else if (options.command == L"uninstall")
      uninstall(options);
    else
      fail(L"Unknown setup command: " + options.command);
  } catch (const Failure& failure) {
    code = 1;
    if (!state.empty()) {
      try {
        create_directories(full_path(state));
        write_utf8(join(full_path(state), L"error.txt"), failure.detail());
      } catch (const Failure&) {
      }
    }
  } catch (const std::exception&) {
    code = 1;
  }
  if (SUCCEEDED(com))
    CoUninitialize();
  return code;
}
}  // namespace taxi_camera::setup
