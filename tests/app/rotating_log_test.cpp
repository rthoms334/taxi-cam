#include "../../src/shared/rotating_log.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct Directory {
  std::wstring path;
  Directory() {
    wchar_t temporary[MAX_PATH]{}, unique[MAX_PATH]{};
    const auto size = GetTempPathW(MAX_PATH, temporary);
    require(size && size < MAX_PATH, "Get own temporary directory");
    require(GetTempFileNameW(temporary, L"tcl", 0, unique) != 0, "Reserve unique fixture name");
    require(DeleteFileW(unique) && CreateDirectoryW(unique, nullptr), "Create isolated log fixture directory");
    path = unique;
  }
  ~Directory() {
    // Delete only files directly inside the unique directory created above.
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
          DeleteFileW((path + L"\\" + entry.cFileName).c_str());
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
  }
  std::wstring file(const wchar_t* name) const { return path + L"\\" + name; }
};
bool exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
void write(const std::wstring& path, std::string_view bytes) {
  Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  require(file.value != INVALID_HANDLE_VALUE, "Create fixture bytes");
  DWORD written = 0;
  require(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(),
          "Write all fixture bytes");
}
std::string read(const std::wstring& path) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
  require(file.value != INVALID_HANDLE_VALUE, "Read own log fixture");
  LARGE_INTEGER length{};
  require(GetFileSizeEx(file.value, &length) && length.QuadPart >= 0 && length.QuadPart <= 16 * 1024 * 1024, "Bound fixture read");
  std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
  DWORD actual = 0;
  require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &actual, nullptr) && actual == bytes.size(),
          "Read complete fixture bytes");
  return bytes;
}
void bounded(const std::wstring& path, std::uint64_t limit) {
  for (const auto& name : {path, path + L".1"})
    if (exists(name))
      require(read(name).size() <= limit, "Active log and archive each obey their configured byte limit");
}
std::string numbered(unsigned sequence) {
  char text[20]{};
  std::snprintf(text, sizeof(text), "row%04u\n", sequence);
  return text;
}
// Another thread of this process owning a log's named lock until destroyed.
struct LockOwner {
  wchar_t name[80]{};
  Handle held{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  Handle thread{nullptr};
  explicit LockOwner(const std::wstring& path) {
    std::swprintf(name, 80, L"Local\\TaxiCam.Log.%016llx", static_cast<unsigned long long>(log_detail::path_key(path)));
    if (held.value && release.value)
      thread.value = CreateThread(nullptr, 0, run, this, 0, nullptr);
  }
  ~LockOwner() {
    if (release.value)
      SetEvent(release.value);
    if (thread.value)
      WaitForSingleObject(thread.value, 10000);
  }
  LockOwner(const LockOwner&) = delete;
  LockOwner& operator=(const LockOwner&) = delete;
  static DWORD WINAPI run(void* parameter) {
    auto& owner = *static_cast<LockOwner*>(parameter);
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, owner.name);
    if (!mutex)
      return 1;
    if (WaitForSingleObject(mutex, 5000) == WAIT_OBJECT_0) {
      SetEvent(owner.held.value);
      WaitForSingleObject(owner.release.value, 10000);
      ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
    return 0;
  }
};
void boundary_and_rotation(const Directory& directory) {
  const auto path = directory.file(L"bound.log");
  constexpr unsigned limit = 16;
  require(append_rotating_log(path, numbered(0), limit), "First complete record");
  require(append_rotating_log(path, numbered(1), limit), "Append exactly to byte limit");
  require(read(path) == numbered(0) + numbered(1) && !exists(path + L".1"), "Exact limit does not rotate prematurely");
  for (unsigned i = 2; i < 24; ++i) {
    require(append_rotating_log(path, numbered(i), limit), "Logging continues after repeated cap crossings");
    bounded(path, limit);
    const auto active = read(path), archive = read(path + L".1");
    require(active == (i % 2 ? numbered(i - 1) + numbered(i) : numbered(i)), "Newest records remain in the active log");
    const unsigned previous = i % 2 ? i - 3 : i - 2;
    require(archive == numbered(previous) + numbered(previous + 1), "Rotation replaces the archive with the newest complete generation");
  }
}
void legacy_tail(const Directory& directory) {
  // Each line has ten bytes and includes a two-byte UTF-8 character. A retained
  // window that cuts inside that character must discard the entire partial line.
  const std::array<std::string, 3> tail{"old-\xc3\xa9-00\n", "old-\xc3\xa9-01\n", "old-\xc3\xa9-02\n"};
  require(tail[0].size() == 10, "Known UTF-8 fixture line width");
  const std::string prefix(BridgeLogBytes + 1024, 'x');
  const auto legacy = prefix + "\n" + tail[0] + tail[1] + tail[2];
  for (const unsigned limit : {30u, 25u}) {
    const auto path = directory.file(limit == 30 ? L"legacy-boundary.log" : L"legacy-cut.log");
    write(path, legacy);
    write(path + L".1", std::string(100, 'z') + "\n");
    require(append_rotating_log(path, "fresh\r\n", limit), "Oversized legacy logs keep accepting current diagnostics");
    require(read(path) == "fresh\r\n", "Rotation writes the new record to an empty current log");
    require(read(path + L".1") == (limit == 30 ? tail[0] + tail[1] + tail[2] : tail[1] + tail[2]),
            "Legacy archive retains newest complete UTF-8 lines without truncating a character or dropping an exact-boundary line");
    bounded(path, limit);
    for (unsigned i = 0; i < 20; ++i)
      require(append_rotating_log(path, "still logging\n", limit), "No permanent logging freeze after the old file-size cap");
    bounded(path, limit);
    require(read(path).ends_with("still logging\n"), "Latest diagnostics remain present after legacy recovery");
  }
  const auto oversized_line = directory.file(L"long-line.log");
  write(oversized_line, std::string(128, 'a') + "\n");
  require(append_rotating_log(oversized_line, "new\n", 16), "An oversized single legacy line does not block logging");
  require(read(oversized_line) == "new\n" && read(oversized_line + L".1").empty(), "No partial legacy line is archived");
  const auto unfinished = directory.file(L"unfinished-line.log");
  write(unfinished, std::string(128, 'x') + "\nkept1\nkept2\nbroken-\xc3");
  require(append_rotating_log(unfinished, "fresh\n", 25), "Incomplete final legacy record does not block logging");
  require(read(unfinished + L".1") == "kept1\nkept2\n" && read(unfinished) == "fresh\n",
          "Legacy recovery drops an incomplete final UTF-8 record and preserves complete preceding lines");
  bounded(unfinished, 25);
}
void invalid_and_failure(const Directory& directory) {
  const auto path = directory.file(L"invalid.log");
  write(path, "unchanged\n");
  const auto original = read(path);
  for (const auto limit : {std::uint64_t{0}, BridgeLogBytes + 1})
    require(!append_rotating_log(path, "record\n", limit), "Invalid size limit is rejected");
  require(!append_rotating_log(path, {}, 16), "Empty record rejected");
  require(!append_rotating_log(path, "record\n", 3), "Record larger than file limit rejected");
  require(!append_rotating_log(path, std::string(8193, 'x'), BridgeLogBytes), "Record larger than 8192 bytes rejected");
  require(!append_rotating_log({}, "record\n", 16), "Empty path rejected");
  require(!append_rotating_log(std::wstring(40000, L'x'), "record\n", 16), "Oversized path rejected");
  require(read(path) == original && !exists(path + L".1"), "Invalid input leaves existing diagnostics unchanged");
  const auto maximum = directory.file(L"maximum.log");
  const std::string maximum_record = std::string(8191, 'a') + "\n";
  require(append_rotating_log(maximum, maximum_record, 8192) && read(maximum) == maximum_record, "Exactly 8192 record bytes accepted");
  const auto opaque = directory.file(L"opaque.log");
  const std::string opaque_record("a\0\xff\r\n", 5);
  require(append_rotating_log(opaque, opaque_record, 16) && read(opaque) == opaque_record,
          "Logger preserves caller bytes without inserting newlines or imposing an encoding");

  const auto blocked = directory.file(L"blocked.log");
  write(blocked, numbered(0) + numbered(1));
  write(blocked + L".1", "archive\n");
  {
    Handle archive(
        CreateFileW((blocked + L".1").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(archive.value != INVALID_HANDLE_VALUE, "Hold an archive handle which denies replacement");
    const auto started = GetTickCount64();
    require(!append_rotating_log(blocked, numbered(2), 16), "Archive replacement failure is nonfatal");
    require(GetTickCount64() - started < 2000, "Unwritable diagnostics do not freeze their caller");
    require(read(blocked) == numbered(0) + numbered(1) && read(blocked + L".1") == "archive\n",
            "Failed archive replacement leaves current and archived diagnostics unchanged");
  }
  require(append_rotating_log(blocked, numbered(2), 16), "Logging recovers when archive replacement becomes possible");
  bounded(blocked, 16);
  const auto absent_parent = directory.file(L"missing\\no.log");
  require(!append_rotating_log(absent_parent, "record\n", 16), "An unwritable path is a nonfatal failed append");
}
std::vector<std::string_view> views(const std::vector<std::string>& records, std::size_t begin, std::size_t count) {
  return {records.begin() + static_cast<std::ptrdiff_t>(begin), records.begin() + static_cast<std::ptrdiff_t>(begin + count)};
}
void batches(const Directory& directory) {
  // Batches of every size up to four rotations: the files must match what
  // single appends of the same records produce, rotations included.
  constexpr unsigned limit = 16;
  const auto batched = directory.file(L"batch.log"), single = directory.file(L"batch-single.log");
  std::vector<std::string> rows;
  for (unsigned i = 0; i < 40; ++i)
    rows.push_back(numbered(i));
  std::size_t next = 0;
  for (const std::size_t size : {1u, 2u, 3u, 4u, 5u, 8u, 9u}) {
    const auto batch = views(rows, next, size);
    require(append_rotating_log_records(batched, batch.data(), batch.size(), limit), "A batch rotates between its records");
    for (const auto record : batch)
      require(append_rotating_log(single, record, limit), "Single reference append");
    const auto archive = batched + L".1";
    require(read(batched) == read(single) && exists(archive) == exists(single + L".1"), "A batch writes what single appends write");
    if (exists(archive))
      require(read(archive) == read(single + L".1") && read(archive).ends_with("\n"),
              "Mid-batch rotation archives the same complete records as single appends");
    bounded(batched, limit);
    next += size;
  }
  std::string expected;
  for (const auto& row : rows)
    expected += row;
  const auto ordered = directory.file(L"batch-order.log");
  const auto all = views(rows, 0, rows.size());
  require(
      append_rotating_log_records(ordered, all.data(), all.size(), BridgeLogBytes) && read(ordered) == expected && !exists(ordered + L".1"),
      "A batch keeps record order");

  // A legacy oversized log rotates on the first record; the rest follow it.
  const std::string legacy = std::string(BridgeLogBytes + 1024, 'x') + "\nold-1\nold-2\n";
  const auto legacy_batch = directory.file(L"batch-legacy.log"), legacy_single = directory.file(L"batch-legacy-single.log");
  write(legacy_batch, legacy);
  write(legacy_single, legacy);
  const std::vector<std::string> fresh{"fresh\r\n", "more\r\n", "last\r\n"};
  const auto fresh_views = views(fresh, 0, fresh.size());
  require(append_rotating_log_records(legacy_batch, fresh_views.data(), fresh_views.size(), 30), "A batch recovers a legacy log");
  for (const auto record : fresh_views)
    require(append_rotating_log(legacy_single, record, 30), "Single legacy reference append");
  require(read(legacy_batch) == "fresh\r\nmore\r\nlast\r\n" && read(legacy_batch) == read(legacy_single) &&
              read(legacy_batch + L".1") == "old-1\nold-2\n" && read(legacy_batch + L".1") == read(legacy_single + L".1"),
          "Legacy recovery in a batch matches single appends");

  // Validation is all or nothing: each invalid record follows valid ones.
  const auto invalid = directory.file(L"batch-invalid.log");
  write(invalid, "unchanged\n");
  const std::string valid = "record\n", oversized(MaxLogRecordBytes + 1, 'x'), over_limit = "0123456789abcdef\n";
  const std::string_view with_empty[] = {valid, valid, {}}, with_oversized[] = {valid, oversized}, with_over_limit[] = {valid, over_limit};
  require(!append_rotating_log_records(invalid, with_empty, 3, 16), "An empty record rejects the whole batch");
  require(!append_rotating_log_records(invalid, with_oversized, 2, BridgeLogBytes), "A record over 8192 bytes rejects the whole batch");
  require(!append_rotating_log_records(invalid, with_over_limit, 2, 16), "A record over the file limit rejects the whole batch");
  require(!append_rotating_log_records(invalid, with_empty, 0, 16), "An empty batch is rejected");
  require(!append_rotating_log_records(invalid, nullptr, 1, 16), "A missing batch is rejected");
  for (const auto bad_limit : {std::uint64_t{0}, BridgeLogBytes + 1})
    require(!append_rotating_log_records(invalid, with_empty, 2, bad_limit), "An invalid size limit rejects the batch");
  require(!append_rotating_log_records({}, with_empty, 2, 16), "An empty path rejects the batch");
  require(read(invalid) == "unchanged\n" && !exists(invalid + L".1"), "A rejected batch writes nothing");

  // A failed write rolls back that record and stops the batch. The locked
  // range is hit only by the long second record; the third would fit before it.
  const auto failing = directory.file(L"batch-write-failure.log");
  write(failing, numbered(0));
  const std::string first = numbered(1), long_row = "row-long-0002..\n", third = numbered(3);
  require(long_row.size() == 16, "Known long fixture row width");
  {
    Handle holder(CreateFileW(failing.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    require(holder.value != INVALID_HANDLE_VALUE, "Open a byte-range lock holder");
    OVERLAPPED region{};
    region.Offset = 24;
    require(LockFileEx(holder.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 8, 0, &region) != FALSE,
            "Lock bytes 24-31 against other writers");
    const std::string_view failing_rows[] = {first, long_row, third};
    require(!append_rotating_log_records(failing, failing_rows, 3, 4096), "A failed write fails the batch");
    region = {};
    region.Offset = 24;
    require(UnlockFileEx(holder.value, 0, 8, 0, &region) != FALSE, "Release the byte-range lock");
  }
  require(read(failing) == numbered(0) + first, "A failed write leaves no part of its record and stops the batch");

  // Another thread holding the log's lock drops the whole batch at once.
  const auto locked = directory.file(L"batch-locked.log");
  write(locked, "kept\n");
  const std::string_view pair[] = {valid, valid};
  {
    LockOwner owner(locked);
    require(owner.thread.value && WaitForSingleObject(owner.held.value, 5000) == WAIT_OBJECT_0, "Another thread holds the log lock");
    const auto started = GetTickCount64();
    require(!append_rotating_log_records(locked, pair, 2, 4096), "A held log lock drops the batch");
    require(GetTickCount64() - started < 2000, "A held log lock does not block the writer");
    require(read(locked) == "kept\n", "A dropped batch writes nothing");
  }
  require(append_rotating_log_records(locked, pair, 2, 4096) && read(locked) == "kept\n" + valid + valid,
          "Batches resume once the lock is free");
}
void record_buffer(const Directory& directory) {
  std::vector<std::vector<std::string>> written;
  const auto collect = [&](const std::string_view* records, std::size_t count) { written.emplace_back(records, records + count); };
  LogRecordBuffer buffer;
  require(!buffer.add(std::string(100, 'a'), collect) && written.empty(), "No record needing storage is held before reserve");
  require(buffer.reserve(), "Reserve the buffer's byte bound once");
  std::vector<std::string> rows;
  for (unsigned i = 0; i <= LogRecordBuffer::MaxRecords; ++i)
    rows.push_back(numbered(i));
  for (unsigned i = 0; i < LogRecordBuffer::MaxRecords; ++i)
    require(buffer.add(rows[i], collect), "Hold records up to the record bound");
  require(written.empty() && buffer.size() == LogRecordBuffer::MaxRecords, "No write before the record bound is exceeded");
  require(buffer.add(rows.back(), collect), "Hold the record after a full buffer");
  require(written.size() == 1 && written[0] == std::vector<std::string>(rows.begin(), rows.end() - 1) && buffer.size() == 1,
          "A full record bound writes every held record in order first");
  buffer.flush(collect);
  require(written.size() == 2 && written[1] == std::vector<std::string>{rows.back()} && buffer.size() == 0, "Flush writes and clears");
  buffer.flush(collect);
  require(written.size() == 2, "An empty buffer writes nothing");

  written.clear();
  const std::string largest(MaxLogRecordBytes, 'b');
  constexpr auto largest_held = LogRecordBuffer::MaxBytes / MaxLogRecordBytes;
  for (unsigned i = 0; i < largest_held; ++i)
    require(buffer.add(largest, collect), "Hold records up to the exact byte bound");
  require(written.empty(), "The exact byte bound is held without a write");
  require(buffer.add("tail\n", collect) && written.size() == 1 && written[0].size() == largest_held && buffer.size() == 1,
          "A record past the byte bound writes the held records first");

  written.clear();
  require(!buffer.add({}, collect) && written.size() == 1 && written[0] == std::vector<std::string>{"tail\n"} && buffer.size() == 0,
          "An empty record is not held, after the held records are written");
  require(!buffer.add(std::string(MaxLogRecordBytes + 1, 'c'), collect) && written.size() == 1,
          "A record over 8192 bytes is not held, for its caller to write alone");

  require(buffer.add("lost\n", collect), "Hold a record for a failing write");
  buffer.flush([](const std::string_view*, std::size_t) { throw std::runtime_error("fixture write failure"); });
  require(buffer.size() == 0, "A failing write still clears the buffer");

  // End to end: the held records reach the file whole and in order.
  const auto path = directory.file(L"buffer.log");
  const auto append = [&](const std::string_view* records, std::size_t count) {
    require(append_rotating_log_records(path, records, count, BridgeLogBytes), "Write buffered rows");
  };
  std::string expected;
  for (unsigned i = 0; i < 70; ++i) {
    const auto row = numbered(i);
    expected += row;
    require(buffer.add(row, append), "Buffer rows for the file");
  }
  buffer.flush(append);
  require(read(path) == expected, "Buffered records reach the log whole and in order");
}

std::string writer_record(unsigned writer, unsigned sequence) {
  char line[32]{};
  std::snprintf(line, sizeof(line), "W%u-%04u|abcdefg\n", writer, sequence);
  return line;
}
// batch 0 appends one record per call; otherwise batch records per call. A
// dropped batch wrote nothing, so it is retried whole, without sleeping, so
// waiting writers contend for the lock between every two records.
int child_writer(const wchar_t* path, unsigned writer, unsigned count, unsigned limit, const wchar_t* event_name, unsigned batch) {
  Handle start(OpenEventW(SYNCHRONIZE, FALSE, event_name));
  require(start.value && start.value != INVALID_HANDLE_VALUE && WaitForSingleObject(start.value, 10000) == WAIT_OBJECT_0,
          "Wait for concurrent writer start");
  const auto deadline = GetTickCount64() + 10000;
  for (unsigned i = 0; i < count; i += batch ? batch : 1) {
    std::vector<std::string> records;
    for (unsigned j = i; j < std::min(count, i + std::max(batch, 1u)); ++j)
      records.push_back(writer_record(writer, j));
    const std::vector<std::string_view> views(records.begin(), records.end());
    while (batch ? !append_rotating_log_records(path, views.data(), views.size(), limit) : !append_rotating_log(path, views[0], limit)) {
      require(GetTickCount64() < deadline, "Concurrent nonblocking writer eventually makes progress");
      Sleep(batch ? 0 : 1);
    }
  }
  return 0;
}
struct Children {
  std::vector<HANDLE> processes;
  ~Children() {
    for (auto process : processes) {
      if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);  // Only a fixture child created by this process.
        WaitForSingleObject(process, 5000);
      }
      CloseHandle(process);
    }
  }
};
void concurrency(const Directory& directory, bool rotate, unsigned batch) {
  constexpr unsigned writers = 4;
  const unsigned count = rotate ? 40 : 64, limit = rotate ? 80 : 4096;
  const auto path = directory.file(rotate ? (batch ? L"concurrent-rotating-batch.log" : L"concurrent-rotating.log")
                                          : (batch ? L"concurrent-all-batch.log" : L"concurrent-all.log"));
  const auto event_name = L"Local\\TaxiCamLogFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(rotate) + L"-" +
                          std::to_wstring(batch);
  Handle event(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
  require(event.value != nullptr && GetLastError() != ERROR_ALREADY_EXISTS, "Create private writer-start event");
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  require(length && length < executable.size(), "Locate own fixture executable");
  executable.resize(length);
  Children children;
  for (unsigned writer = 0; writer < writers; ++writer) {
    auto command = L"\"" + executable + L"\" --writer \"" + path + L"\" " + std::to_wstring(writer) + L" " + std::to_wstring(count) + L" " +
                   std::to_wstring(limit) + L" \"" + event_name + L"\" " + std::to_wstring(batch);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != FALSE,
            "Start an isolated concurrent fixture writer");
    CloseHandle(process.hThread);
    children.processes.push_back(process.hProcess);
  }
  require(SetEvent(event.value) != FALSE, "Release concurrent writers together");
  for (auto process : children.processes) {
    require(WaitForSingleObject(process, 15000) == WAIT_OBJECT_0, "Concurrent writer finishes without blocking indefinitely");
    DWORD result = 1;
    require(GetExitCodeProcess(process, &result) && result == 0, "Every child completes all accepted records");
  }
  bounded(path, limit);
  std::set<std::string> expected, actual;
  for (unsigned writer = 0; writer < writers; ++writer)
    for (unsigned i = 0; i < count; ++i)
      expected.insert(writer_record(writer, i));
  const auto inspect = [&](const std::wstring& name) {
    const auto bytes = read(name);
    require(bytes.size() % 16 == 0, "Concurrent writes retain whole fixed-width records");
    for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
      const auto record = bytes.substr(offset, 16);
      require(expected.contains(record), "No interleaved, truncated or corrupt concurrent record");
      require(actual.insert(record).second, "No duplicate accepted record across active and archived logs");
    }
  };
  inspect(path);
  if (rotate) {
    inspect(path + L".1");
    require(actual.size() >= 6 && actual.size() <= 10, "Concurrent rotation retains bounded newest generations");
    require(append_rotating_log(path, "latest record\r\n", limit) && read(path).ends_with("latest record\r\n"),
            "Diagnostics continue after concurrent cap crossings");
    bounded(path, limit);
  } else {
    require(actual == expected && !exists(path + L".1"), "No accepted concurrent records are lost at the exact file limit");
    // One lock per batch: no other writer's record lands inside a batch.
    const auto bytes = read(path);
    for (std::size_t offset = 0; batch && offset < bytes.size(); offset += 16) {
      unsigned writer = 0, sequence = 0;
      require(std::sscanf(bytes.c_str() + offset, "W%u-%u|", &writer, &sequence) == 2, "Parse a concurrent fixture record");
      if (sequence % batch)
        continue;
      for (unsigned j = 1; j < batch && sequence + j < count; ++j)
        require(bytes.compare(offset + 16 * j, 16, writer_record(writer, sequence + j)) == 0,
                "A batch's records stay together under concurrent writers");
    }
  }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 8 && std::wcscmp(argv[1], L"--writer") == 0)
      return child_writer(
          argv[2], static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10)), static_cast<unsigned>(std::wcstoul(argv[4], nullptr, 10)),
          static_cast<unsigned>(std::wcstoul(argv[5], nullptr, 10)), argv[6], static_cast<unsigned>(std::wcstoul(argv[7], nullptr, 10)));
    require(argc == 1, "Only the internal writer mode accepts arguments");
    require(BridgeLogBytes == 8 * 1024 * 1024 && LauncherLogBytes == 4 * 1024 * 1024, "Production log byte limits");
    Directory directory;
    boundary_and_rotation(directory);
    legacy_tail(directory);
    invalid_and_failure(directory);
    batches(directory);
    record_buffer(directory);
    for (const unsigned batch : {0u, 4u}) {
      concurrency(directory, false, batch);
      concurrency(directory, true, batch);
    }
    std::printf(
        "PASS rotating logs: %u checks; bounds, current diagnostics, complete UTF-8 legacy tails, failure safety, batches and "
        "concurrent processes\n",
        checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL rotating logs: %s (Windows error %lu)\n", error.what(), GetLastError());
    return 1;
  }
}
