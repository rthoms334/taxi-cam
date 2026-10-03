#include "updater.hpp"
#include "../shared/version.hpp"
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <array>
#include <cstdlib>
#include <tuple>
#include <vector>

namespace taxi_camera::standalone {
namespace {
constexpr LONGLONG DownloadLimit = 256LL * 1024 * 1024;
constexpr std::int64_t MetadataLimit = 1024 * 1024;
constexpr ULONGLONG CheckTimeoutMs = 180000;
constexpr char Repository[] = "rthoms334/taxi-cam";
std::wstring wide(std::string_view s) {
  std::wstring result;
  for (char c : s)
    result += static_cast<unsigned char>(c);
  return result;
}
bool ascii(std::string_view s) {
  for (char c : s)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
      return false;
  return true;
}
bool hex_hash(const std::wstring& hash) {
  if (hash.size() != 64)
    return false;
  for (wchar_t c : hash)
    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
      return false;
  return true;
}
bool hex_hash(std::string_view hash) {
  return ascii(hash) && hex_hash(wide(hash));
}
std::string lower(std::string text) {
  for (auto& c : text)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return text;
}
void clean_cache(const std::wstring& path) {
  if (path.empty())
    return;
  // Delete only the fixed files in the directory this instance created.
  for (auto name : {L"\\setup.exe", L"\\setup.exe.partial"})
    DeleteFileW((path + name).c_str());
  RemoveDirectoryW(path.c_str());
}
const json::Value* string_member(const json::Value& object, std::string_view key) {
  const auto* value = object.find(key);
  return value && value->is_string() ? value : nullptr;
}
std::vector<const json::Value*> named_assets(const json::Value& assets, std::string_view name) {
  std::vector<const json::Value*> found;
  for (const auto& asset : assets.items)
    if (const auto* value = string_member(asset, "name"); value && value->text == name)
      found.push_back(&asset);
  return found;
}
struct CrackedUrl {
  std::wstring host, path;
};
bool crack_trusted_url(const std::wstring& url, CrackedUrl& out) {
  if (url.empty() || url.size() > 16384 || url.find_first_of(L"# \t\r\n") != std::wstring::npos)
    return false;
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = parts.dwUrlPathLength =
      parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
      parts.nPort != INTERNET_DEFAULT_HTTPS_PORT || parts.dwUserNameLength || parts.dwPasswordLength || !parts.dwHostNameLength)
    return false;
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  for (auto& c : host)
    if (c >= L'A' && c <= L'Z')
      c = static_cast<wchar_t>(c - L'A' + L'a');
  bool allowed = false;
  for (auto trusted : {L"api.github.com", L"github.com", L"objects.githubusercontent.com", L"release-assets.githubusercontent.com"})
    allowed = allowed || host == trusted;
  if (!allowed)
    return false;
  out.host = std::move(host);
  out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  if (out.path.empty() || out.path[0] != L'/')
    return false;
  return true;
}
std::wstring make_cache() {
  wchar_t local[MAX_PATH]{};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)))
    return {};
  const std::wstring root = std::wstring(local) + L"\\Taxi Cam";
  CreateDirectoryW(root.c_str(), nullptr);
  const std::wstring updates = root + L"\\Updates";
  CreateDirectoryW(updates.c_str(), nullptr);
  std::array<unsigned char, 16> random{};
  if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    return {};
  std::wstring path = updates + L"\\";
  for (auto c : random) {
    path += L"0123456789abcdef"[c >> 4];
    path += L"0123456789abcdef"[c & 15];
  }
  return CreateDirectoryW(path.c_str(), nullptr) ? path : L"";
}
}  // namespace
bool parse_update_version(const std::wstring& tag, UpdateVersion& value) {
  if (tag.size() > 64 || tag.empty() || tag[0] != L'v')
    return false;
  size_t pos = 1;
  auto number = [&](std::uint32_t& out) {
    const auto start = pos;
    std::uint64_t n = 0;
    while (pos < tag.size() && tag[pos] >= L'0' && tag[pos] <= L'9') {
      n = n * 10 + tag[pos++] - L'0';
      if (n > UINT32_MAX)
        return false;
    }
    if (pos == start || (pos - start > 1 && tag[start] == L'0'))
      return false;
    out = static_cast<std::uint32_t>(n);
    return true;
  };
  UpdateVersion parsed;
  if (!number(parsed.major) || pos >= tag.size() || tag[pos++] != L'.' || !number(parsed.minor) ||
      pos >= tag.size() || tag[pos++] != L'.' || !number(parsed.patch) || tag.compare(pos, 7, L"-build.") != 0)
    return false;
  pos += 7;
  if (!number(parsed.build) || pos != tag.size())
    return false;
  value = parsed;
  return true;
}
bool newer_update(const UpdateVersion& candidate, const UpdateVersion& current) {
  return std::tie(candidate.major, candidate.minor, candidate.patch, candidate.build) >
         std::tie(current.major, current.minor, current.patch, current.build);
}
bool simulator_blocks_update() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return true;  // Cannot establish that the DLL is unlocked.
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool blocked = !Process32FirstW(snapshot, &entry);
  if (!blocked)
    do {
      if (!_wcsicmp(entry.szExeFile, L"FlightSimulator2024.exe") || !_wcsicmp(entry.szExeFile, L"FlightSimulator.exe")) {
        blocked = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return blocked;
}
HANDLE verified_update_file(const std::wstring& path, const std::wstring& sha256) {
  if (!hex_hash(sha256))
    return INVALID_HANDLE_VALUE;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return file;
  LARGE_INTEGER size{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  bool ok = GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
            !(attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
            GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= DownloadLimit;
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  std::array<unsigned char, 32> digest{};
  if (ok)
    ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
         BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::array<unsigned char, 65536> buffer{};
  LONGLONG total{};
  while (ok) {
    DWORD read{};
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      ok = false;
      break;
    }
    if (!read)
      break;
    total += read;
    ok = total <= DownloadLimit && BCryptHashData(hash, buffer.data(), read, 0) >= 0;
  }
  if (ok)
    ok = total == size.QuadPart && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
  if (hash)
    BCryptDestroyHash(hash);
  if (algorithm)
    BCryptCloseAlgorithmProvider(algorithm, 0);
  std::wstring actual;
  for (auto c : digest) {
    actual += L"0123456789abcdef"[c >> 4];
    actual += L"0123456789abcdef"[c & 15];
  }
  if (!ok || _wcsicmp(actual.c_str(), sha256.c_str())) {
    CloseHandle(file);
    return INVALID_HANDLE_VALUE;
  }
  return file;
}
std::wstring update_installer_arguments(const std::wstring& directory, DWORD pid) {
  if (!pid || directory.empty() || directory.find_first_of(L"\"\r\n") != std::wstring::npos ||
      directory.back() == L'\\' || directory.back() == L'/')
    return {};
  return L"/DIR=\"" + directory + L"\" /UPDATEFROMPID=" + std::to_wstring(pid);
}
bool trusted_download_url(const std::wstring& url) {
  CrackedUrl cracked;
  return crack_trusted_url(url, cracked);
}
UpdateSelection select_update_asset(const json::Value& release, const UpdateVersion& current, UpdateAsset& asset) {
  const auto* draft = release.find("draft");
  const auto* prerelease = release.find("prerelease");
  if (!draft || !draft->is_bool() || !prerelease || !prerelease->is_bool() || draft->boolean || prerelease->boolean)
    return UpdateSelection::Invalid;  // Not a stable published release.
  const auto* tag_value = string_member(release, "tag_name");
  UpdateVersion version{};
  if (!tag_value || tag_value->text.size() > 64 || !ascii(tag_value->text) || !parse_update_version(wide(tag_value->text), version))
    return UpdateSelection::Invalid;
  if (!newer_update(version, current))
    return UpdateSelection::Current;
  const auto* assets = release.find("assets");
  if (!assets || !assets->is_array())
    return UpdateSelection::Invalid;
  const std::string& tag = tag_value->text;
  std::string name = "taxi-cam-" + std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                     std::to_string(version.patch) + "-windows-x64-setup.exe";
  const std::string legacy_name = "taxi-cam-" + tag.substr(1) + "-windows-x64-setup.exe";
  auto matches = named_assets(*assets, name);
  const auto legacy = named_assets(*assets, legacy_name);
  if (matches.size() > 1 || legacy.size() > 1)
    return UpdateSelection::Invalid;  // Duplicate Windows installer assets.
  // An advertised clean name is authoritative, even when its metadata is bad.
  // Only releases without it can use the exact older build-number filename.
  if (matches.empty()) {
    name = legacy_name;
    matches = legacy;
  }
  if (matches.size() != 1)
    return UpdateSelection::Invalid;
  const auto& found = *matches[0];
  const std::string base = std::string("https://github.com/") + Repository + "/releases/download/" + tag + "/";
  const auto* url = string_member(found, "browser_download_url");
  const auto* state = string_member(found, "state");
  const auto* size = found.find("size");
  std::int64_t bytes{};
  if (!url || url->text != base + name || !state || state->text != "uploaded" || !size || !size->integer(1, DownloadLimit, bytes) ||
      !trusted_download_url(wide(url->text)))
    return UpdateSelection::Invalid;
  UpdateAsset selected;
  selected.tag = tag;
  selected.name = name;
  selected.url = url->text;
  selected.size = bytes;
  const auto* digest = found.find("digest");
  if (digest && !digest->is_null() && !(digest->is_string() && digest->text.empty())) {
    constexpr std::string_view prefix = "sha256:";
    if (!digest->is_string() || digest->text.compare(0, prefix.size(), prefix) != 0 || !hex_hash(digest->text.substr(prefix.size())))
      return UpdateSelection::Invalid;
    selected.digest = lower(digest->text.substr(prefix.size()));
  } else {
    const auto sums = named_assets(*assets, "SHA256SUMS.txt");
    if (sums.size() != 1)
      return UpdateSelection::Invalid;  // No unambiguous SHA256 checksum asset.
    const auto* sums_url = string_member(*sums[0], "browser_download_url");
    const auto* sums_state = string_member(*sums[0], "state");
    const auto* sums_size = sums[0]->find("size");
    std::int64_t sums_bytes{};
    if (!sums_url || sums_url->text != base + "SHA256SUMS.txt" || !sums_state || sums_state->text != "uploaded" || !sums_size ||
        !sums_size->integer(1, MetadataLimit, sums_bytes))
      return UpdateSelection::Invalid;
    selected.sums_url = sums_url->text;
  }
  asset = std::move(selected);
  return UpdateSelection::Selected;
}
bool read_installer_checksum(std::string_view text, std::string_view name, std::string& digest) {
  std::string found;
  int count = 0;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (line.size() > 66 && hex_hash(line.substr(0, 64)) && line[64] == ' ' && (line[65] == ' ' || line[65] == '*') &&
        line.substr(66) == name) {
      found = lower(std::string(line.substr(0, 64)));
      ++count;
    }
  }
  if (count != 1)
    return false;  // Missing or ambiguous installer checksum.
  digest = std::move(found);
  return true;
}
Updater::~Updater() {
  stop();
  clean_cache(cache_);
}
void Updater::stop() {
  cancelled_ = true;
  {
    std::lock_guard lock(request_mutex_);
    if (request_) {
      WinHttpCloseHandle(request_);  // Cancels a pending synchronous call.
      request_ = nullptr;
    }
  }
  if (worker_.joinable())
    worker_.join();
}
bool Updater::track(HINTERNET request) {
  std::lock_guard lock(request_mutex_);
  if (cancelled_) {
    WinHttpCloseHandle(request);
    return false;
  }
  request_ = request;
  return true;
}
bool Updater::untrack() {
  std::lock_guard lock(request_mutex_);
  if (!request_)
    return false;  // stop() closed the request.
  WinHttpCloseHandle(request_);
  request_ = nullptr;
  return true;
}
bool Updater::download(HINTERNET session,
                       std::wstring url,
                       std::int64_t limit,
                       ULONGLONG deadline,
                       std::string* body,
                       HANDLE file,
                       std::int64_t& total) {
  total = 0;
  for (int redirect = 0; redirect <= 5; ++redirect) {
    CrackedUrl target;
    if (cancelled_ || GetTickCount64() > deadline || !crack_trusted_url(url, target))
      return false;
    HINTERNET connection = WinHttpConnect(session, target.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection)
      return false;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", target.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !track(request)) {
      WinHttpCloseHandle(connection);
      return false;
    }
    // Every redirect target is checked against the trusted hosts before it is requested.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
    DWORD status{}, size = sizeof(status);
    bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr) &&
              WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                  &size, WINHTTP_NO_HEADER_INDEX);
    std::wstring location;
    bool redirected = false;
    if (ok && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
      DWORD bytes{};
      WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                          WINHTTP_NO_HEADER_INDEX);
      if (bytes && bytes <= 32768 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        location.resize(bytes / sizeof(wchar_t));
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &bytes,
                                 WINHTTP_NO_HEADER_INDEX);
        location.resize(bytes / sizeof(wchar_t));
      } else {
        ok = false;
      }
      // Relative redirects stay on the current trusted host.
      if (ok && location.size() > 1 && location[0] == L'/' && location[1] != L'/')
        location = L"https://" + target.host + location;
      redirected = ok && redirect < 5;
      ok = false;
    } else if (ok && status != 200) {
      ok = false;
    }
    if (ok) {
      wchar_t length[32]{};
      DWORD length_size = sizeof(length);
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size,
                              WINHTTP_NO_HEADER_INDEX))
        ok = wcstoull(length, nullptr, 10) <= static_cast<ULONGLONG>(limit);
    }
    std::array<char, 65536> buffer{};
    while (ok) {
      DWORD read{};
      if (cancelled_ || GetTickCount64() > deadline || !WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
        ok = false;
      } else if (!read) {
        break;
      } else if (total + read > limit) {
        ok = false;
      } else {
        total += read;
        DWORD written{};
        if (body)
          body->append(buffer.data(), read);
        else
          ok = WriteFile(file, buffer.data(), read, &written, nullptr) && written == read;
      }
    }
    ok = untrack() && ok;
    WinHttpCloseHandle(connection);
    if (redirected) {
      url = std::move(location);
      continue;
    }
    return ok && total > 0;
  }
  return false;
}
void Updater::check(const std::wstring& cache, UpdateResult& result) {
  const ULONGLONG deadline = GetTickCount64() + CheckTimeoutMs;
  UpdateVersion current{};
  if (!parse_update_version(L"v" + wide(TAXI_CAM_VERSION) + L"-build." + std::to_wstring(TAXI_CAM_BUILD_NUMBER), current))
    return;
  struct Session {
    HINTERNET handle;
    ~Session() {
      if (handle)
        WinHttpCloseHandle(handle);
    }
  } session{WinHttpOpen(L"Taxi-Cam-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.handle)
    return;
  WinHttpSetTimeouts(session.handle, 10000, 10000, 30000, 30000);
  std::string body;
  std::int64_t total{};
  json::Value release;
  if (!download(session.handle, L"https://api.github.com/repos/" + wide(Repository) + L"/releases/latest", MetadataLimit, deadline, &body,
                nullptr, total) ||
      !json::parse(body, release))
    return;
  UpdateAsset asset;
  const auto selection = select_update_asset(release, current, asset);
  if (selection == UpdateSelection::Current)
    result.error.clear();
  if (selection != UpdateSelection::Selected)
    return;
  std::string digest = asset.digest;
  if (digest.empty()) {
    std::string sums;
    if (!download(session.handle, wide(asset.sums_url), MetadataLimit, deadline, &sums, nullptr, total) ||
        !read_installer_checksum(sums, asset.name, digest))
      return;
  }
  const std::wstring partial = cache + L"\\setup.exe.partial";
  const HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  const bool downloaded = download(session.handle, wide(asset.url), DownloadLimit, deadline, nullptr, file, total);
  CloseHandle(file);
  if (!downloaded || total != asset.size)
    return;
  const HANDLE checked = verified_update_file(partial, wide(digest));
  if (checked == INVALID_HANDLE_VALUE)
    return;  // Installer integrity check failed.
  CloseHandle(checked);
  const std::wstring installer = cache + L"\\setup.exe";
  if (!MoveFileExW(partial.c_str(), installer.c_str(), 0))
    return;
  result.tag = wide(asset.tag);
  result.sha256 = wide(digest);
  result.installer = installer;
  const HANDLE verified = verified_update_file(result.installer, result.sha256);
  if (verified != INVALID_HANDLE_VALUE) {
    CloseHandle(verified);
    result.available = true;
    result.error.clear();
  }
}
bool Updater::begin(const std::wstring&, bool manual) {
  if (busy_)
    return false;
  if (worker_.joinable())
    worker_.join();
  {
    std::lock_guard lock(mutex_);
    if (manual && result_.available && !cache_.empty() && result_.installer == cache_ + L"\\setup.exe") {
      result_.manual = true;
      ready_ = true;
      return true;  // Reuse a declined/blocked download; launch always verifies its bytes again.
    }
    ready_ = false;
  }
  clean_cache(cache_);
  cache_ = make_cache();
  cancelled_ = false;
  busy_ = true;
  worker_ = std::thread([this, cache = cache_, manual] {
    UpdateResult result;
    result.manual = manual;
    result.error = L"Could not check for updates. Please try again later.";
    if (!cache.empty())
      check(cache, result);
    {
      std::lock_guard lock(mutex_);
      result_ = std::move(result);
      ready_ = !cancelled_;
    }
    busy_ = false;
  });
  return true;
}
bool Updater::take(UpdateResult& result) {
  std::lock_guard lock(mutex_);
  if (!ready_)
    return false;
  result = result_;
  ready_ = false;
  return true;
}
bool Updater::launch(const UpdateResult& result, const std::wstring& installation, std::wstring& error) {
  {
    std::lock_guard lock(mutex_);
    result_.available = false;  // A failed handoff must permit a fresh check/download.
  }
  error = L"The installer could not be verified or started. Please check for updates again.";
  if (!result.available || cache_.empty() || result.installer != cache_ + L"\\setup.exe")
    return false;
  if (simulator_blocks_update()) {
    error = L"Close Microsoft Flight Simulator before installing this update, then check for updates again.";
    return false;
  }
  const auto arguments = update_installer_arguments(installation, GetCurrentProcessId());
  if (arguments.empty())
    return false;
  HANDLE verified = verified_update_file(result.installer, result.sha256);
  if (verified == INVALID_HANDLE_VALUE)
    return false;
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"open";
  execute.lpFile = result.installer.c_str();
  execute.lpParameters = arguments.c_str();
  execute.nShow = SW_SHOWNORMAL;
  const bool launched = ShellExecuteExW(&execute) && execute.hProcess;
  CloseHandle(verified);
  if (execute.hProcess)
    CloseHandle(execute.hProcess);
  if (launched)
    cache_.clear();  // The installer still needs its source; retain this unique directory.
  return launched;
}
}  // namespace taxi_camera::standalone
