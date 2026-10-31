#pragma once
#include <string>
#include <vector>

namespace taxi_camera::setup {
enum class ImageStatus { ok, missing, unreadable, not_x64 };
struct ImageCheck {
  ImageStatus status = ImageStatus::missing;
  unsigned long error = 0;  // Win32 error for missing/unreadable, otherwise 0.
  std::wstring detail;      // Short reason, such as "machine 0x14C (x86)".
};
// Inspects bounded PE headers only. Never loads or executes the file.
ImageCheck inspect_image(const std::wstring& path, bool dll = true);
bool amd64_image(const std::wstring& path, bool dll = true);
// Content of a Store/Xbox package file can be denied to an installer while the
// owning simulator process can still load it. Presence is then the strongest
// check available; the launcher verifies the loaded image at connection time.
bool content_unreadable_but_present(const ImageCheck& check);
// One sentence naming which of missing / unreadable / not x64 applies and the exact path checked.
std::wstring describe_image_issue(const std::wstring& label, const std::wstring& path, const ImageCheck& check);
std::vector<std::wstring> prerequisite_issues(const std::wstring& simulator_directory, const std::wstring& system_directory);
void assert_prerequisites(const std::wstring& simulator_directory);
}  // namespace taxi_camera::setup
