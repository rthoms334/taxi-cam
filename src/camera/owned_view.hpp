#pragma once

#include "view_pool.hpp"

namespace taxi_camera::engine_camera {

enum class OwnedViewStatus {
  not_inspected,
  pending,
  ready,
  invalid_request,
  invalid_pool,
  invalid_pointer,
  id_mismatch,
  invalid_ready_byte,
  invalid_view_index,
  pool_changed,
  node_unavailable,
  node_mismatch,
  wrong_camera_type,
  invalid_fov,
  material_mismatch,
  read_failed,
  changed,
  read_budget_exhausted
};

inline const char* owned_view_status_name(OwnedViewStatus status) noexcept {
  switch (status) {
    case OwnedViewStatus::not_inspected:
      return "not_inspected";
    case OwnedViewStatus::pending:
      return "pending";
    case OwnedViewStatus::ready:
      return "ready";
    case OwnedViewStatus::invalid_request:
      return "invalid_request";
    case OwnedViewStatus::invalid_pool:
      return "invalid_pool";
    case OwnedViewStatus::invalid_pointer:
      return "invalid_pointer";
    case OwnedViewStatus::id_mismatch:
      return "id_mismatch";
    case OwnedViewStatus::invalid_ready_byte:
      return "invalid_ready_byte";
    case OwnedViewStatus::invalid_view_index:
      return "invalid_view_index";
    case OwnedViewStatus::pool_changed:
      return "pool_changed";
    case OwnedViewStatus::node_unavailable:
      return "node_unavailable";
    case OwnedViewStatus::node_mismatch:
      return "node_mismatch";
    case OwnedViewStatus::wrong_camera_type:
      return "wrong_camera_type";
    case OwnedViewStatus::invalid_fov:
      return "invalid_fov";
    case OwnedViewStatus::material_mismatch:
      return "material_mismatch";
    case OwnedViewStatus::read_failed:
      return "read_failed";
    case OwnedViewStatus::changed:
      return "changed";
    case OwnedViewStatus::read_budget_exhausted:
      return "read_budget_exhausted";
  }
  return "unknown";
}

struct OwnedViewSnapshot {
  // complete includes a stable pending entry; ready requires the camera chain.
  bool complete = false;
  bool ready = false;
  OwnedViewStatus status = OwnedViewStatus::not_inspected;
  const char* error = "";
  // Attempted bytes including failed reads; cap 8192, current full trace 518.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::int32_t view_index = -1;
  // Exact ready-entry E24 mode, reread with the full trace. Raw observation;
  // callers that require independent-pose semantics must require mode == 2.
  std::uint32_t mode = 0;
  // Internal, borrowed addresses, published only after the entire trace reread.
  // Never log/serialize or retain them beyond the caller's proven engine phase.
  std::uint64_t view_address = 0;
  std::uint64_t node_address = 0;
  std::uint64_t camera_address = 0;
  // Opaque member for lifecycle-bracketed resource identity matching only.
  // Never dereference/AddRef or send to UI/logs; no resource lifetime is acquired.
  std::uint64_t resource_address = 0;
  float fov = 0;
  // Numeric diagnostics only, published with the complete ready snapshot.
  // P+16/+20, P+24/+28, P+32/+36, respectively. The captured setup copies
  // primary-view dimensions into these pairs; the third is used for output
  // allocation. These observations are not a resource description or setters.
  std::array<std::array<std::int32_t, 2>, 3> dimensions{};
  // Slot0 Bitmap+40/+44 metadata, read only when a nonnull resource member is
  // present, and published only after the complete trace reread. Zero when
  // output is unavailable. Signed values are observations, not size validation
  // or a native GPU resource description; callers must check desired bounds.
  std::array<std::int32_t, 2> output_dimensions{};
  // Exact P+48/P+56 flag words. Preserve as integers/hexadecimal strings in
  // consumers; no guessed meanings, masking, writes or floating conversion.
  std::array<std::uint64_t, 2> flags{};
  // Only a nonnull pointer-shaped resource member was observed. No COM method,
  // interface check, AddRef, description, resource state or GPU data is inspected.
  bool resource_present = false;
};

// Separate authority for an owned mode2 gate closure only. Never convertible to
// OwnedViewSnapshot: no Camera, pose or output-resource proof is supplied.
// Every retained field, including both Node handles, is exactly reread before
// complete becomes true. Pending/unavailable/mismatching results publish no
// borrowed address. Current full close trace:16 fields,32 reads,258 bytes.
struct OwnedViewCloseSnapshot {
  bool complete = false;
  OwnedViewStatus status = OwnedViewStatus::not_inspected;
  const char* error = "";
  std::uint32_t read_bytes = 0, read_failures = 0;
  std::int32_t view_index = -1;
  // Borrowed only in this observer phase, for verifying the close's flag write.
  std::uint64_t view_address = 0;
  std::array<std::array<std::int32_t, 2>, 3> dimensions{};
  std::array<std::uint64_t, 2> flags{};
};

// Caller retains the full current manager/table/pool/lifetime guards. This only
// admits E8==1/E24==2, matching E0/E16, a current pool index and equal nonnull
// generation-resolved E96/P104 Node identities. It does not dereference Node,
// Camera, material or Bitmap. No native call, publication or ownership acquired.
OwnedViewCloseSnapshot inspect_owned_view_for_close(MemoryReader& reader,
                                                    std::uint64_t entry_address,
                                                    std::uint64_t expected_id,
                                                    const ViewPoolSnapshot& pool) noexcept;
// Fixed captured layout, not a general engine ABI. The caller must supply a
// freshly verified owned entry and complete pool from the same renderer/update
// phase. Check E+0/+16 IDs, E+8 ready, E+76 pool index and its current array slot.
// Resolve generation-checked E+96/P+104 to one Node; Node+256 must point to a
// Camera with WORD+160=7 and finite positive float+1616.
//
// Optional output observation resolves E+80/P+144 to matching material payloads,
// then Material+520 bitmap handle -> Bitmap+88 record -> record+16 wrapper ->
// wrapper+168 resource member. Null/stale output references report no resource;
// malformed/unreadable/mismatched or changing fields refuse the entire result.
// No output is published on failure, including failures in this optional chain.
// Every observed byte is reread, but this does not make a snapshot atomic or
// acquire references. No engine calls, global reads or payload virtual calls.
// Reader owns accessible-region/lifetime/build checks and must not throw.
// For Node, material and bitmap handles, control-record +28/+0 reads alone
// permit byte alignment. No low bits are masked or rounded. Entry, view,
// payload, record, wrapper and resource pointers retain eight-byte alignment;
// null/generation/range/region/budget/full-trace checks remain required.
// The three dimension pairs and two flag words above are included in that
// trace. Zero, negative or unusual dimensions remain diagnostic observations;
// this reader does not infer validity limits or silently resize a view.
OwnedViewSnapshot inspect_owned_view(MemoryReader& reader,
                                     std::uint64_t entry_address,
                                     std::uint64_t expected_id,
                                     const ViewPoolSnapshot& pool) noexcept;

}  // namespace taxi_camera::engine_camera
