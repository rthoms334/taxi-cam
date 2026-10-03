#pragma once
#include <string>
#include <vector>

// Known saved settings shared by every installation for this Windows user.
// Setup only removes these exact files; logs and unknown files are retained.
namespace taxi_camera::setup {
struct SettingsTarget {
  std::wstring path, root;
};
struct SettingsEntry {
  std::wstring path, root, backup;
  bool existed{};
  std::wstring prior_hash;
  bool removed{};
};
// Refuses paths outside root, reparse points in any ancestor and directories
// in place of settings files.
void assert_settings_path(const std::wstring& path, const std::wstring& root);
// settings.ini, hotkeys.ini, startup-state and every catalog profile in the
// current and former settings folders; optionally the installation's mounts.
std::vector<SettingsTarget> settings_targets(const std::wstring& installation, bool include_mount);
// Copies each existing target into backup_directory and verifies the copy.
std::vector<SettingsEntry> snapshot_settings(const std::vector<SettingsTarget>& targets, const std::wstring& backup_directory);
// Deletes the snapshot's files after verifying none changed since the snapshot.
void remove_settings(std::vector<SettingsEntry>& snapshot);
// Restores removed files from their backups. Never overwrites a file that
// another writer created after removal; such paths are reported.
void restore_settings(std::vector<SettingsEntry>& snapshot);
}  // namespace taxi_camera::setup
