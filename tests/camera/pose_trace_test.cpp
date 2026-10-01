#include "../../src/camera/pose_trace.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using taxi_camera::native_camera::PoseTrace;
using taxi_camera::native_camera::PoseTraceEntry;
unsigned checks = 0;
bool require(bool value, const char* message) {
  ++checks;
  if (!value)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
PoseTraceEntry entry(std::uint64_t update) {
  PoseTraceEntry e;
  e.update = update;
  e.read = {static_cast<double>(update), 0, 0};
  return e;
}
}  // namespace

int main() {
  bool ok = true;
  static PoseTrace trace;
  std::vector<std::pair<unsigned, std::uint64_t>> written;  // window, first update
  const auto sink = [&](unsigned window, const PoseTraceEntry* entries, std::size_t count) {
    ok &= require(count == PoseTrace::kFrames, "A window was handed over before it was full");
    written.emplace_back(window, entries[0].update);
  };
  // Parked: nothing is wanted or recorded.
  ok &= require(!trace.wants(0.5), "A parked aircraft was traced");
  trace.record(entry(1), 0.5);
  ok &= require(!trace.take(sink), "A parked update produced a window");
  // Taxi speed opens window 0 and keeps it open regardless of speed until full.
  ok &= require(trace.wants(3), "Taxi speed did not start the taxi window");
  std::uint64_t update = 10;
  for (std::size_t i = 0; i < PoseTrace::kFrames - 1; ++i) {
    trace.record(entry(update++), i ? 0.1 : 3);
    ok &= require(trace.wants(0.1), "An open window stopped wanting updates");
  }
  ok &= require(!trace.take(sink), "An unfinished window was handed over");
  trace.record(entry(update++), 3);
  ok &= require(trace.take(sink) && written.size() == 1 && written[0] == std::pair<unsigned, std::uint64_t>{0, 10},
                "The full taxi window was not handed over once");
  ok &= require(!trace.take(sink), "A taken window was handed over twice");
  // The taxi window does not reopen; take-off speed opens window 1.
  ok &= require(!trace.wants(10) && trace.wants(31), "Window 0 reopened or window 1 did not start at take-off speed");
  const auto takeoff = update;
  for (std::size_t i = 0; i < PoseTrace::kFrames; ++i)
    trace.record(entry(update++), 60);
  ok &= require(trace.take(sink) && written.size() == 2 && written[1] == std::pair<unsigned, std::uint64_t>{1, takeoff},
                "The take-off window was not handed over");
  // Both windows done: nothing more is wanted or recorded.
  ok &= require(!trace.wants(100), "A finished trace wanted more updates");
  trace.record(entry(update++), 100);
  ok &= require(!trace.take(sink), "A finished trace recorded again");
  // Candidate scan: position-like double triples near the origin, in chunks.
  struct Memory final : taxi_camera::engine_camera::MemoryReader {
    std::uint64_t base = 0x40000;
    std::array<double, 600> words{};  // 4800 bytes
    bool read(std::uint64_t address, void* destination, std::size_t size) override {
      if (address < base || address - base + size > sizeof(words))
        return false;
      std::memcpy(destination, reinterpret_cast<const unsigned char*>(words.data()) + (address - base), size);
      return true;
    }
  } memory;
  const taxi_camera::native_camera::Vector3 origin{4079673.7, 1425314.4, 4675524.2};
  memory.words[4] = origin[0] + 0.5;  // A copy 0.5/1/-2 m away at offset 32.
  memory.words[5] = origin[1] + 1;
  memory.words[6] = origin[2] - 2;
  memory.words[31] = origin[0];  // Straddles the 256-byte chunk end (offset 248).
  memory.words[32] = origin[1];
  memory.words[33] = origin[2];
  memory.words[100] = origin[0] + 500;  // Too far on one axis.
  memory.words[101] = origin[1];
  memory.words[102] = origin[2];
  PoseTraceEntry scanned;
  taxi_camera::native_camera::scan_position_candidates(memory, memory.base, 4096, 2, origin, scanned);
  ok &= require(scanned.candidate_count == 2 && scanned.candidates[0].object == 2 && scanned.candidates[0].offset == 32 &&
                    scanned.candidates[0].value[2] == origin[2] - 2 && scanned.candidates[1].offset == 248,
                "The candidate scan missed a copy, the chunk-straddling copy, or accepted a distant triple");
  // An object running past readable memory only loses its unreadable chunks.
  PoseTraceEntry partial;
  taxi_camera::native_camera::scan_position_candidates(memory, memory.base, 8192, 1, origin, partial);
  ok &= require(partial.candidate_count == 2, "An unreadable tail chunk lost the readable candidates");
  PoseTraceEntry none;
  taxi_camera::native_camera::scan_position_candidates(memory, 0, 4096, 1, origin, none);
  taxi_camera::native_camera::scan_position_candidates(memory, memory.base + 4, 4096, 1, origin, none);
  ok &= require(none.candidate_count == 0, "A null or misaligned object was scanned");
  if (!ok)
    return 1;
  std::printf("PASS pose trace: %u checks; taxi and take-off windows, full-only handover, taken once, candidate scan.\n", checks);
  return 0;
}
