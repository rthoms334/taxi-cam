// Records every thread of a frozen MSFS 2024 process: state and wait reason,
// CPU times, description and a call stack as module+RVA frames. It suspends
// each thread only for the duration of its own unwind, reads memory through
// ReadProcessMemory and DbgHelp's function-table access, and never writes,
// injects or creates threads in the target. Bridge frames are symbolised
// afterwards by freeze_stacks.ps1 from the DLL's own symbol table.
#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Module {
  std::uintptr_t base{}, size{};
  std::wstring path;
};

std::vector<Module> modules(HANDLE process) {
  std::vector<Module> result;
  std::vector<HMODULE> handles(1024);
  DWORD needed{};
  if (!EnumProcessModulesEx(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed, LIST_MODULES_64BIT))
    return result;
  handles.resize(std::min<std::size_t>(needed / sizeof(HMODULE), handles.size()));
  for (HMODULE handle : handles) {
    MODULEINFO info{};
    wchar_t path[32768]{};
    if (!GetModuleInformation(process, handle, &info, sizeof(info)) || !GetModuleFileNameExW(process, handle, path, 32768))
      continue;
    result.push_back({reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage, path});
  }
  std::sort(result.begin(), result.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
  return result;
}

const Module* find_module(const std::vector<Module>& list, std::uintptr_t address) {
  for (const auto& module : list)
    if (address >= module.base && address < module.base + module.size)
      return &module;
  return nullptr;
}

std::string narrow(const std::wstring& value) {
  if (value.empty())
    return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::string leaf(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  return narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

// Thread state and wait reason come from the system process snapshot; the
// public winternl.h omits the per-thread array, so the layout is declared here.
struct ThreadInfo {
  LARGE_INTEGER kernel_time, user_time, create_time;
  ULONG wait_time;
  PVOID start_address;
  CLIENT_ID client_id;
  LONG priority, base_priority;
  ULONG context_switches, state, wait_reason;
};
struct ProcessInfo {
  ULONG next_entry_offset, thread_count;
  LARGE_INTEGER working_set_private_size;
  ULONG hard_fault_count, number_of_threads_high_watermark;
  ULONGLONG cycle_time;
  LARGE_INTEGER create_time, user_time, kernel_time;
  UNICODE_STRING image_name;
  LONG base_priority;
  HANDLE process_id, parent_process_id;
  ULONG handle_count, session_id;
  ULONG_PTR unique_process_key;
  SIZE_T peak_virtual_size, virtual_size;
  ULONG page_fault_count;
  SIZE_T peak_working_set_size, working_set_size, quota_peak_paged_pool_usage, quota_paged_pool_usage,
      quota_peak_non_paged_pool_usage, quota_non_paged_pool_usage, pagefile_usage, peak_pagefile_usage, private_page_count;
  LARGE_INTEGER read_operation_count, write_operation_count, other_operation_count, read_transfer_count, write_transfer_count,
      other_transfer_count;
  ThreadInfo threads[1];
};
using QuerySystemInformation = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

std::vector<ThreadInfo> thread_states(DWORD pid) {
  std::vector<ThreadInfo> result;
  const auto query = reinterpret_cast<QuerySystemInformation>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
  if (!query)
    return result;
  std::vector<unsigned char> buffer(1 << 20);
  for (int attempt = 0; attempt < 8; ++attempt) {
    ULONG needed{};
    const NTSTATUS status = query(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    if (status == 0)
      break;
    if (static_cast<ULONG>(status) != 0xC0000004u)  // STATUS_INFO_LENGTH_MISMATCH
      return result;
    buffer.resize(std::max<std::size_t>(needed + (1 << 16), buffer.size() * 2));
  }
  std::size_t offset = 0;
  while (offset + sizeof(ProcessInfo) <= buffer.size()) {
    const auto* process = reinterpret_cast<const ProcessInfo*>(buffer.data() + offset);
    if (reinterpret_cast<std::uintptr_t>(process->process_id) == pid) {
      for (ULONG i = 0; i < process->thread_count; ++i) {
        const auto* thread = reinterpret_cast<const ThreadInfo*>(reinterpret_cast<const unsigned char*>(process->threads) + i * sizeof(ThreadInfo));
        if (reinterpret_cast<const unsigned char*>(thread + 1) > buffer.data() + buffer.size())
          break;
        result.push_back(*thread);
      }
      break;
    }
    if (!process->next_entry_offset)
      break;
    offset += process->next_entry_offset;
  }
  return result;
}

const char* wait_reason_name(ULONG reason) {
  static const char* names[]{"Executive",   "FreePage",       "PageIn",        "PoolAllocation", "DelayExecution",
                             "Suspended",   "UserRequest",    "WrExecutive",   "WrFreePage",     "WrPageIn",
                             "WrPoolAllocation", "WrDelayExecution", "WrSuspended", "WrUserRequest", "WrEventPair",
                             "WrQueue",     "WrLpcReceive",   "WrLpcReply",    "WrVirtualMemory", "WrPageOut",
                             "WrRendezvous", "WrKeyedEvent",  "WrTerminated",  "WrProcessInSwap", "WrCpuRateControl",
                             "WrCalloutStack", "WrKernel",    "WrResource",    "WrPushLock",     "WrMutex",
                             "WrQuantumEnd", "WrDispatchInt", "WrPreempted",   "WrYieldExecution", "WrFastMutex",
                             "WrGuardedMutex", "WrRundown",   "WrAlertByThreadId", "WrDeferredPreempt", "WrPhysicalFault"};
  return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "?";
}
const char* state_name(ULONG state) {
  static const char* names[]{"Initialized", "Ready", "Running", "Standby", "Terminated", "Waiting", "Transition", "DeferredReady", "GateWait"};
  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

BOOL CALLBACK read_memory(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
  SIZE_T count{};
  const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &count);
  *read = static_cast<DWORD>(count);
  return ok;
}

using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::wstring description(HANDLE thread) {
  static const auto get = reinterpret_cast<GetDescription>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  PWSTR text{};
  if (!get || FAILED(get(thread, &text)) || !text)
    return {};
  std::wstring result(text);
  LocalFree(text);
  return result;
}

std::string frame_text(const std::vector<Module>& list, std::uintptr_t address) {
  char text[128];
  if (const auto* module = find_module(list, address))
    std::snprintf(text, sizeof(text), "%s+0x%llx", leaf(module->path).c_str(), static_cast<unsigned long long>(address - module->base));
  else
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
  return text;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  DWORD pid{};
  bool any_image = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::wcscmp(argv[i], L"--pid") && i + 1 < argc)
      pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
    else if (!std::wcscmp(argv[i], L"--allow-any-image"))
      any_image = true;
    else {
      std::fprintf(stderr, "Usage: freeze-stacks.exe --pid <FlightSimulator2024 PID> [--allow-any-image]\n");
      return 2;
    }
  }
  if (!pid) {
    std::fprintf(stderr, "Usage: freeze-stacks.exe --pid <FlightSimulator2024 PID> [--allow-any-image]\n");
    return 2;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) {
    std::fprintf(stderr, "Cannot open process %lu (Windows %lu).\n", pid, GetLastError());
    return 1;
  }
  wchar_t image[32768]{};
  DWORD length = 32768;
  if (!QueryFullProcessImageNameW(process, 0, image, &length)) {
    std::fprintf(stderr, "Cannot read the process image name (Windows %lu).\n", GetLastError());
    return 1;
  }
  if (!any_image && _stricmp(leaf(image).c_str(), "FlightSimulator2024.exe") != 0) {
    std::fprintf(stderr, "Process %lu is not FlightSimulator2024.exe.\n", pid);
    return 1;
  }
  const auto list = modules(process);
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
  if (!SymInitializeW(process, nullptr, TRUE)) {
    std::fprintf(stderr, "DbgHelp initialization failed (Windows %lu).\n", GetLastError());
    return 1;
  }
  const auto states = thread_states(pid);
  std::printf("process %lu image %s modules %zu threads_in_snapshot %zu\n", pid, narrow(image).c_str(), list.size(), states.size());
  for (const auto& module : list)
    if (leaf(module.path) == "taxi-camera-bridge.dll" || leaf(module.path) == "FlightSimulator2024.exe")
      std::printf("module %s base 0x%llx size 0x%llx path %s\n", leaf(module.path).c_str(), static_cast<unsigned long long>(module.base),
                  static_cast<unsigned long long>(module.size), narrow(module.path).c_str());

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "Thread snapshot failed (Windows %lu).\n", GetLastError());
    return 1;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  unsigned captured = 0, failed = 0;
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid)
      continue;
    const DWORD tid = entry.th32ThreadID;
    const ThreadInfo* state = nullptr;
    for (const auto& candidate : states)
      if (reinterpret_cast<std::uintptr_t>(candidate.client_id.UniqueThread) == tid)
        state = &candidate;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    std::printf("\nthread %lu", tid);
    if (state)
      std::printf(" state %s wait %s wait_time %lu kernel_ms %llu user_ms %llu switches %lu", state_name(state->state),
                  wait_reason_name(state->wait_reason), state->wait_time,
                  static_cast<unsigned long long>(state->kernel_time.QuadPart / 10000),
                  static_cast<unsigned long long>(state->user_time.QuadPart / 10000), state->context_switches);
    if (!thread) {
      std::printf(" open_error %lu\n", GetLastError());
      ++failed;
      continue;
    }
    const auto name = description(thread);
    if (!name.empty())
      std::printf(" name \"%s\"", narrow(name).c_str());
    if (state && state->start_address)
      std::printf(" start %s", frame_text(list, reinterpret_cast<std::uintptr_t>(state->start_address)).c_str());
    std::printf("\n");
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
      std::printf("  suspend_error %lu\n", GetLastError());
      CloseHandle(thread);
      ++failed;
      continue;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) {
      std::printf("  context_error %lu\n", GetLastError());
      ResumeThread(thread);
      CloseHandle(thread);
      ++failed;
      continue;
    }
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 64; ++depth) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, read_memory, SymFunctionTableAccess64,
                       SymGetModuleBase64, nullptr) ||
          !frame.AddrPC.Offset)
        break;
      std::printf("  %2d %s\n", depth, frame_text(list, static_cast<std::uintptr_t>(frame.AddrPC.Offset)).c_str());
    }
    ResumeThread(thread);
    CloseHandle(thread);
    ++captured;
  }
  CloseHandle(snapshot);
  SymCleanup(process);
  CloseHandle(process);
  std::printf("\ncaptured %u threads, %u failed\n", captured, failed);
  return 0;
}
