#include <cstdio>
#include <stdexcept>
#include "../../src/app/launcher.hpp"
#include "../../src/app/launcher_log.hpp"

namespace {
void require(bool value, const char* label) {
  if (!value)
    throw std::runtime_error(label);
}
struct Files {
  std::wstring original, alias, copy;
  ~Files() {
    for (const auto* path : {&alias, &copy, &original})
      if (!path->empty())
        DeleteFileW(path->c_str());
  }
};
struct ModuleFixture {
  unsigned snapshots = 0, closed = 0, pauses = 0, fail_snapshots = 0;
  DWORD failure = ERROR_BAD_LENGTH, last_error = 0;
  bool fail_after_entry = false;
  HANDLE snapshot(DWORD) {
    ++snapshots;
    if (snapshots <= fail_snapshots) {
      last_error = failure;
      return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(std::uintptr_t(123));
  }
  bool first(HANDLE, MODULEENTRY32W& entry) {
    require(entry.dwSize == sizeof(entry), "Module entry size initialized");
    std::wcscpy(entry.szModule, snapshots == 1 && fail_after_entry ? L"partial.dll" : L"complete.dll");
    std::wcscpy(entry.szExePath, L"C:\\fixture\\module.dll");
    entry.modBaseAddr = reinterpret_cast<BYTE*>(std::uintptr_t(0x10000));
    entry.modBaseSize = 4096;
    return true;
  }
  bool next(HANDLE, MODULEENTRY32W&) {
    last_error = fail_after_entry && snapshots == 1 ? failure : ERROR_NO_MORE_FILES;
    return false;
  }
  DWORD error() { return last_error; }
  void close(HANDLE) { ++closed; }
  void pause() { ++pauses; }
};
void module_checks() {
  using taxi_camera::standalone::module_inventory;
  ModuleFixture racing;
  racing.fail_snapshots = 2;
  auto result = module_inventory(1, racing);
  require(!result.error && result.entries.size() == 1 && result.attempts == 3 && racing.closed == 1 && racing.pauses == 2,
          "Transient loader races retry the snapshot and keep the successful inventory");
  ModuleFixture partial;
  partial.fail_after_entry = true;
  result = module_inventory(1, partial);
  require(!result.error && result.attempts == 2 && result.entries.size() == 1 && result.entries[0].name == L"complete.dll" &&
              partial.closed == 2,
          "Partial inventories are discarded before a whole-snapshot retry");
  ModuleFixture perpetual;
  perpetual.fail_snapshots = 100;
  result = module_inventory(1, perpetual);
  require(result.error == ERROR_BAD_LENGTH && result.entries.empty() && result.attempts == 32 && perpetual.pauses == 31,
          "Changing loader inventory cannot cause an unbounded startup wait");
  ModuleFixture denied;
  denied.fail_snapshots = 1;
  denied.failure = ERROR_ACCESS_DENIED;
  result = module_inventory(1, denied);
  require(result.error == ERROR_ACCESS_DENIED && result.entries.empty() && result.attempts == 1 && denied.pauses == 0,
          "Access failure remains an error, not a missing bridge or retry");
  partial = {};
  partial.fail_after_entry = true;
  partial.failure = ERROR_PARTIAL_COPY;
  result = module_inventory(1, partial);
  require(result.error == ERROR_PARTIAL_COPY && result.entries.empty() && result.attempts == 1 && partial.closed == 1,
          "A failed enumeration must never publish an incomplete module list");
  partial.failure = 0;
  partial.snapshots = partial.closed = 0;
  result = module_inventory(1, partial);
  require(result.error == ERROR_GEN_FAILURE && result.entries.empty(), "Failed API without an error still refuses partial contents");
  DWORD error = 1;
  unsigned attempts = 0;
  const auto current = taxi_camera::standalone::modules(GetCurrentProcessId(), &error, &attempts);
  require(!error && !current.empty() && attempts > 0, "Real Windows module enumeration remains usable");
}
}  // namespace

int main() {
  try {
    module_checks();
    using taxi_camera::standalone::same_path;
    wchar_t temporary[MAX_PATH]{}, original[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temporary) != 0, "Temporary directory");
    require(GetTempFileNameW(temporary, L"tax", 0, original) != 0, "Unique fixture file");
    Files files{original, std::wstring(original) + L".alias", std::wstring(original) + L".copy"};
    require(CreateHardLinkW(files.alias.c_str(), files.original.c_str(), nullptr) != FALSE, "Create alternate path to the same file");
    require(CopyFileW(files.original.c_str(), files.copy.c_str(), TRUE) != FALSE, "Create separate file with identical contents");

    require(same_path(files.original, files.original), "Identical path");
    require(same_path(files.original, files.alias), "Hard-linked simulator paths must match");
    require(same_path(files.alias, files.original), "Path identity must be symmetric");
    require(!same_path(files.original, files.copy), "A separate copy must not match");
    require(!same_path(files.original, files.original + L".missing"), "Missing alias must not match");
    require(!same_path(L"", files.original) && !same_path(files.original, L""), "Empty path must not match");

    require(DeleteFileW(files.alias.c_str()) != FALSE, "Remove test alias");
    require(CopyFileW(files.copy.c_str(), files.alias.c_str(), TRUE) != FALSE, "Replace alias with a distinct file");
    require(!same_path(files.original, files.alias), "Replaced path must not retain stale identity");
    using taxi_camera::standalone::LaunchResult;
    using taxi_camera::standalone::LaunchRetry;
    const LaunchResult pending{false, ERROR_BAD_EXE_FORMAT, L"Headers not ready", true};
    LaunchRetry retry;
    require(retry.ready(1000), "First startup attempt immediate");
    require(retry.schedule(pending, 1000) && !retry.ready(1999) && retry.ready(2000), "Preflight retry waits one second");
    for (unsigned i = 1; i < 59; ++i)
      require(retry.schedule(pending, 1000 + i * 1000), "Bounded preflight retry available");
    require(!retry.schedule(pending, 61000), "No more than 60 total load attempts");
    for (const auto error : {WAIT_TIMEOUT, ERROR_ACCESS_DENIED, ERROR_MOD_NOT_FOUND, ERROR_INVALID_ADDRESS}) {
      LaunchRetry terminal;
      require(!terminal.schedule({false, static_cast<DWORD>(error), L"Load or start may have begun"}, 1000),
              "Never repeat remote load/start, timeout, identity or other terminal errors");
    }
    LaunchRetry complete;
    require(!complete.schedule({true, 0, L"Loaded"}, 1000), "Successful load never retried");
    LaunchRetry overflow;
    require(!overflow.schedule(pending, UINT64_MAX), "Retry deadline cannot overflow");
    std::puts("PASS launcher paths and startup: aliases, replaced files, bounded preflight retries and terminal remote-load failures.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL launcher paths: %s\n", error.what());
    return 1;
  }
}
