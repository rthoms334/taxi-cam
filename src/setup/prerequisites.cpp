#include "prerequisites.hpp"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "setup_common.hpp"

namespace taxi_camera::setup {
bool amd64_image(const std::wstring& path, bool dll) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER length{};
  const auto read_at = [&](std::int64_t offset, void* data, DWORD size) {
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    DWORD read{};
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, data, size, &read, nullptr) && read == size;
  };
  std::uint16_t mz{}, machine{}, sections{}, optional_size{}, flags{}, magic{};
  std::uint32_t offset{}, signature{};
  bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= 64 && read_at(0, &mz, 2) && mz == 0x5A4D && read_at(60, &offset, 4) &&
            offset >= 64 && offset <= 1024 * 1024 && static_cast<std::int64_t>(offset) + 26 <= length.QuadPart &&
            read_at(offset, &signature, 4) && signature == 0x4550 && read_at(offset + 4, &machine, 2) && machine == 0x8664 &&
            read_at(offset + 6, &sections, 2) && read_at(offset + 20, &optional_size, 2) && read_at(offset + 22, &flags, 2);
  ok = ok && sections >= 1 && sections <= 96 && optional_size >= 112 && optional_size <= 4096 &&
       static_cast<std::int64_t>(offset) + 24 + optional_size + 40LL * sections <= length.QuadPart && (flags & 2) &&
       ((flags & 0x2000) != 0) == dll && read_at(offset + 24, &magic, 2) && magic == 0x20B;
  CloseHandle(file);
  return ok;
}
std::vector<std::wstring> prerequisite_issues(const std::wstring& simulator_directory, const std::wstring& system_directory) {
  std::vector<std::wstring> issues;
  for (const auto* name : {L"d3d12.dll", L"dxgi.dll", L"d3dcompiler_47.dll", L"ucrtbase.dll"})
    if (!amd64_image(join(system_directory, name)))
      issues.push_back(std::wstring(L"Required Windows component is missing, unreadable or not x64: ") + name +
                       L". Repair Windows and install Windows updates.");
  // Xbox/MS Store installations can deny file-content reads even for a valid EXE.
  // Check presence here; the launcher verifies the loaded AMD64 image at connection time.
  if (!is_file(join(simulator_directory, L"FlightSimulator2024.exe")))
    issues.push_back(L"Select the MSFS 2024 installation folder containing FlightSimulator2024.exe.");
  if (!amd64_image(join(simulator_directory, L"SimConnect_internal.dll")))
    issues.push_back(
        L"SimConnect_internal.dll is missing, unreadable or not x64 in the selected MSFS folder. Verify or repair MSFS 2024; a separate "
        L"SimConnect SDK installation is not needed.");
  // These are imports of MSFS 2024's SimConnect client, not of Taxi Cam's static runtime.
  // Respect app-local DLL precedence: a wrong-architecture local copy can shadow a valid system copy.
  for (const auto* name : {L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
    auto path = join(simulator_directory, name);
    if (!is_file(path))
      path = join(system_directory, name);
    if (!amd64_image(path))
      issues.push_back(std::wstring(L"The MSFS SimConnect client requires the Microsoft Visual C++ v14 x64 runtime: ") + name +
                       L" is missing, unreadable or not x64. Install or repair it from https://aka.ms/vc14/vc_redist.x64.exe. If a bad "
                       L"copy is in the simulator folder, verify or repair MSFS 2024.");
  }
  return issues;
}
void assert_prerequisites(const std::wstring& simulator_directory) {
  const auto issues = prerequisite_issues(simulator_directory, system_directory());
  if (issues.empty())
    return;
  std::wstring message = L"Required components are missing or invalid:";
  for (const auto& issue : issues)
    message += L"\r\n- " + issue;
  fail(message);
}
}  // namespace taxi_camera::setup
