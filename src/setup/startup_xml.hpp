#pragma once
#include <windows.h>

#include <msxml6.h>
#include <wrl/client.h>
#include <functional>
#include <string>

// MSFS exe.xml launch documents. Unrelated entries, comments and whitespace are
// preserved; every write is a verified-backup, atomic sibling replacement.
namespace taxi_camera::setup {
using Microsoft::WRL::ComPtr;

struct LaunchDocument {
  ComPtr<IXMLDOMDocument2> document;
  ComPtr<IXMLDOMElement> root;
};
// Reads an existing launch document, or prepares a new one when the file is
// absent. repair_header converts only a recognizable launch-only document that
// carries a copied SimConnect header, in memory.
LaunchDocument read_launch_xml(const std::wstring& path, bool repair_header = false);
LaunchDocument parse_launch_xml(const std::string& bytes, bool repair_header = false);
std::string serialize_launch_xml(const LaunchDocument& document);
// Adds or updates the single Taxi Cam entry; a former-name entry is renamed in place.
void set_startup_entry(LaunchDocument& document, const std::wstring& executable, const std::wstring& simulator);
// Removes Taxi Cam entries (current or former name) that the predicate accepts by Path.
unsigned remove_startup_entries(LaunchDocument& document, const std::function<bool(const std::wstring& path)>& owned);
// True when the document's global Disabled or Launch.ManualLoad flag is True.
bool startup_disabled_globally(const LaunchDocument& document);
unsigned startup_entry_count(const LaunchDocument& document);

// Packaged desktop hosts can redirect AppData writes into their own cache. Hash
// checks succeed there, but MSFS cannot see the requested path.
bool redirected_install_path(const std::wstring& requested, const std::wstring& physical);
std::wstring physical_file_path(const std::wstring& path);
void assert_visible_install_path(const std::wstring& path);

// File operations used by save_launch_xml, replaceable by isolated fault tests.
struct StartupIo {
  virtual ~StartupIo() = default;
  virtual void copy_backup(const std::wstring& source, const std::wstring& backup);
  virtual DWORD file_attributes(const std::wstring& path);
  // Gives the empty replacement the original's EFS protection by copying the
  // original over it; Windows encrypts the copy or refuses it.
  virtual void protect_like(const std::wstring& original, const std::wstring& replacement);
  virtual void assert_visible(const std::wstring& path);
};
// Writes the document to path when its current SHA-256 equals expected_hash
// (empty: the file must not exist). Returns the verified sibling backup, or an
// empty string for a new file. written_hash receives the committed bytes' hash.
// Failures carry Failure::conflict for a concurrent writer and Failure::unsafe
// when the original state cannot be proved unchanged.
std::wstring save_launch_xml(const LaunchDocument& document,
                             const std::wstring& path,
                             const std::wstring& expected_hash,
                             std::wstring& written_hash,
                             bool require_visible_path,
                             StartupIo& io);
std::wstring save_launch_xml(const LaunchDocument& document,
                             const std::wstring& path,
                             const std::wstring& expected_hash,
                             std::wstring& written_hash,
                             bool require_visible_path = false);
}  // namespace taxi_camera::setup
