#include "../../src/graphics/metadata_batch_cache.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace {
struct Record {
  std::atomic<bool> alive{true};
  std::uint64_t id{}, recording{};
};
using Cache = taxi_camera::standalone::MetadataBatchCache<Record, std::uintptr_t>;
unsigned checks{};
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Scope {
  Cache& cache;
  Scope(Cache& value, std::uintptr_t native, std::uint64_t id, Record* record) : cache(value) { cache.begin(native, id, record); }
  ~Scope() { cache.end(); }
};
}
int main() {
  try {
    Cache cache;
    auto first = std::make_shared<Record>();
    first->id = 7;
    first->recording = 10;
    auto second = std::make_shared<Record>();
    second->id = 8;
    second->recording = 11;
    require(!cache.current(100, 7), "Absent scope was reused");
    {
      Scope unobserved(cache, 100, 7, nullptr);
      require(!cache.current(100, 7), "Scope without a record served metadata");
    }
    {
      Scope outer(cache, 100, 7, first.get());
      require(cache.current(100, 7) == first.get(), "Live batch was not cached");
      require(!cache.current(101, 7) && !cache.current(100, 8), "Foreign native or generation reused batch");
      {
        Scope nested(cache, 200, 8, second.get());
        require(cache.current(200, 8) == second.get() && !cache.current(100, 7), "Nested scope did not replace context");
      }
      require(cache.current(100, 7) == first.get(), "Nested end lost outer context");
      {
        Scope nested(cache, 100, 7, first.get());
        require(cache.current(100, 7) == first.get(), "Same-list nested scope lost context");
      }
      require(cache.current(100, 7) == first.get(), "Same-list nested end lost outer context");
      ++first->recording;
      require(!cache.current(100, 7), "Reset reused pre-reset recording");
    }
    require(!cache.current(100, 7), "End leaked metadata scope");
    {
      Scope outer(cache, 100, 7, first.get());
      first->alive = false;
      require(!cache.current(100, 7), "Retired record was reused");
      first->alive = true;
      first->id = 9;
      require(!cache.current(100, 7) && !cache.current(100, 9), "Address/generation reuse revived stale scope");
    }
    first->id = 7;
    for (unsigned n = 0; n < 10; ++n)
      cache.begin(100, 7, first.get());
    require(!cache.current(100, 7), "Nesting overflow did not request ordinary lookup");
    cache.end();
    require(!cache.current(100, 7), "Partial overflow unwind exposed wrong scope");
    cache.end();
    require(cache.current(100, 7) == first.get(), "Overflow unwind lost bounded prior scope");
    for (unsigned n = 0; n < 8; ++n)
      cache.end();
    require(!cache.current(100, 7), "Nested scopes not fully cleared");
    std::weak_ptr<Record> weak;
    try {
      auto ephemeral = std::make_shared<Record>();
      ephemeral->id = 3;
      weak = ephemeral;
      // Declared after its owner, as the bridge's hook-scoped use nests, so the
      // scope ends before the record can go.
      Scope exception_scope(cache, 300, 3, ephemeral.get());
      require(cache.current(300, 3) == ephemeral.get() && weak.use_count() == 1, "Scope did not borrow metadata without owning it");
      throw 1;
    } catch (int) {
    }
    require(weak.expired() && !cache.current(300, 3), "Exception unwind retained scope or metadata");

    // Isolated registry-lookup microbenchmark, not simulator/frame-time evidence.
    std::recursive_mutex mutex;
    std::unordered_map<std::uintptr_t, std::shared_ptr<Record>> records{{100, first}};
    std::uint64_t calls = 0, checksum = 0;
    const auto lookup = [&] {
      const std::lock_guard lock(mutex);
      ++calls;
      return records.at(100);
    };
    constexpr unsigned Barriers = 10171, Batches = 100;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned batch = 0; batch < Batches; ++batch)
      for (unsigned n = 0; n < Barriers; ++n)
        for (unsigned operation = 0; operation < 3; ++operation)
          checksum += lookup()->id;
    const auto old_end = std::chrono::steady_clock::now();
    const auto old_calls = calls;
    calls = 0;
    for (unsigned batch = 0; batch < Batches; ++batch) {
      const auto owner = lookup();
      Scope scope(cache, 100, 7, owner.get());
      for (unsigned n = 0; n < Barriers; ++n)
        for (unsigned operation = 0; operation < 3; ++operation) {
          const auto* record = cache.current(100, 7);
          if (!record)
            throw std::runtime_error("Valid benchmark batch fell back");
          checksum += record->id;
        }
    }
    const auto new_end = std::chrono::steady_clock::now();
    require(old_calls == std::uint64_t{Barriers} * Batches * 3 && calls == Batches, "Lookup-count reduction differs from contract");
    require(checksum == std::uint64_t{Barriers} * Batches * 42, "Metadata workload changed");
    std::printf("{\"checks\":%u,\"barriersPerBatch\":%u,\"batches\":%u,\"oldLookups\":%llu,\"batchedLookups\":%llu,\"oldMs\":%.3f,\"batchedMs\":%.3f}\n",
                checks, Barriers, Batches, old_calls, calls,
                std::chrono::duration<double, std::milli>(old_end - start).count(),
                std::chrono::duration<double, std::milli>(new_end - old_end).count());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}