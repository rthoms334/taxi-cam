#include "source_view.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace taxi_camera::native_camera {
namespace {
constexpr std::uint32_t kReadLimit = 8192;
constexpr double kBasisTolerance = 1e-3;

bool pointer_range(std::uint64_t address, std::uint64_t offset, std::uint32_t size, bool require_aligned = true) noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  return address != 0 && (!require_aligned || address % 8 == 0) && offset <= maximum - address && size <= maximum - (address + offset);
}

std::uint32_t u32(const std::uint8_t* bytes) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index)
    value |= std::uint32_t(bytes[index]) << (index * 8);
  return value;
}

std::uint64_t u64(const std::uint8_t* bytes) noexcept {
  return std::uint64_t(u32(bytes)) | (std::uint64_t(u32(bytes + 4)) << 32);
}

bool fail(SourceViewSnapshot& result, SourceViewStatus status, const char* error) noexcept {
  result.status = status;
  result.error = error;
  return false;
}

struct Observation {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, 24> bytes{};
  const char* field = "";
};

class BoundedReader {
 public:
  BoundedReader(engine_camera::MemoryReader& reader, SourceViewSnapshot& result) noexcept : reader_(reader), result_(result) {}

  bool field(std::uint64_t address,
             std::uint64_t offset,
             std::uint32_t size,
             std::uint8_t* output,
             const char* name,
             bool require_aligned = true) noexcept {
    result_.field = name;
    if (!pointer_range(address, offset, size, require_aligned)) {
      result_.rejected_alignment = static_cast<std::uint8_t>(address & 7);
      const char* reason = address == 0                          ? "The source-view field has a null base pointer."
                           : require_aligned && address % 8 != 0 ? "The source-view base is not aligned to eight bytes."
                                                                 : "The source-view field exceeds the pointer address range.";
      return fail(result_, SourceViewStatus::invalid_pointer, reason);
    }
    if (count_ == observations_.size())
      return fail(result_, SourceViewStatus::read_limit, "The source-view observation count was exhausted.");
    if (!read(address + offset, size, output))
      return false;
    auto& item = observations_[count_++];
    item.address = address + offset;
    item.size = size;
    item.field = name;
    std::copy_n(output, size, item.bytes.begin());
    return true;
  }

  bool word(std::uint64_t address, std::uint64_t offset, std::uint64_t& output, const char* name, bool require_aligned = true) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    if (!field(address, offset, bytes.size(), bytes.data(), name, require_aligned))
      return false;
    output = u64(bytes.data());
    return true;
  }

  bool node(std::uint64_t view, std::uint64_t& output) noexcept {
    output = 0;
    std::array<std::uint8_t, 16> bytes{};
    if (!field(view, 104, bytes.size(), bytes.data(), "source.node_handle"))
      return false;
    const auto control = u64(bytes.data());
    const auto generation = u32(bytes.data() + 8);
    if (control == 0)
      return true;
    // The captured MOV-based handle resolver imposes no control-record
    // alignment. Read this exact address; never mask, round or remove low bits.
    // Only these control fields permit byte alignment. Payload objects retain
    // their separate alignment/range checks at the point they are followed.
    if (!field(control, 28, 4, bytes.data(), "node_handle.generation", false))
      return false;
    if (u32(bytes.data()) != generation)
      return true;
    return word(control, 0, output, "node_handle.payload", false);
  }

  bool recheck() noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
      const auto& item = observations_[index];
      result_.field = item.field;
      std::array<std::uint8_t, 24> bytes{};
      if (!read(item.address, item.size, bytes.data()))
        return false;
      if (!std::equal(bytes.begin(), bytes.begin() + item.size, item.bytes.begin()))
        return fail(result_, SourceViewStatus::changed, "A source-view field changed during the full consistency recheck.");
    }
    return true;
  }

 private:
  bool read(std::uint64_t address, std::uint32_t size, std::uint8_t* output) noexcept {
    if (size == 0 || size > 24 || size > kReadLimit - result_.read_bytes)
      return fail(result_, SourceViewStatus::read_limit, "The source-view attempted-read allowance was exhausted.");
    result_.read_bytes += size;
    if (reader_.read(address, output, size))
      return true;
    ++result_.read_failures;
    return fail(result_, SourceViewStatus::read_failed, "A required source-view field could not be read exactly.");
  }

  engine_camera::MemoryReader& reader_;
  SourceViewSnapshot& result_;
  std::array<Observation, 96> observations_{};
  std::size_t count_ = 0;
};

bool valid_pool(const engine_camera::ViewPoolSnapshot& pool) noexcept {
  using engine_camera::ViewAssociation;
  if (!pool.valid || pool.status != engine_camera::ViewPoolStatus::complete || pool.slots_examined != 8 || pool.read_failures != 0 ||
      !pointer_range(pool.array_address, 0, 64))
    return false;
  for (std::uint32_t index = 0; index < pool.slots.size(); ++index) {
    const auto& slot = pool.slots[index];
    if (slot.index != index || !pointer_range(slot.view_address, 0, 8) || slot.association == ViewAssociation::unobserved ||
        slot.association_valid != (slot.association == ViewAssociation::occupied) || slot.free == slot.association_valid)
      return false;
    for (std::uint32_t previous = 0; previous < index; ++previous) {
      if (slot.view_address == pool.slots[previous].view_address)
        return false;
    }
  }
  return true;
}

bool finite_vector(const std::array<std::uint8_t, 24>& bytes, std::array<double, 3>& values) noexcept {
  bool finite = true;
  for (unsigned index = 0; index < 3; ++index) {
    values[index] = std::bit_cast<double>(u64(bytes.data() + index * 8));
    finite &= std::isfinite(values[index]);
  }
  return finite;
}

bool orthonormal(const std::array<std::array<double, 3>, 3>& basis) noexcept {
  for (unsigned row = 0; row < 3; ++row) {
    for (unsigned other = row; other < 3; ++other) {
      double dot = 0;
      for (unsigned column = 0; column < 3; ++column)
        dot += basis[row][column] * basis[other][column];
      if (!std::isfinite(dot) || std::abs(dot - (row == other ? 1.0 : 0.0)) > kBasisTolerance)
        return false;
    }
  }
  return true;
}

struct Candidate {
  bool usable = false;
  std::uint64_t node = 0;
  std::uint64_t camera = 0;
  float fov = 0;
  std::array<double, 3> translation{};
};

bool inspect_candidate(BoundedReader& source, std::uint64_t address, Candidate& candidate) noexcept {
  if (!source.node(address, candidate.node))
    return false;
  if (candidate.node == 0)
    return true;
  if (!source.word(candidate.node, 256, candidate.camera, "node.camera_pointer"))
    return false;
  if (candidate.camera == 0)
    return true;
  std::array<std::uint8_t, 24> bytes{};
  if (!source.field(candidate.camera, 160, 2, bytes.data(), "camera.type"))
    return false;
  if ((std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8)) != 7)
    return true;
  std::uint64_t matrix = 0;
  if (!source.word(candidate.node, 296, matrix, "node.matrix_pointer"))
    return false;
  if (matrix == 0)
    return true;
  if (!source.field(matrix, 96, bytes.size(), bytes.data(), "matrix.translation"))
    return false;
  if (!finite_vector(bytes, candidate.translation))
    return true;
  if (!source.field(candidate.camera, 1616, 4, bytes.data(), "camera.fov"))
    return false;
  candidate.fov = std::bit_cast<float>(u32(bytes.data()));
  if (!std::isfinite(candidate.fov) || candidate.fov <= 0)
    return true;
  std::array<std::array<double, 3>, 3> basis{};
  bool finite = true;
  for (unsigned row = 0; row < 3; ++row) {
    constexpr const char* fields[]{"camera.basis_x", "camera.basis_y", "camera.basis_z"};
    if (!source.field(candidate.camera, 1648 + row * 32, bytes.size(), bytes.data(), fields[row]))
      return false;
    finite &= finite_vector(bytes, basis[row]);
  }
  candidate.usable = finite && orthonormal(basis);
  return true;
}

void publish(SourceViewSnapshot& result, std::uint64_t address, const Candidate& candidate) noexcept {
  result.complete = true;
  result.status = SourceViewStatus::ready;
  result.field = "";
  result.source_address = address;
  result.node_address = candidate.node;
  result.camera_address = candidate.camera;
  result.fov = candidate.fov;
}

}  // namespace

SourceViewSnapshot select_source_view(engine_camera::MemoryReader& reader,
                                      const engine_camera::ViewPoolSnapshot& pool,
                                      SourceViewPredicate accept,
                                      void* context,
                                      std::array<double, 3>* validated_position) noexcept {
  if (validated_position)
    *validated_position = {};
  SourceViewSnapshot result;
  if (!valid_pool(pool)) {
    fail(result, SourceViewStatus::invalid_pool, "Source selection requires a complete pool of eight distinct classified views.");
    return result;
  }
  BoundedReader source(reader, result);
  bool saw_usable = false;
  for (const auto& slot : pool.slots) {
    if (!slot.association_valid)
      continue;
    ++result.candidates_examined;
    std::uint64_t view = 0;
    if (!source.word(pool.array_address, slot.index * 8, view, "pool.view_pointer"))
      return result;
    if (view != slot.view_address) {
      fail(result, SourceViewStatus::pool_changed, "An occupied pool pointer no longer matches the supplied snapshot.");
      return result;
    }
    Candidate candidate;
    if (!inspect_candidate(source, view, candidate))
      return result;
    if (!candidate.usable)
      continue;
    saw_usable = true;
    if (accept && !accept(candidate.translation, candidate.fov, context))
      continue;
    if (!source.recheck())
      return result;
    publish(result, view, candidate);
    result.view_index = static_cast<std::int32_t>(slot.index);
    if (validated_position)
      *validated_position = candidate.translation;
    return result;
  }
  if (source.recheck())
    fail(result, SourceViewStatus::no_source,
         saw_usable ? "No occupied view camera matches the current public WORLD sample."
                    : "No occupied view has the required generation-valid Camera and finite orthonormal pose.");
  return result;
}
SourceViewSnapshot inspect_source_view(engine_camera::MemoryReader& reader, const engine_camera::ViewPoolSnapshot& pool) noexcept {
  return select_source_view(reader, pool, nullptr, nullptr, nullptr);
}

SourceViewSnapshot inspect_source_pose(engine_camera::MemoryReader& reader,
                                       std::uint64_t source_with_node_handle_at104,
                                       std::array<double, 3>* validated_position) noexcept {
  if (validated_position)
    *validated_position = {};
  SourceViewSnapshot result;
  BoundedReader source(reader, result);
  Candidate candidate;
  result.candidates_examined = 1;
  if (!inspect_candidate(source, source_with_node_handle_at104, candidate) || !source.recheck())
    return result;
  if (candidate.usable) {
    publish(result, source_with_node_handle_at104, candidate);
    if (validated_position)
      *validated_position = candidate.translation;
  } else
    fail(result, SourceViewStatus::no_source,
         "The explicit source lacks the required generation-valid Camera and finite orthonormal pose.");
  return result;
}

}  // namespace taxi_camera::native_camera
