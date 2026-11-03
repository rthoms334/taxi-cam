#pragma once
#include <windows.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <winver.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "launcher.hpp"
#include "module_inventory.hpp"
#include "report_bundle.hpp"

namespace taxi_camera::standalone {
// The computer and simulator details a bug report fills in, read without
// touching the simulator: no paths, account names or file contents beyond the
// MSFS graphics options. Each line reads "unknown" rather than guessing.
struct SystemDetails {
  std::string windows, cpu, memory, gpus, simulator, graphics, hooks, tools;
  std::wstring simulator_path, user_config;  // For the ZIP; never in the form.
};

// Printable ASCII for the form; anything else becomes '?'. Newlines stay when
// allowed. Bounded so one value cannot crowd the others out of the URL.
inline std::string report_text(std::string_view text, std::size_t limit, bool newlines = false) {
  std::string out;
  for (const unsigned char c : text) {
    if (out.size() >= limit)
      break;
    if (c == '\n' && newlines)
      out += '\n';
    else if (c >= 32 && c < 127)
      out += static_cast<char>(c);
    else if (c != '\r' && c != 0)
      out += c == '\t' || c == '\n' ? ' ' : '?';
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n'))
    out.pop_back();
  return out;
}
inline std::string report_text(std::wstring_view text, std::size_t limit) {
  std::string narrow;
  for (const wchar_t c : text)
    narrow += c < 128 ? static_cast<char>(c) : '?';
  return report_text(narrow, limit);
}

// NVIDIA's user-mode driver version 32.0.15.6094 is the 560.94 release: the
// last digit of the third part and the fourth part, as five digits.
inline std::string driver_version(std::uint32_t vendor, std::uint64_t umd) {
  const unsigned parts[]{static_cast<unsigned>(umd >> 48), static_cast<unsigned>(umd >> 32 & 0xFFFF),
                         static_cast<unsigned>(umd >> 16 & 0xFFFF), static_cast<unsigned>(umd & 0xFFFF)};
  char text[64];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", parts[0], parts[1], parts[2], parts[3]);
  if (vendor != 0x10DE || parts[3] > 9999)
    return text;
  const unsigned release = parts[2] % 10 * 10000 + parts[3];
  char nvidia[96];
  std::snprintf(nvidia, sizeof(nvidia), "%u.%02u (%s)", release / 100, release % 100, text);
  return nvidia;
}

inline std::string simulator_store(std::wstring_view path) {
  const std::wstring p(path);
  if (contains_ci(p, L"\\XboxGames\\") || contains_ci(p, L"\\WindowsApps\\Microsoft.Limitless_"))
    return "Microsoft Store/Xbox app";
  if (contains_ci(p, L"\\steamapps\\"))
    return "Steam";
  return "store unknown";
}

// The display and upscaling choices from MSFS's UserCfg.opt {Video} block and
// the {Graphics} preset. Values are copied as MSFS writes them.
inline std::string user_config_summary(std::string_view text) {
  std::vector<std::string_view> sections;
  std::string video;
  const auto add = [](std::string& out, std::string_view key, std::string_view value) {
    if (!out.empty())
      out += "; ";
    out += key;
    out += ' ';
    out += value;
  };
  constexpr std::string_view video_keys[]{
      "Windowed",        "FullscreenBorderless", "Resolution",      "FullScreenResolution", "AntiAliasing", "DLSSMode", "FSRMode",
      "FrameGeneration", "PrimaryScaling",       "DynamicSettings", "TargetFrameRate",      "VSync",        "HDR10"};
  for (std::size_t begin = 0; begin < text.size();) {
    auto end = text.find('\n', begin);
    if (end == std::string_view::npos)
      end = text.size();
    auto line = text.substr(begin, end - begin);
    begin = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.remove_prefix(1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    if (line.starts_with('{')) {
      sections.push_back(line.substr(1));
      continue;
    }
    if (line == "}") {
      if (!sections.empty())
        sections.pop_back();
      continue;
    }
    if (sections.size() != 1)
      continue;
    const auto space = line.find_first_of(" \t");
    if (space == std::string_view::npos)
      continue;
    const auto key = line.substr(0, space);
    auto value = line.substr(space + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    if (sections[0] == "Video") {
      for (const auto wanted : video_keys)
        if (key == wanted)
          add(video, key, value);
    } else if (sections[0] == "Graphics" && key == "Preset")
      add(video, key, value);
  }
  return video;
}

namespace system_detail {
inline std::wstring registry_text(HKEY root, const wchar_t* key, const wchar_t* name) {
  wchar_t value[512]{};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
    return {};
  return value;
}
inline DWORD registry_number(HKEY root, const wchar_t* key, const wchar_t* name) {
  DWORD value{}, bytes = sizeof(value);
  return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS ? value : 0;
}
// "ProductName ProductVersion" from a file's version resource, else its fixed
// file version; empty when it has none.
inline std::string file_product(const std::wstring& path) {
  DWORD ignored{};
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (!size)
    return {};
  std::vector<unsigned char> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    return {};
  struct Translation {
    WORD language, codepage;
  }* translation{};
  UINT bytes{};
  std::wstring product;
  if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &bytes) &&
      bytes >= sizeof(Translation)) {
    for (const wchar_t* field : {L"ProductName", L"ProductVersion"}) {
      wchar_t query[96];
      std::swprintf(query, 96, L"\\StringFileInfo\\%04x%04x\\%ls", translation->language, translation->codepage, field);
      wchar_t* value{};
      if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value), &bytes) && bytes > 1)
        product += (product.empty() ? L"" : L" ") + std::wstring(value);
    }
  }
  if (!product.empty())
    return report_text(product, 80);
  VS_FIXEDFILEINFO* fixed{};
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &bytes) || !fixed || !fixed->dwFileVersionMS)
    return {};
  return driver_version(0, static_cast<std::uint64_t>(fixed->dwFileVersionMS) << 32 | fixed->dwFileVersionLS);
}
inline std::wstring folder_of(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}
inline std::wstring leaf_of(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
inline bool in_windows_folder(const std::wstring& path) {
  wchar_t windows[MAX_PATH]{};
  const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
  return n && n < MAX_PATH && path.size() > n && !_wcsnicmp(path.c_str(), windows, n) && (path[n] == L'\\' || path[n] == L'/');
}
// DLL names that replace or hook a system graphics/input DLL when loaded from
// the simulator folder (ReShade, OptiScaler and similar loaders).
inline constexpr const wchar_t* ProxyNames[]{L"dxgi.dll", L"d3d12.dll", L"d3d11.dll", L"dinput8.dll", L"winmm.dll", L"version.dll"};
// Fragments of overlay, capture and frame-generation module names.
inline constexpr std::pair<const wchar_t*, const char*> HookModules[]{{L"reshade", "ReShade"},
                                                                      {L"optiscaler", "OptiScaler"},
                                                                      {L"rtsshooks", "RivaTuner overlay"},
                                                                      {L"graphics-hook", "OBS capture"},
                                                                      {L"discordhook", "Discord overlay"},
                                                                      {L"gameoverlayrenderer", "Steam overlay"},
                                                                      {L"nvngx_dlssg", "DLSS frame generation"},
                                                                      {L"sl.dlss_g", "DLSS frame generation (Streamline)"}};
inline constexpr std::pair<const wchar_t*, const char*> ToolProcesses[]{{L"RTSS.exe", "RivaTuner Statistics Server"},
                                                                        {L"MSIAfterburner.exe", "MSI Afterburner"},
                                                                        {L"obs64.exe", "OBS Studio"},
                                                                        {L"Discord.exe", "Discord"},
                                                                        {L"NVIDIA Overlay.exe", "NVIDIA overlay"},
                                                                        {L"LosslessScaling.exe", "Lossless Scaling"},
                                                                        {L"vrserver.exe", "SteamVR"},
                                                                        {L"OVRServer_x64.exe", "Meta Quest Link"},
                                                                        {L"VirtualDesktop.Streamer.exe", "Virtual Desktop"}};
inline void append(std::string& out, std::string_view text) {
  if (text.empty())
    return;
  if (!out.empty())
    out += "; ";
  out += text;
}
}  // namespace system_detail

inline std::string windows_details() {
  using namespace system_detail;
  using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto get = ntdll ? reinterpret_cast<RtlGetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
  if (!get || get(&version) != 0)
    return "unknown";
  constexpr auto key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  const auto display = report_text(registry_text(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"), 16);
  char text[128];
  std::snprintf(text, sizeof(text), "Windows %s%s%s (build %lu.%lu)", version.dwBuildNumber >= 22000 ? "11" : "10",
                display.empty() ? "" : " ", display.c_str(), version.dwBuildNumber, registry_number(HKEY_LOCAL_MACHINE, key, L"UBR"));
  return text;
}

inline std::string cpu_details() {
  auto name = report_text(
      system_detail::registry_text(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"), 96);
  if (name.empty())
    name = "unknown";
  const auto threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return threads ? name + " (" + std::to_string(threads) + " threads)" : name;
}

// Installed memory; Windows reports a little less as usable.
inline std::string memory_details() {
  ULONGLONG kilobytes{};
  if (GetPhysicallyInstalledSystemMemory(&kilobytes) && kilobytes)
    return std::to_string((kilobytes + (1u << 19)) >> 20) + " GB";
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (!GlobalMemoryStatusEx(&memory))
    return "unknown";
  return std::to_string((memory.ullTotalPhys + (std::uint64_t{1} << 29)) >> 30) + " GB usable";
}

inline std::string gpu_details() {
  IDXGIFactory1* factory{};
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return "unknown";
  std::string out;
  IDXGIAdapter1* adapter{};
  for (UINT i = 0; i < 8 && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414) {
      // An integrated GPU has little or no memory of its own.
      auto line = report_text(desc.Description, 96) +
                  (desc.DedicatedVideoMemory < (std::size_t{1} << 30)
                       ? std::string(", integrated")
                       : ", " + std::to_string((desc.DedicatedVideoMemory + (std::size_t{1} << 29)) >> 30) + " GB");
      if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        line += ", driver " + driver_version(desc.VendorId, static_cast<std::uint64_t>(umd.QuadPart));
      if (!out.empty())
        out += " | ";
      out += line;
    }
    adapter->Release();
  }
  factory->Release();
  return out.empty() ? "unknown" : out;
}

// The running simulator's image, else the configured or first installed one.
inline std::wstring simulator_image(DWORD pid, const std::wstring& configured) {
  if (pid) {
    if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
      wchar_t path[32768]{};
      DWORD size = 32768;
      const bool ok = QueryFullProcessImageNameW(process, 0, path, &size);
      CloseHandle(process);
      if (ok)
        return path;
    }
  }
  const auto installed = discover_msfs2024_executables(configured);
  return installed.empty() ? std::wstring{} : installed.front();
}

// Store builds carry no file version; their package full name does.
inline std::string store_package_version() {
  using GetPackages = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR*, UINT32*, WCHAR*);
  const auto kernel = GetModuleHandleW(L"kernel32.dll");
  const auto get =
      kernel ? reinterpret_cast<GetPackages>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetPackagesByPackageFamily"))) : nullptr;
  if (!get)
    return {};
  constexpr auto family = L"Microsoft.Limitless_8wekyb3d8bbwe";
  UINT32 count{}, length{};
  if (get(family, &count, nullptr, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !count || !length)
    return {};
  std::vector<PWSTR> names(count);
  std::vector<WCHAR> buffer(length);
  if (get(family, &count, names.data(), &length, buffer.data()) != ERROR_SUCCESS || !count)
    return {};
  // Microsoft.Limitless_1.9.21.0_x64__8wekyb3d8bbwe
  const std::wstring name(names[0]);
  const auto begin = name.find(L'_'), end = begin == std::wstring::npos ? begin : name.find(L'_', begin + 1);
  return end == std::wstring::npos ? std::string{} : report_text(std::wstring_view(name).substr(begin + 1, end - begin - 1), 32);
}

inline std::wstring user_config_path(const std::wstring& simulator) {
  wchar_t folder[32768]{};
  const bool steam = simulator_store(simulator) == "Steam";
  const DWORD n = GetEnvironmentVariableW(steam ? L"APPDATA" : L"LOCALAPPDATA", folder, 32768);
  if (!n || n > 32000)
    return {};
  return std::wstring(folder) + (steam ? L"\\Microsoft Flight Simulator 2024\\UserCfg.opt"
                                       : L"\\Packages\\Microsoft.Limitless_8wekyb3d8bbwe\\LocalCache\\UserCfg.opt");
}

// Graphics DLLs in the simulator folder and, while it runs, hook modules in
// the simulator process (a read-only module list; nothing is opened in it).
inline std::string simulator_hooks(DWORD pid, const std::wstring& simulator) {
  using namespace system_detail;
  std::string out;
  const auto folder = folder_of(simulator);
  if (!folder.empty())
    for (const auto* proxy : ProxyNames) {
      const auto path = folder + L"\\" + proxy;
      if (file_exists(path)) {
        const auto product = file_product(path);
        append(out, report_text(std::wstring_view(proxy), 32) + " in simulator folder" + (product.empty() ? "" : " (" + product + ")"));
      }
    }
  if (!pid)
    return out.empty() ? "simulator folder: none; simulator not running" : out;
  DWORD error{};
  const auto loaded = modules(pid, &error);
  if (loaded.empty())
    return out.empty() ? "simulator folder: none; module list unavailable" : out;
  std::vector<const char*> seen;
  for (const auto& module : loaded) {
    std::wstring lower = module.name;
    for (auto& c : lower)
      c = static_cast<wchar_t>(towlower(c));
    for (const auto& [fragment, label] : HookModules)
      if (lower.find(fragment) != std::wstring::npos && std::find(seen.begin(), seen.end(), label) == seen.end()) {
        seen.push_back(label);
        append(out, std::string(label) + " loaded");
      }
    for (const auto* proxy : ProxyNames)
      if (!_wcsicmp(module.name.c_str(), proxy) && !in_windows_folder(module.path) && !contains_ci(module.path, folder.c_str())) {
        const auto product = file_product(module.path);
        append(out,
               report_text(std::wstring_view(proxy), 32) + " loaded from outside Windows" + (product.empty() ? "" : " (" + product + ")"));
      }
  }
  return out.empty() ? "none found" : out;
}

inline std::string running_tools() {
  using namespace system_detail;
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return "unknown";
  std::vector<const char*> found;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (bool ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
    for (const auto& [name, label] : ToolProcesses)
      if (!_wcsicmp(entry.szExeFile, name) && std::find(found.begin(), found.end(), label) == found.end())
        found.push_back(label);
  CloseHandle(snapshot);
  std::string out;
  for (const auto* label : found)
    append(out, label);
  return out.empty() ? "none detected" : out;
}

inline SystemDetails system_details(DWORD simulator_pid, const std::wstring& configured_simulator) {
  SystemDetails d;
  d.windows = windows_details();
  d.cpu = cpu_details();
  d.memory = memory_details();
  d.gpus = gpu_details();
  d.simulator_path = simulator_image(simulator_pid, configured_simulator);
  if (d.simulator_path.empty())
    d.simulator = "MSFS 2024 not found";
  else {
    const auto store = simulator_store(d.simulator_path);
    auto version = store == "Steam" ? system_detail::file_product(d.simulator_path) : store_package_version();
    d.simulator = "MSFS 2024 " + (version.empty() ? std::string("version unknown") : version) + ", " + store +
                  (simulator_pid ? ", running" : ", not running");
  }
  d.user_config = user_config_path(d.simulator_path);
  d.graphics = "unknown";
  if (!d.user_config.empty()) {
    if (const auto summary = user_config_summary(read_file_tail(d.user_config, 1 << 20)); !summary.empty())
      d.graphics = report_text(summary, 400);
  }
  d.hooks = simulator_hooks(simulator_pid, d.simulator_path);
  d.tools = running_tools();
  return d;
}
}  // namespace taxi_camera::standalone
