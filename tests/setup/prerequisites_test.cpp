#include "../../src/setup/prerequisites.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/setup/setup_common.hpp"

using namespace taxi_camera::setup;
namespace {
int failures = 0, checks = 0;
void expect(bool ok, const char* name) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
  }
}
// Header fixture only. It has no executable code and is never loaded or launched.
std::string image(bool dll = true, std::uint16_t machine = 0x8664) {
  std::string bytes(512, '\0');
  const auto put16 = [&](std::size_t at, std::uint16_t value) { std::memcpy(bytes.data() + at, &value, 2); };
  const auto put32 = [&](std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
  put16(0, 0x5A4D);
  put32(60, 128);
  put32(128, 0x4550);
  put16(132, machine);
  put16(134, 1);
  put16(148, 240);
  put16(150, dll ? 0x2002 : 2);
  put16(152, 0x20B);
  return bytes;
}
void write(const std::wstring& path, const std::string& bytes) {
  create_directories(parent_path(path));
  write_file(path, bytes);
}
bool mentions(const std::vector<std::wstring>& issues, const std::wstring& name) {
  for (const auto& issue : issues)
    if (issue.find(name) != std::wstring::npos)
      return true;
  return false;
}
}  // namespace

int main() {
  wchar_t module[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  const auto root = join(parent_path(module), L"prerequisites-test-" + new_guid());
  const auto sim = join(root, L"sim with spaces"), system = join(root, L"system");
  try {
    const std::vector<std::wstring> system_dlls{L"d3d12.dll",    L"dxgi.dll",         L"d3dcompiler_47.dll", L"ucrtbase.dll",
                                                L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"};
    for (const auto& name : system_dlls)
      write(join(system, name), image());
    write(join(sim, L"FlightSimulator2024.exe"), image(false));
    write(join(sim, L"SimConnect_internal.dll"), image());
    const auto issues = [&] { return prerequisite_issues(sim, system); };
    expect(issues().empty(), "complete x64 prerequisites accepted");
    for (const auto& name : system_dlls) {
      const auto path = join(system, name);
      delete_file(path);
      expect(mentions(issues(), name), "missing system component diagnosed");
      write(path, image(true, 0x14C));
      expect(mentions(issues(), name), "32-bit system component refused");
      write(path, image());
    }
    const auto simulator = join(sim, L"FlightSimulator2024.exe");
    delete_file(simulator);
    expect(mentions(issues(), L"FlightSimulator2024.exe"), "missing simulator executable diagnosed");
    write(simulator, image(false));
    const auto client = join(sim, L"SimConnect_internal.dll");
    delete_file(client);
    expect(mentions(issues(), L"SimConnect_internal.dll"), "missing SimConnect client diagnosed");
    write(client, image(true, 0x14C));
    expect(mentions(issues(), L"SimConnect_internal.dll"), "32-bit SimConnect client refused");
    write(client, image(false));
    expect(mentions(issues(), L"SimConnect_internal.dll"), "EXE in place of the SimConnect DLL refused");
    for (std::uint32_t offset : {0u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
      auto bytes = image();
      std::memcpy(bytes.data() + 60, &offset, 4);
      write(client, bytes);
      expect(!amd64_image(client), "malformed PE header offset refused");
    }
    for (std::size_t size : {0u, 1u, 63u, 64u, 153u, 300u}) {
      write(client, image().substr(0, size));
      expect(!amd64_image(client), "truncated PE image refused");
    }
    write(client, image());
    // Respect app-local DLL precedence: a wrong-architecture local copy shadows a valid system copy.
    const auto local = join(sim, L"msvcp140.dll");
    write(local, image(true, 0x14C));
    expect(mentions(issues(), L"msvcp140.dll"), "invalid app-local runtime not hidden by a valid system copy");
    write(local, image());
    delete_file(join(system, L"msvcp140.dll"));
    expect(issues().empty(), "valid app-local runtime accepted");
  } catch (const Failure& failure) {
    std::fprintf(stderr, "FAIL: unexpected failure: %ls\n", failure.detail().c_str());
    ++failures;
  }
  try {
    remove_directory_tree(root);
  } catch (const Failure&) {
  }
  if (!failures)
    std::printf("PASS prerequisites: %d missing-component, image-header, architecture, truncation and local-runtime checks.\n", checks);
  return failures ? 1 : 0;
}
