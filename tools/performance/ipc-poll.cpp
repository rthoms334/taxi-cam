#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include "../../src/shared/protocol.hpp"

namespace ipc = taxi_camera::standalone;

bool copy_status_without_mutex_unless_torn(const ipc::Shared& shared, ipc::Status& status) {
  ipc::Status second{};
  std::memcpy(&status, &shared.status, sizeof(status));
  std::memcpy(&second, &shared.status, sizeof(second));
  return std::memcmp(&status, &second, sizeof(status)) == 0;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    std::fwprintf(stderr, L"usage: ipc-poll <pid> <seconds> <out.csv> [poll_ms=2]\n");
    return 2;
  }
  const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
  const int seconds = std::wcstol(argv[2], nullptr, 10);
  const int poll_ms = argc > 4 ? std::wcstol(argv[4], nullptr, 10) : 2;
  wchar_t name[128];
  std::swprintf(name, 128, L"Local\\380TaxiCamera.Data.%lu", pid);
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!mapping) {
    std::fwprintf(stderr, L"OpenFileMapping failed: %lu\n", GetLastError());
    return 1;
  }
  const auto* shared = static_cast<const ipc::Shared*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ipc::Shared)));
  if (!shared) {
    std::fwprintf(stderr, L"MapViewOfFile failed: %lu\n", GetLastError());
    return 1;
  }
  if (shared->magic != ipc::ProtocolMagic || shared->version != ipc::ProtocolVersion || shared->bytes != sizeof(ipc::Shared)) {
    std::fwprintf(stderr, L"protocol mismatch magic=%08x version=%u bytes=%u expected=%zu\n", shared->magic, shared->version, shared->bytes,
                  sizeof(ipc::Shared));
    return 1;
  }
  FILE* out = _wfopen(argv[3], L"wb");
  if (!out) {
    std::fwprintf(stderr, L"cannot open output\n");
    return 1;
  }
  std::fprintf(out,
               "utc_ms,tick_ms,heartbeat,taxi_mask,camera_rate,captures,composed,stamps,hook_failures,probe_ms,probe_max_ms,"
               "st_manager,st_pool,st_lifecycle,st_entries,st_view1,st_view2,st_handoff,st_pose,st_activation,st_publication,torn_reads\n");
  ipc::Status a{};
  std::uint64_t last_heartbeat = 0, torn = 0, rows = 0;
  const auto end = GetTickCount64() + static_cast<std::uint64_t>(seconds) * 1000;
  while (GetTickCount64() < end) {
    if (!copy_status_without_mutex_unless_torn(*shared, a)) {
      ++torn;
      continue;
    }
    if (a.heartbeat != last_heartbeat) {
      last_heartbeat = a.heartbeat;
      FILETIME ft{};
      GetSystemTimePreciseAsFileTime(&ft);
      const auto utc_ms = ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ull - 11644473600000ull;
      std::fprintf(out, "%llu,%llu,%llu,%u,%u,%llu,%llu,%llu,%llu,%.4f,%.4f", utc_ms, static_cast<unsigned long long>(GetTickCount64()),
                   static_cast<unsigned long long>(a.heartbeat), a.taxi_mask, shared->settings.camera_rate,
                   static_cast<unsigned long long>(a.captures), static_cast<unsigned long long>(a.composed),
                   static_cast<unsigned long long>(a.stamps), static_cast<unsigned long long>(a.hook_failures), a.probe_cpu_ms,
                   a.probe_max_ms);
      for (const double v : a.stage_ms)
        std::fprintf(out, ",%.4f", v);
      std::fprintf(out, ",%llu\n", static_cast<unsigned long long>(torn));
      ++rows;
    }
    Sleep(static_cast<DWORD>(poll_ms));
  }
  std::fclose(out);
  std::wprintf(L"rows=%llu torn=%llu\n", static_cast<unsigned long long>(rows), static_cast<unsigned long long>(torn));
  return 0;
}
