#pragma once
#include "launcher.hpp"
#include <cstdio>

namespace taxi_camera::standalone {
inline void log_launch(const std::wstring& directory, DWORD pid, const LaunchResult& result, const LaunchDiagnostics& trace) noexcept {
  try {
    if (directory.empty())
      return;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t prefix[512]{};
    std::swprintf(prefix, 512,
                  L"%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu ok=%u error=%lu phase=%ls load_started=%u module_error=%lu "
                  L"module_attempts=%u loader_thread_low32=0x%08lx loader_result_error=%lu retry_before_load=%u | ",
                  utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, pid, unsigned(result.ok), result.error, trace.phase,
                  unsigned(trace.load_started), trace.module_error, trace.module_attempts, trace.loader_thread_result,
                  trace.loader_result_error, unsigned(result.retry_before_load));
    const auto line = std::wstring(prefix) + result.message + L"\r\n";
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > 8192)
      return;
    std::string bytes(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), bytes.data(), size, nullptr,
                            nullptr) != size)
      return;
    const auto path = directory + L"\\launcher.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return;
    LARGE_INTEGER length{};
    DWORD written = 0;
    if (GetFileSizeEx(file, &length) && length.QuadPart >= 0 && length.QuadPart < 4 * 1024 * 1024)
      WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
  } catch (...) {
    // Diagnostics must never change the result or disrupt startup.
  }
}
}  // namespace taxi_camera::standalone
