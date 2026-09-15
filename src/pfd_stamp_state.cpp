#include "pfd_stamp_state.hpp"

#include <algorithm>
#include <cmath>

namespace taxi_camera {
namespace {
std::uint64_t mask(UINT count) noexcept {
  return count == 64 ? UINT64_MAX : ((std::uint64_t{1} << count) - 1);
}
}  // namespace

void PfdGraphicsState::reset(std::uint64_t generation, bool native_observations) noexcept {
  *this = PfdGraphicsState{};
  generation_ = generation;
  observed_ = native_observations;
}
void PfdGraphicsState::depth_bias(d3d12_extended::CommandList9* native, float bias, float clamp, float slope) noexcept {
  if (!native || (dynamic_native_ && dynamic_native_ != native) || !std::isfinite(bias) || !std::isfinite(clamp) || !std::isfinite(slope)) {
    invalidate("dynamic_depth_bias_invalid");
    return;
  }
  dynamic_native_ = native;
  depth_bias_ = {bias, clamp, slope};
  depth_bias_known_ = true;
}
void PfdGraphicsState::strip_cut(d3d12_extended::CommandList9* native, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) noexcept {
  if (!native || (dynamic_native_ && dynamic_native_ != native) || value < D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED ||
      value > D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF) {
    invalidate("dynamic_strip_cut_invalid");
    return;
  }
  dynamic_native_ = native;
  strip_cut_ = value;
  strip_cut_known_ = true;
}
void PfdGraphicsState::sample_positions(ID3D12GraphicsCommandList1* native,
                                        UINT samples,
                                        UINT pixels,
                                        const D3D12_SAMPLE_POSITION* positions) noexcept {
  if (!native || (sample_native_ && sample_native_ != native)) {
    invalidate("sample_position_interface_mismatch");
    return;
  }
  if (!samples && !pixels && !positions) {
    sample_native_ = nullptr;
    sample_count_ = sample_pixels_ = 0;
    sample_positions_ = {};
    return;
  }
  if ((samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) || (pixels != 1 && pixels != 4) ||
      samples > 16 / pixels || !positions) {
    invalidate("sample_position_arguments_invalid");
    return;
  }
  for (UINT n = 0; n < samples * pixels; ++n)
    if (positions[n].X < -8 || positions[n].X > 7 || positions[n].Y < -8 || positions[n].Y > 7) {
      invalidate("sample_position_coordinates_invalid");
      return;
    }
  sample_native_ = native;
  sample_count_ = samples;
  sample_pixels_ = pixels;
  std::copy_n(positions, samples * pixels, sample_positions_.begin());
}
void PfdGraphicsState::restore_sample_positions(ID3D12GraphicsCommandList* native) const noexcept {
  if (!has_sample_positions() || sample_native_ != native)
    return;
  auto positions = sample_positions_;
  sample_native_->SetSamplePositions(sample_count_, sample_pixels_, positions.data());
}
void PfdGraphicsState::bind_root(ID3D12RootSignature* root,
                                 std::uint64_t generation,
                                 const PfdRootLayout& layout,
                                 bool exact_native_change) noexcept {
  if (root_ == root && layout_generation_ == generation && layout_.valid && layout.valid)
    return;
  observed_arguments_ = false;
  root_ = root;
  layout_generation_ = generation;
  layout_ = layout;
  values_ = {};
  native_root_observed_ = exact_native_change && observed_;
  undefined_tables_ = {};
  constants_ = {};
  constant_offsets_ = {};
  if (!layout_.valid || layout_.count > 64) {
    layout_.valid = false;
    return;
  }
  UINT offset = 0, cost = 0;
  for (UINT n = 0; n < layout_.count; ++n) {
    const auto& param = layout_.parameters[n];
    if (param.kind == PfdRootKind::table)
      undefined_tables_[n] = native_root_observed_;
    if (param.kind == PfdRootKind::constants) {
      if (!param.count || param.count > 64 - offset) {
        layout_.valid = false;
        return;
      }
      constant_offsets_[n] = static_cast<std::uint8_t>(offset);
      offset += param.count;
      cost += param.count;
    } else {
      if (param.count != 1 || param.kind > PfdRootKind::uav) {
        layout_.valid = false;
        return;
      }
      cost += param.kind == PfdRootKind::table ? 1 : 2;
    }
    if (cost > 64) {
      layout_.valid = false;
      return;
    }
  }
}
void PfdGraphicsState::bind_observed_root(ID3D12RootSignature* root, std::uint64_t generation) noexcept {
  if (observed_arguments_ && root_ == root && layout_generation_ == generation)
    return;
  bind_root(root, generation, {}, true);
  observed_arguments_ = true;
  observed_word_count_ = 0;
  observed_word_keys_ = {};
  // A list first encountered mid-recording is never admitted here. An observed
  // Reset or creation establishes the initial undefined root argument state.
  layout_.valid = observed_ && root && generation;
}
bool PfdGraphicsState::parameter(UINT index, PfdRootKind kind) noexcept {
  if (observed_arguments_ && layout_.valid && index < 64) {
    auto& param = layout_.parameters[index];
    if (!param.count) {
      param = {kind, 1};
      layout_.count = std::max(layout_.count, index + 1);
    }
  }
  if (!layout_.valid || index >= layout_.count || layout_.parameters[index].kind != kind) {
    invalidate("root_parameter_kind_or_index");
    return false;
  }
  return true;
}
void PfdGraphicsState::constants(UINT index, UINT first, UINT count, const void* data) noexcept {
  if (!parameter(index, PfdRootKind::constants))
    return;
  if (observed_arguments_) {
    if (!data || !count || first >= 64 || count > 64 - first) {
      invalidate("observed_root_constant_bounds");
      return;
    }
    const auto* words = static_cast<const std::uint32_t*>(data);
    for (UINT n = 0; n < count; ++n) {
      const auto key = static_cast<std::uint16_t>(index * 64 + first + n);
      UINT slot = 0;
      while (slot < observed_word_count_ && observed_word_keys_[slot] != key)
        ++slot;
      if (slot == 64) {
        invalidate("observed_root_constant_capacity");
        return;
      }
      if (slot == observed_word_count_) {
        observed_word_keys_[slot] = key;
        ++observed_word_count_;
      }
      std::memcpy(&constants_[slot], words + n, sizeof(std::uint32_t));
    }
    return;
  }
  if (!data || !count || first > layout_.parameters[index].count || count > layout_.parameters[index].count - first) {
    invalidate("root_constant_bounds");
    return;
  }
  std::memcpy(constants_.data() + constant_offsets_[index] + first, data, count * 4);
  values_[index].known |= mask(count) << first;
}
void PfdGraphicsState::table(UINT index, UINT64 address) noexcept {
  if (parameter(index, PfdRootKind::table)) {
    values_[index].address = address;
    values_[index].known = 1;
    undefined_tables_[index] = false;
  }
}
void PfdGraphicsState::descriptor(UINT index, PfdRootKind kind, UINT64 address) noexcept {
  if (kind == PfdRootKind::table || kind == PfdRootKind::constants) {
    invalidate("root_descriptor_kind");
    return;
  }
  if (parameter(index, kind)) {
    values_[index].address = address;
    values_[index].known = 1;
  }
}
void PfdGraphicsState::descriptor_heaps(UINT count, ID3D12DescriptorHeap* const* heaps) noexcept {
  if (count > heaps_.size() || (count && !heaps) || (count && !heaps[0]) || (count == 2 && (!heaps[1] || heaps[0] == heaps[1]))) {
    heaps_known_ = false;
    descriptor_heaps_changed();
    invalidate("descriptor_heap_arguments_invalid");
    return;
  }
  // Microsoft: redundant same-heap binding does not make table settings
  // undefined. Compare the complete set without dereferencing heap objects;
  // the order of the two distinct shader-visible heap types is immaterial.
  // https://learn.microsoft.com/windows/win32/direct3d12/setting-descriptor-heaps
  const bool same =
      heaps_known_ && heap_count_ == count &&
      (!count || (count == 1 ? heaps_[0] == heaps[0]
                             : ((heaps_[0] == heaps[0] && heaps_[1] == heaps[1]) || (heaps_[0] == heaps[1] && heaps_[1] == heaps[0]))));
  if (!same) {
    descriptor_heaps_changed();
    if (native_root_observed_)
      for (UINT n = 0; n < layout_.count; ++n)
        if (layout_.parameters[n].kind == PfdRootKind::table)
          undefined_tables_[n] = true;
  }
  heaps_known_ = true;
  heap_count_ = count;
  heaps_ = {};
  for (UINT n = 0; n < count; ++n)
    heaps_[n] = heaps[n];
}
void PfdGraphicsState::descriptor_heaps_changed() noexcept {
  for (UINT n = 0; n < layout_.count; ++n)
    if (layout_.parameters[n].kind == PfdRootKind::table) {
      values_[n].known = 0;
      undefined_tables_[n] = false;
    }
}
void PfdGraphicsState::viewports(UINT first, UINT count, const D3D12_VIEWPORT* data) noexcept {
  // D3D12 replaces the entire array, including when it shrinks. Nonzero first
  // would be another API's semantics and cannot establish a native snapshot.
  if (first || !count || count > 16 || !data) {
    invalidate("viewport_array_bounds");
    return;
  }
  for (UINT n = 0; n < count; ++n) {
    const auto& v = data[n];
    if (!std::isfinite(v.TopLeftX) || !std::isfinite(v.TopLeftY) || !std::isfinite(v.Width) || !std::isfinite(v.Height) ||
        !std::isfinite(v.MinDepth) || !std::isfinite(v.MaxDepth)) {
      invalidate("viewport_nonfinite");
      return;
    }
  }
  viewport_count_ = count;
  std::copy_n(data, count, viewports_.begin());
}
void PfdGraphicsState::scissors(UINT first, UINT count, const D3D12_RECT* data) noexcept {
  if (first || !count || count > 16 || !data) {
    invalidate("scissor_array_bounds");
    return;
  }
  scissor_count_ = count;
  std::copy_n(data, count, scissors_.begin());
}
bool PfdGraphicsState::complete() const noexcept {
  return incomplete_reason() == nullptr;
}
const char* PfdGraphicsState::incomplete_reason() const noexcept {
  if (invalid_reason_)
    return invalid_reason_;
  if (!observed_)
    return "native_observations_missing";
  if (!generation_)
    return "recording_generation_missing";
  if (!pipeline_)
    return "pipeline_missing";
  if (!root_ || !layout_generation_ || !layout_.valid)
    return "root_layout_missing";
  if (topology_ == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
    return "topology_missing";
  if (!viewport_count_)
    return "viewport_missing";
  if (!scissor_count_)
    return "scissor_missing";
  if (observed_arguments_)
    return nullptr;
  for (UINT n = 0; n < layout_.count; ++n)
    if (values_[n].known != mask(layout_.parameters[n].count)) {
      if (layout_.parameters[n].kind == PfdRootKind::table && undefined_tables_[n])
        continue;
      switch (layout_.parameters[n].kind) {
        case PfdRootKind::constants:
          return "root_constants_incomplete";
        case PfdRootKind::table:
          return "root_table_missing_or_invalidated";
        case PfdRootKind::cbv:
          return "root_cbv_missing";
        case PfdRootKind::srv:
          return "root_srv_missing";
        case PfdRootKind::uav:
          return "root_uav_missing";
      }
    }
  return nullptr;
}
UINT PfdGraphicsState::undefined_table_count() const noexcept {
  UINT count = 0;
  for (UINT n = 0; n < layout_.count; ++n)
    if (layout_.parameters[n].kind == PfdRootKind::table && !values_[n].known && undefined_tables_[n])
      ++count;
  return count;
}
void PfdGraphicsState::restore(ID3D12GraphicsCommandList* list, PfdStateGroup group) const noexcept {
  const bool pipeline = includes_pfd_state_group(group, PfdStateGroup::pipeline);
  const bool raster = includes_pfd_state_group(group, PfdStateGroup::raster);
  if (!valid_pfd_state_group(group) || (pipeline && (depth_bias_known_ || strip_cut_known_) && dynamic_native_ != list) ||
      (raster && has_sample_positions() && sample_native_ != list))
    return;
  if (pipeline) {
    list->SetPipelineState(pipeline_);
    if (depth_bias_known_)
      dynamic_native_->RSSetDepthBias(depth_bias_[0], depth_bias_[1], depth_bias_[2]);
    if (strip_cut_known_)
      dynamic_native_->IASetIndexBufferStripCutValue(strip_cut_);
  }
  if (includes_pfd_state_group(group, PfdStateGroup::root_bindings)) {
    list->SetGraphicsRootSignature(root_);
    if (observed_arguments_)
      for (UINT n = 0; n < observed_word_count_; ++n)
        list->SetGraphicsRoot32BitConstant(observed_word_keys_[n] / 64, constants_[n], observed_word_keys_[n] % 64);
    for (UINT n = 0; n < layout_.count; ++n) {
      if (observed_arguments_ &&
          (!layout_.parameters[n].count || layout_.parameters[n].kind == PfdRootKind::constants || !values_[n].known))
        continue;
      const auto& value = values_[n];
      switch (layout_.parameters[n].kind) {
        case PfdRootKind::constants:
          list->SetGraphicsRoot32BitConstants(n, layout_.parameters[n].count, constants_.data() + constant_offsets_[n], 0);
          break;
        case PfdRootKind::table:
          // SetGraphicsRootSignature restored all argument slots to undefined.
          // Preserve that prior state for a positively undefined table; never
          // invent a null or stale descriptor handle. All known tables replay.
          if (value.known)
            list->SetGraphicsRootDescriptorTable(n, {value.address});
          break;
        case PfdRootKind::cbv:
          list->SetGraphicsRootConstantBufferView(n, value.address);
          break;
        case PfdRootKind::srv:
          list->SetGraphicsRootShaderResourceView(n, value.address);
          break;
        case PfdRootKind::uav:
          list->SetGraphicsRootUnorderedAccessView(n, value.address);
          break;
      }
    }
  }
  if (raster) {
    restore_sample_positions(list);
    list->IASetPrimitiveTopology(topology_);
    list->RSSetViewports(viewport_count_, viewports_.data());
    list->RSSetScissorRects(scissor_count_, scissors_.data());
  }
}

}  // namespace taxi_camera
