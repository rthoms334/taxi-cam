#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "../../src/app/simulator_graphics.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
bool write_text(const std::wstring& path, const std::string& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written{};
  const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
  CloseHandle(file);
  return ok;
}
}  // namespace

int main() {
  using namespace taxi_camera::standalone;
  try {
    // The simulator writes tab-indented "Key Value" lines inside braces.
    const std::string store_config =
        "{Graphics\r\n\tAntiAliasing DLSS\r\n\tDLSSMode PERFORMANCE\r\n\tReflex ON\r\n\tFSRMode QUALITY\r\n"
        "\tFrameGeneration DLSSG\r\n\tSharpenAmount 1.000000\r\n}\r\n{GraphicsVR\r\n\tFrameGenerationVR NONE\r\n}\r\n";
    require(parse_frame_generation(store_config) == FrameGeneration::on, "DLSSG means frame generation on");
    require(parse_frame_generation("\tFrameGeneration NONE\r\n") == FrameGeneration::off, "NONE means off");
    require(parse_frame_generation("\tFrameGeneration FSR3\n") == FrameGeneration::on, "FSR3 means on");
    require(parse_frame_generation("\tFrameGenerationVR DLSSG\n") == FrameGeneration::unknown, "The VR key is not the desktop key");
    require(parse_frame_generation("\tFrameGenerationVR DLSSG\n\tFrameGeneration NONE\n") == FrameGeneration::off,
            "The desktop key is found after the VR key");
    require(parse_frame_generation("FrameGeneration\tDLSSG") == FrameGeneration::on, "Tab separator accepted");
    require(parse_frame_generation("FrameGeneration ") == FrameGeneration::unknown, "Empty value is unknown");
    require(parse_frame_generation("") == FrameGeneration::unknown, "Empty file is unknown");
    require(parse_frame_generation("\tAntiAliasing TAA\n") == FrameGeneration::unknown, "Missing key is unknown");

    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read fixture root");
    const auto root = std::wstring(cwd) + L"\\build\\simulator-graphics-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(root.c_str(), nullptr), "Create fixture directory");
    const auto exe = root + L"\\FlightSimulator2024.exe";
    require(write_text(exe, "not an image"), "Create fixture executable");
    require(!graphics_hook_beside(exe), "No hook beside a clean executable");
    require(write_text(root + L"\\dxgi.dll", "hook"), "Create fixture hook");
    std::wstring found;
    require(graphics_hook_beside(exe, &found) && found == root + L"\\dxgi.dll", "dxgi.dll beside the executable is a hook");
    require(DeleteFileW((root + L"\\dxgi.dll").c_str()), "Remove fixture hook");
    require(write_text(root + L"\\d3d12.dll", "hook"), "Create fixture d3d12 hook");
    require(graphics_hook_beside(exe), "d3d12.dll beside the executable is a hook");
    require(!graphics_hook_beside(L"FlightSimulator2024.exe"), "A bare name has no folder to inspect");

    // Candidate order follows the executable location; both stores are listed.
    const auto store = user_config_candidates(L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\FlightSimulator2024.exe");
    require(store.size() == 2 && store[0].find(L"Microsoft.Limitless_8wekyb3d8bbwe") != std::wstring::npos,
            "Store install reads LocalCache first");
    const auto steam = user_config_candidates(L"C:\\Steam\\steamapps\\common\\Microsoft Flight Simulator 2024\\FlightSimulator2024.exe");
    require(steam.size() == 2 && steam[0].find(L"Microsoft Flight Simulator 2024\\UserCfg.opt") != std::wstring::npos,
            "Steam install reads Roaming first");

    // The read helper handles a missing file and a real one.
    std::string text;
    require(!read_small_file(root + L"\\missing.opt", text) && text.empty(), "Missing file reads false");
    require(write_text(root + L"\\UserCfg.opt", store_config) && read_small_file(root + L"\\UserCfg.opt", text) && text == store_config,
            "Round-trip a configuration file");

    // Preference persistence.
    require(!load_allow_frame_generation(root), "Default does not allow frame generation");
    require(save_allow_frame_generation(root, true) && load_allow_frame_generation(root), "Persist allow on");
    require(save_allow_frame_generation(root, false) && !load_allow_frame_generation(root), "Persist allow off");
    require(!load_allow_frame_generation(L"") && !save_allow_frame_generation(L"", true), "Empty directory fails closed");

    // The monitor rereads at most once per second and follows the executable path.
    SimulatorGraphicsMonitor monitor;
    const auto& first = monitor.sample(exe, 1000);
    require(first.graphics_hook, "Monitor sees the hook");
    require(DeleteFileW((root + L"\\d3d12.dll").c_str()), "Remove fixture d3d12 hook");
    require(monitor.sample(exe, 1500).graphics_hook, "Cached within one second");
    require(!monitor.sample(exe, 2100).graphics_hook, "Rereads after one second");
    require(!monitor.sample(exe, 0).graphics_hook, "A zero clock still samples");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "simulator graphics test failed: %s\n", error.what());
    return 1;
  }
  std::printf("simulator graphics test passed (%u checks)\n", checks);
  return 0;
}
