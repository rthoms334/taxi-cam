#include "setup_common.hpp"
#include <bcrypt.h>
#include <array>
#include <vector>

namespace taxi_camera::setup {
Failure::Failure(std::wstring text, DWORD code) : message(std::move(text)), win32(code), utf8(narrow(message)) {}
std::wstring Failure::detail() const {
  std::wstring text = message;
  if (win32)
    text += L"\r\nWindows error: " + std::to_wstring(win32);
  if (!operation.empty())
    text += L"\r\nOperation: " + operation;
  if (!source.empty())
    text += L"\r\nSource: " + source;
  if (!destination.empty())
    text += L"\r\nDestination: " + destination;
  return text;
}
void fail(std::wstring message) {
  throw Failure(std::move(message));
}
void fail_win32(std::wstring message, DWORD code) {
  throw Failure(std::move(message), code);
}

std::wstring widen(std::string_view utf8) {
  if (utf8.empty())
    return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring text(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), text.data(), size);
  return text;
}
std::string narrow(std::wstring_view text) {
  if (text.empty())
    return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
  return bytes;
}
bool equal_insensitive(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool starts_with_insensitive(std::wstring_view text, std::wstring_view prefix) {
  return text.size() >= prefix.size() && equal_insensitive(text.substr(0, prefix.size()), prefix);
}
std::wstring lower(std::wstring text) {
  if (!text.empty())
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
  return text;
}

std::wstring full_path(const std::wstring& path) {
  if (path.empty())
    fail(L"A path is required.");
  const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!size)
    fail_win32(L"Invalid path: " + path);
  std::wstring result(size, L'\0');
  const DWORD written = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
  if (!written || written >= size)
    fail_win32(L"Invalid path: " + path);
  result.resize(written);
  return result;
}
std::wstring parent_path(const std::wstring& path) {
  const auto end = path.find_last_of(L"\\/");
  if (end == std::wstring::npos)
    return {};
  std::wstring parent = path.substr(0, end);
  // Keep the separator of a drive root such as C:\.
  if (parent.size() == 2 && parent[1] == L':')
    return path.size() > 3 ? parent + L"\\" : std::wstring{};
  if (parent.empty())
    return {};
  return parent;
}
std::wstring file_name(const std::wstring& path) {
  const auto end = path.find_last_of(L"\\/");
  return end == std::wstring::npos ? path : path.substr(end + 1);
}
std::wstring join(const std::wstring& base, std::wstring_view child) {
  std::wstring relative(child);
  for (auto& c : relative)
    if (c == L'/')
      c = L'\\';
  if (base.empty())
    return relative;
  if (base.back() == L'\\' || base.back() == L'/')
    return base + relative;
  return base + L"\\" + relative;
}
bool path_rooted(const std::wstring& path) {
  return (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
         (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'));
}
std::wstring path_root(const std::wstring& path) {
  if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
    return path.substr(0, 3);
  return {};
}
std::wstring trim_separators(std::wstring path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    path.pop_back();
  return path;
}

DWORD attributes(const std::wstring& path) {
  return GetFileAttributesW(path.c_str());
}
bool exists(const std::wstring& path) {
  return attributes(path) != INVALID_FILE_ATTRIBUTES;
}
bool is_file(const std::wstring& path) {
  const DWORD value = attributes(path);
  return value != INVALID_FILE_ATTRIBUTES && !(value & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_directory(const std::wstring& path) {
  const DWORD value = attributes(path);
  return value != INVALID_FILE_ATTRIBUTES && (value & FILE_ATTRIBUTE_DIRECTORY);
}
void create_directories(const std::wstring& path) {
  if (path.empty() || is_directory(path))
    return;
  const auto parent = parent_path(path);
  if (!parent.empty() && parent != path)
    create_directories(parent);
  if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    fail_win32(L"Could not create directory: " + path);
}
namespace {
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
}  // namespace
std::string read_file(const std::wstring& path) {
  Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr)};
  if (file.value == INVALID_HANDLE_VALUE)
    fail_win32(L"Could not read file: " + path);
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.value, &size) || size.QuadPart > 512LL * 1024 * 1024)
    fail_win32(L"Could not read file: " + path);
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t total = 0;
  while (total < bytes.size()) {
    DWORD read{};
    if (!ReadFile(file.value, bytes.data() + total, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - total, 1 << 20)), &read,
                  nullptr))
      fail_win32(L"Could not read file: " + path);
    if (!read)
      break;
    total += read;
  }
  bytes.resize(total);
  return bytes;
}
void write_file(const std::wstring& path, std::string_view bytes) {
  Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE)
    fail_win32(L"Could not write file: " + path);
  std::size_t total = 0;
  while (total < bytes.size()) {
    DWORD written{};
    if (!WriteFile(file.value, bytes.data() + total, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - total, 1 << 20)), &written,
                   nullptr) ||
        !written)
      fail_win32(L"Could not write file: " + path);
    total += written;
  }
}
void copy_file(const std::wstring& source, const std::wstring& destination, bool overwrite) {
  if (!CopyFileExW(source.c_str(), destination.c_str(), nullptr, nullptr, nullptr, overwrite ? 0 : COPY_FILE_FAIL_IF_EXISTS))
    fail_win32(L"Could not copy '" + source + L"' to '" + destination + L"'.");
}
void move_file(const std::wstring& source, const std::wstring& destination) {
  if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
    fail_win32(L"Could not move '" + source + L"' to '" + destination + L"'.");
}
void replace_file(const std::wstring& replaced, const std::wstring& replacement) {
  if (!ReplaceFileW(replaced.c_str(), replacement.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
    fail_win32(L"Could not replace '" + replaced + L"'.");
}
void delete_file(const std::wstring& path) {
  if (!DeleteFileW(path.c_str()))
    fail_win32(L"Could not delete file: " + path);
}
void remove_directory_tree(const std::wstring& path) {
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileExW(join(path, L"*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = entry.cFileName;
      if (name == L"." || name == L"..")
        continue;
      const auto child = join(path, name);
      // Never follow a junction or symbolic link out of the tree being removed.
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        remove_directory_tree(child);
      else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        RemoveDirectoryW(child.c_str());
      else
        delete_file(child);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  if (!RemoveDirectoryW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    fail_win32(L"Could not remove directory: " + path);
}
namespace {
std::wstring hex_upper(const unsigned char* data, std::size_t size) {
  std::wstring text;
  for (std::size_t i = 0; i < size; ++i) {
    text += L"0123456789ABCDEF"[data[i] >> 4];
    text += L"0123456789ABCDEF"[data[i] & 15];
  }
  return text;
}
struct Sha256 {
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
      fail(L"SHA-256 is unavailable.");
  }
  ~Sha256() {
    if (hash)
      BCryptDestroyHash(hash);
    if (algorithm)
      BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  void add(const void* data, std::size_t size) {
    if (size && BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) < 0)
      fail(L"SHA-256 failed.");
  }
  std::wstring finish() {
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
      fail(L"SHA-256 failed.");
    return hex_upper(digest.data(), digest.size());
  }
};
}  // namespace
std::wstring sha256_file(const std::wstring& path) {
  Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr)};
  if (file.value == INVALID_HANDLE_VALUE)
    fail_win32(L"Could not hash file: " + path);
  Sha256 hash;
  std::vector<char> buffer(1 << 16);
  for (;;) {
    DWORD read{};
    if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
      fail_win32(L"Could not hash file: " + path);
    if (!read)
      break;
    hash.add(buffer.data(), read);
  }
  return hash.finish();
}
std::wstring file_hash_or_empty(const std::wstring& path) {
  return is_file(path) ? sha256_file(path) : std::wstring{};
}

std::wstring new_guid() {
  std::array<unsigned char, 16> random{};
  if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    fail(L"Random identifiers are unavailable.");
  return lower(hex_upper(random.data(), random.size()));
}
namespace {
std::wstring two(unsigned value) {
  return (value < 10 ? L"0" : L"") + std::to_wstring(value);
}
}  // namespace
std::wstring utc_file_tag() {
  SYSTEMTIME t{};
  GetSystemTime(&t);
  std::wstring ms = std::to_wstring(t.wMilliseconds);
  ms.insert(0, 3 - ms.size(), L'0');
  return std::to_wstring(t.wYear) + two(t.wMonth) + two(t.wDay) + L"-" + two(t.wHour) + two(t.wMinute) + two(t.wSecond) + L"-" + ms;
}
std::wstring utc_iso_timestamp() {
  FILETIME now{};
  GetSystemTimePreciseAsFileTime(&now);
  SYSTEMTIME t{};
  FileTimeToSystemTime(&now, &t);
  ULARGE_INTEGER ticks{};
  ticks.LowPart = now.dwLowDateTime;
  ticks.HighPart = now.dwHighDateTime;
  std::wstring fraction = std::to_wstring(ticks.QuadPart % 10000000);
  fraction.insert(0, 7 - fraction.size(), L'0');
  return std::to_wstring(t.wYear) + L"-" + two(t.wMonth) + L"-" + two(t.wDay) + L"T" + two(t.wHour) + L":" + two(t.wMinute) + L":" +
         two(t.wSecond) + L"." + fraction + L"Z";
}
std::wstring environment(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size)
    return {};
  std::wstring value(size, L'\0');
  const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
  value.resize(written < size ? written : 0);
  return value;
}
std::wstring temp_directory() {
  wchar_t path[MAX_PATH + 1]{};
  const DWORD size = GetTempPathW(MAX_PATH + 1, path);
  if (!size || size > MAX_PATH)
    fail_win32(L"The temporary directory is unavailable.");
  return path;
}
std::wstring system_directory() {
  wchar_t path[MAX_PATH]{};
  const UINT size = GetSystemDirectoryW(path, MAX_PATH);
  if (!size || size >= MAX_PATH)
    fail_win32(L"The Windows system directory is unavailable.");
  return path;
}
}  // namespace taxi_camera::setup
