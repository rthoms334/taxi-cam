#include "startup_xml.hpp"
#include <shlwapi.h>
#include <array>
#include <vector>
#include "setup_common.hpp"

namespace taxi_camera::setup {
namespace {
struct Bstr {
  BSTR value{};
  Bstr() = default;
  explicit Bstr(const std::wstring& text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
  ~Bstr() {
    if (value)
      SysFreeString(value);
  }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  std::wstring text() const { return value ? std::wstring(value, SysStringLen(value)) : std::wstring{}; }
};
void check(HRESULT result, const wchar_t* message) {
  if (FAILED(result))
    throw Failure(message, static_cast<DWORD>(result));
}
DOMNodeType node_type(IXMLDOMNode* node) {
  DOMNodeType type{};
  node->get_nodeType(&type);
  return type;
}
std::wstring node_name(IXMLDOMNode* node) {
  Bstr name;
  node->get_nodeName(&name.value);
  return name.text();
}
std::wstring namespace_uri(IXMLDOMNode* node) {
  Bstr uri;
  node->get_namespaceURI(&uri.value);
  return uri.text();
}
std::vector<ComPtr<IXMLDOMNode>> children(IXMLDOMNode* node) {
  std::vector<ComPtr<IXMLDOMNode>> result;
  ComPtr<IXMLDOMNode> child;
  node->get_firstChild(&child);
  while (child) {
    result.push_back(child);
    ComPtr<IXMLDOMNode> next;
    child->get_nextSibling(&next);
    child = next;
  }
  return result;
}
// Child elements with an exact name and no namespace, as XPath "name" selects them.
std::vector<ComPtr<IXMLDOMNode>> child_elements(IXMLDOMNode* node, const wchar_t* name = nullptr) {
  std::vector<ComPtr<IXMLDOMNode>> result;
  for (auto& child : children(node))
    if (node_type(child.Get()) == NODE_ELEMENT && (!name || (node_name(child.Get()) == name && namespace_uri(child.Get()).empty())))
      result.push_back(child);
  return result;
}
ComPtr<IXMLDOMNode> first_child_element(IXMLDOMNode* node, const wchar_t* name) {
  auto found = child_elements(node, name);
  return found.empty() ? nullptr : found[0];
}
// Concatenated text, CDATA and whitespace of all descendants, excluding comments.
std::wstring inner_text(IXMLDOMNode* node) {
  std::wstring text;
  for (auto& child : children(node)) {
    const auto type = node_type(child.Get());
    if (type == NODE_TEXT || type == NODE_CDATA_SECTION) {
      VARIANT value{};
      child->get_nodeValue(&value);
      if (value.vt == VT_BSTR && value.bstrVal)
        text.append(value.bstrVal, SysStringLen(value.bstrVal));
      VariantClear(&value);
    } else if (type == NODE_ELEMENT) {
      text += inner_text(child.Get());
    }
  }
  return text;
}
bool whitespace(const std::wstring& text) {
  return text.find_first_not_of(L" \t\r\n") == std::wstring::npos;
}
bool whitespace_text(IXMLDOMNode* node) {
  return node && node_type(node) == NODE_TEXT && whitespace(inner_text(node));
}
std::wstring node_text_value(IXMLDOMNode* node) {
  VARIANT value{};
  node->get_nodeValue(&value);
  std::wstring text = value.vt == VT_BSTR && value.bstrVal ? std::wstring(value.bstrVal, SysStringLen(value.bstrVal)) : L"";
  VariantClear(&value);
  return text;
}
void set_inner_text(IXMLDOMDocument2* document, IXMLDOMNode* node, const std::wstring& text) {
  for (auto& child : children(node)) {
    ComPtr<IXMLDOMNode> removed;
    check(node->removeChild(child.Get(), &removed), L"Could not update exe.xml.");
  }
  ComPtr<IXMLDOMText> value;
  check(document->createTextNode(Bstr(text).value, &value), L"Could not update exe.xml.");
  ComPtr<IXMLDOMNode> added;
  check(node->appendChild(value.Get(), &added), L"Could not update exe.xml.");
}
std::wstring attribute(IXMLDOMElement* element, const wchar_t* name) {
  VARIANT value{};
  element->getAttribute(Bstr(name).value, &value);
  std::wstring text = value.vt == VT_BSTR && value.bstrVal ? std::wstring(value.bstrVal, SysStringLen(value.bstrVal)) : L"";
  VariantClear(&value);
  return text;
}
ComPtr<IXMLDOMNode> append_text(IXMLDOMDocument2* document, IXMLDOMNode* parent, const std::wstring& text, IXMLDOMNode* before) {
  ComPtr<IXMLDOMText> node;
  check(document->createTextNode(Bstr(text).value, &node), L"Could not update exe.xml.");
  ComPtr<IXMLDOMNode> added;
  if (before) {
    VARIANT reference{};
    reference.vt = VT_UNKNOWN;
    reference.punkVal = before;
    check(parent->insertBefore(node.Get(), reference, &added), L"Could not update exe.xml.");
  } else {
    check(parent->appendChild(node.Get(), &added), L"Could not update exe.xml.");
  }
  return added;
}
ComPtr<IXMLDOMDocument2> new_document() {
  ComPtr<IXMLDOMDocument2> document;
  check(CoCreateInstance(CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document)),
        L"The Windows XML parser (MSXML 6) is unavailable.");
  document->put_async(VARIANT_FALSE);
  document->put_preserveWhiteSpace(VARIANT_TRUE);
  document->put_resolveExternals(VARIANT_FALSE);
  document->put_validateOnParse(VARIANT_FALSE);
  VARIANT prohibit{};
  prohibit.vt = VT_BOOL;
  prohibit.boolVal = VARIANT_TRUE;
  document->setProperty(Bstr(L"ProhibitDTD").value, prohibit);
  return document;
}
void load_bytes(IXMLDOMDocument2* document, const std::string& bytes) {
  IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
  if (!stream)
    fail(L"Could not read exe.xml.");
  VARIANT source{};
  source.vt = VT_UNKNOWN;
  source.punkVal = stream;
  VARIANT_BOOL loaded{};
  const HRESULT result = document->load(source, &loaded);
  stream->Release();
  if (FAILED(result) || loaded != VARIANT_TRUE) {
    ComPtr<IXMLDOMParseError> error;
    Bstr reason;
    if (SUCCEEDED(document->get_parseError(&error)) && error)
      error->get_reason(&reason.value);
    auto text = reason.text();
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
      text.pop_back();
    fail(L"exe.xml is not valid XML: " + text);
  }
}
void repair_launch_header(LaunchDocument& launch) {
  IXMLDOMElement* root = launch.root.Get();
  if (node_name(root) != L"SimBase.Document" || attribute(root, L"Type") != L"SimConnect" || !namespace_uri(root).empty())
    return;
  // Some add-ons write launch entries under a copied SimConnect header. Repair
  // only that recognizable launch-only shape. A real or mixed SimConnect
  // configuration must never become a launch file.
  constexpr std::array<const wchar_t*, 5> allowed{L"Descr", L"Filename", L"Disabled", L"Launch.ManualLoad", L"Launch.Addon"};
  for (auto& element : child_elements(root)) {
    Bstr local;
    element->get_baseName(&local.value);
    bool known = false;
    for (const auto* name : allowed)
      known = known || local.text() == name;
    if (!known || !namespace_uri(element.Get()).empty() || node_name(element.Get()) != local.text())
      return;
  }
  for (const auto* name : allowed)
    if (std::wstring(name) != L"Launch.Addon" && child_elements(root, name).size() > 1)
      return;
  for (const auto* name : {L"Descr", L"Filename"})
    for (auto& header : child_elements(root, name))
      for (auto& child : children(header.Get()))
        if (node_type(child.Get()) != NODE_TEXT && node_type(child.Get()) != NODE_CDATA_SECTION)
          return;
  auto filename = first_child_element(root, L"Filename");
  if (child_elements(root, L"Launch.Addon").empty() || !filename || inner_text(filename.Get()) != L"SimConnect.xml")
    return;
  VARIANT type{};
  type.vt = VT_BSTR;
  type.bstrVal = SysAllocString(L"Launch");
  root->setAttribute(Bstr(L"Type").value, type);
  VariantClear(&type);
  set_inner_text(launch.document.Get(), filename.Get(), L"exe.xml");
  auto description = first_child_element(root, L"Descr");
  if (!description) {
    ComPtr<IXMLDOMElement> created;
    check(launch.document->createElement(Bstr(L"Descr").value, &created), L"Could not update exe.xml.");
    ComPtr<IXMLDOMNode> first;
    root->get_firstChild(&first);
    VARIANT reference{};
    if (first) {
      reference.vt = VT_UNKNOWN;
      reference.punkVal = first.Get();
    }
    check(root->insertBefore(created.Get(), reference, &description), L"Could not update exe.xml.");
  }
  // Disabled/ManualLoad flags, add-on entries and comments stay as supplied.
  set_inner_text(launch.document.Get(), description.Get(), L"Launch");
}
bool taxi_entry_name(const std::wstring& name) {
  return equal_insensitive(name, L"Taxi Cam") || equal_insensitive(name, L"380 Taxi Cam");
}
std::vector<ComPtr<IXMLDOMNode>> taxi_entries(const LaunchDocument& launch) {
  std::vector<ComPtr<IXMLDOMNode>> found;
  for (auto& entry : child_elements(launch.root.Get(), L"Launch.Addon"))
    if (auto name = first_child_element(entry.Get(), L"Name"); name && taxi_entry_name(inner_text(name.Get())))
      found.push_back(entry);
  return found;
}
Failure annotated(Failure failure, const std::wstring& operation, const std::wstring& source, const std::wstring& destination) {
  failure.operation = operation;
  failure.source = source;
  failure.destination = destination;
  return failure;
}
Failure conflict(std::wstring message) {
  Failure failure(std::move(message));
  failure.conflict = true;
  return failure;
}
}  // namespace

LaunchDocument parse_launch_xml(const std::string& bytes, bool repair_header) {
  LaunchDocument launch;
  launch.document = new_document();
  load_bytes(launch.document.Get(), bytes);
  launch.document->get_documentElement(&launch.root);
  if (!launch.root)
    fail(L"Unrecognized exe.xml launch document.");
  if (repair_header)
    repair_launch_header(launch);
  if (!equal_insensitive(node_name(launch.root.Get()), L"SimBase.Document") ||
      !equal_insensitive(attribute(launch.root.Get(), L"Type"), L"Launch"))
    fail(L"Unrecognized exe.xml launch document.");
  return launch;
}
LaunchDocument read_launch_xml(const std::wstring& path, bool repair_header) {
  if (exists(path))
    return parse_launch_xml(read_file(path), repair_header);
  return parse_launch_xml(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<SimBase.Document Type=\"Launch\" version=\"1,0\">\r\n  <Descr>Launch</Descr>\r\n"
      "  <Filename>exe.xml</Filename>\r\n  <Disabled>False</Disabled>\r\n  <Launch.ManualLoad>False</Launch.ManualLoad>\r\n"
      "</SimBase.Document>\r\n");
}
std::string serialize_launch_xml(const LaunchDocument& launch) {
  IStream* stream = SHCreateMemStream(nullptr, 0);
  if (!stream)
    fail(L"Could not write exe.xml.");
  VARIANT target{};
  target.vt = VT_UNKNOWN;
  target.punkVal = stream;
  const HRESULT result = launch.document->save(target);
  STATSTG stat{};
  std::string bytes;
  if (SUCCEEDED(result) && SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart < (16u << 20)) {
    bytes.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG read{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size())
      bytes.clear();
  }
  stream->Release();
  if (bytes.empty())
    fail(L"Could not write exe.xml.");
  // MSXML drops whitespace outside the root element; keep the root on its own line.
  if (const auto end = bytes.find("?>");
      bytes.starts_with("<?xml") && end != std::string::npos && end + 2 < bytes.size() && bytes[end + 2] == '<')
    bytes.insert(end + 2, "\r\n");
  return bytes;
}
void set_startup_entry(LaunchDocument& launch, const std::wstring& executable, const std::wstring& simulator) {
  if (!path_rooted(executable) || !path_rooted(simulator))
    fail(L"Startup executable paths must be absolute.");
  auto* document = launch.document.Get();
  auto* root = launch.root.Get();
  // Recognize the former name so an upgrade replaces its entry in place.
  const auto matches = taxi_entries(launch);
  if (matches.size() > 1)
    fail(L"Multiple Taxi Cam startup entries found; refusing an ambiguous update.");
  ComPtr<IXMLDOMNode> entry = matches.empty() ? nullptr : matches[0];
  std::wstring indent;
  const bool created = !entry;
  if (created) {
    ComPtr<IXMLDOMElement> element;
    check(document->createElement(Bstr(L"Launch.Addon").value, &element), L"Could not update exe.xml.");
    entry = element;
    // Follow the indentation of the document's last element, when it has one.
    auto elements = child_elements(root);
    if (!elements.empty()) {
      ComPtr<IXMLDOMNode> previous;
      elements.back()->get_previousSibling(&previous);
      if (whitespace_text(previous.Get()))
        indent = node_text_value(previous.Get());
    }
  }
  const std::array<std::pair<const wchar_t*, std::wstring>, 6> values{{{L"Name", L"Taxi Cam"},
                                                                       {L"Disabled", L"False"},
                                                                       {L"ManualLoad", L"False"},
                                                                       {L"Path", executable},
                                                                       {L"CommandLine", L"--background --simulator \"" + simulator + L"\""},
                                                                       {L"NewConsole", L"False"}}};
  for (const auto& [name, value] : values) {
    auto node = first_child_element(entry.Get(), name);
    if (!node) {
      if (created && !indent.empty())
        append_text(document, entry.Get(), indent + L"  ", nullptr);
      ComPtr<IXMLDOMElement> element;
      check(document->createElement(Bstr(name).value, &element), L"Could not update exe.xml.");
      check(entry->appendChild(element.Get(), &node), L"Could not update exe.xml.");
    }
    set_inner_text(document, node.Get(), value);
  }
  if (created) {
    if (!indent.empty())
      append_text(document, entry.Get(), indent, nullptr);
    ComPtr<IXMLDOMNode> last;
    root->get_lastChild(&last);
    IXMLDOMNode* before = whitespace_text(last.Get()) ? last.Get() : nullptr;
    if (!indent.empty())
      append_text(document, root, indent, before);
    ComPtr<IXMLDOMNode> added;
    if (before) {
      VARIANT reference{};
      reference.vt = VT_UNKNOWN;
      reference.punkVal = before;
      check(root->insertBefore(entry.Get(), reference, &added), L"Could not update exe.xml.");
    } else {
      check(root->appendChild(entry.Get(), &added), L"Could not update exe.xml.");
    }
  }
}
unsigned remove_startup_entries(LaunchDocument& launch, const std::function<bool(const std::wstring& path)>& owned) {
  unsigned removed = 0;
  for (auto& entry : taxi_entries(launch)) {
    auto path = first_child_element(entry.Get(), L"Path");
    if (!path || !owned(inner_text(path.Get())))
      continue;
    // Remove the indentation that introduced this entry together with it.
    ComPtr<IXMLDOMNode> previous, gone;
    entry->get_previousSibling(&previous);
    if (whitespace_text(previous.Get()))
      launch.root->removeChild(previous.Get(), &gone);
    check(launch.root->removeChild(entry.Get(), &gone), L"Could not update exe.xml.");
    ++removed;
  }
  return removed;
}
bool startup_disabled_globally(const LaunchDocument& launch) {
  for (const auto* name : {L"Disabled", L"Launch.ManualLoad"})
    if (auto node = first_child_element(launch.root.Get(), name); node && equal_insensitive(inner_text(node.Get()), L"True"))
      return true;
  return false;
}
unsigned startup_entry_count(const LaunchDocument& launch) {
  return static_cast<unsigned>(taxi_entries(launch).size());
}

bool redirected_install_path(const std::wstring& requested, const std::wstring& physical) {
  const auto cache = [](const std::wstring& path) {
    const auto lowered = lower(path);
    for (auto at = lowered.find(L"\\appdata\\local\\packages\\"); at != std::wstring::npos;
         at = lowered.find(L"\\appdata\\local\\packages\\", at + 1)) {
      const auto package = at + 24;
      const auto end = lowered.find(L'\\', package);
      if (end == std::wstring::npos || end == package)
        continue;
      const auto rest = std::wstring_view(lowered).substr(end);
      if (rest.starts_with(L"\\localcache\\local\\") || rest.starts_with(L"\\localcache\\roaming\\"))
        return true;
    }
    return false;
  };
  return cache(physical) && !cache(full_path(requested));
}
std::wstring physical_file_path(const std::wstring& path) {
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail_win32(L"Could not open " + path);
  std::wstring physical(32768, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(file, physical.data(), static_cast<DWORD>(physical.size()), 0);
  const DWORD error = GetLastError();
  CloseHandle(file);
  if (!length || length >= physical.size())
    fail_win32(L"Could not resolve " + path, error);
  physical.resize(length);
  return physical;
}
void assert_visible_install_path(const std::wstring& path) {
  const auto physical = physical_file_path(path);
  if (redirected_install_path(path, physical))
    fail(L"Windows redirected the installation into a packaged app cache: " + physical + L". Choose a folder outside AppData, such as " +
         environment(L"USERPROFILE") + L"\\Apps\\Taxi Cam, or run the installer from a normal Windows terminal.");
}

void StartupIo::copy_backup(const std::wstring& source, const std::wstring& backup) {
  copy_file(source, backup, false);
}
DWORD StartupIo::file_attributes(const std::wstring& path) {
  const DWORD value = attributes(path);
  if (value == INVALID_FILE_ATTRIBUTES)
    fail_win32(L"Could not read attributes of " + path);
  return value;
}
void StartupIo::protect_like(const std::wstring& original, const std::wstring& replacement) {
  copy_file(original, replacement, true);
}
void StartupIo::assert_visible(const std::wstring& path) {
  assert_visible_install_path(path);
}

std::wstring save_launch_xml(const LaunchDocument& document,
                             const std::wstring& path,
                             const std::wstring& expected_hash,
                             std::wstring& written_hash,
                             bool require_visible_path,
                             StartupIo& io) {
  written_hash.clear();
  const auto absolute = full_path(path);
  if (!equal_insensitive(file_name(absolute), L"exe.xml"))
    fail(L"Startup target must be named exe.xml.");
  const auto parent = parent_path(absolute);
  std::wstring temporary, backup, operation = L"Prepare startup directory", source = absolute, destination = parent;
  bool backup_verified = false, committed = false, encrypted_known = false, encrypted = false;
  const auto is_encrypted = [&](const std::wstring& file) { return (io.file_attributes(file) & FILE_ATTRIBUTE_ENCRYPTED) != 0; };
  const auto cleanup_temporary = [&] {
    // Cleanup is best effort and cannot turn a committed write into an error.
    if (!temporary.empty() && is_file(temporary)) {
      SetFileAttributesW(temporary.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(temporary.c_str());
    }
  };
  try {
    create_directories(parent);
    operation = L"Verify original startup file";
    destination = absolute;
    if (exists(absolute)) {
      if (expected_hash.empty() || !equal_insensitive(sha256_file(absolute), expected_hash))
        throw conflict(L"exe.xml changed during installation; no startup entry was written.");
      encrypted = is_encrypted(absolute);
      encrypted_known = true;
    } else if (!expected_hash.empty()) {
      throw conflict(L"exe.xml disappeared during installation.");
    }
    // Reserve a private sibling and protect it before writing XML content. Never
    // copy encrypted user configuration into an unencrypted temporary folder.
    temporary = join(parent, L"exe.xml.taxi-" + new_guid() + L".tmp");
    operation = L"Create startup replacement";
    destination = temporary;
    HANDLE reserved = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reserved == INVALID_HANDLE_VALUE)
      fail_win32(L"Could not create " + temporary);
    CloseHandle(reserved);
    if (require_visible_path) {
      // Verify the actual destination before any XML is written or backed up,
      // so a private redirected copy cannot report automatic startup success.
      operation = L"Verify startup path visibility";
      io.assert_visible(temporary);
      if (exists(absolute))
        io.assert_visible(absolute);
    }
    if (encrypted) {
      operation = L"Encrypt startup replacement";
      // Store app data on a secondary drive encrypts new files on creation with
      // Windows-managed keys; only protect a replacement that did not inherit it.
      if (!is_encrypted(temporary))
        io.protect_like(absolute, temporary);
      if (!is_encrypted(temporary))
        fail(L"Could not preserve exe.xml encryption on the replacement file.");
    }
    operation = L"Write startup replacement";
    const auto bytes = serialize_launch_xml(document);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      fail_win32(L"Could not write " + temporary);
    DWORD wrote{};
    const bool written =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size() && FlushFileBuffers(file);
    const DWORD write_error = GetLastError();
    CloseHandle(file);
    if (!written)
      fail_win32(L"Could not write " + temporary, write_error);
    operation = L"Verify startup replacement";
    (void)read_launch_xml(temporary);
    const auto prepared_hash = sha256_file(temporary);
    if (!expected_hash.empty()) {
      operation = L"Verify original startup file before backup";
      destination = absolute;
      if (!exists(absolute) || !equal_insensitive(sha256_file(absolute), expected_hash))
        throw conflict(L"exe.xml changed before backup.");
      backup = absolute + L".taxi-backup-" + utc_file_tag() + L"-" + new_guid();
      operation = L"Back up startup file";
      destination = backup;
      io.copy_backup(absolute, backup);
      operation = L"Verify startup backup";
      if (!equal_insensitive(sha256_file(backup), expected_hash))
        throw conflict(L"Startup backup verification failed.");
      if (encrypted && !is_encrypted(backup))
        fail(L"Could not preserve exe.xml encryption on the backup file.");
      backup_verified = true;
      operation = L"Verify original startup file before replacement";
      destination = absolute;
      if (!equal_insensitive(sha256_file(absolute), expected_hash))
        throw conflict(L"exe.xml changed before replacement.");
      if (is_encrypted(absolute) != encrypted)
        throw conflict(L"exe.xml encryption changed before replacement.");
      operation = L"Replace startup file";
      source = temporary;
      replace_file(absolute, temporary);
    } else {
      operation = L"Create startup file";
      source = temporary;
      destination = absolute;
      if (exists(absolute))
        throw conflict(L"exe.xml appeared during installation; no startup entry was written.");
      move_file(temporary, absolute);
    }
    committed = true;
    // Nothing that accesses the filesystem may fail after the atomic commit.
    // Callers use this prepared digest to distinguish our write from later edits.
    written_hash = prepared_hash;
    cleanup_temporary();
    return backup;
  } catch (const Failure& caught) {
    Failure failure = annotated(caught, operation, source, destination);
    if (!backup.empty() && !backup_verified) {
      // A failed copy can leave partial or plaintext data. Delete only this
      // invocation's unverified backup before any manual-startup fallback.
      if (!DeleteFileW(backup.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        Failure cleanup(L"Could not remove unverified startup backup '" + backup + L"': " + failure.message, GetLastError());
        cleanup.unsafe = true;
        cleanup = annotated(cleanup, L"Remove unverified startup backup", backup, backup);
        cleanup_temporary();
        throw cleanup;
      }
    }
    if (!failure.conflict) {
      bool unchanged = false;
      if (!committed) {
        try {
          if (!expected_hash.empty()) {
            unchanged = is_file(absolute) && equal_insensitive(sha256_file(absolute), expected_hash);
            if (unchanged && encrypted_known)
              unchanged = is_encrypted(absolute) == encrypted;
          } else {
            unchanged = !exists(absolute);
          }
        } catch (const Failure&) {
          unchanged = false;
        }
      }
      if (!unchanged)
        failure.unsafe = true;
    }
    cleanup_temporary();
    throw failure;
  }
}
std::wstring save_launch_xml(const LaunchDocument& document,
                             const std::wstring& path,
                             const std::wstring& expected_hash,
                             std::wstring& written_hash,
                             bool require_visible_path) {
  StartupIo io;
  return save_launch_xml(document, path, expected_hash, written_hash, require_visible_path, io);
}
}  // namespace taxi_camera::setup
