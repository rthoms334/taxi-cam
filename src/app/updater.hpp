#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include "json_value.hpp"

namespace taxi_camera::standalone {
struct UpdateVersion {
  std::uint32_t major{}, minor{}, patch{}, build{};
};
bool parse_update_version(const std::wstring& tag, UpdateVersion& value);
bool newer_update(const UpdateVersion& candidate, const UpdateVersion& current);
bool simulator_blocks_update();
// Returns an open, verified handle denying file writes/replacement until closed.
HANDLE verified_update_file(const std::wstring& path, const std::wstring& sha256);
std::wstring update_installer_arguments(const std::wstring& directory, DWORD pid);
// HTTPS on port 443 to the fixed GitHub release hosts only, without user info or fragment.
bool trusted_download_url(const std::wstring& url);
struct UpdateAsset {
  std::string tag, name, url;
  std::int64_t size{};
  std::string digest;    // Lowercase SHA-256 from the release metadata, when GitHub provides one.
  std::string sums_url;  // SHA256SUMS.txt, used only when the digest is absent.
};
enum class UpdateSelection { Current, Selected, Invalid };
// Selects the one exact Windows installer of a stable, newer GitHub release.
UpdateSelection select_update_asset(const json::Value& release, const UpdateVersion& current, UpdateAsset& asset);
// Finds the single "<sha256> <name>" or "<sha256> *<name>" line for an exact name.
bool read_installer_checksum(std::string_view text, std::string_view name, std::string& digest);
struct UpdateResult {
  bool available{}, manual{};
  std::wstring tag, sha256, installer, error;
};
class Updater {
 public:
  ~Updater();
  bool begin(const std::wstring& installation, bool manual);
  bool take(UpdateResult& result);
  void stop();
  bool busy() const { return busy_; }
  bool launch(const UpdateResult& result, const std::wstring& installation, std::wstring& error);

 private:
  void check(const std::wstring& cache, UpdateResult& result);
  bool download(HINTERNET session,
                std::wstring url,
                std::int64_t limit,
                ULONGLONG deadline,
                std::string* body,
                HANDLE file,
                std::int64_t& total);
  bool track(HINTERNET request);
  bool untrack();

  std::atomic<bool> cancelled_{false}, busy_{false};
  std::thread worker_;
  std::mutex mutex_;
  UpdateResult result_;
  bool ready_{};
  std::wstring cache_;
  std::mutex request_mutex_;
  HINTERNET request_{};
};
}  // namespace taxi_camera::standalone
