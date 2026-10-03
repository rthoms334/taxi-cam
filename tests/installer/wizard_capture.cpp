#include <windows.h>
#include <string>
#include "../../src/app/json_value.hpp"

// Stands in for taxi-cam.exe in the wizard argument fixture. "discover"
// returns the fixture record's choices; "check" records the wizard's final
// arguments and always refuses, so no installation is ever performed.
namespace {
std::wstring widen(const std::string& text) {
  std::wstring result(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)),
                      L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), static_cast<int>(result.size()));
  return result;
}
std::string narrow(const std::wstring& text) {
  std::string result(
      static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)),
      '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), static_cast<int>(result.size()), nullptr,
                      nullptr);
  return result;
}
void write(const std::wstring& path, const std::string& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written{};
  WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
}
std::string read(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  std::string bytes(1 << 16, '\0');
  DWORD count{};
  ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr);
  CloseHandle(file);
  bytes.resize(count);
  return bytes;
}
std::string quoted(const std::wstring& text) {
  std::string out = "\"";
  for (char c : narrow(text)) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out + "\"";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3 || std::wstring(argv[1]) != L"--setup")
    return 2;
  const std::wstring command = argv[2];
  std::wstring destination, state, simulator, exe_xml, startup;
  bool exe_xml_passed = false;
  for (int i = 3; i + 1 < argc; ++i) {
    const std::wstring name = argv[i];
    if (name == L"--destination")
      destination = argv[++i];
    else if (name == L"--state")
      state = argv[++i];
    else if (name == L"--simulator")
      simulator = argv[++i];
    else if (name == L"--exe-xml") {
      exe_xml = argv[++i];
      exe_xml_passed = true;
    } else if (name == L"--startup")
      startup = argv[++i];
  }
  CreateDirectoryW(state.c_str(), nullptr);
  if (command == L"discover") {
    taxi_camera::standalone::json::Value record;
    taxi_camera::standalone::json::parse(read(destination + L"\\installation.json"), record);
    std::wstring recorded = record.find("simulator") ? widen(record.find("simulator")->text) : L"";
    recorded = recorded.substr(0, recorded.find_last_of(L"\\/"));
    const std::wstring xml = record.find("exeXml") ? widen(record.find("exeXml")->text) : L"";
    const std::wstring text = L"\xFEFF[Paths]\r\nSimulator=" + recorded + L"\r\nExeXml=" + xml + L"\r\nStartup=automatic\r\n";
    write(state + L"\\choices.ini", std::string(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)));
    return 0;
  }
  if (command == L"check") {
    write(destination + L"\\captured-arguments.json", "{\"simulator\":" + quoted(simulator) + ",\"exeXml\":" + quoted(exe_xml) +
                                                          ",\"exeXmlWasPassed\":" + (exe_xml_passed ? "true" : "false") +
                                                          ",\"startup\":" + quoted(startup) + "}");
    write(state + L"\\error.txt", "Wizard argument fixture stopped before installation.");
    return 1;
  }
  return 1;
}
