#pragma once

#include "view_pool.hpp"

#include <array>

namespace taxi_camera::native_camera {

enum class SourceViewStatus {
  not_inspected,
  ready,
  no_source,
  invalid_pool,
  invalid_pointer,
  pool_changed,
  read_failed,
  changed,
  read_limit
};

struct SourceViewSnapshot {
  bool complete = false;  // True only after a usable source and complete trace reread.
  SourceViewStatus status = SourceViewStatus::not_inspected;
  const char* error = "";
  // Semantic field name only; no address or pointed-to data is logged.
  const char* field = "";
  std::uint8_t rejected_alignment = 0;  // Low three bits only, never an address.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t candidates_examined = 0;
  std::int32_t view_index = -1;
  // Borrowed internal addresses, never log/serialize or retain across the
  // caller's proven update phase. No address is published on any failure.
  std::uint64_t source_address = 0;
  std::uint64_t node_address = 0;
  std::uint64_t camera_address = 0;
  float fov = 0;
};

// PRECREATION ONLY: caller guarantees its owned pair does not yet exist, and
// supplies a fresh pool from the same validated renderer/update phase. There is
// no owned-ID list here, so this helper cannot independently exclude an existing
// owned view. Choose the first occupied pool slot with a valid camera pose.
//
// Recheck current array slot; resolve P+104 handle (control+28 generation/+0
// payload) to Node. Require Node+256 Camera WORD+160=7, finite positive float
// Camera+1616, Node+296 matrix pointer with finite translation doubles+96..119,
// and finite 3x3 Camera doubles at +1648/+1680/+1712. Norm-squared and pairwise
// dot products must be within 1e-3 of an orthonormal basis. No pose conversion.
// Stable absent/unusable candidates are skipped; malformed/unreadable/changing
// fields refuse all output. Reread every observation, including skipped views.
// Maximum eight candidates, 8192 attempted bytes (current worst case 2464).
// No global reads, engine/COM calls, acquired references or scheduling claims.
// Reader owns readable-region/build/lifetime checks and must not throw.
// Control-record +28/+0 reads alone permit byte alignment, reproducing the
// captured MOV accesses. The exact pointer is used without clearing low bits;
// null/overflow, region, byte-budget and trace checks still apply. Other object
// pointers keep their eight-byte alignment guard. This admission by itself
// does not establish a valid live handle or a tagged-pointer interpretation.
SourceViewSnapshot inspect_source_view(engine_camera::MemoryReader& reader, const engine_camera::ViewPoolSnapshot& pool) noexcept;
// Same eight-slot budget as inspect_source_view. A null predicate accepts the
// first usable camera. A predicate may skip a usable camera, including the
// aircraft-object camera, without ending the scan or publishing its address.
// validated_position is cleared first and set only for the accepted camera.
using SourceViewPredicate = bool (*)(const std::array<double, 3>& translation, float fov, void* context);
SourceViewSnapshot select_source_view(engine_camera::MemoryReader& reader,
                                      const engine_camera::ViewPoolSnapshot& pool,
                                      SourceViewPredicate accept,
                                      void* context,
                                      std::array<double, 3>* validated_position = nullptr) noexcept;

// Same validation for one independently verified source object with its Node
// handle at +104. Does not read a pool or infer the source object's identity.
// Caller must establish the exact source lifetime/layout before access. A
// successful result describes the captured current view, not an aircraft body
// pose or taxi-camera geometry. view_index remains -1. Maximum 292 read bytes.
// Optional numeric output uses the already observed translation and adds no
// reads. It is cleared first and published only after the full trace recheck.
SourceViewSnapshot inspect_source_pose(engine_camera::MemoryReader& reader,
                                       std::uint64_t source_with_node_handle_at104,
                                       std::array<double, 3>* validated_position = nullptr) noexcept;

}  // namespace taxi_camera::native_camera
