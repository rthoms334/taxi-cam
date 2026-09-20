#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace taxi_camera::standalone {
// What the simulator's own graphics configuration says about frame generation.
// The value is read from UserCfg.opt, which the simulator rewrites when
// graphics options are applied, so it follows in-sim changes without a restart.
enum class FrameGeneration : std::uint8_t { unknown, off, on };

// Parses the non-VR "FrameGeneration <value>" line. "NONE" means off; any other
// value (DLSSG, FSR3, ...) means on. "FrameGenerationVR" is a different key.
inline FrameGeneration parse_frame_generation(std::string_view text) noexcept {
  constexpr std::string_view key = "FrameGeneration";
  std::size_t position = 0;
  while (position < text.size()) {
    auto end = text.find('\n', position);
    if (end == std::string_view::npos)
      end = text.size();
    auto line = text.substr(position, end - position);
    position = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
      line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
      line.remove_suffix(1);
    if (line.size() <= key.size() || line.substr(0, key.size()) != key)
      continue;
    const char separator = line[key.size()];
    if (separator != ' ' && separator != '\t')
      continue;  // FrameGenerationVR or another key sharing the prefix
    auto value = line.substr(key.size() + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    if (value.empty())
      return FrameGeneration::unknown;
    return value == "NONE" ? FrameGeneration::off : FrameGeneration::on;
  }
  return FrameGeneration::unknown;
}

// Reads a small text file completely; false when it is missing or unreadable.
inline bool read_small_file(const std::wstring& path, std::string& out, std::size_t limit = 1 << 20) {
  out.clear();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= limit;
  if (ok) {
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
  }
  CloseHandle(file);
  if (!ok)
    out.clear();
  return ok;
}

inline std::wstring environment_folder(const wchar_t* name) {
  wchar_t buffer[32768]{};
  const DWORD n = GetEnvironmentVariableW(name, buffer, 32768);
  return n && n < 32768 ? std::wstring(buffer, n) : std::wstring();
}

inline bool regular_file_exists(const std::wstring& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Candidate UserCfg.opt locations for the running simulator. The Microsoft
// Store build keeps it under its package LocalCache; Steam under Roaming.
// The executable path decides the order; both are tried.
inline std::vector<std::wstring> user_config_candidates(const std::wstring& simulator_exe) {
  const auto lower = [](std::wstring value) {
    for (auto& c : value)
      c = static_cast<wchar_t>(towlower(c));
    return value;
  };
  const auto store = environment_folder(L"LOCALAPPDATA") + L"\\Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\UserCfg.opt";
  const auto steam = environment_folder(L"APPDATA") + L"\\Microsoft Flight Simulator 2024\\UserCfg.opt";
  const auto exe = lower(simulator_exe);
  const bool store_first = exe.find(L"windowsapps") != std::wstring::npos || exe.find(L"xboxgames") != std::wstring::npos;
  return store_first ? std::vector<std::wstring>{store, steam} : std::vector<std::wstring>{steam, store};
}

inline FrameGeneration read_frame_generation(const std::wstring& simulator_exe, std::wstring* used_path = nullptr) {
  std::string text;
  for (const auto& candidate : user_config_candidates(simulator_exe)) {
    if (!read_small_file(candidate, text))
      continue;
    if (used_path)
      *used_path = candidate;
    return parse_frame_generation(text);
  }
  if (used_path)
    used_path->clear();
  return FrameGeneration::unknown;
}

// A dxgi.dll or d3d12.dll beside the simulator executable is a third-party
// graphics hook (ReShade installs itself that way); Windows never places
// those files in an application folder. Capture stays at zero while it is
// loaded, so the companion names it instead of showing silent counters.
inline bool graphics_hook_beside(const std::wstring& simulator_exe, std::wstring* found = nullptr) {
  const auto slash = simulator_exe.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
    return false;
  const auto folder = simulator_exe.substr(0, slash + 1);
  for (const wchar_t* name : {L"dxgi.dll", L"d3d12.dll"}) {
    if (regular_file_exists(folder + name)) {
      if (found)
        *found = folder + name;
      return true;
    }
  }
  return false;
}

// Companion preference: run the cameras even when frame generation is on.
inline constexpr UINT kDefaultAllowFrameGeneration = 0;

inline bool load_allow_frame_generation(const std::wstring& directory) {
  if (directory.empty())
    return kDefaultAllowFrameGeneration != 0;
  const auto path = directory + L"\\settings.ini";
  return GetPrivateProfileIntW(L"companion", L"allow_frame_generation", kDefaultAllowFrameGeneration, path.c_str()) != 0;
}

inline bool save_allow_frame_generation(const std::wstring& directory, bool allowed) {
  if (directory.empty())
    return false;
  const auto path = directory + L"\\settings.ini";
  return WritePrivateProfileStringW(L"companion", L"allow_frame_generation", allowed ? L"1" : L"0", path.c_str()) != FALSE;
}

// Caches the frame-generation reading so the 200 ms attach loop does not
// reread the file every tick; the file is reread once per second.
class SimulatorGraphicsMonitor {
 public:
  struct Sample {
    FrameGeneration frame_generation = FrameGeneration::unknown;
    bool graphics_hook = false;
    std::wstring config_path, hook_path;
  };
  const Sample& sample(const std::wstring& simulator_exe, std::uint64_t now_ms) {
    if (!last_ms_ || now_ms < last_ms_ || now_ms - last_ms_ >= 1000 || simulator_exe != exe_) {
      exe_ = simulator_exe;
      last_ms_ = now_ms ? now_ms : 1;
      sample_.frame_generation = read_frame_generation(simulator_exe, &sample_.config_path);
      sample_.graphics_hook = graphics_hook_beside(simulator_exe, &sample_.hook_path);
      if (!sample_.graphics_hook)
        sample_.hook_path.clear();
    }
    return sample_;
  }

 private:
  std::wstring exe_;
  std::uint64_t last_ms_ = 0;
  Sample sample_;
};

inline constexpr const wchar_t* FrameGenerationBlockedMessage =
    L"Frame Generation is on in MSFS graphics options. Cameras stay off: on this PC they freeze the simulator with Frame "
    L"Generation. Turn it off (Options > Graphics), or allow it on Diagnostics to test.";
}  // namespace taxi_camera::standalone
