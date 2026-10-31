#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <new>

#include "../shared/wide_path_buffer.hpp"

namespace taxi_camera::standalone::crash_evidence {
// Small local diagnostic record for the renderer instruction seen in the A350
// reports. Version 2 adds two bounded copies taken at the fault: the render
// context's output-merger state (RBX+0x79a0..0x7a40: eight slot pairs, count
// at +0x7910 is outside and read separately) and the first 0x100 bytes of the
// slot-1 render-target record, each only after VirtualQuery shows a committed,
// readable range. No stack pages or image data are copied. Never sent over IPC.
struct Record {
  std::uint32_t magic = 0x54434352, version = 2, bytes = sizeof(Record), process = 0;
  volatile LONG state = 0;  // 0 empty, 1 writing, 2 complete
  DWORD code = 0, parameters = 0, thread = 0;
  std::uint64_t module = 0, fault_rva = 0;
  ULONG_PTR information[EXCEPTION_MAXIMUM_PARAMETERS]{};
  CONTEXT context{};
  // Bit 0: output_merger_state copied. Bit 1: slot1_record copied.
  // Bit 2: bound-target count at RBX+0x7910 copied into bound_targets.
  std::uint32_t evidence_flags = 0;
  std::uint32_t bound_targets = 0;
  std::uint64_t slot1_record_address = 0;
  std::uint8_t output_merger_state[0xA0]{};
  std::uint8_t slot1_record[0x100]{};
};
inline constexpr std::size_t record_file_bytes = 8192;
inline constexpr std::size_t legacy_record_file_bytes = 4096;
static_assert(sizeof(Record) <= record_file_bytes);
inline constexpr std::uint64_t output_merger_state_offset = 0x79a0, bound_target_count_offset = 0x7910, slot1_record_offset = 0x79b8;
inline Record* record = nullptr;
inline HANDLE record_file = INVALID_HANDLE_VALUE;
struct CodeCaptureResult {
  bool written = false;
  bool searched = false;
  std::uint32_t match_rva = 0, window_rva = 0, window_bytes = 0;
  const char* error = "";
};
inline CodeCaptureResult* code_capture = nullptr;
inline constexpr unsigned retained_records = 16;
inline constexpr unsigned maintenance_entries = 4096, maintenance_passes = 32;
inline constexpr std::uint64_t maintenance_milliseconds = 250;

struct Mapping {
  HANDLE file = INVALID_HANDLE_VALUE;
  Record* memory = nullptr;
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  void close() noexcept {
    if (memory)
      UnmapViewOfFile(memory);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
    memory = nullptr;
    file = INVALID_HANDLE_VALUE;
  }
  ~Mapping() { close(); }
};

inline bool record_filename(const wchar_t* name) noexcept {
  constexpr wchar_t prefix[] = L"renderer-fault-";
  if (!name || std::wcsncmp(name, prefix, std::size(prefix) - 1) != 0)
    return false;
  const wchar_t* cursor = name + std::size(prefix) - 1;
  const auto number = [&cursor](std::uint64_t maximum, wchar_t separator, bool positive) noexcept {
    if (*cursor < L'0' || *cursor > L'9' || (*cursor == L'0' && cursor[1] >= L'0' && cursor[1] <= L'9'))
      return false;
    std::uint64_t value = 0;
    do {
      const auto digit = static_cast<unsigned>(*cursor - L'0');
      if (value > (maximum - digit) / 10)
        return false;
      value = value * 10 + digit;
      ++cursor;
    } while (*cursor >= L'0' && *cursor <= L'9');
    return (!positive || value != 0) && *cursor++ == separator;
  };
  return number((std::numeric_limits<DWORD>::max)(), L'-', true) && number((std::numeric_limits<std::uint64_t>::max)(), L'.', false) &&
         std::wcscmp(cursor, L"bin") == 0;
}

struct Candidate {
  FILETIME created{};
  DWORD volume{}, index_high{}, index_low{};
  LONG state{};
  wchar_t name[64]{};
};
inline bool older(const Candidate& a, const Candidate& b) noexcept {
  if (a.state != b.state)
    return a.state < b.state;  // Empty, incomplete, then completed evidence.
  const auto comparison = CompareFileTime(&a.created, &b.created);
  return comparison < 0 || (comparison == 0 && std::wcscmp(a.name, b.name) < 0);
}
inline bool inspect_record(HANDLE file, Candidate& candidate) noexcept {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(file, &information) ||
      (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || information.nFileSizeHigh ||
      (information.nFileSizeLow != 0 && information.nFileSizeLow != legacy_record_file_bytes &&
       information.nFileSizeLow != record_file_bytes))
    return false;
  LONG state = 0;
  if (information.nFileSizeLow) {
    struct Prefix {
      std::uint32_t magic, version, bytes, process;
      LONG state;
    } prefix{};
    static_assert(sizeof(Prefix) == offsetof(Record, state) + sizeof(LONG));
    DWORD read{};
    if (!ReadFile(file, &prefix, sizeof(prefix), &read, nullptr) || read != sizeof(prefix) || prefix.magic != 0x54434352 ||
        prefix.version < 1 || prefix.version > 2 || prefix.bytes == 0 || prefix.bytes > information.nFileSizeLow || prefix.state < 0 ||
        prefix.state > 2)
      return false;
    state = prefix.state;
  }
  candidate.created = information.ftCreationTime;
  candidate.volume = information.dwVolumeSerialNumber;
  candidate.index_high = information.nFileIndexHigh;
  candidate.index_low = information.nFileIndexLow;
  candidate.state = state;
  return true;
}

// Hold the directory's exclusive retention lock while calling this. Each pass
// stores only the oldest sixteen remaining names, even for a large old archive.
// Failed deletions advance the cursor, so locked/mapped records cannot prevent
// trying newer inactive files. No record is ever opened for writing or resized.
inline bool make_room(const wchar_t* directory) noexcept {
  Candidate cursor{};
  bool after_cursor = false;
  taxi_camera::WidePathBuffer path_buffer;  // 64 KiB: heap, not this thread's stack.
  if (!path_buffer)
    return false;
  wchar_t* const path = path_buffer.get();
  const auto started = GetTickCount64();
  unsigned inspected = 0;
  for (unsigned pass = 0; pass < maintenance_passes; ++pass) {
    if (GetTickCount64() - started >= maintenance_milliseconds)
      return false;
    std::array<Candidate, retained_records> candidates{};
    unsigned selected = 0;
    std::uint64_t count = 0;
    if (std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\renderer-fault-*.bin", directory) < 0)
      return false;
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW(path, &entry);
    if (search == INVALID_HANDLE_VALUE)
      return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
      if (++inspected > maintenance_entries || GetTickCount64() - started >= maintenance_milliseconds) {
        FindClose(search);
        return false;
      }
      if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !record_filename(entry.cFileName))
        continue;
      ++count;
      Candidate candidate;
      std::wcscpy(candidate.name, entry.cFileName);  // Canonical numeric name is at most 50 characters.
      if (std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\%ls", directory, candidate.name) < 0) {
        FindClose(search);
        return false;
      }
      const auto inspection = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (inspection == INVALID_HANDLE_VALUE)
        continue;
      const bool valid = inspect_record(inspection, candidate);
      CloseHandle(inspection);
      if (!valid)
        continue;
      if (after_cursor && !older(cursor, candidate))
        continue;
      unsigned position = 0;
      while (position < selected && !older(candidate, candidates[position]))
        ++position;
      if (position == candidates.size())
        continue;
      if (selected < candidates.size())
        ++selected;
      for (unsigned i = selected - 1; i > position; --i)
        candidates[i] = candidates[i - 1];
      candidates[position] = candidate;
    } while (FindNextFileW(search, &entry));
    const auto enumeration_error = GetLastError();
    FindClose(search);
    if (enumeration_error != ERROR_NO_MORE_FILES)
      return false;
    if (count < retained_records)
      return true;
    if (!selected)
      return false;
    for (unsigned i = 0; i < selected && count >= retained_records; ++i) {
      if (GetTickCount64() - started >= maintenance_milliseconds)
        return false;
      const auto& candidate = candidates[i];
      if (std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\%ls", directory, candidate.name) < 0)
        return false;
      const auto file = CreateFileW(path, DELETE | GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        continue;
      Candidate verified;
      FILE_DISPOSITION_INFO disposition{TRUE};
      if (inspect_record(file, verified) && verified.volume == candidate.volume && verified.index_high == candidate.index_high &&
          verified.index_low == candidate.index_low && verified.state == candidate.state &&
          SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
        --count;
      CloseHandle(file);
    }
    if (count < retained_records)
      return true;
    cursor = candidates[selected - 1];
    after_cursor = true;
  }
  return false;
}

// Initialization-only I/O. The held file handle deliberately omits delete
// sharing, protecting the mapped record until this session releases it.
inline bool create_record(const wchar_t* directory,
                          DWORD process,
                          std::uint64_t ticks,
                          std::uint64_t module,
                          Mapping& destination) noexcept {
  if (!directory || !process || destination.memory || destination.file != INVALID_HANDLE_VALUE)
    return false;
  taxi_camera::WidePathBuffer path_buffer;
  if (!path_buffer)
    return false;
  wchar_t* const path = path_buffer.get();
  if (std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\renderer-fault-retention.lock", directory) < 0)
    return false;
  const auto retention_lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (retention_lock == INVALID_HANDLE_VALUE)
    return false;
  struct Unlock {
    HANDLE handle;
    ~Unlock() { CloseHandle(handle); }
  } unlock{retention_lock};
  BY_HANDLE_FILE_INFORMATION lock_information{};
  if (!GetFileInformationByHandle(retention_lock, &lock_information) ||
      (lock_information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !make_room(directory) ||
      std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\renderer-fault-%lu-%llu.bin", directory, process,
                    static_cast<unsigned long long>(ticks)) < 0)
    return false;
  const auto file =
      CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(record_file_bytes), nullptr);
  auto* memory = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, record_file_bytes) : nullptr;
  if (mapping)
    CloseHandle(mapping);
  if (!memory) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(file);
    return false;
  }
  destination.file = file;
  destination.memory = new (memory) Record{};
  destination.memory->process = process;
  destination.memory->module = module;
  return true;
}

// Copy `size` bytes from `source` only when VirtualQuery reports one committed,
// readable, non-guard region covering the whole range. Safe inside the vectored
// handler: no allocation, lock or engine call, and no touch of unverified memory.
inline bool copy_readable(void* destination, std::uint64_t source, std::size_t size) noexcept {
  if (!destination || !source || size == 0 || source > (std::numeric_limits<std::uint64_t>::max)() - size)
    return false;
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(reinterpret_cast<const void*>(source), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
      (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
    return false;
  const auto protection = region.Protect & 0xff;
  if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
      protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
    return false;
  const auto begin = reinterpret_cast<std::uint64_t>(region.BaseAddress);
  if (source < begin || region.RegionSize < size || source - begin > region.RegionSize - size)
    return false;
  std::memcpy(destination, reinterpret_cast<const void*>(source), size);
  return true;
}

inline LONG capture(Record* destination, const EXCEPTION_POINTERS* exception) noexcept {
  if (!destination || !exception || !exception->ExceptionRecord || !exception->ContextRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  const auto& e = *exception->ExceptionRecord;
  const auto& c = *exception->ContextRecord;
  const auto address = reinterpret_cast<std::uint64_t>(e.ExceptionAddress);
  if (e.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !destination->module || address < destination->module)
    return EXCEPTION_CONTINUE_SEARCH;
  // Only the renderer instruction sequence located in the current image at arm
  // time (capture_fault_site_code; 1.9.12.0: +0x3E5C054). No fixed site.
  const auto rva = address - destination->module;
  const bool located = code_capture && code_capture->match_rva && rva == code_capture->match_rva;
  if (!located || c.Rip != address || e.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS ||
      InterlockedCompareExchange(&destination->state, 1, 0) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  // Only bounded copies into an already mapped, initialized record. No file
  // operations, allocation, locks, engine calls or exception suppression here.
  destination->code = e.ExceptionCode;
  destination->parameters = e.NumberParameters;
  destination->thread = GetCurrentThreadId();
  destination->fault_rva = rva;
  for (DWORD i = 0; i < e.NumberParameters; ++i)
    destination->information[i] = e.ExceptionInformation[i];
  destination->context = c;
  // Bounded copies of the render context's output-merger state and the slot-1
  // record the captured binder would visit next. VirtualQuery only; a range
  // that is not wholly committed and readable is skipped, never touched.
  destination->evidence_flags = 0;
  if (copy_readable(destination->output_merger_state, c.Rbx + output_merger_state_offset, sizeof(destination->output_merger_state)))
    destination->evidence_flags |= 1u;
  if (copy_readable(&destination->bound_targets, c.Rbx + bound_target_count_offset, sizeof(destination->bound_targets)))
    destination->evidence_flags |= 4u;
  if (destination->evidence_flags & 1u) {
    std::uint64_t slot1 = 0;
    std::memcpy(&slot1, destination->output_merger_state + (slot1_record_offset - output_merger_state_offset), sizeof(slot1));
    destination->slot1_record_address = slot1;
    if (slot1 && copy_readable(destination->slot1_record, slot1, sizeof(destination->slot1_record)))
      destination->evidence_flags |= 2u;
  }
  InterlockedExchange(&destination->state, 2);
  return EXCEPTION_CONTINUE_SEARCH;
}
inline LONG CALLBACK handler(EXCEPTION_POINTERS* exception) noexcept {
  return capture(record, exception);
}

// Read-only arm-time copy of the main image's code around the renderer fault
// instruction. The store executable file is not readable from a user account,
// so the caller frames of the retained RenderThreadProc faults can only be
// read from the loaded image. The fault instruction sequence is located by its
// bytes in the executable sections; a fixed window around the match is written
// once per session. No engine call, write to image memory or exception path.
struct CodeCaptureHeader {
  std::uint32_t magic = 0x43434354, version = 1, bytes = sizeof(CodeCaptureHeader), process = 0;
  std::uint64_t module = 0;
  std::uint32_t image_size = 0, timestamp = 0;
  std::uint32_t match_rva = 0, window_rva = 0, window_bytes = 0, pattern_bytes = 0;
};
// leaq 0xad84(%rcx),%r12; nopl; movq (%rsi,%r14),%rdi; movq 0x10(%rdi),%rax;
// movq %rax,(%r14); movl 0x28(%rdi),%eax; movl %eax,(%r12); movq 0x48(%rdi),%rax
// from the 1.9.12.0 dump; the fault instruction starts at pattern offset 16.
inline constexpr std::array<std::uint8_t, 34> renderer_fault_pattern{0x4c, 0x8d, 0xa1, 0x84, 0xad, 0x00, 0x00, 0x0f, 0x1f, 0x44, 0x00, 0x00,
                                                                     0x4a, 0x8b, 0x3c, 0x36, 0x48, 0x8b, 0x47, 0x10, 0x49, 0x89, 0x06, 0x8b,
                                                                     0x47, 0x28, 0x41, 0x89, 0x04, 0x24, 0x48, 0x8b, 0x47, 0x48};
inline constexpr std::uint32_t renderer_fault_pattern_fault_offset = 16;
// 128 KiB before the fault covers frames 0-4 of the retained callstack; 768 KiB
// after covers frames 5-12 (+0x3EF47BF..+0x3F1954E on 1.9.12.0).
inline constexpr std::uint32_t code_window_before = 128 * 1024, code_window_after = 768 * 1024;

inline CodeCaptureResult capture_fault_site_code(const wchar_t* directory, HMODULE module, DWORD process) noexcept {
  CodeCaptureResult result;
  if (!directory || !module || !process) {
    result.error = "code_capture_arguments";
    return result;
  }
  taxi_camera::WidePathBuffer path_buffer, old_path_buffer;  // 128 KiB total: heap.
  if (!path_buffer || !old_path_buffer) {
    result.error = "code_capture_allocation";
    return result;
  }
  wchar_t* const path = path_buffer.get();
  wchar_t* const old_path = old_path_buffer.get();
  const auto base = reinterpret_cast<const std::uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) {
    result.error = "code_capture_dos_header";
    return result;
  }
  const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (headers->Signature != IMAGE_NT_SIGNATURE || headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      headers->FileHeader.NumberOfSections == 0 || headers->FileHeader.NumberOfSections > 96) {
    result.error = "code_capture_nt_header";
    return result;
  }
  const auto image_size = headers->OptionalHeader.SizeOfImage;
  const auto* section = IMAGE_FIRST_SECTION(headers);
  result.searched = true;
  for (unsigned index = 0; index < headers->FileHeader.NumberOfSections; ++index, ++section) {
    if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) || !section->Misc.VirtualSize)
      continue;
    const auto begin = section->VirtualAddress;
    if (begin >= image_size || section->Misc.VirtualSize > image_size - begin || section->Misc.VirtualSize < renderer_fault_pattern.size())
      continue;
    const auto end = begin + section->Misc.VirtualSize;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(base + begin, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        !(region.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_EXECUTE)))
      continue;
    const std::uint8_t* cursor = base + begin;
    const std::uint8_t* limit = base + end - renderer_fault_pattern.size();
    while (cursor <= limit) {
      const auto* hit =
          static_cast<const std::uint8_t*>(std::memchr(cursor, renderer_fault_pattern[0], static_cast<std::size_t>(limit - cursor) + 1));
      if (!hit)
        break;
      if (std::memcmp(hit, renderer_fault_pattern.data(), renderer_fault_pattern.size()) == 0) {
        const auto match = static_cast<std::uint32_t>(hit - base);
        const auto window_begin = match > begin + code_window_before ? match - code_window_before : begin;
        const auto window_end = end - match > code_window_after ? match + code_window_after : end;
        // One capture is retained: the current session replaces earlier files.
        if (std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\renderer-fault-code-*.bin", directory) >= 0) {
          WIN32_FIND_DATAW entry{};
          const auto search = FindFirstFileW(path, &entry);
          if (search != INVALID_HANDLE_VALUE) {
            do {
              if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
              if (std::swprintf(old_path, taxi_camera::WidePathBuffer::size(), L"%ls\\%ls", directory, entry.cFileName) >= 0)
                DeleteFileW(old_path);
            } while (FindNextFileW(search, &entry));
            FindClose(search);
          }
        }
        if (std::swprintf(path, taxi_camera::WidePathBuffer::size(), L"%ls\\renderer-fault-code-%lu.bin", directory, process) < 0) {
          result.error = "code_capture_path";
          return result;
        }
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
          result.error = "code_capture_create";
          return result;
        }
        CodeCaptureHeader header;
        header.process = process;
        header.module = reinterpret_cast<std::uint64_t>(base);
        header.image_size = image_size;
        header.timestamp = headers->FileHeader.TimeDateStamp;
        header.match_rva = match + renderer_fault_pattern_fault_offset;
        header.window_rva = window_begin;
        header.window_bytes = window_end - window_begin;
        header.pattern_bytes = static_cast<std::uint32_t>(renderer_fault_pattern.size());
        DWORD written = 0;
        const bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
                        WriteFile(file, base + window_begin, header.window_bytes, &written, nullptr) && written == header.window_bytes;
        CloseHandle(file);
        result.written = ok;
        result.match_rva = header.match_rva;
        result.window_rva = header.window_rva;
        result.window_bytes = header.window_bytes;
        result.error = ok ? "" : "code_capture_write";
        return result;
      }
      cursor = hit + 1;
    }
  }
  result.error = "code_capture_pattern_absent";
  return result;
}

// Called outside DllMain, after the bridge verifies its host and pins itself.
// The process owns this mapping/handler until exit; Windows retains the dirty
// mapped-file pages after a process crash. No debugger is attached.
inline bool initialize() noexcept {
  if (record)
    return true;
  taxi_camera::WidePathBuffer directory_buffer, folder_buffer;  // Heap, not stack.
  if (!directory_buffer || !folder_buffer)
    return false;
  wchar_t* const directory = directory_buffer.get();
  const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, taxi_camera::WidePathBuffer::size());
  if (!length || length > 32000)
    return false;
  wchar_t* const folder = folder_buffer.get();
  std::swprintf(folder, taxi_camera::WidePathBuffer::size(), L"%ls\\Taxi Cam", directory);
  CreateDirectoryW(folder, nullptr);
  Mapping mapping;
  if (!create_record(folder, GetCurrentProcessId(), GetTickCount64(), reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)), mapping))
    return false;
  record = mapping.memory;
  if (!AddVectoredExceptionHandler(1, handler)) {
    record = nullptr;
    return false;
  }
  record_file = mapping.file;
  mapping.file = INVALID_HANDLE_VALUE;
  mapping.memory = nullptr;
  // Process-lifetime diagnostic result; the bridge pins itself until exit.
  static CodeCaptureResult capture_result;
  capture_result = capture_fault_site_code(folder, GetModuleHandleW(nullptr), GetCurrentProcessId());
  code_capture = &capture_result;
  return true;
}
}  // namespace taxi_camera::standalone::crash_evidence
