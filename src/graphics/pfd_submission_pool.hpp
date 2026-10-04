#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace taxi_camera::pfd_submission {

// The manager's submission mutex serializes all methods. Only service performs GPU allocation or
// resets. A recording owns its source/target references until cancellation is
// serviced or its covering outer-receipt fence completes. Uncertain work is
// retained permanently, never recycled based on a later unrelated Signal.
class Pool {
 public:
  static constexpr unsigned Capacity = 8;
  struct Copy {
    ID3D12Resource* target = nullptr;
    ID3D12Resource* source = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    // COMMON is accepted only with the caller's explicit final-transition
    // proof. An omitted state must never silently invent that proof.
    D3D12_RESOURCE_STATES state = static_cast<D3D12_RESOURCE_STATES>(~UINT{0});
    UINT x = 0, y = 0, width = 0, height = 0;
    // Readback: copy the target's base-mip region into the source buffer, which
    // must be in a READBACK heap, instead of writing the buffer into the target.
    // When the covering fence passes, completed is raised to completed_value
    // (never lowered, so an older request finishing later cannot mark a newer
    // one done). A cancelled or quarantined packet never raises it. The
    // counter must outlive the packet.
    bool readback = false;
    std::atomic<std::uint64_t>* completed = nullptr;
    std::uint64_t completed_value = 0;
  };
  struct Recording {
    unsigned slot = Capacity;
    ID3D12CommandList* list = nullptr;
    explicit operator bool() const noexcept { return list != nullptr; }
  };

  Pool() noexcept = default;
  ~Pool();
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  // Service thread only, WITHOUT the manager metadata lock: releasing the last
  // native resource reference may reenter its retirement callback. completed must come from the
  // SAME device timeline used by submit; UINT64_MAX means device removal.
  // Native device identity is immutable after the first successful service.
  bool service(ID3D12Device* device, std::uint64_t completed) noexcept;
  // Submission callback: no allocation, Reset, CPU wait or mutex acquisition.
  // The caller proves whole-batch boundary/state/generation and serializes the
  // stable patch against its writer on the manager's outer receipt timeline.
  // Exact RT is permitted only for insertion BEFORE a list whose initial target
  // state is proven RT, restoring it before that application's first transition.
  Recording record(const Copy&) noexcept;
  void submit(unsigned slot, std::uint64_t covering_value) noexcept;
  void cancel(unsigned slot) noexcept;
  void quarantine(unsigned slot) noexcept;
  unsigned ready_count() const noexcept;

 private:
  enum class State { empty, ready, recorded, submitted, retired, quarantined };
  struct Packet {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* source = nullptr;
    std::atomic<std::uint64_t>* completed = nullptr;
    std::uint64_t completed_value = 0;
    std::uint64_t covering_value = 0;
    State state = State::empty;
  };
  static void release_resources(Packet&) noexcept;
  bool valid_copy(const Copy&) const noexcept;
  ID3D12Device* device_ = nullptr;
  std::array<Packet, Capacity> packets_{};
};

}  // namespace taxi_camera::pfd_submission
