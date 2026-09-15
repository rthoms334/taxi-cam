#pragma once
#include <windows.h>
#include <cstring>
#include <string>

namespace taxi_camera::standalone {
// Per-user first-use state is independent of aircraft settings and app versions.
class StartupSettings {
  inline static constexpr char Marker[] = "settings-shown-v1\n";
  std::wstring path_;
  bool preview_{}, shown_{}, show_{};

  bool read_shown() const {
    if (path_.empty())
      return false;
    const auto file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return false;
    char data[sizeof(Marker)]{};
    DWORD bytes{};
    const bool ok = ReadFile(file, data, sizeof(data), &bytes, nullptr) && bytes == sizeof(Marker) - 1 &&
                    std::memcmp(data, Marker, sizeof(Marker) - 1) == 0;
    CloseHandle(file);
    return ok;
  }

 public:
  StartupSettings(const std::wstring& directory, bool background, bool preview) : preview_(preview), show_(!background) {
    // Preview must neither read nor consume the user's real first-use state.
    if (preview_)
      return;
    if (!directory.empty())
      path_ = directory + L"\\startup-state";
    shown_ = read_shown();
    show_ = show_ || !shown_;
  }

  bool should_show() const noexcept { return show_; }

  bool record_shown(bool window_visible, bool service_started) const {
    if (preview_)
      return true;
    if (!window_visible || !service_started)
      return false;
    if (shown_)
      return true;
    if (path_.empty())
      return false;
    const auto temporary = path_ + L".tmp." + std::to_wstring(GetCurrentProcessId());
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return false;
    DWORD bytes{};
    bool ok = WriteFile(file, Marker, sizeof(Marker) - 1, &bytes, nullptr) && bytes == sizeof(Marker) - 1 && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok)
      ok = MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok)
      DeleteFileW(temporary.c_str());
    return ok;
  }
};
}  // namespace taxi_camera::standalone
