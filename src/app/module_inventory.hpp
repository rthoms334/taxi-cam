#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace taxi_camera::standalone {
struct Module {
  std::wstring name, path;
  std::uintptr_t base{};
  DWORD bytes{};
};
struct ModuleInventory {
  std::vector<Module> entries;
  DWORD error = ERROR_SUCCESS;
  unsigned attempts = 0;
};
struct WindowsModuleReader {
  HANDLE snapshot(DWORD pid) { return CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid); }
  bool first(HANDLE snapshot, MODULEENTRY32W& entry) { return Module32FirstW(snapshot, &entry) != FALSE; }
  bool next(HANDLE snapshot, MODULEENTRY32W& entry) { return Module32NextW(snapshot, &entry) != FALSE; }
  DWORD error() { return GetLastError(); }
  void close(HANDLE snapshot) { CloseHandle(snapshot); }
  void pause() { Sleep(10); }
};
// A module snapshot can race Windows loading/unloading another DLL. Retry the
// documented ERROR_BAD_LENGTH only; never return a failed or partial inventory
// as evidence that a particular module is absent. No target operation is retried.
// https://learn.microsoft.com/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot
template <class Reader>
ModuleInventory module_inventory(DWORD pid, Reader& reader) {
  ModuleInventory result;
  constexpr unsigned MaximumAttempts = 32;
  constexpr std::size_t MaximumModules = 4096;
  for (unsigned attempt = 0; attempt < MaximumAttempts; ++attempt) {
    ++result.attempts;
    result.entries.clear();
    const auto snapshot = reader.snapshot(pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
      result.error = reader.error();
      if (!result.error)
        result.error = ERROR_GEN_FAILURE;
    } else {
      MODULEENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      bool available = reader.first(snapshot, entry);
      result.error = available ? ERROR_SUCCESS : reader.error();
      if (!available && !result.error)
        result.error = ERROR_GEN_FAILURE;
      while (available) {
        if (result.entries.size() == MaximumModules) {
          result.error = ERROR_BUFFER_OVERFLOW;
          break;
        }
        result.entries.push_back({entry.szModule, entry.szExePath, reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize});
        available = reader.next(snapshot, entry);
        if (!available) {
          result.error = reader.error();
          if (!result.error)
            result.error = ERROR_GEN_FAILURE;
        }
      }
      reader.close(snapshot);
      if (result.error == ERROR_NO_MORE_FILES)
        result.error = ERROR_SUCCESS;
    }
    if (result.error == ERROR_SUCCESS)
      return result;
    result.entries.clear();
    if (result.error != ERROR_BAD_LENGTH || attempt + 1 == MaximumAttempts)
      return result;
    reader.pause();
  }
  return result;
}
inline std::vector<Module> modules(DWORD pid, DWORD* error = nullptr, unsigned* attempts = nullptr) {
  WindowsModuleReader reader;
  auto result = module_inventory(pid, reader);
  if (error)
    *error = result.error;
  if (attempts)
    *attempts = result.attempts;
  return std::move(result.entries);
}
}  // namespace taxi_camera::standalone
