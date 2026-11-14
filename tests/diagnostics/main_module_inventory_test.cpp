#include "../../tools/diagnostics/main_module_inventory.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace taxi_camera::discovery;

std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void image_page_protection() {
  for (const DWORD protection :
       {PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY}) {
    require(readable_image_page(protection), "A readable or copy-on-write packaged image page was refused");
  }
  for (const DWORD protection :
       {DWORD(PAGE_EXECUTE), DWORD(PAGE_NOACCESS), DWORD(PAGE_READONLY | PAGE_GUARD), DWORD(PAGE_EXECUTE_READ | PAGE_GUARD), DWORD(0)}) {
    require(!readable_image_page(protection), "An execute-only, inaccessible or guard page was treated as readable");
  }
}

void protection_change_after_query() {
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, system.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  require(page != nullptr, "The protection-change fixture page could not be allocated");
  std::memset(page, 0x5a, 16);
  std::uint8_t copy[16]{};
  DWORD error = 0;
  require(
      read_process_memory_rechecking_protection(GetCurrentProcess(), reinterpret_cast<std::uintptr_t>(page), copy, sizeof(copy), error) &&
          copy[0] == 0x5a && copy[15] == 0x5a && error == 0,
      "A readable page could not be read");
  MEMORY_BASIC_INFORMATION queried{};
  require(VirtualQuery(page, &queried, sizeof(queried)) == sizeof(queried) && readable_image_page(queried.Protect),
          "The fixture page was not readable when queried");
  DWORD previous = 0;
  require(VirtualProtect(page, system.dwPageSize, PAGE_NOACCESS, &previous) != FALSE, "The fixture page protection could not change");
  require(
      !read_process_memory_rechecking_protection(GetCurrentProcess(), reinterpret_cast<std::uintptr_t>(page), copy, sizeof(copy), error) &&
          error != 0,
      "A read after a protection change since the query did not fail cleanly");
  VirtualFree(page, 0, MEM_RELEASE);
}

void explicit_main_module_only() {
  for (const auto module : {HMODULE(nullptr), GetModuleHandleW(L"kernel32.dll")}) {
    const auto refused = inspect_main_module(module);
    require(!refused.main_module_verified && !refused.memory_read_attempted && !refused.image.valid_image &&
                refused.image.error == "Only an explicitly selected main executable module is permitted.",
            "A null or non-main module handle was inspected");
  }
  const auto own = inspect_main_module(GetModuleHandleW(nullptr));
  require(own.main_module_verified && own.image.valid_image && own.identity_method == "current_main_handle" &&
              own.module_basename == L"main-module-inventory-test.exe",
          "The explicit main module was not inspected under its executable basename");
}

void simulator_selection_refusals() {
  auto refused = inspect_simulator_main_module(0);
  require(!refused.main_module_verified && !refused.memory_read_attempted &&
              refused.image.error == "An explicit nonzero simulator PID is required.",
          "A zero PID was not refused");
  refused = inspect_simulator_main_module(GetCurrentProcessId());
  require(!refused.main_module_verified && !refused.memory_read_attempted && refused.module_basename == L"main-module-inventory-test.exe" &&
              refused.image.error == "The selected PID is not FlightSimulator2024.exe.",
          "A process other than the simulator was inspected");
}

}

int main() {
  try {
    image_page_protection();
    protection_change_after_query();
    explicit_main_module_only();
    simulator_selection_refusals();
    std::printf("PASS: %u main-module checks; copy-on-write pages, protection changes after query, explicit main handle and PID refusal.\n",
                checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
