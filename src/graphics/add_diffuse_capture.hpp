#pragma once

#include <d3d12.h>

#include <cstdint>

namespace taxi_camera::add_diffuse {

// Proven native RT exit only. Restores RENDER_TARGET before returning.
// Does not publish a taxi-camera feed and does not copy the primary image
// into an owned view.
void note_legacy_exit(ID3D12GraphicsCommandList* list, std::uint64_t generation, ID3D12Resource* resource) noexcept;
void note_enhanced_exit(ID3D12GraphicsCommandList7* list, std::uint64_t generation, ID3D12Resource* resource) noexcept;

// Pointers only. No dereference, AddRef or GPU work. Used for legacy batches
// larger than the generic 256-barrier scan.
UINT diagnostic_targets(ID3D12Resource** targets, UINT capacity) noexcept;

// True when this batch contains a recorded diagnostic copy and after() must run.
bool arm_submission(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept;
void after_submission(ID3D12CommandQueue* queue) noexcept;
void submission_refused(ID3D12CommandQueue* queue) noexcept;
void note_list_reset(ID3D12GraphicsCommandList* list) noexcept;
void poll() noexcept;

}  // namespace taxi_camera::add_diffuse
