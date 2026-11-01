#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "../shared/version.hpp"
#include "changelog.hpp"

namespace taxi_camera::standalone {
inline constexpr wchar_t ChangelogHost[] = L"raw.githubusercontent.com";
inline constexpr wchar_t ChangelogRoot[] = L"/rthoms334/taxi-cam/main/";
inline constexpr char ChangelogFile[] = "changelog.json";
inline constexpr std::size_t kWhatsNewImageMaxBytes = 2 * 1024 * 1024;
struct ChangelogFetchResult {
  bool ok{};
  std::string path, body;
};
// One bounded HTTPS GET of a file in the fixed repository's main branch, off
// the UI thread: changelog.json, or a feature picture under docs/whats-new/.
// stop() closes the pending request so shutdown does not wait for network
// timeouts. With a local root (the --preview UI), the file is read from that
// repository checkout instead, so unmerged notes and pictures can be checked.
class ChangelogFetcher {
 public:
  ~ChangelogFetcher() { stop(); }
  bool busy() const { return busy_; }
  void set_local_root(std::wstring root) { local_root_ = std::move(root); }
  bool begin(HWND notify, UINT message, std::string path = ChangelogFile, std::size_t limit = kChangelogMaxBytes) {
    if (path != ChangelogFile && !valid_whats_new_image(path))
      return false;
    if (busy_)
      return false;
    if (worker_.joinable())
      worker_.join();
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return false;
      ready_ = false;
    }
    busy_ = true;
    worker_ = std::thread([this, notify, message, path = std::move(path), limit] {
      ChangelogFetchResult result;
      result.path = path;
      result.ok = local_root_.empty() ? fetch(path, limit, result.body) : read_local(path, limit, result.body);
      {
        std::lock_guard lock(mutex_);
        result_ = std::move(result);
        ready_ = true;
      }
      busy_ = false;
      PostMessageW(notify, message, 0, 0);
    });
    return true;
  }
  bool take(ChangelogFetchResult& result) {
    std::lock_guard lock(mutex_);
    if (!ready_)
      return false;
    result = std::move(result_);
    ready_ = false;
    return true;
  }
  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      if (request_) {
        WinHttpCloseHandle(request_);  // Cancels a pending synchronous call.
        request_ = nullptr;
      }
    }
    if (worker_.joinable())
      worker_.join();
  }

 private:
  bool read_local(const std::string& path, std::size_t limit, std::string& body) {
    std::wstring file = local_root_ + L"\\";
    for (const char c : path)
      file += c == '/' ? L'\\' : static_cast<wchar_t>(c);
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart > 0 && static_cast<unsigned long long>(size.QuadPart) <= limit;
    if (ok) {
      body.resize(static_cast<std::size_t>(size.QuadPart));
      DWORD read{};
      ok = ReadFile(handle, body.data(), static_cast<DWORD>(body.size()), &read, nullptr) && read == body.size();
    }
    CloseHandle(handle);
    return ok;
  }
  bool fetch(const std::string& path, std::size_t limit, std::string& body) {
    std::wstring request_path = ChangelogRoot;
    for (const char c : path)
      request_path += static_cast<wchar_t>(c);
    HINTERNET session = WinHttpOpen(L"Taxi Cam/" TAXI_CAM_VERSION_WIDE, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
      return false;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connection = WinHttpConnect(session, ChangelogHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", request_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    bool ok = false;
    if (request) {
      {
        std::lock_guard lock(mutex_);
        if (stopping_) {
          WinHttpCloseHandle(request);
          request = nullptr;
        } else {
          request_ = request;
        }
      }
      if (request) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        DWORD status{}, size = sizeof(status);
        ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(request, nullptr) &&
             WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                 &size, WINHTTP_NO_HEADER_INDEX) &&
             status == 200;
        char buffer[8192];
        DWORD read{};
        while (ok) {
          if (!WinHttpReadData(request, buffer, sizeof(buffer), &read) || body.size() + read > limit)
            ok = false;
          else if (!read)
            break;
          else
            body.append(buffer, read);
        }
        std::lock_guard lock(mutex_);
        if (request_) {
          WinHttpCloseHandle(request_);
          request_ = nullptr;
        } else {
          ok = false;  // stop() closed the request.
        }
      }
    }
    if (connection)
      WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
  }

  std::atomic<bool> busy_{false};
  std::thread worker_;
  std::mutex mutex_;
  HINTERNET request_{};
  bool ready_{}, stopping_{};
  std::wstring local_root_;
  ChangelogFetchResult result_;
};
}  // namespace taxi_camera::standalone
