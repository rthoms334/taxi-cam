#include "prerequisites.hpp"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include "setup_common.hpp"

namespace taxi_camera::setup {
namespace {
ImageCheck result(ImageStatus status, unsigned long error, std::wstring detail) {
  return {status, error, std::move(detail)};
}
std::wstring hex(unsigned value) {
  wchar_t text[16]{};
  std::swprintf(text, 16, L"0x%X", value);
  return text;
}
std::wstring machine_name(std::uint16_t machine) {
  switch (machine) {
    case 0x14C:
      return L"x86";
    case 0xAA64:
      return L"ARM64";
    case 0x1C4:
      return L"ARM";
    default:
      return L"unknown";
  }
}
}  // namespace

ImageCheck inspect_image(const std::wstring& path, bool dll) {
  if (is_directory(path))
    return result(ImageStatus::missing, ERROR_DIRECTORY, L"the path is a folder, not a file");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return result(ImageStatus::missing, error, L"");
    return result(ImageStatus::unreadable, error, L"");
  }
  LARGE_INTEGER length{};
  bool io_failed = false;
  const auto read_at = [&](std::int64_t offset, void* data, DWORD size) {
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    DWORD read{};
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) || !ReadFile(file, data, size, &read, nullptr)) {
      io_failed = true;
      return false;
    }
    return read == size;
  };
  const auto finish = [&](ImageCheck check) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (io_failed && check.status != ImageStatus::ok)
      return result(ImageStatus::unreadable, error, L"");
    return check;
  };
  std::uint16_t mz{}, machine{}, sections{}, optional_size{}, flags{}, magic{};
  std::uint32_t offset{}, signature{};
  if (!GetFileSizeEx(file, &length))
    return finish(result(ImageStatus::unreadable, GetLastError(), L"the file size could not be read"));
  io_failed = false;
  if (length.QuadPart < 64 || !read_at(0, &mz, 2) || mz != 0x5A4D)
    return finish(result(ImageStatus::not_x64, 0, L"not a Windows executable image (no MZ header)"));
  if (!read_at(60, &offset, 4) || offset < 64 || offset > 1024 * 1024 || static_cast<std::int64_t>(offset) + 26 > length.QuadPart ||
      !read_at(offset, &signature, 4) || signature != 0x4550)
    return finish(result(ImageStatus::not_x64, 0, L"not a valid PE image (bad PE header)"));
  if (!read_at(offset + 4, &machine, 2))
    return finish(result(ImageStatus::not_x64, 0, L"truncated PE header"));
  if (machine != 0x8664)
    return finish(result(ImageStatus::not_x64, 0, L"machine type " + hex(machine) + L" (" + machine_name(machine) + L"), expected 0x8664 (x64)"));
  if (!read_at(offset + 6, &sections, 2) || !read_at(offset + 20, &optional_size, 2) || !read_at(offset + 22, &flags, 2))
    return finish(result(ImageStatus::not_x64, 0, L"truncated PE header"));
  if (sections < 1 || sections > 96 || optional_size < 112 || optional_size > 4096 ||
      static_cast<std::int64_t>(offset) + 24 + optional_size + 40LL * sections > length.QuadPart || !(flags & 2))
    return finish(result(ImageStatus::not_x64, 0, L"malformed or truncated PE header"));
  if (((flags & 0x2000) != 0) != dll)
    return finish(result(ImageStatus::not_x64, 0, dll ? L"an executable, not a DLL" : L"a DLL, not an executable"));
  if (!read_at(offset + 24, &magic, 2) || magic != 0x20B)
    return finish(result(ImageStatus::not_x64, 0, L"optional header is not PE32+"));
  CloseHandle(file);
  return result(ImageStatus::ok, 0, L"");
}
bool amd64_image(const std::wstring& path, bool dll) {
  return inspect_image(path, dll).status == ImageStatus::ok;
}
bool content_unreadable_but_present(const ImageCheck& check) {
  return check.status == ImageStatus::unreadable &&
         (check.error == ERROR_ACCESS_DENIED || check.error == ERROR_SHARING_VIOLATION || check.error == ERROR_LOCK_VIOLATION);
}
std::wstring describe_image_issue(const std::wstring& label, const std::wstring& path, const ImageCheck& check) {
  const std::wstring where = L" (checked: " + path + L")";
  switch (check.status) {
    case ImageStatus::ok:
      return {};
    case ImageStatus::missing:
      return label + L" is missing" + (check.detail.empty() ? L"" : L": " + check.detail) + where + L".";
    case ImageStatus::unreadable:
      return label + L" exists but could not be read, Windows error " + std::to_wstring(check.error) +
             (check.detail.empty() ? L"" : L", " + check.detail) + where + L".";
    case ImageStatus::not_x64:
      return label + L" is not a valid x64 DLL/executable: " + check.detail + where + L".";
  }
  return {};
}
namespace {
// Points at the usual wrong-level selections: the package root above Content, or a subfolder inside it.
std::wstring relocation_hint(const std::wstring& simulator_directory, const std::wstring& file) {
  std::vector<std::wstring> candidates{join(join(simulator_directory, L"Content"), file), join(parent_path(simulator_directory), file)};
  for (const auto& candidate : candidates)
    if (is_file(candidate))
      return L" The file was found at " + candidate + L"; select " + parent_path(candidate) + L" instead.";
  return {};
}
}  // namespace
std::vector<std::wstring> prerequisite_issues(const std::wstring& simulator_directory, const std::wstring& system_directory) {
  std::vector<std::wstring> issues;
  for (const auto* name : {L"d3d12.dll", L"dxgi.dll", L"d3dcompiler_47.dll", L"ucrtbase.dll"}) {
    const auto path = join(system_directory, name);
    if (const auto check = inspect_image(path); check.status != ImageStatus::ok)
      issues.push_back(L"Required Windows component " + describe_image_issue(name, path, check) +
                       L" Repair Windows and install Windows updates.");
  }
  // Xbox/MS Store installations can deny file-content reads even for a valid EXE.
  // Check presence here; the launcher verifies the loaded AMD64 image at connection time.
  const auto simulator_exe = join(simulator_directory, L"FlightSimulator2024.exe");
  if (!is_file(simulator_exe))
    issues.push_back(L"FlightSimulator2024.exe was not found in the selected folder " + simulator_directory +
                     L". Select the MSFS 2024 installation folder containing it." +
                     relocation_hint(simulator_directory, L"FlightSimulator2024.exe"));
  const auto client = join(simulator_directory, L"SimConnect_internal.dll");
  // The installer may be denied content access to a Store package file that MSFS itself can load.
  // That is reported by presence only; a missing or wrong-architecture file is still refused.
  if (const auto check = inspect_image(client); check.status != ImageStatus::ok && !content_unreadable_but_present(check))
    issues.push_back(describe_image_issue(L"SimConnect_internal.dll", client, check) +
                     L" It must be in the same folder as FlightSimulator2024.exe. Verify or repair MSFS 2024; a separate SimConnect SDK "
                     L"installation is not needed." +
                     (check.status == ImageStatus::missing ? relocation_hint(simulator_directory, L"SimConnect_internal.dll")
                                                           : std::wstring()));
  // These are imports of MSFS 2024's SimConnect client, not of Taxi Cam's static runtime.
  // Respect app-local DLL precedence: a wrong-architecture local copy can shadow a valid system copy.
  for (const auto* name : {L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
    auto path = join(simulator_directory, name);
    if (!is_file(path))
      path = join(system_directory, name);
    if (const auto check = inspect_image(path); check.status != ImageStatus::ok)
      issues.push_back(L"The MSFS SimConnect client requires the Microsoft Visual C++ v14 x64 runtime: " +
                       describe_image_issue(name, path, check) +
                       L" Install or repair it from https://aka.ms/vc14/vc_redist.x64.exe. If a bad copy is in the simulator folder, "
                       L"verify or repair MSFS 2024.");
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
