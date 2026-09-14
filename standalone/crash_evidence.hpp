#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <new>

namespace taxi_camera::standalone::crash_evidence {
// Small local diagnostic record for the renderer instruction seen in the A350
// reports. No stack/heap pages or image data are copied. Never sent over IPC.
struct Record {
  std::uint32_t magic = 0x54434352, version = 1, bytes = sizeof(Record), process = 0;
  volatile LONG state = 0;  // 0 empty, 1 writing, 2 complete
  DWORD code = 0, parameters = 0, thread = 0;
  std::uint64_t module = 0, fault_rva = 0;
  ULONG_PTR information[EXCEPTION_MAXIMUM_PARAMETERS]{};
  CONTEXT context{};
};
static_assert(sizeof(Record) <= 4096);
inline Record* record = nullptr;
inline constexpr std::uint64_t renderer_fault_rva = 64028644;

inline LONG capture(Record* destination, const EXCEPTION_POINTERS* exception) noexcept {
  if (!destination || !exception || !exception->ExceptionRecord || !exception->ContextRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  const auto& e = *exception->ExceptionRecord;
  const auto& c = *exception->ContextRecord;
  const auto address = reinterpret_cast<std::uint64_t>(e.ExceptionAddress);
  if (e.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !destination->module || address < destination->module ||
      address - destination->module != renderer_fault_rva || c.Rip != address || e.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS ||
      InterlockedCompareExchange(&destination->state, 1, 0) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  // Only bounded copies into an already mapped, initialized record. No file
  // operations, allocation, locks, engine calls or exception suppression here.
  destination->code = e.ExceptionCode;
  destination->parameters = e.NumberParameters;
  destination->thread = GetCurrentThreadId();
  destination->fault_rva = renderer_fault_rva;
  for (DWORD i = 0; i < e.NumberParameters; ++i)
    destination->information[i] = e.ExceptionInformation[i];
  destination->context = c;
  InterlockedExchange(&destination->state, 2);
  return EXCEPTION_CONTINUE_SEARCH;
}
inline LONG CALLBACK handler(EXCEPTION_POINTERS* exception) noexcept {
  return capture(record, exception);
}

// Called outside DllMain, after the bridge verifies its host and pins itself.
// The process owns this mapping/handler until exit; Windows retains the dirty
// mapped-file pages after a process crash. No debugger is attached.
inline bool initialize() noexcept {
  if (record)
    return true;
  wchar_t directory[32768]{};
  const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
  if (!length || length > 32000)
    return false;
  wchar_t folder[32768]{}, path[32768]{};
  std::swprintf(folder, 32768, L"%ls\\Taxi Cam", directory);
  CreateDirectoryW(folder, nullptr);
  std::swprintf(path, 32768, L"%ls\\renderer-fault-%lu-%llu.bin", folder, GetCurrentProcessId(),
                static_cast<unsigned long long>(GetTickCount64()));
  const auto file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
  if (!mapping) {
    CloseHandle(file);
    return false;
  }
  auto* memory = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096);
  CloseHandle(mapping);
  CloseHandle(file);
  if (!memory)
    return false;
  record = new (memory) Record{};
  record->process = GetCurrentProcessId();
  record->module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
  if (!AddVectoredExceptionHandler(1, handler)) {
    UnmapViewOfFile(memory);
    record = nullptr;
    return false;
  }
  return true;
}
}  // namespace taxi_camera::standalone::crash_evidence
