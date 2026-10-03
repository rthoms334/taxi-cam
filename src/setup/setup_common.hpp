#pragma once
#include <windows.h>
#include <exception>
#include <string>
#include <string_view>

// Shared file, path, hashing and error helpers for the native setup helper.
// Every operation reports failure as setup::Failure with the Windows error code.
namespace taxi_camera::setup {
struct Failure : std::exception {
  std::wstring message;
  DWORD win32{};
  // A concurrent writer changed a file this transaction guards; never continue.
  bool conflict{};
  // The original state could not be proved unchanged; never fall back silently.
  bool unsafe{};
  std::wstring operation, source, destination;
  std::string utf8;

  explicit Failure(std::wstring text, DWORD code = 0);
  const char* what() const noexcept override { return utf8.c_str(); }
  // Message, Windows error and the exact file operation, one item per line.
  std::wstring detail() const;
};
[[noreturn]] void fail(std::wstring message);
[[noreturn]] void fail_win32(std::wstring message, DWORD code = GetLastError());

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view text);
bool equal_insensitive(std::wstring_view a, std::wstring_view b);
bool starts_with_insensitive(std::wstring_view text, std::wstring_view prefix);
std::wstring lower(std::wstring text);

std::wstring full_path(const std::wstring& path);
// Directory part of a path, or empty for a root or a bare name.
std::wstring parent_path(const std::wstring& path);
std::wstring file_name(const std::wstring& path);
// Appends a relative child, accepting either separator in the child.
std::wstring join(const std::wstring& base, std::wstring_view child);
bool path_rooted(const std::wstring& path);
std::wstring path_root(const std::wstring& path);
std::wstring trim_separators(std::wstring path);

bool exists(const std::wstring& path);
bool is_file(const std::wstring& path);
bool is_directory(const std::wstring& path);
DWORD attributes(const std::wstring& path);  // INVALID_FILE_ATTRIBUTES when absent.
void create_directories(const std::wstring& path);
std::string read_file(const std::wstring& path);
// Writes all bytes, creating or truncating the file.
void write_file(const std::wstring& path, std::string_view bytes);
void copy_file(const std::wstring& source, const std::wstring& destination, bool overwrite);
void move_file(const std::wstring& source, const std::wstring& destination);
void replace_file(const std::wstring& replaced, const std::wstring& replacement);
void delete_file(const std::wstring& path);
void remove_directory_tree(const std::wstring& path);

// Uppercase SHA-256 hex, matching the hashes in validation receipts and records.
std::wstring sha256_file(const std::wstring& path);
// Empty when the file is absent.
std::wstring file_hash_or_empty(const std::wstring& path);

std::wstring new_guid();           // 32 lowercase hex digits.
std::wstring utc_file_tag();       // yyyyMMdd-HHmmss-fff
std::wstring utc_iso_timestamp();  // Round-trip ISO 8601 UTC.
std::wstring environment(const wchar_t* name);
std::wstring temp_directory();
std::wstring system_directory();
}  // namespace taxi_camera::setup
