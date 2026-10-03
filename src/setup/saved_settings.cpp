#include "saved_settings.hpp"
#include "../profiles/catalog.hpp"
#include "processes.hpp"
#include "setup_common.hpp"

namespace taxi_camera::setup {
void assert_settings_path(const std::wstring& path, const std::wstring& root) {
  if (!path_rooted(path) || !path_rooted(root))
    fail(L"Settings paths must be absolute.");
  const auto absolute = full_path(path);
  const auto parent = trim_separators(full_path(root));
  if (equal_insensitive(parent, trim_separators(path_root(parent))) || !starts_with_insensitive(absolute, parent + L"\\"))
    fail(L"Settings path is outside its dedicated directory: " + absolute);
  // Check the file and every existing ancestor, including LOCALAPPDATA itself.
  // Never follow junctions/symlinks or treat a directory as a settings file.
  for (std::wstring cursor = absolute; !cursor.empty();) {
    const DWORD value = attributes(cursor);
    if (value != INVALID_FILE_ATTRIBUTES) {
      if (value & FILE_ATTRIBUTE_REPARSE_POINT)
        fail(L"Settings path contains a reparse point: " + cursor);
      if (cursor == absolute && (value & FILE_ATTRIBUTE_DIRECTORY))
        fail(L"Settings file is a directory: " + absolute);
      if (cursor != absolute && !(value & FILE_ATTRIBUTE_DIRECTORY))
        fail(L"Settings parent is not a directory: " + cursor);
    } else if (const DWORD error = GetLastError(); error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      fail_win32(L"Settings path could not be inspected: " + cursor, error);
    }
    const auto next = parent_path(cursor);
    if (next.empty() || next == cursor)
      break;
    cursor = next;
  }
}
std::vector<SettingsTarget> settings_targets(const std::wstring& installation, bool include_mount) {
  const auto local_value = environment(L"LOCALAPPDATA");
  if (local_value.empty() || !path_rooted(local_value))
    fail(L"LOCALAPPDATA must identify an absolute settings directory.");
  const auto local = full_path(local_value);
  const auto current = join(local, L"Taxi Cam"), legacy = join(local, L"380 Taxi Cam");
  std::vector<SettingsTarget> targets;
  for (const auto* name : {L"settings.ini", L"hotkeys.ini", L"startup-state"})
    targets.push_back({join(current, name), current});
  for (const auto* profile : profiles::Catalog) {
    const auto name = L"profiles\\" + std::wstring(profile->key.begin(), profile->key.end()) + L".ini";
    for (const auto& folder : {current, legacy})
      targets.push_back({join(folder, name), folder});
  }
  if (include_mount) {
    const auto root = full_path(installation);
    targets.push_back({join(root, L"taxi-camera-mounts.cfg"), root});
  }
  for (const auto& target : targets)
    assert_settings_path(target.path, target.root);
  return targets;
}
namespace {
std::wstring entry_hash(const SettingsEntry& entry) {
  assert_settings_path(entry.path, entry.root);
  return file_hash_or_empty(entry.path);
}
}  // namespace
std::vector<SettingsEntry> snapshot_settings(const std::vector<SettingsTarget>& targets, const std::wstring& backup_directory) {
  std::vector<SettingsEntry> snapshot;
  unsigned index = 0;
  for (const auto& target : targets) {
    SettingsEntry entry;
    entry.path = target.path;
    entry.root = target.root;
    entry.backup = join(backup_directory, L"settings-" + std::to_wstring(index++) + L".backup");
    entry.prior_hash = entry_hash(entry);
    entry.existed = !entry.prior_hash.empty();
    if (entry.existed) {
      copy_file(entry.path, entry.backup, false);
      if (sha256_file(entry.backup) != entry.prior_hash || entry_hash(entry) != entry.prior_hash)
        fail(L"Settings changed while taking the recovery snapshot: " + entry.path);
    }
    snapshot.push_back(std::move(entry));
  }
  return snapshot;
}
void remove_settings(std::vector<SettingsEntry>& snapshot) {
  assert_settings_closed();
  // Preflight the entire explicit list before the first deletion.
  for (const auto& entry : snapshot)
    if (entry_hash(entry) != entry.prior_hash)
      fail(L"Settings changed before removal: " + entry.path);
  for (auto& entry : snapshot) {
    if (!entry.existed)
      continue;
    if (entry_hash(entry) != entry.prior_hash)
      fail(L"Settings changed during removal: " + entry.path);
    delete_file(entry.path);
    entry.removed = true;
  }
}
void restore_settings(std::vector<SettingsEntry>& snapshot) {
  std::wstring conflicts;
  for (auto& entry : snapshot) {
    if (!entry.removed)
      continue;
    try {
      if (!entry_hash(entry).empty())
        fail(L"changed by another writer");
      if (sha256_file(entry.backup) != entry.prior_hash)
        fail(L"backup changed");
      copy_file(entry.backup, entry.path, false);
      entry.removed = false;
    } catch (const Failure&) {
      conflicts += (conflicts.empty() ? L"" : L", ") + entry.path;
    }
  }
  if (!conflicts.empty())
    fail(L"Settings rollback preserved files changed by another writer or unsafe path: " + conflicts);
}
}  // namespace taxi_camera::setup
