#pragma once
#include <string>
#include <vector>

namespace taxi_camera::setup {
// Inspects bounded PE headers only. Never loads or executes the file.
bool amd64_image(const std::wstring& path, bool dll = true);
std::vector<std::wstring> prerequisite_issues(const std::wstring& simulator_directory, const std::wstring& system_directory);
void assert_prerequisites(const std::wstring& simulator_directory);
}  // namespace taxi_camera::setup
