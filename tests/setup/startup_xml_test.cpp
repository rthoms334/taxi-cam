#include "../../src/setup/startup_xml.hpp"
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "../../src/setup/setup_common.hpp"

using namespace taxi_camera::setup;
namespace {
int failures = 0, checks = 0;
void expect(bool ok, const char* name) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
  }
}
std::wstring root_directory;
std::wstring fixture(const std::wstring& name) {
  const auto path = join(root_directory, name);
  create_directories(path);
  return path;
}
std::vector<std::wstring> matching(const std::wstring& directory, const std::wstring& pattern) {
  std::vector<std::wstring> found;
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileW(join(directory, pattern).c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE)
    return found;
  do
    if (std::wstring(entry.cFileName) != L"." && std::wstring(entry.cFileName) != L"..")
      found.push_back(join(directory, entry.cFileName));
  while (FindNextFileW(search, &entry));
  FindClose(search);
  return found;
}
std::string text(const LaunchDocument& document) {
  return serialize_launch_xml(document);
}
bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}
template <class Action>
bool throws(Action action, Failure* out = nullptr) {
  try {
    action();
  } catch (const Failure& failure) {
    if (out)
      *out = failure;
    return true;
  }
  return false;
}
const std::wstring Exe = L"C:\\Native Camera & Tools\\taxi-cam.exe";
const std::wstring Sim = L"C:\\MSFS\\FlightSimulator2024.exe";

void preserve_and_update() {
  const auto directory = fixture(L"preserve");
  const auto path = join(directory, L"exe.xml");
  write_file(
      path,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<SimBase.Document Type=\"Launch\" version=\"1,0\">\r\n"
      "<!-- An unrelated add-on owns this comment -->\r\n<Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad>\r\n"
      "<Launch.Addon><Name>Existing &amp; Addon</Name><Path>C:\\Other App\\other.exe</Path><CommandLine>--keep=\"exact\"</CommandLine>"
      "<Disabled>True</Disabled><Custom>preserved</Custom></Launch.Addon>\r\n</SimBase.Document>\r\n");
  const std::string untouched =
      "<Launch.Addon><Name>Existing &amp; Addon</Name><Path>C:\\Other App\\other.exe</Path><CommandLine>--keep=\"exact\"</CommandLine>"
      "<Disabled>True</Disabled><Custom>preserved</Custom></Launch.Addon>";
  const auto original = sha256_file(path);
  auto document = read_launch_xml(path);
  set_startup_entry(document, Exe, Sim);
  std::wstring written;
  const auto backup = save_launch_xml(document, path, original, written);
  expect(!backup.empty() && sha256_file(backup) == original, "original startup file preserved in backup");
  expect(written == sha256_file(path), "written hash describes committed bytes");
  auto read = read_launch_xml(path);
  const auto saved = text(read);
  expect(contains(saved, untouched), "unrelated entry unchanged");
  expect(contains(saved, "<!-- An unrelated add-on owns this comment -->"), "comment preserved");
  expect(contains(saved, "<CommandLine>--background --simulator \"C:\\MSFS\\FlightSimulator2024.exe\"</CommandLine>"),
         "argument quoting exact");
  expect(contains(saved, "<Path>C:\\Native Camera &amp; Tools\\taxi-cam.exe</Path>"), "escaped executable path");
  set_startup_entry(read, Exe, Sim);
  expect(startup_entry_count(read) == 1, "no duplicate startup entry");
  // A former-name entry is migrated in place.
  auto renamed = parse_launch_xml(
      "<SimBase.Document Type=\"Launch\"><Launch.Addon><Name>380 Taxi Cam</Name><Path>C:\\Old\\380-taxi-cam.exe</Path></Launch.Addon>"
      "<Launch.Addon><Name>Other</Name></Launch.Addon></SimBase.Document>");
  set_startup_entry(renamed, Exe, Sim);
  expect(startup_entry_count(renamed) == 1 && !contains(text(renamed), "380 Taxi Cam") && contains(text(renamed), "<Name>Other</Name>"),
         "rename migrates existing entry in place");
  auto mixed = parse_launch_xml(
      "<SimBase.Document Type=\"Launch\"><Launch.Addon><Name>Taxi Cam</Name></Launch.Addon><Launch.Addon><Name>380 Taxi Cam</Name>"
      "</Launch.Addon></SimBase.Document>");
  expect(throws([&] { set_startup_entry(mixed, Exe, Sim); }), "mixed old/new entries refuse an ambiguous upgrade");
  Failure failure(L"");
  expect(throws([&] { save_launch_xml(read, path, std::wstring(64, L'0'), written); }, &failure) && failure.conflict,
         "concurrent edit guard refuses");
  const auto removed = remove_startup_entries(read, [](const std::wstring& entry) { return equal_insensitive(entry, Exe); });
  expect(removed == 1 && startup_entry_count(read) == 0 && contains(text(read), untouched), "targeted removal keeps unrelated startup");
  auto other_owner = read_launch_xml(path);
  expect(remove_startup_entries(other_owner, [](const std::wstring&) { return false; }) == 0 && startup_entry_count(other_owner) == 1,
         "entries for another installation are kept");
  write_file(join(directory, L"dtd.xml"),
             "<!DOCTYPE SimBase.Document [<!ENTITY external SYSTEM \"file:///C:/Windows/win.ini\">]><SimBase.Document "
             "Type=\"Launch\">&external;</SimBase.Document>");
  expect(throws([&] { read_launch_xml(join(directory, L"dtd.xml")); }), "external XML entity refused");
  auto created = read_launch_xml(join(directory, L"absent\\exe.xml"));
  set_startup_entry(created, Exe, Sim);
  expect(contains(text(created), "<Launch.ManualLoad>False</Launch.ManualLoad>") && startup_entry_count(created) == 1,
         "new launch document");
  auto flags = parse_launch_xml("<SimBase.Document Type=\"Launch\"><Disabled>true</Disabled></SimBase.Document>");
  expect(startup_disabled_globally(flags), "global disabled flag detected");
  flags = parse_launch_xml("<SimBase.Document Type=\"Launch\"><Launch.ManualLoad>True</Launch.ManualLoad></SimBase.Document>");
  expect(startup_disabled_globally(flags), "global manual-load flag detected");
  expect(!startup_disabled_globally(read), "enabled launch document");
}

void header_repair() {
  const std::string launch = "<Launch.Addon><Name>Keep Me</Name><Path>C:\\Other.exe</Path></Launch.Addon>";
  const std::string contents =
      "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<SimBase.Document Type=\"SimConnect\" version=\"1,0\">\r\n"
      "  <Descr>SimConnect</Descr><Filename>SimConnect.xml</Filename>\r\n  <Disabled>False</Disabled>"
      "<Launch.ManualLoad>False</Launch.ManualLoad>\r\n  <!-- Preserve the first add-on's startup details -->\r\n"
      "  <Launch.Addon><Name>Synaptic A220</Name><Path>C:\\Other Aircraft\\Synaptic.exe</Path><CommandLine>--keep=\"exact &amp; "
      "intact\"</CommandLine><Disabled>False</Disabled><Custom>preserved</Custom></Launch.Addon>\r\n"
      "  <Launch.Addon><!-- Keep the add-on's own comment --><Name>FSRealistic</Name><Path>C:\\Other App\\FSRealistic.exe</Path>"
      "<Disabled>True</Disabled><ManualLoad>True</ManualLoad></Launch.Addon>\r\n</SimBase.Document>\r\n";
  const auto directory = fixture(L"repair");
  const auto path = join(directory, L"exe.xml");
  write_file(path, contents);
  const auto hash = sha256_file(path);
  expect(throws([&] { read_launch_xml(path); }), "default reading does not repair a SimConnect header");
  auto document = read_launch_xml(path, true);
  const auto repaired = text(document);
  expect(contains(repaired, "Type=\"Launch\"") && contains(repaired, "<Descr>Launch</Descr>") &&
             contains(repaired, "<Filename>exe.xml</Filename>") && contains(repaired, "version=\"1,0\""),
         "opt-in repair normalizes only the launch header");
  expect(sha256_file(path) == hash && matching(directory, L"*").size() == 1, "repair is memory-only before commit");
  set_startup_entry(document, Exe, Sim);
  std::wstring written;
  const auto backup = save_launch_xml(document, path, hash, written);
  expect(!backup.empty() && sha256_file(backup) == hash, "repair backup holds exact original bytes");
  expect(written == sha256_file(path), "repair reports committed hash");
  auto saved = read_launch_xml(path);
  const auto round = text(saved);
  expect(startup_entry_count(saved) == 1 && contains(round, "<Name>Synaptic A220</Name>") && contains(round, "<Name>FSRealistic</Name>") &&
             contains(round, "<!-- Keep the add-on's own comment -->") && contains(round, "intact\"</CommandLine>"),
         "repair keeps add-ons and comments");
  auto again = read_launch_xml(path, true);
  expect(text(again) == round, "opt-in reading leaves a valid launch document unchanged");
  set_startup_entry(again, Exe, Sim);
  expect(startup_entry_count(again) == 1, "repaired file is idempotent");

  const std::string headers =
      "<Descr>SimConnect</Descr><Filename>SimConnect.xml</Filename><Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad>";
  std::vector<std::string> refusals{
      "<SimBase.Document Type=\"SimConnect\">" + headers +
          "<SimConnect.Comm><Protocol>IPv4</Protocol></SimConnect.Comm></SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\">" + headers + launch +
          "<SimConnect.Comm><Protocol>IPv4</Protocol></SimConnect.Comm></SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\">" + headers + launch + "<Unexpected>Keep</Unexpected></SimBase.Document>",
      "<Unexpected Type=\"SimConnect\">" + headers + launch + "</Unexpected>",
      "<SimBase.Document Type=\"Other\">" + headers + launch + "</SimBase.Document>",
      "<SimBase.Document xmlns=\"urn:other-startup\" Type=\"SimConnect\">" + headers + launch + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\">" + headers + launch +
          "<Disabled xmlns=\"urn:other-startup\">False</Disabled></SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\">" + headers + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Descr>SimConnect</Descr>" + launch + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Filename>Other.xml</Filename>" + launch + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Filename>SimConnect.xml </Filename>" + launch + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Filename><Value>SimConnect.xml</Value></Filename>" + launch + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Filename><!-- Preserve me -->SimConnect.xml</Filename>" + launch + "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Descr><Value>SimConnect</Value></Descr><Filename>SimConnect.xml</Filename>" + launch +
          "</SimBase.Document>",
      "<SimBase.Document Type=\"SimConnect\"><Descr><!-- Preserve me -->SimConnect</Descr><Filename>SimConnect.xml</Filename>" + launch +
          "</SimBase.Document>"};
  for (const auto& [header, value] : std::vector<std::pair<std::string, std::string>>{
           {"Descr", "SimConnect"}, {"Filename", "SimConnect.xml"}, {"Disabled", "False"}, {"Launch.ManualLoad", "False"}})
    refusals.push_back("<SimBase.Document Type=\"SimConnect\">" + headers + "<" + header + ">" + value + "</" + header + ">" + launch +
                       "</SimBase.Document>");
  for (const auto& refusal : refusals)
    expect(throws([&] { parse_launch_xml(refusal, true); }), "unsafe or ambiguous header repair refused");
  for (const auto& [disabled, manual] :
       std::vector<std::pair<std::string, std::string>>{{"True", "False"}, {"False", "True"}, {"True", "True"}}) {
    auto flagged =
        parse_launch_xml("<SimBase.Document Type=\"SimConnect\"><Descr>SimConnect</Descr><Filename>SimConnect.xml</Filename><Disabled>" +
                             disabled + "</Disabled><Launch.ManualLoad>" + manual + "</Launch.ManualLoad>" + launch + "</SimBase.Document>",
                         true);
    set_startup_entry(flagged, Exe, Sim);
    expect(contains(text(flagged), "<Disabled>" + disabled + "</Disabled><Launch.ManualLoad>" + manual + "</Launch.ManualLoad>") &&
               startup_disabled_globally(flagged),
           "repair keeps global disabled/manual flags");
  }
}

// Fault injection for the backup, encryption and visibility steps.
struct FaultIo : StartupIo {
  std::wstring copy_failure;
  std::set<std::wstring> encrypted;
  bool inherit_encryption{}, refuse_protection{}, protect_called{};
  std::wstring refuse_visible;
  std::vector<std::wstring> visible_calls;
  void copy_backup(const std::wstring& source, const std::wstring& backup) override {
    if (copy_failure == L"encryption")
      throw Failure(L"Fixture encryption failure.", 6000);
    if (copy_failure == L"permission")
      throw Failure(L"Fixture denies the backup operation.", ERROR_ACCESS_DENIED);
    if (copy_failure == L"partial-backup") {
      write_file(backup, "<SimBase.Document");
      throw Failure(L"Fixture partial backup.", 6000);
    }
    if (copy_failure == L"cleanup-failure") {
      write_file(backup, "<SimBase.Document");
      SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_READONLY);
      throw Failure(L"Fixture partial backup.", 6000);
    }
    if (copy_failure == L"changed-source") {
      auto bytes = read_file(source);
      write_file(source, bytes + "<!-- concurrent edit during failed backup -->");
      throw Failure(L"Fixture changed source.", 6000);
    }
    if (copy_failure == L"corrupt-backup") {
      write_file(backup, "not the original XML");
      return;
    }
    StartupIo::copy_backup(source, backup);
  }
  DWORD file_attributes(const std::wstring& path) override {
    DWORD value = StartupIo::file_attributes(path);
    if (encrypted.count(path) || (inherit_encryption && path.find(L".taxi-") != std::wstring::npos))
      value |= FILE_ATTRIBUTE_ENCRYPTED;
    return value;
  }
  void protect_like(const std::wstring&, const std::wstring& replacement) override {
    protect_called = true;
    if (refuse_protection)
      throw Failure(L"Access to the path '" + replacement + L"' is denied.", ERROR_ACCESS_DENIED);
    encrypted.insert(replacement);
  }
  void assert_visible(const std::wstring& path) override {
    visible_calls.push_back(path);
    if (path.find(L".tmp") != std::wstring::npos && (!is_file(path) || !read_file(path).empty()))
      fail(L"Visibility check did not precede writing XML to an existing empty sibling.");
    if (!refuse_visible.empty() && path.find(refuse_visible) != std::wstring::npos)
      fail(L"Fixture detects a redirected path.");
  }
};

void failure_resilience() {
  const auto directory = fixture(L"failures");
  const auto path = join(directory, L"exe.xml");
  const std::string contents =
      "<SimBase.Document Type=\"Launch\"><Launch.Addon><Name>Keep Me</Name><Path>C:\\Other.exe</Path></Launch.Addon></SimBase.Document>";
  write_file(path, contents);
  const auto hash = sha256_file(path);
  auto document = read_launch_xml(path);
  set_startup_entry(document, L"C:\\Native Camera\\taxi-cam.exe", Sim);
  const auto no_leftovers = [&](const char* name) {
    expect(matching(directory, L"exe.xml.taxi-*.tmp").empty(), name);
    expect(matching(directory, L"exe.xml.taxi-backup-*").empty(), name);
  };
  for (const auto* kind : {L"encryption", L"permission", L"partial-backup"}) {
    FaultIo io;
    io.copy_failure = kind;
    Failure failure(L"");
    std::wstring written = L"not committed";
    expect(throws([&] { save_launch_xml(document, path, hash, written, false, io); }, &failure) && !failure.unsafe && !failure.conflict,
           "safe backup failure is recoverable");
    expect(failure.operation == L"Back up startup file" && failure.source == path &&
               failure.destination.rfind(path + L".taxi-backup-", 0) == 0,
           "failed backup names its operation, source and destination");
    expect(std::wstring(kind) != L"encryption" || failure.win32 == 6000, "encryption error keeps Windows code 6000");
    expect(written.empty() && sha256_file(path) == hash, "failed backup leaves original and reports no commit");
    no_leftovers("failed backup leaves no temporary or unverified copy");
  }
  {
    FaultIo io;
    io.copy_failure = L"corrupt-backup";
    Failure failure(L"");
    std::wstring written;
    expect(throws([&] { save_launch_xml(document, path, hash, written, false, io); }, &failure) && failure.conflict,
           "backup verification failure keeps the conflict guard");
    expect(sha256_file(path) == hash, "bad backup leaves original");
    no_leftovers("hash-mismatched backup removed");
  }
  {
    FaultIo io;
    io.copy_failure = L"cleanup-failure";
    Failure failure(L"");
    std::wstring written;
    expect(throws([&] { save_launch_xml(document, path, hash, written, false, io); }, &failure) && failure.unsafe,
           "unverified backup cleanup failure blocks manual-startup fallback");
    const auto remaining = matching(directory, L"exe.xml.taxi-backup-*");
    expect(remaining.size() == 1 && failure.message.find(remaining.empty() ? L"?" : remaining[0]) != std::wstring::npos,
           "cleanup failure names the retained backup");
    expect(sha256_file(path) == hash, "cleanup failure leaves original");
    for (const auto& file : remaining) {
      SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(file.c_str());
    }
  }
  {
    FaultIo io;
    io.copy_failure = L"changed-source";
    Failure failure(L"");
    std::wstring written;
    expect(throws([&] { save_launch_xml(document, path, hash, written, false, io); }, &failure) && failure.unsafe,
           "failed backup with uncertain original is unsafe");
    expect(contains(read_file(path), "<!-- concurrent edit during failed backup -->"), "another writer's edit kept");
    expect(matching(directory, L"exe.xml.taxi-*.tmp").empty(), "no temporary XML after uncertain failure");
  }
  // The backup copy succeeds without its expected EFS flag.
  write_file(path, contents);
  {
    FaultIo io;
    io.encrypted.insert(path);
    Failure failure(L"");
    std::wstring written = L"not committed";
    expect(throws([&] { save_launch_xml(document, path, hash, written, false, io); }, &failure) &&
               failure.message == L"Could not preserve exe.xml encryption on the backup file." && !failure.unsafe && !failure.conflict,
           "encryption-mismatched backup is refused safely");
    expect(written.empty() && sha256_file(path) == hash, "encryption mismatch leaves original");
    no_leftovers("encryption-mismatched backup removed");
  }
  // Application Protected folders encrypt new files on creation; a replacement
  // that inherits protection commits without a protection copy.
  for (bool inherits : {true, false}) {
    write_file(path, contents);
    FaultIo io;
    io.encrypted.insert(path);
    io.inherit_encryption = inherits;
    io.refuse_protection = true;
    Failure failure(L"");
    std::wstring written, backup;
    const bool failed = throws([&] { backup = save_launch_xml(document, path, hash, written, false, io); }, &failure);
    if (inherits) {
      expect(!failed && !io.protect_called && written == sha256_file(path) && sha256_file(backup) == hash,
             "inherited protection commits with exact backup");
      if (!backup.empty())
        DeleteFileW(backup.c_str());
    } else {
      expect(failed && failure.operation == L"Encrypt startup replacement" && !failure.unsafe && !failure.conflict,
             "refused replacement protection reports its operation and stays safe");
      expect(written.empty() && sha256_file(path) == hash && matching(directory, L"exe.xml.taxi-backup-*").empty(),
             "refused protection leaves original and no backup");
    }
    expect(matching(directory, L"exe.xml.taxi-*.tmp").empty(), "protection fixture leaves no temporary XML");
  }
  // A real access-denied replacement after a verified backup is safe only
  // because the original bytes and encryption remain unchanged.
  write_file(path, contents);
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
  {
    Failure failure(L"");
    std::wstring written;
    expect(throws([&] { save_launch_xml(document, path, hash, written); }, &failure) && !failure.unsafe && written.empty(),
           "read-only original is a safe uncommitted failure");
    expect(sha256_file(path) == hash && matching(directory, L"exe.xml.taxi-*.tmp").empty(), "read-only original unchanged");
    bool verified = false;
    for (const auto& file : matching(directory, L"exe.xml.taxi-backup-*"))
      verified = verified || sha256_file(file) == hash;
    expect(verified, "failed replacement keeps its verified recovery backup");
  }
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  // A new file has no backup but still reports its committed bytes.
  const auto new_path = join(fixture(L"failures\\new"), L"exe.xml");
  auto created = read_launch_xml(new_path);
  set_startup_entry(created, L"C:\\Native Camera\\taxi-cam.exe", Sim);
  std::wstring written;
  const auto backup = save_launch_xml(created, new_path, L"", written);
  expect(backup.empty() && written == sha256_file(new_path), "new-file transaction ownership");
}

void visibility() {
  const auto directory = fixture(L"visibility");
  const auto path = join(directory, L"exe.xml");
  write_file(
      path,
      "<SimBase.Document Type=\"Launch\"><Launch.Addon><Name>Keep Me</Name><Path>C:\\Other.exe</Path></Launch.Addon></SimBase.Document>");
  const auto hash = sha256_file(path);
  auto document = read_launch_xml(path);
  set_startup_entry(document, L"C:\\Native Camera\\taxi-cam.exe", Sim);
  for (const auto* refused : {L".tmp", L"\\exe.xml"}) {
    FaultIo io;
    io.refuse_visible = refused;
    Failure failure(L"");
    std::wstring written = L"not committed";
    expect(throws([&] { save_launch_xml(document, path, hash, written, true, io); }, &failure) &&
               failure.message == L"Fixture detects a redirected path.",
           "visibility guard reached");
    expect(written.empty() && sha256_file(path) == hash && matching(directory, L"exe.xml.taxi-*").empty(),
           "refused visibility leaves no change, temporary or backup");
  }
  FaultIo io;
  std::wstring written;
  const auto backup = save_launch_xml(document, path, hash, written, true, io);
  unsigned temporary_checks = 0;
  bool original_checked = false;
  for (const auto& call : io.visible_calls) {
    temporary_checks += call.find(L".tmp") != std::wstring::npos;
    original_checked = original_checked || call == path;
  }
  expect(!backup.empty() && sha256_file(backup) == hash && written == sha256_file(path) && temporary_checks == 1 && original_checked,
         "successful visibility proof checks original and empty sibling once");
  FaultIo quiet;
  quiet.refuse_visible = L"exe.xml";
  const auto current = written;
  save_launch_xml(document, path, current, written, false, quiet);
  expect(quiet.visible_calls.empty(), "default saving does not request visibility proof");
  expect(redirected_install_path(
             L"C:\\Users\\Pilot\\AppData\\Local\\Taxi Cam\\app\\taxi-cam.exe",
             L"\\\\?\\C:\\Users\\Pilot\\AppData\\Local\\Packages\\Example.Desktop_123\\LocalCache\\Local\\Taxi Cam\\app\\taxi-cam.exe"),
         "packaged LocalAppData redirection detected");
  expect(redirected_install_path(L"C:\\Users\\Pilot\\AppData\\Roaming\\x.xml",
                                 L"\\\\?\\C:\\Users\\Pilot\\AppData\\Local\\Packages\\Example.Desktop_123\\LocalCache\\Roaming\\x.xml"),
         "packaged roaming redirection detected");
  expect(!redirected_install_path(L"C:\\Users\\Pilot\\AppData\\Local\\Taxi Cam\\app\\taxi-cam.exe",
                                  L"\\\\?\\C:\\Users\\Pilot\\AppData\\Local\\Taxi Cam\\app\\taxi-cam.exe"),
         "ordinary per-user path accepted");
  expect(!redirected_install_path(L"C:\\Users\\Pilot\\Apps\\Taxi Cam\\taxi-cam.exe", L"\\\\?\\D:\\Apps\\Taxi Cam\\taxi-cam.exe"),
         "normal filesystem alias accepted");
  const auto cache = L"C:\\Users\\Pilot\\AppData\\Local\\Packages\\Example.Desktop_123\\LocalCache\\Local\\Taxi Cam\\taxi-cam.exe";
  expect(!redirected_install_path(cache, std::wstring(L"\\\\?\\") + cache), "explicit physical cache path accepted");
  expect(physical_file_path(path).size() > path.size() - 3, "physical path lookup");
}
}  // namespace

int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  wchar_t module[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  root_directory = join(parent_path(module), L"startup-xml-test-" + new_guid());
  try {
    preserve_and_update();
    header_repair();
    failure_resilience();
    visibility();
  } catch (const Failure& failure) {
    std::fprintf(stderr, "FAIL: unexpected failure: %ls\n", failure.detail().c_str());
    ++failures;
  }
  try {
    remove_directory_tree(root_directory);
  } catch (const Failure&) {
  }
  CoUninitialize();
  if (!failures)
    std::printf("PASS exe.xml: %d preservation, header repair, failure resilience, encryption and visibility checks.\n", checks);
  return failures ? 1 : 0;
}
