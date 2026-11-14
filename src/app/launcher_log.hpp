#pragma once
#include "launcher.hpp"
#include "../bridge/crash_evidence.hpp"
#include "../shared/rotating_log.hpp"
#include <cstdio>
#include <string>

namespace taxi_camera::standalone {
inline void append_launcher_log(const std::wstring& directory, const std::wstring& line) noexcept {
  try {
    if (directory.empty() || line.empty() || line.size() > MaxLogRecordBytes)
      return;
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > 8192)
      return;
    std::string bytes(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), bytes.data(), size, nullptr,
                            nullptr) != size)
      return;
    append_rotating_log(directory + L"\\launcher.log", bytes, LauncherLogBytes);
  } catch (...) {
    // Diagnostics must never change the result or disrupt startup.
  }
}
inline void log_launch(const std::wstring& directory, DWORD pid, const LaunchResult& result, const LaunchDiagnostics& trace) noexcept {
  try {
    if (directory.empty())
      return;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t prefix[1536]{};
    std::swprintf(prefix, 1536,
                  L"%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu ok=%u error=%lu phase=%ls load_started=%u module_error=%lu "
                  L"module_attempts=%u loader_thread_low32=0x%08lx loader_result_error=%lu retry_before_load=%u "
                  L"started_ms=%llu elapsed_ms=%llu preflight_ms=%llu load_create_ms=%llu load_wait_ms=%llu post_load_ms=%llu "
                  L"export_ms=%llu start_create_ms=%llu start_wait_ms=%llu load_tid=%lu start_tid=%lu "
                  L"load_wait_observed=%u load_wait=0x%08lx load_wait_error=%lu load_cancelled=%u "
                  L"start_wait_observed=%u start_wait=0x%08lx start_wait_error=%lu start_cancelled=%u "
                  L"start_thread_result=%lu start_result_error=%lu | ",
                  utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, pid, unsigned(result.ok), result.error, trace.phase,
                  unsigned(trace.load_started), trace.module_error, trace.module_attempts, trace.loader_thread_result,
                  trace.loader_result_error, unsigned(result.retry_before_load), static_cast<unsigned long long>(trace.started_ms),
                  static_cast<unsigned long long>(trace.elapsed_ms), static_cast<unsigned long long>(trace.preflight_ms),
                  static_cast<unsigned long long>(trace.load_create_ms), static_cast<unsigned long long>(trace.load_wait_ms),
                  static_cast<unsigned long long>(trace.post_load_ms), static_cast<unsigned long long>(trace.export_ms),
                  static_cast<unsigned long long>(trace.start_create_ms), static_cast<unsigned long long>(trace.start_wait_ms),
                  trace.loader_thread_id, trace.start_thread_id, unsigned(trace.loader_wait.observed), trace.loader_wait.result,
                  trace.loader_wait.error, unsigned(trace.loader_wait.cancelled), unsigned(trace.start_wait.observed),
                  trace.start_wait.result, trace.start_wait.error, unsigned(trace.start_wait.cancelled), trace.start_thread_result,
                  trace.start_result_error);
    append_launcher_log(directory, std::wstring(prefix) + result.message + L"\r\n");
  } catch (...) {
  }
}
inline void log_attach(const std::wstring& directory, const SimulatorAttach& attach, const std::wstring& configured) noexcept {
  try {
    if (directory.empty() || !attach.pid)
      return;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t prefix[768]{};
    std::swprintf(
        prefix, 768,
        L"%04u-%02u-%02uT%02u:%02u:%02uZ attach pid=%lu accepted=%u used_configured=%u fallback=%u configured=%ls selected=%ls\r\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, attach.pid, attach.accepted, unsigned(attach.used_configured),
        unsigned(!attach.used_configured), configured.empty() ? L"" : configured.c_str(), attach.path.empty() ? L"" : attach.path.c_str());
    append_launcher_log(directory, prefix);
  } catch (...) {
  }
}
// A simulator that ended with a non-zero exit code, and the bridge's stack
// overflow record for that process when it wrote one (crash_evidence.hpp).
inline void log_simulator_exit(const std::wstring& directory, DWORD pid, DWORD code) noexcept {
  try {
    if (directory.empty() || !pid || !code)
      return;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t prefix[160]{};
    std::swprintf(prefix, 160, L"%04u-%02u-%02uT%02u:%02u:%02uZ simulator exit pid=%lu code=0x%08lx", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond, pid, code);
    std::wstring line = prefix;
    // The newest record of this process ID: IDs are reused across sessions.
    WIN32_FIND_DATAW entry{}, newest{};
    const auto search = FindFirstFileW((directory + L"\\renderer-fault-" + std::to_wstring(pid) + L"-*.bin").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if (!(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            crash_evidence::record_filename(entry.cFileName) &&
            (!newest.cFileName[0] || CompareFileTime(&entry.ftLastWriteTime, &newest.ftLastWriteTime) > 0))
          newest = entry;
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
    if (newest.cFileName[0]) {
      const auto file =
          CreateFileW((directory + L"\\" + newest.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        crash_evidence::Record record;
        DWORD read{};
        char text[3072]{};
        if (ReadFile(file, static_cast<void*>(&record), sizeof(record), &read, nullptr) && read == sizeof(record) &&
            record.bytes == sizeof(record) && record.process == pid && crash_evidence::describe_overflow(record, text, sizeof(text))) {
          line += L" | stack overflow: ";
          for (const char* letter = text; *letter; ++letter)
            line += static_cast<wchar_t>(static_cast<unsigned char>(*letter));
        }
        CloseHandle(file);
      }
    }
    append_launcher_log(directory, line + L"\r\n");
  } catch (...) {
  }
}
}  // namespace taxi_camera::standalone
