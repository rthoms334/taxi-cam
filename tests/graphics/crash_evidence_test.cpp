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

// A real overflow in a child process: the handler has to work on the stack an
// overflow leaves, and the mapped record has to outlive the process.
volatile unsigned recursion_limit = ~0u;
[[gnu::noinline]] unsigned recurse(unsigned depth) {
  volatile char pad[256];
  pad[0] = static_cast<char>(depth);
  if (depth >= recursion_limit)
    return 0;
  return recurse(depth + 1) + static_cast<unsigned>(pad[0]);
}
// Registered last: ends the child with the exception code, without a Windows
// error report for a crash the test caused on purpose.
LONG CALLBACK end_process(EXCEPTION_POINTERS* exception) {
  TerminateProcess(GetCurrentProcess(), exception->ExceptionRecord->ExceptionCode);
  return EXCEPTION_CONTINUE_SEARCH;
}
int overflow_child(const wchar_t* directory) {
  static ce::Mapping mapping;
  if (!ce::create_record(directory, GetCurrentProcessId(), GetTickCount64(), reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)),
                         mapping))
    return 2;
  ce::record = mapping.memory;
  if (!AddVectoredExceptionHandler(1, ce::handler) || !AddVectoredExceptionHandler(0, end_process))
    return 3;
  return recurse(0) ? 4 : 5;
}
void real_overflow_case() {
  Directory directory;
  wchar_t image[32768]{};
  assert(GetModuleFileNameW(nullptr, image, 32768));
  assert(SetEnvironmentVariableW(L"TAXI_CAM_OVERFLOW_CHILD", directory.path.c_str()));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  assert(CreateProcessW(image, nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
  SetEnvironmentVariableW(L"TAXI_CAM_OVERFLOW_CHILD", nullptr);
  assert(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0);
  DWORD code = 0;
  assert(GetExitCodeProcess(child.hProcess, &code) && code == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW));
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  WIN32_FIND_DATAW entry{};
  const auto search = FindFirstFileW(directory.file(L"renderer-fault-*.bin").c_str(), &entry);
  assert(search != INVALID_HANDLE_VALUE && ce::record_filename(entry.cFileName));
  FindClose(search);
  const auto file = CreateFileW(directory.file(entry.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
  assert(file != INVALID_HANDLE_VALUE);
  ce::Record record;
  DWORD read = 0;
  assert(ReadFile(file, static_cast<void*>(&record), sizeof(record), &read, nullptr) && read == sizeof(record));
  CloseHandle(file);
  assert(record.state == 2 && record.process == child.dwProcessId && record.code == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW));
  assert(record.stack_base > record.stack_limit && record.frame_count >= 1);
  // The recursion's return address repeats all the way up the scanned stack.
  bool repeated = false;
  for (std::uint32_t i = 0; i < record.frame_count; ++i)
    repeated = repeated || (record.frames[i].module == record.module && record.frames[i].hits >= 16);
  assert(repeated);
  char text[4096]{};
  assert(ce::describe_overflow(record, text, sizeof(text)) && std::strstr(text, "main+0x"));
  std::printf("PASS real stack overflow in a child process: %s\n", text);
}
}  // namespace

int main() {
  wchar_t child_directory[32768]{};
  if (const auto length = GetEnvironmentVariableW(L"TAXI_CAM_OVERFLOW_CHILD", child_directory, 32768); length && length < 32768)
    return overflow_child(child_directory);
  retention_cases();
  real_overflow_case();
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
  // A stack overflow is recorded wherever it happens: the values on the stack
  // that point into executable image memory, by module and offset with a
  // repeat count, and nothing else from the stack.
  {
    // The scan gives up after this many unreadable pages; a shorter tail would let it reach other memory.
    const std::size_t tail = std::size_t{0x1000} * ce::overflow_unreadable_pages;
    auto* stack = static_cast<std::uint64_t*>(VirtualAlloc(nullptr, 0x2000 + tail, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(stack);
    DWORD previous = 0;
    // The page at the stack pointer is the guard page; the pages after the
    // values are unreadable, which ends the scan.
    assert(VirtualProtect(stack, 0x1000, PAGE_READWRITE | PAGE_GUARD, &previous));
    assert(VirtualProtect(reinterpret_cast<unsigned char*>(stack) + 0x2000, tail, PAGE_NOACCESS, &previous));
    const auto own = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    const auto system = reinterpret_cast<std::uint64_t>(GetModuleHandleW(L"ntdll.dll"));
    const auto own_code = reinterpret_cast<std::uint64_t>(static_cast<void (*)()>(retention_cases));
    const auto system_code = reinterpret_cast<std::uint64_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlCaptureContext"));
    assert(own && system && own_code > own && system_code > system);
    auto* values = stack + 0x1000 / 8;
    // The read-only shared system page: no allocation of this test lies in or just below it.
    const std::uint64_t fake_limit = 0x7ffe0000, fake_base = 0x7ffe1000;
    values[0] = 0;                                         // Not an address.
    values[1] = reinterpret_cast<std::uint64_t>(&values);  // Data, not an image.
    values[2] = own_code;
    values[3] = fake_limit + 0x80;  // A pointer into the thread's own stack.
    values[4] = system_code;
    values[5] = own_code;
    values[6] = own;  // Image headers are not executable.
    values[7] = system_code;
    values[8] = own_code;
    CONTEXT overflow_context{};
    overflow_context.Rsp = reinterpret_cast<std::uint64_t>(stack) + 0xff4;  // Unaligned, inside the guard page.
    EXCEPTION_RECORD overflow{};
    overflow.ExceptionCode = static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW);
    overflow.ExceptionAddress = reinterpret_cast<void*>(system_code + 1);
    ce::Record recorded;
    recorded.module = own;
    assert(ce::capture_overflow(&recorded, overflow, overflow_context, fake_limit, fake_base) == EXCEPTION_CONTINUE_SEARCH);
    assert(recorded.state == 2 && recorded.code == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW) && recorded.version == 3);
    assert(recorded.fault_address == system_code + 1 && recorded.stack_limit == fake_limit && recorded.stack_base == fake_base);
    assert(recorded.frame_count == 3 && recorded.module_count == 2);
    assert(recorded.frames[0].module == system && recorded.frames[0].rva == system_code + 1 - system && recorded.frames[0].hits == 1);
    assert(recorded.frames[1].module == own && recorded.frames[1].rva == own_code - own && recorded.frames[1].hits == 3);
    assert(recorded.frames[2].module == system && recorded.frames[2].rva == system_code - system && recorded.frames[2].hits == 2);
    assert(recorded.modules[0].base == system && recorded.modules[0].size && !_stricmp(recorded.modules[0].name, "ntdll.dll"));
    assert(recorded.modules[1].base == own && recorded.modules[1].size && !recorded.modules[1].name[0]);
    // The guard page was queried, never touched.
    MEMORY_BASIC_INFORMATION guard{};
    assert(VirtualQuery(stack, &guard, sizeof(guard)) && (guard.Protect & PAGE_GUARD));
    // One shot: a second overflow leaves the record alone.
    overflow.ExceptionAddress = reinterpret_cast<void*>(own_code);
    assert(ce::capture_overflow(&recorded, overflow, overflow_context, fake_limit, fake_base) == EXCEPTION_CONTINUE_SEARCH);
    assert(recorded.fault_address == system_code + 1 && recorded.frame_count == 3);
    char text[512]{};
    assert(ce::describe_overflow(recorded, text, sizeof(text)) == std::strlen(text));
    char expected[512]{};
    std::snprintf(expected, sizeof(expected), "thread=%lu stack_kb=4 fault=0x%llx chain=%s+0x%lx*1,main+0x%lx*3,%s+0x%lx*2",
                  GetCurrentThreadId(), static_cast<unsigned long long>(system_code + 1), recorded.modules[0].name,
                  static_cast<unsigned long>(recorded.frames[0].rva), static_cast<unsigned long>(recorded.frames[1].rva),
                  recorded.modules[0].name, static_cast<unsigned long>(recorded.frames[2].rva));
    assert(!std::strcmp(text, expected));
    // A short buffer is filled and terminated; counts read from a file are bounded.
    char brief[24]{};
    assert(ce::describe_overflow(recorded, brief, sizeof(brief)) == sizeof(brief) - 1 && !std::strncmp(brief, expected, sizeof(brief) - 1));
    recorded.frame_count = 1000;
    recorded.module_count = 1000;
    char bounded[4096]{};
    assert(ce::describe_overflow(recorded, bounded, sizeof(bounded)) && std::strlen(bounded) < sizeof(bounded) - 1);
    // Through the handler entry, with this thread's real stack bounds.
    ce::Record dispatched;
    EXCEPTION_POINTERS overflow_pointers{&overflow, &overflow_context};
    assert(ce::capture(&dispatched, &overflow_pointers) == EXCEPTION_CONTINUE_SEARCH && dispatched.state == 2);
    assert(dispatched.stack_base > dispatched.stack_limit && dispatched.frame_count >= 2);
    // Other records and codes have no overflow description.
    assert(!ce::describe_overflow(record, text, sizeof(text)) && !ce::describe_overflow(ce::Record{}, text, sizeof(text)));
    assert(VirtualFree(stack, 0, MEM_RELEASE));
  }
  // Legacy 4 KiB version-1 records remain valid retention candidates; current
  // 8 KiB records of versions 2 and 3 are too, and other sizes or versions are not.
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
    assert(write(3, ce::record_file_bytes, 3, static_cast<std::uint32_t>(sizeof(ce::Record))));
    assert(!write(6, ce::record_file_bytes, 4, 1400));
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
      "code capture pattern search, stack overflow chain by module and offset, exception continues unchanged.");
}
