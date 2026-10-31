#include "../../src/bridge/crash_evidence.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace ce = taxi_camera::standalone::crash_evidence;
namespace {
struct Directory {
  std::wstring path;
  Directory() {
    static unsigned sequence = 0;
    wchar_t root[32768]{}, name[100]{};
    assert(GetFullPathNameW(L"build", 32768, root, nullptr) != 0);
    CreateDirectoryW(root, nullptr);
    std::swprintf(name, std::size(name), L"\\crash-retention-%lu-%llu-%u", GetCurrentProcessId(),
                  static_cast<unsigned long long>(GetTickCount64()), ++sequence);
    path = root;
    path += name;
    assert(CreateDirectoryW(path.c_str(), nullptr));
  }
  std::wstring file(const wchar_t* name) const { return path + L"\\" + name; }
  std::wstring record(unsigned ticks) const {
    wchar_t name[64]{};
    std::swprintf(name, std::size(name), L"renderer-fault-123-%u.bin", ticks);
    return file(name);
  }
  unsigned count() const {
    unsigned count = 0;
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW(file(L"*").c_str(), &entry);
    assert(search != INVALID_HANDLE_VALUE);
    do {
      if (!(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && ce::record_filename(entry.cFileName))
        ++count;
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return count;
  }
  ~Directory() {
    // This uniquely created fixture contains only flat test-owned files and
    // empty test-owned directories. No recursive deletion or path traversal.
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW(file(L"*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if (!std::wcscmp(entry.cFileName, L".") || !std::wcscmp(entry.cFileName, L".."))
          continue;
        const auto item = file(entry.cFileName);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
          assert(RemoveDirectoryW(item.c_str()));
        else
          assert(DeleteFileW(item.c_str()));
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
    assert(RemoveDirectoryW(path.c_str()));
  }
};
void creation_time(HANDLE file, unsigned ordinal) {
  ULARGE_INTEGER value{};
  value.QuadPart = 130000000000000000ull + static_cast<std::uint64_t>(ordinal) * 10000000;
  const FILETIME time{value.LowPart, value.HighPart};
  assert(SetFileTime(file, &time, nullptr, nullptr));
}
bool exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
void raw_record(const Directory& directory, unsigned ticks, LONG state, bool zero_length = false) {
  const auto file = CreateFileW(directory.record(ticks).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  assert(file != INVALID_HANDLE_VALUE);
  if (!zero_length) {
    std::array<unsigned char, ce::record_file_bytes> bytes{};
    ce::Record record;
    record.process = 123;
    record.state = state;
    std::memcpy(bytes.data(), &record, sizeof(record));
    DWORD written{};
    assert(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
  }
  creation_time(file, ticks);
  CloseHandle(file);
}
void retention_cases() {
  assert(ce::record_filename(L"renderer-fault-1-0.bin"));
  assert(ce::record_filename(L"renderer-fault-4294967295-18446744073709551615.bin"));
  for (const auto* name : {L"renderer-fault-0-1.bin", L"renderer-fault-01-2.bin", L"renderer-fault-1-02.bin",
                           L"renderer-fault-4294967296-1.bin", L"renderer-fault-1-18446744073709551616.bin", L"renderer-fault-1-2.bin.bak",
                           L"renderer-fault-1-2.BIN", L"renderer-fault-notes.bin"})
    assert(!ce::record_filename(name));
  {
    Directory directory;
    for (unsigned i = 1; i <= 80; ++i) {
      ce::Mapping current;
      assert(ce::create_record(directory.path.c_str(), 123, i, 0x10000000, current));
      creation_time(current.file, i);
      assert(current.memory->state == 0 && current.memory->magic == 0x54434352 && current.memory->process == 123);
      assert(directory.count() == (std::min)(i, ce::retained_records));
    }
    for (unsigned i = 1; i <= 80; ++i)
      assert(exists(directory.record(i)) == (i >= 65));
  }
  {
    Directory directory;
    for (unsigned i = 1; i <= 80; ++i)
      raw_record(directory, i, i <= 60 ? 0 : 2);
    ce::Mapping current;
    assert(ce::create_record(directory.path.c_str(), 123, 81, 1, current));
    assert(directory.count() == ce::retained_records);
    for (unsigned i = 1; i <= 80; ++i)
      assert(exists(directory.record(i)) == (i >= 66));
  }
  {
    Directory directory;
    raw_record(directory, 1, 2);
    for (unsigned i = 2; i <= 16; ++i)
      raw_record(directory, i, 0);
    ce::Mapping current;
    assert(ce::create_record(directory.path.c_str(), 123, 17, 1, current));
    assert(exists(directory.record(1)) && !exists(directory.record(2)) && exists(directory.record(3)));
    assert(directory.count() == ce::retained_records);
  }
  {
    Directory directory;
    for (unsigned i = 1; i <= 15; ++i)
      raw_record(directory, i, 2);
    raw_record(directory, 16, 0, true);
    ce::Mapping current;
    assert(ce::create_record(directory.path.c_str(), 123, 17, 1, current));
    assert(exists(directory.record(1)) && !exists(directory.record(16)) && directory.count() == ce::retained_records);
  }
  {
    Directory directory;
    std::array<ce::Mapping, ce::retained_records> active;
    for (unsigned i = 0; i < active.size(); ++i) {
      assert(ce::create_record(directory.path.c_str(), 123, i + 1, 0x1000 + i, active[i]));
      creation_time(active[i].file, i + 1);
      active[i].memory->context.Rbx = 500 + i;
    }
    ce::Mapping refused;
    assert(!ce::create_record(directory.path.c_str(), 123, 17, 1, refused));
    assert(directory.count() == ce::retained_records && !exists(directory.record(17)));
    for (unsigned i = 0; i < active.size(); ++i)
      assert(exists(directory.record(i + 1)) && active[i].memory->context.Rbx == 500 + i);
    active[5].close();
    assert(ce::create_record(directory.path.c_str(), 123, 17, 1, refused));
    assert(!exists(directory.record(6)) && directory.count() == ce::retained_records);
  }
  {
    Directory directory;
    ce::Mapping legacy;
    assert(ce::create_record(directory.path.c_str(), 123, 1, 1, legacy));
    creation_time(legacy.file, 1);
    legacy.memory->context.Rbx = 999;
    CloseHandle(legacy.file);  // Prior builds retained the mapping alone.
    legacy.file = INVALID_HANDLE_VALUE;
    for (unsigned i = 2; i <= 24; ++i) {
      ce::Mapping current;
      assert(ce::create_record(directory.path.c_str(), 123, i, 1, current));
      creation_time(current.file, i);
      assert(exists(directory.record(1)) && legacy.memory->context.Rbx == 999 && directory.count() <= ce::retained_records);
    }
  }
  {
    Directory directory;
    for (unsigned i = 1; i <= ce::retained_records; ++i) {
      const auto file = CreateFileW(directory.record(i).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      assert(file != INVALID_HANDLE_VALUE);
      std::array<unsigned char, ce::record_file_bytes> unknown{};
      DWORD written{};
      assert(WriteFile(file, unknown.data(), static_cast<DWORD>(unknown.size()), &written, nullptr) && written == unknown.size());
      CloseHandle(file);
    }
    ce::Mapping refused;
    assert(!ce::create_record(directory.path.c_str(), 123, 17, 1, refused));
    assert(directory.count() == ce::retained_records && !exists(directory.record(17)));
    for (unsigned i = 1; i <= ce::retained_records; ++i)
      assert(exists(directory.record(i)));
  }
  {
    Directory directory;
    for (unsigned i = 1; i <= ce::maintenance_entries + 1; ++i)
      raw_record(directory, i, 0, true);
    ce::Mapping refused;
    assert(!ce::create_record(directory.path.c_str(), 123, ce::maintenance_entries + 2, 1, refused));
    assert(directory.count() == ce::maintenance_entries + 1 && !exists(directory.record(ce::maintenance_entries + 2)));
  }
  {
    Directory directory;
    const auto lock = CreateFileW(directory.file(L"renderer-fault-retention.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    ce::Mapping refused;
    assert(!ce::create_record(directory.path.c_str(), 123, 1, 1, refused) && directory.count() == 0);
    CloseHandle(lock);
    for (const auto* name : {L"renderer-fault-notes.bin", L"renderer-fault-1-2.bin.bak", L"renderer-fault-0-1.bin", L"keep.bin"}) {
      const auto file = CreateFileW(directory.file(name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      assert(file != INVALID_HANDLE_VALUE);
      DWORD written{};
      assert(WriteFile(file, "keep", 4, &written, nullptr) && written == 4);
      CloseHandle(file);
    }
    assert(CreateDirectoryW(directory.file(L"renderer-fault-999-999.bin").c_str(), nullptr));
    for (unsigned i = 1; i <= 40; ++i) {
      ce::Mapping current;
      assert(ce::create_record(directory.path.c_str(), 123, i, 1, current));
    }
    for (const auto* name : {L"renderer-fault-notes.bin", L"renderer-fault-1-2.bin.bak", L"renderer-fault-0-1.bin", L"keep.bin"}) {
      const auto file = CreateFileW(directory.file(name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
      assert(file != INVALID_HANDLE_VALUE);
      char value[4]{};
      DWORD read{};
      assert(ReadFile(file, value, 4, &read, nullptr) && read == 4 && std::memcmp(value, "keep", 4) == 0);
      CloseHandle(file);
    }
    assert(exists(directory.file(L"renderer-fault-999-999.bin")));
  }
  std::puts(
      "PASS renderer fault retention: sixteen records, empty-first oldest pruning, active mappings/locks preserved, unrelated files "
      "untouched.");
}
}  // namespace

int main() {
  retention_cases();
  ce::Record record;
  record.module = 0x10000000;
  CONTEXT context{};
  EXCEPTION_RECORD exception{};
  EXCEPTION_POINTERS pointers{&exception, &context};
  assert(ce::capture(&record, nullptr) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  // The site the arm-time locator found in the loaded image.
  ce::CodeCaptureResult site;
  site.match_rva = 0x3E64BC4;
  ce::code_capture = &site;
  exception.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  exception.ExceptionAddress = reinterpret_cast<void*>(record.module + site.match_rva);
  context.Rip = reinterpret_cast<std::uint64_t>(exception.ExceptionAddress);
  exception.NumberParameters = 2;
  exception.ExceptionInformation[0] = 0;   // Read access violation.
  exception.ExceptionInformation[1] = 16;  // Null binding object +16.
  --context.Rip;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  ++context.Rip;
  exception.ExceptionCode = EXCEPTION_BREAKPOINT;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  exception.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  exception.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS + 1;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  exception.NumberParameters = 2;
  context.Rdi = 0;
  context.Rbx = 1234;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 2);
  assert(record.information[1] == 16 && record.context.Rdi == 0 && record.context.Rbx == 1234);
  // RBX did not name committed memory: registers are kept, nothing was copied.
  assert(record.evidence_flags == 0 && record.slot1_record_address == 0);
  context.Rbx = 4321;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.context.Rbx == 1234);
  // With RBX naming a readable render context, the output-merger state, the
  // bound-target count and the slot-1 record are copied after VirtualQuery.
  {
    auto* context_memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x9000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* record_memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(context_memory && record_memory);
    for (unsigned i = 0; i < 0xA0; ++i)
      context_memory[ce::output_merger_state_offset + i] = static_cast<unsigned char>(0x30 + i);
    for (unsigned i = 0; i < 0x100; ++i)
      record_memory[i] = static_cast<unsigned char>(0xC0 + i);
    const std::uint64_t slot1 = reinterpret_cast<std::uint64_t>(record_memory);
    std::memcpy(context_memory + ce::slot1_record_offset, &slot1, sizeof(slot1));
    const std::uint32_t count = 1;
    std::memcpy(context_memory + ce::bound_target_count_offset, &count, sizeof(count));
    ce::Record copied;
    copied.module = 0x10000000;
    context.Rbx = reinterpret_cast<std::uint64_t>(context_memory);
    assert(ce::capture(&copied, &pointers) == EXCEPTION_CONTINUE_SEARCH && copied.state == 2);
    assert(copied.evidence_flags == 7 && copied.bound_targets == 1 && copied.slot1_record_address == slot1);
    assert(std::memcmp(copied.output_merger_state, context_memory + ce::output_merger_state_offset, 0xA0) == 0);
    assert(std::memcmp(copied.slot1_record, record_memory, 0x100) == 0);
    // A slot-1 pointer into unreadable memory is recorded but not followed.
    ce::Record unreadable;
    unreadable.module = 0x10000000;
    DWORD previous = 0;
    assert(VirtualProtect(record_memory, 0x1000, PAGE_NOACCESS, &previous));
    assert(ce::capture(&unreadable, &pointers) == EXCEPTION_CONTINUE_SEARCH && unreadable.state == 2);
    assert(unreadable.evidence_flags == 5 && unreadable.slot1_record_address == slot1);
    assert(VirtualProtect(record_memory, 0x1000, PAGE_READWRITE, &previous));
    // A context whose state straddles the end of its committed region is skipped.
    ce::Record straddled;
    straddled.module = 0x10000000;
    context.Rbx = reinterpret_cast<std::uint64_t>(context_memory) + 0x9000 - ce::output_merger_state_offset - 0x10;
    assert(ce::capture(&straddled, &pointers) == EXCEPTION_CONTINUE_SEARCH && straddled.state == 2 && (straddled.evidence_flags & 1u) == 0);
    context.Rbx = 1234;
    VirtualFree(record_memory, 0, MEM_RELEASE);
    VirtualFree(context_memory, 0, MEM_RELEASE);
  }
  // Legacy 4 KiB version-1 records remain valid retention candidates; a
  // current 8 KiB record is one too, and other sizes or versions are not.
  {
    Directory directory;
    const auto write = [&](unsigned ticks, std::size_t size, std::uint32_t version, std::uint32_t bytes) {
      const auto file = CreateFileW(directory.record(ticks).c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
      assert(file != INVALID_HANDLE_VALUE);
      std::vector<unsigned char> image(size);
      const std::uint32_t header[4]{0x54434352, version, bytes, 123};
      std::memcpy(image.data(), header, sizeof(header));
      DWORD written{};
      assert(WriteFile(file, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) && written == image.size());
      assert(SetFilePointer(file, 0, nullptr, FILE_BEGIN) == 0);
      ce::Candidate candidate;
      const bool valid = ce::inspect_record(file, candidate);
      CloseHandle(file);
      return valid;
    };
    assert(write(1, ce::legacy_record_file_bytes, 1, 1400));
    assert(write(2, ce::record_file_bytes, 2, static_cast<std::uint32_t>(sizeof(ce::Record))));
    assert(!write(3, ce::legacy_record_file_bytes, 3, 1400));
    assert(!write(4, 2048, 1, 1400));
    assert(!write(5, ce::legacy_record_file_bytes, 1, 4097));
  }
  assert(ce::capture(nullptr, &pointers) == EXCEPTION_CONTINUE_SEARCH);
  // The arm-time locator admits the instruction at the current image's RVA;
  // without a located site no fault is accepted, at any RVA.
  ce::code_capture = nullptr;
  ce::Record unlocated;
  unlocated.module = 0x10000000;
  exception.ExceptionAddress = reinterpret_cast<void*>(unlocated.module + 64028644);  // The 1.8.16.0 site.
  context.Rip = reinterpret_cast<std::uint64_t>(exception.ExceptionAddress);
  assert(ce::capture(&unlocated, &pointers) == EXCEPTION_CONTINUE_SEARCH && unlocated.state == 0);
  ce::Record located;
  located.module = 0x10000000;
  exception.ExceptionAddress = reinterpret_cast<void*>(located.module + 0x3E5C054);
  context.Rip = reinterpret_cast<std::uint64_t>(exception.ExceptionAddress);
  assert(ce::capture(&located, &pointers) == EXCEPTION_CONTINUE_SEARCH && located.state == 0);
  ce::CodeCaptureResult capture_result;
  capture_result.match_rva = 0x3E5C054;
  ce::code_capture = &capture_result;
  assert(ce::capture(&located, &pointers) == EXCEPTION_CONTINUE_SEARCH && located.state == 2 && located.fault_rva == 0x3E5C054);
  ce::code_capture = nullptr;
  // This test image does not contain the renderer instruction sequence: the
  // executable sections are searched, nothing is written, and the error names it.
  {
    Directory directory;
    const auto absent = ce::capture_fault_site_code(directory.path.c_str(), GetModuleHandleW(nullptr), GetCurrentProcessId());
    assert(absent.searched && !absent.written && absent.match_rva == 0 && !std::strcmp(absent.error, "code_capture_pattern_absent"));
    assert(!ce::capture_fault_site_code(nullptr, GetModuleHandleW(nullptr), 1).searched);
    assert(!ce::capture_fault_site_code(directory.path.c_str(), nullptr, 1).searched);
    assert(directory.count() == 0);
  }
  std::puts(
      "PASS renderer fault evidence: exact-site filter, located-site filter, bounded register copy, one-shot publication, "
      "code capture pattern search, exception continues unchanged.");
}
