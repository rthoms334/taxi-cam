#include "main_module_inventory.hpp"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kMaximumImageSize = 0x80000000;

bool readable_image_page(DWORD protection) {
  if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  // Packaged executables can map headers/read-only sections as copy-on-write.
  // Reading does not trigger a write; the parser separately excludes writable
  // PE sections. PAGE_EXECUTE alone grants no documented read permission.
  const auto base_protection = protection & 0xff;
  return base_protection == PAGE_READONLY || base_protection == PAGE_READWRITE || base_protection == PAGE_WRITECOPY ||
         base_protection == PAGE_EXECUTE_READ || base_protection == PAGE_EXECUTE_READWRITE || base_protection == PAGE_EXECUTE_WRITECOPY;
}

class MainImageReader final : public ImageReader {
 public:
  MainImageReader(HANDLE process, HMODULE module, std::uint32_t image_size)
      : process_(process),
        module_(module),
        base_(reinterpret_cast<std::uintptr_t>(module)),
        limit_(std::min(image_size, kMaximumImageSize)) {}

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (rva >= limit_ || maximum == 0 || rva > std::numeric_limits<std::uintptr_t>::max() - base_) {
      return {};
    }
    maximum = std::min(maximum, limit_ - rva);
    const auto address = base_ + rva;
    MEMORY_BASIC_INFORMATION region{};
    const auto queried = process_ == GetCurrentProcess()
                             ? VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region))
                             : VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region));
    if (queried != sizeof(region)) {
      return {};
    }
    const auto region_base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region_base > address || region.RegionSize == 0 || region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - region_base) {
      return {};
    }
    const auto region_end = region_base + region.RegionSize;
    if (region_end <= address) {
      return {};
    }
    const auto length = static_cast<std::uint32_t>(std::min<std::uintptr_t>(maximum, region_end - address));
    const bool readable =
        region.State == MEM_COMMIT && region.Type == MEM_IMAGE && region.AllocationBase == module_ && readable_image_page(region.Protect);
    return {length, readable};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    if (rva >= limit_ || size > limit_ - rva || (size != 0 && destination == nullptr)) {
      return false;
    }
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t completed = 0;
    while (completed < size) {
      const auto offset = rva + static_cast<std::uint32_t>(completed);
      const auto window = query(offset, static_cast<std::uint32_t>(size - completed));
      if (!window.readable || window.size == 0 || window.size > size - completed) {
        return false;
      }
      SIZE_T copied = 0;
      // VirtualQuery is a snapshot. ReadProcessMemory checks the selected process
      // mapping again and fails safely if protection changes before this read.
      read_attempted_ = true;
      if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(base_ + offset), output + completed, window.size, &copied) ||
          copied != window.size) {
        last_read_error_ = GetLastError();
        return false;
      }
      completed += window.size;
    }
    return true;
  }

  DWORD last_read_error() const { return last_read_error_; }
  bool read_attempted() const { return read_attempted_; }

 private:
  HANDLE process_;
  HMODULE module_;
  std::uintptr_t base_;
  std::uint32_t limit_;
  DWORD last_read_error_ = 0;
  bool read_attempted_ = false;
};

struct OwnedHandle {
  HANDLE value = nullptr;
  ~OwnedHandle() {
    if (value != nullptr) {
      CloseHandle(value);
    }
  }
  OwnedHandle() = default;
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
};

bool fail(MainModuleInventory& result, const char* message, DWORD error = 0) {
  result.windows_error = error;
  result.image.error = message;
  return false;
}

std::wstring basename(const std::wstring& path) {
  const auto separator = path.find_last_of(L"/\\");
  return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

bool file_identity(const std::wstring& path, FILE_ID_INFO& identity, MainModuleInventory& result) {
  // This opens attributes only, not file contents. The mapped executable can
  // have Xbox/package path aliases even when both names refer to the same file.
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return fail(result, "Opening executable file attributes for the alias identity check failed.", GetLastError());
  }
  OwnedHandle file;
  file.value = handle;
  if (!GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity))) {
    return fail(result, "Reading executable file identity for the alias check failed.", GetLastError());
  }
  return true;
}

bool same_executable_file(const std::wstring& process_path, const std::wstring& module_path, MainModuleInventory& result) {
  if (CompareStringOrdinal(module_path.c_str(), static_cast<int>(module_path.size()), process_path.c_str(),
                           static_cast<int>(process_path.size()), TRUE) == CSTR_EQUAL) {
    result.identity_method = "matching_path";
    return true;
  }
  FILE_ID_INFO process_file{};
  FILE_ID_INFO module_file{};
  if (!file_identity(process_path, process_file, result) || !file_identity(module_path, module_file, result)) {
    return false;
  }
  if (process_file.VolumeSerialNumber != module_file.VolumeSerialNumber ||
      !std::equal(std::begin(process_file.FileId.Identifier), std::end(process_file.FileId.Identifier),
                  std::begin(module_file.FileId.Identifier))) {
    return fail(result, "The process executable and enumerated module have different Windows file identities.");
  }
  result.identity_method = "matching_file_id";
  return true;
}

bool token_user(HANDLE process, std::vector<std::uint8_t>& data, MainModuleInventory& result) {
  OwnedHandle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) {
    return fail(result, "Reading the process user token failed.", GetLastError());
  }
  DWORD required = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0 || required > 65536) {
    return fail(result, "The process user token has an invalid size.", GetLastError());
  }
  data.resize(required);
  if (!GetTokenInformation(token.value, TokenUser, data.data(), required, &required)) {
    return fail(result, "Reading the process user token failed.", GetLastError());
  }
  return true;
}

void inspect_verified(MainModuleInventory& result, HANDLE process, HMODULE module, Limits limits) {
  MODULEINFO information{};
  if (!K32GetModuleInformation(process, module, &information, sizeof(information)) || information.lpBaseOfDll != module ||
      information.SizeOfImage == 0 || information.SizeOfImage > kMaximumImageSize) {
    fail(result, "Reading the main-image mapping bounds failed.", GetLastError());
    return;
  }
  MainImageReader reader(process, module, information.SizeOfImage);
  result.mapped_image_size = information.SizeOfImage;
  MEMORY_BASIC_INFORMATION header_region{};
  if (VirtualQueryEx(process, module, &header_region, sizeof(header_region)) == sizeof(header_region)) {
    result.header_region_state = header_region.State;
    result.header_region_type = header_region.Type;
    result.header_region_protection = header_region.Protect;
    result.header_allocation_matches = header_region.AllocationBase == module;
  } else {
    result.header_query_error = GetLastError();
  }
  result.image = inspect_image(reader, limits);
  result.last_memory_read_error = reader.last_read_error();
  result.memory_read_attempted = reader.read_attempted();
  if (result.image.valid_image && result.image.image_size != information.SizeOfImage) {
    result.image.valid_image = false;
    result.image.error = "The PE image size does not match the verified main-module mapping.";
    result.image.records.clear();
  }
}

}  // namespace

MainModuleInventory inspect_main_module(HMODULE explicitly_selected_main, Limits limits) {
  MainModuleInventory result;
  if (explicitly_selected_main == nullptr || explicitly_selected_main != GetModuleHandleW(nullptr)) {
    result.image.error = "Only an explicitly selected main executable module is permitted.";
    return result;
  }
  result.main_module_verified = true;
  result.identity_method = "current_main_handle";
  result.process_id = GetCurrentProcessId();
  std::array<wchar_t, 32768> path{};
  const auto length = GetModuleFileNameW(explicitly_selected_main, path.data(), static_cast<DWORD>(path.size()));
  if (length != 0 && length < path.size()) {
    const std::wstring full_path(path.data(), length);
    result.module_name = basename(full_path);
  }
  inspect_verified(result, GetCurrentProcess(), explicitly_selected_main, limits);
  return result;
}

MainModuleInventory inspect_simulator_main_module(std::uint32_t explicitly_selected_pid, Limits limits) {
  MainModuleInventory result;
  result.process_id = explicitly_selected_pid;
  if (explicitly_selected_pid == 0) {
    fail(result, "An explicit nonzero simulator PID is required.");
    return result;
  }
  OwnedHandle process;
  process.value = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, explicitly_selected_pid);
  if (process.value == nullptr) {
    fail(result, "OpenProcess with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ failed.", GetLastError());
    return result;
  }
  std::array<wchar_t, 32768> process_path{};
  DWORD path_size = static_cast<DWORD>(process_path.size());
  if (!QueryFullProcessImageNameW(process.value, 0, process_path.data(), &path_size) || path_size == 0 ||
      path_size >= process_path.size()) {
    fail(result, "Reading the selected process executable identity failed.", GetLastError());
    return result;
  }
  const std::wstring executable_path(process_path.data(), path_size);
  result.module_name = basename(executable_path);
  if (CompareStringOrdinal(result.module_name.c_str(), -1, L"FlightSimulator2024.exe", -1, TRUE) != CSTR_EQUAL) {
    fail(result, "The selected PID is not FlightSimulator2024.exe.");
    return result;
  }
  std::vector<std::uint8_t> current_user;
  std::vector<std::uint8_t> process_user;
  if (!token_user(GetCurrentProcess(), current_user, result) || !token_user(process.value, process_user, result)) {
    return result;
  }
  if (!EqualSid(reinterpret_cast<const TOKEN_USER*>(current_user.data())->User.Sid,
                reinterpret_cast<const TOKEN_USER*>(process_user.data())->User.Sid)) {
    fail(result, "The selected simulator process belongs to a different user.");
    return result;
  }
  // Request just the first module handle, then prove that its filename is the
  // selected process image. A mismatch is rejected, never searched around.
  HMODULE main_module = nullptr;
  DWORD module_bytes = 0;
  if (!K32EnumProcessModulesEx(process.value, &main_module, sizeof(main_module), &module_bytes, LIST_MODULES_64BIT) ||
      module_bytes < sizeof(main_module) || main_module == nullptr) {
    fail(result, "Reading the selected process main-module handle failed.", GetLastError());
    return result;
  }
  std::array<wchar_t, 32768> module_path{};
  const auto module_name_size =
      K32GetModuleFileNameExW(process.value, main_module, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (module_name_size == 0 || module_name_size >= module_path.size()) {
    fail(result, "Reading the enumerated main-module filename failed.", GetLastError());
    return result;
  }
  const std::wstring enumerated_path(module_path.data(), module_name_size);
  result.candidate_module_name = basename(enumerated_path);
  if (CompareStringOrdinal(result.candidate_module_name.c_str(), -1, L"FlightSimulator2024.exe", -1, TRUE) != CSTR_EQUAL) {
    fail(result, "The first enumerated module is not FlightSimulator2024.exe.");
    return result;
  }
  if (!same_executable_file(executable_path, enumerated_path, result)) {
    return result;
  }
  result.main_module_verified = true;
  inspect_verified(result, process.value, main_module, limits);
  return result;
}

}  // namespace taxi_camera::discovery
