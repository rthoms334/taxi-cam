#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>

namespace taxi_camera::standalone {
// Comparison-only metadata for adding an owned copy list immediately before
// a proven leading RT exit, or after a barrier-only exit list, inside the SAME ExecuteCommandLists
// call. Never split the application's batch into separate Execute calls.
// A prefix is revoked if the same display later becomes writable again: the
// overlay would run first and the native instrument (or TAA/DLSS history) would
// show through for that stamp frame. A later list in the same Execute that
// writes the display (UAV TAA, a second instrument pass, or a draw/clear that
// keeps the previous list's RT or UAV state) moves the copy after that last
// insertable write instead of stamping first.
//
// The caller observes every recording from a real successful Reset through
// successful Close, reports every GPU command and pass, and invalidates omitted
// or unknown commands. It retains exact native object/resource incarnations and
// snapshots under its existing recording synchronization. This ledger neither
// owns COM objects nor grants concurrent access, resource shape, queue/device,
// source-patch availability or GPU lifetime permission.
class PfdSubmissionProof {
 public:
  static constexpr std::size_t maximum_batch = 256;
  static constexpr std::size_t maximum_resources = 16;
  enum class Refusal : unsigned {
    none,
    unobserved,
    reset_generation,
    external,
    closed_mutation,
    close_failed,
    close_generation,
    close_twice,
    open_pass,
    pass_scope,
    invalid_transition,
    resource_reuse,
    capacity,
    transition_chain,
    barrier_uncertainty,
    enhanced_barrier,
    unsupported_command,
    boundary_invalidation,
    count
  };
  static constexpr const char* refusal_name(Refusal value) noexcept {
    constexpr const char* names[]{"none",
                                  "unobserved",
                                  "reset_generation",
                                  "external",
                                  "closed_mutation",
                                  "close_failed",
                                  "close_generation",
                                  "close_twice",
                                  "open_pass",
                                  "pass_scope",
                                  "invalid_transition",
                                  "resource_reuse",
                                  "capacity",
                                  "transition_chain",
                                  "barrier_uncertainty",
                                  "enhanced_barrier",
                                  "unsupported_command",
                                  "boundary_invalidation"};
    return static_cast<unsigned>(value) < static_cast<unsigned>(Refusal::count) ? names[static_cast<unsigned>(value)] : "invalid_reason";
  }
  Refusal refusal() const noexcept { return refusal_; }
  UINT first_gpu_work() const noexcept { return first_gpu_work_; }
  UINT prefix_blocker() const noexcept { return prefix_blocker_; }
  unsigned diagnostic_flags(std::uint64_t generation) const noexcept {
    return (known_ ? 1u : 0u) | (closed_ ? 2u : 0u) | (active_pass_ ? 4u : 0u) | (barrier_only_ ? 8u : 0u) |
           (generation && generation == recording_ ? 16u : 0u);
  }
  struct Key {
    std::uint64_t resource{}, generation{};
    bool operator==(const Key&) const = default;
  };
  struct Candidate {
    Key key{};
    std::uint64_t recording{};
    D3D12_RESOURCE_STATES first_before{}, state_after{};
    UINT subresource{};
    explicit operator bool() const noexcept { return key.resource && key.generation && recording; }
  };
  struct Recording {
    const PfdSubmissionProof* proof{};
    std::uint64_t generation{};
  };
  struct PrefixRefusal {
    Key key{};
    UINT operation = UINT_MAX;
  };
  PrefixRefusal prefix_refusal(std::size_t index, std::uint64_t generation) const noexcept {
    if (index < slots_.size() && complete(generation)) {
      const auto& slot = slots_[index];
      if (slot.seen && slot.prefix_blocker != UINT_MAX)
        return {slot.key, slot.prefix_blocker};
    }
    return {};
  }

  // Generation belongs to this exact live command-list incarnation. Never
  // renew a stale receipt by reusing a recording generation, even after failure.
  void reset(std::uint64_t generation, bool observed) noexcept {
    const bool advances = generation && generation > recording_;
    slots_ = {};
    recording_ = generation > recording_ ? generation : recording_;
    known_ = advances && observed;
    refusal_ = known_ ? Refusal::none : advances ? Refusal::unobserved : Refusal::reset_generation;
    closed_ = active_pass_ = false;
    barrier_only_ = true;
    prefix_interference_ = false;
    first_gpu_work_ = prefix_blocker_ = UINT_MAX;
  }
  void invalidate(Refusal reason = Refusal::external) noexcept {
    if (known_)
      refusal_ = reason;
    known_ = false;
  }
  void close(std::uint64_t generation, bool succeeded) noexcept {
    if (!known_ || closed_ || active_pass_ || generation != recording_ || !succeeded) {
      invalidate(closed_                    ? Refusal::close_twice
                 : active_pass_             ? Refusal::open_pass
                 : generation != recording_ ? Refusal::close_generation
                                            : Refusal::close_failed);
      return;
    }
    closed_ = true;
  }

  // All draws, dispatches, copies, clears, resolves, bundles, indirect commands,
  // Every GPU operation excludes a tail insertion. For a prefix, the first
  // explicit StateBefore=RT proves that this base mip has remained RT since
  // list entry: the caller has reported every earlier transition/alias.
  // Only operations that can access a texture in RT state interfere. Queries,
  // buffers and shader/copy/depth/resolve operations require other states and
  // cannot have accessed that base mip in a valid stream. Unknown work is never
  // classified as disjoint. Full closed-batch/pass/lifetime guards still apply.
  void gpu_work(UINT operation = 0, bool may_render_target = true) noexcept {
    if (!open())
      return;
    barrier_only_ = false;
    if (first_gpu_work_ == UINT_MAX)
      first_gpu_work_ = operation;
    if (may_render_target) {
      prefix_interference_ = true;
      if (prefix_blocker_ == UINT_MAX)
        prefix_blocker_ = operation;
      // COMMON promotes to RT on the first write in the Execute. A prefix
      // overlay copied before that promotion is then replaced by the instrument.
      for (auto& slot : slots_)
        if (slot.seen && slot.after == D3D12_RESOURCE_STATE_COMMON) {
          slot.wrote = true;
          if (!slot.prefix_exit)
            continue;
          slot.prefix_exit = false;
          if (slot.prefix_blocker == UINT_MAX)
            slot.prefix_blocker = operation;
        }
    }
  }
  void state_disjoint_work(UINT operation) noexcept { gpu_work(operation, false); }
  // ordinary_access excludes PRESERVE_LOCAL and unrecognized access types.
  // SUSPENDING/RESUMING are refused across the ENTIRE batch, even when the pair
  // eventually balances: no copy may be inserted between those two passes.
  void begin_pass(D3D12_RENDER_PASS_FLAGS flags, bool ordinary_access = true) noexcept {
    if (!open())
      return;
    gpu_work(68);
    if (active_pass_ || !ordinary_access || (static_cast<UINT>(flags) & ~static_cast<UINT>(D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES))) {
      invalidate(Refusal::pass_scope);
      return;
    }
    active_pass_ = true;
  }
  void end_pass() noexcept {
    if (!open())
      return;
    if (!active_pass_) {
      invalidate(Refusal::pass_scope);
      return;
    }
    active_pass_ = false;
  }

  // Call for EVERY transition involving admitted display resource incarnations,
  // including those that do not qualify as exits. ALL and base subresource zero
  // both establish base-mip state; the owned copy touches only subresource zero.
  // Resource shape/plane admission remains external (FBW five mips is valid).
  // Aliases, split barriers, unknown types and enhanced barriers without an
  // equivalent complete model must call invalidate(), even on other resources.
  void observe_legacy(Key key,
                      D3D12_RESOURCE_STATES before,
                      D3D12_RESOURCE_STATES after,
                      UINT subresource,
                      D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE) noexcept {
    if (!open())
      return;
    if (active_pass_ || !key.resource || !key.generation || flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
        (subresource != 0 && subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) || before == after) {
      invalidate(Refusal::invalid_transition);
      return;
    }
    Slot* slot = nullptr;
    for (auto& item : slots_) {
      if (item.key == key) {
        slot = &item;
        break;
      }
      if (item.key.resource == key.resource) {
        invalidate(Refusal::resource_reuse);  // Native pointer reuse cannot rewrite an older incarnation.
        return;
      }
    }
    if (!slot)
      for (auto& item : slots_)
        if (!item.key.resource) {
          item.key = key;
          item.first_before = before;
          slot = &item;
          break;
        }
    if (!slot || (slot->seen && slot->after != before)) {
      invalidate(slot ? Refusal::transition_chain : Refusal::capacity);
      return;
    }
    // The first explicit RT exit, before any work able to access RT, proves the state at
    // list entry. A submission-time prefix copy can restore RT before the
    // original list starts, including when that list later samples the display.
    // A later writable transition of the same base mip (TAA/DLSS resolve, a
    // second instrument pass) overwrites that overlay; revoke it so the native
    // instrument cannot flash through a stamp that already ran.
    // Close and the whole-batch pass proof must still succeed before insertion.
    if (!slot->seen && before == D3D12_RESOURCE_STATE_RENDER_TARGET && copy_restorable(after)) {
      slot->leading_exit = true;
      slot->prefix_exit = !prefix_interference_;
      slot->prefix_blocker = prefix_interference_ ? prefix_blocker_ : UINT_MAX;
    }
    slot->seen = true;
    slot->after = after;
    slot->subresource = subresource;
    slot->exit = before == D3D12_RESOURCE_STATE_RENDER_TARGET && copy_restorable(after);
    if (writable_gpu_state(after))
      slot->wrote = true;
    if (slot->prefix_exit && writable_gpu_state(after)) {
      slot->prefix_exit = false;
      if (slot->prefix_blocker == UINT_MAX)
        slot->prefix_blocker = 26;
    }
  }

  bool complete(std::uint64_t generation) const noexcept {
    return known_ && closed_ && !active_pass_ && generation && generation == recording_;
  }
  // Metadata alone is insufficient. The batch-level function below also
  // inspects the producer prefix AND all later lists for pass uncertainty.
  Candidate candidate(Key key, std::uint64_t generation) const noexcept {
    if (complete(generation) && barrier_only_)
      for (const auto& slot : slots_)
        if (slot.key == key && slot.seen && slot.exit)
          return {key, generation, slot.first_before, slot.after, slot.subresource};
    return {};
  }
  Candidate candidate(std::size_t index, std::uint64_t generation) const noexcept {
    if (index < slots_.size() && complete(generation) && barrier_only_) {
      const auto& slot = slots_[index];
      if (slot.seen && slot.exit)
        return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    }
    return {};
  }
  Candidate prefix_candidate(Key key, std::uint64_t generation) const noexcept {
    if (complete(generation))
      for (const auto& slot : slots_)
        if (slot.key == key && slot.seen && slot.prefix_exit)
          return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    return {};
  }
  Candidate prefix_candidate(std::size_t index, std::uint64_t generation) const noexcept {
    if (index < slots_.size() && complete(generation)) {
      const auto& slot = slots_[index];
      if (slot.seen && slot.prefix_exit)
        return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    }
    return {};
  }
  // Compute TAA/DLSS often writes the same display after an RT→SRV exit, in this
  // list or a later list in the same Execute. That overwrite is not a leading RT
  // exit, but the overlay must run after the last proven write.
  Candidate suffix_candidate(Key key, std::uint64_t generation) const noexcept {
    if (complete(generation))
      for (const auto& slot : slots_)
        if (suffix_site(slot))
          if (slot.key == key)
            return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    return {};
  }
  Candidate suffix_candidate(std::size_t index, std::uint64_t generation) const noexcept {
    if (index < slots_.size() && complete(generation)) {
      const auto& slot = slots_[index];
      if (suffix_site(slot))
        return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    }
    return {};
  }
  bool overwrote(Key key, std::uint64_t generation) const noexcept {
    if (complete(generation))
      for (const auto& slot : slots_)
        if (slot.key == key && slot.seen && slot.wrote)
          return true;
    return false;
  }
  // Dispatch/ClearUAV cannot touch an RT-state base mip, so they stay
  // prefix-safe before the first RT exit. After an explicit UAV or COMMON
  // state they can replace a stamp that already ran.
  void compute_work(UINT operation = 14) noexcept {
    state_disjoint_work(operation);
    if (!open())
      return;
    for (auto& slot : slots_) {
      if (!slot.seen)
        continue;
      if (slot.after != D3D12_RESOURCE_STATE_COMMON && !(static_cast<UINT>(slot.after) & D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        continue;
      slot.wrote = true;
      slot.prefix_exit = false;
      if (slot.prefix_blocker == UINT_MAX)
        slot.prefix_blocker = operation;
      if (prefix_blocker_ == UINT_MAX)
        prefix_blocker_ = operation;
    }
  }
  // A draw or clear can keep using RT established by an earlier list. That
  // list has no new barrier, so it is not a suffix, and it runs after the
  // queue copy. The frame is then the clear colour instead of the taxi image
  // or the instrument. Record the write and copy after it, restoring RT.
  // Do not invent RT over an explicit other state.
  void note_render_target_write(Key key, UINT operation = 12) noexcept {
    gpu_work(operation);
    note_carried_write(key, D3D12_RESOURCE_STATE_RENDER_TARGET, operation);
  }
  // Same gap for a UAV clear whose transition was in an earlier list. The
  // caller must already know the resource allows unordered access; this ledger
  // does not see resource flags.
  void note_unordered_access_write(Key key, UINT operation = 50) noexcept {
    compute_work(operation);
    note_carried_write(key, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, operation);
  }
  // A later writable return revokes prefix insertion, but the leading RT exit
  // still proves this display completed. Autodetect activity uses that evidence
  // without stamping before TAA/DLSS or a second instrument pass.
  Candidate activity_candidate(Key key, std::uint64_t generation) const noexcept {
    if (complete(generation))
      for (const auto& slot : slots_)
        if (slot.key == key && slot.seen && slot.leading_exit)
          return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    return {};
  }
  Candidate activity_candidate(std::size_t index, std::uint64_t generation) const noexcept {
    if (index < slots_.size() && complete(generation)) {
      const auto& slot = slots_[index];
      if (slot.seen && slot.leading_exit)
        return {slot.key, generation, slot.first_before, slot.after, slot.subresource};
    }
    return {};
  }
  static bool batch_allows(const Recording* batch, std::size_t count) noexcept {
    if (!batch || !count || count > maximum_batch)
      return false;
    for (std::size_t i = 0; i < count; ++i)
      if (!batch[i].proof || !batch[i].proof->complete(batch[i].generation))
        return false;
    return true;
  }
  static Candidate batch_candidate(const Recording* batch, std::size_t count, std::size_t after_list, Key key) noexcept {
    if (after_list >= count || !batch_allows(batch, count))
      return {};
    return batch[after_list].proof->candidate(key, batch[after_list].generation);
  }
  struct Overlay {
    Candidate candidate{};
    std::size_t list = static_cast<std::size_t>(-1);
    bool before = false;
    explicit operator bool() const noexcept { return static_cast<bool>(candidate) && list != static_cast<std::size_t>(-1); }
  };
  // Prefer a copy after the last proven write in this Execute. A prefix/tail
  // site is refused when a later list overwrote the same display without an
  // insertable after-state: stamping first would flash the native instrument.
  static Overlay batch_overlay(const Recording* batch, std::size_t count, Key key) noexcept {
    Overlay write{}, safe{};
    if (!batch_allows(batch, count))
      return {};
    for (std::size_t i = 0; i < count; ++i) {
      const auto generation = batch[i].generation;
      const auto* proof = batch[i].proof;
      if (auto suffix = proof->suffix_candidate(key, generation))
        write = {suffix, i, false};
      if (!safe.candidate) {
        if (auto tail = proof->candidate(key, generation))
          safe = {tail, i, false};
        else if (auto prefix = proof->prefix_candidate(key, generation))
          safe = {prefix, i, true};
      }
    }
    if (write.candidate)
      return write;
    if (safe.candidate) {
      const auto after = safe.list + (safe.before ? 0 : 1);
      for (std::size_t i = after; i < count; ++i)
        if (batch[i].proof->overwrote(key, batch[i].generation))
          return {};
    }
    return safe;
  }

 private:
  struct Slot {
    Key key{};
    D3D12_RESOURCE_STATES first_before{}, after{};
    UINT subresource{};
    UINT prefix_blocker = UINT_MAX;
    bool seen{}, exit{}, prefix_exit{}, leading_exit{}, wrote{};
  };
  static bool copy_restorable(D3D12_RESOURCE_STATES state) noexcept {
    // COMMON is exact only because an explicit full RT exit established it and
    // this entire insertion-site list has no GPU work to promote it afterward.
    // The owned list explicitly restores COMMON after its copy, preserving the
    // application's later implicit promotion. No Execute boundary is added.
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
    constexpr UINT mask = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return !(static_cast<UINT>(state) & ~mask);
  }
  static bool writable_gpu_state(D3D12_RESOURCE_STATES state) noexcept {
    constexpr UINT mask = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST |
                          D3D12_RESOURCE_STATE_RESOLVE_DEST;
    return (static_cast<UINT>(state) & mask) != 0;
  }
  // Queue copies restore RT, UAV, or SRV/NSR. COMMON after GPU work is not
  // insertable: the first write implicitly promotes it.
  static bool insertable_after(D3D12_RESOURCE_STATES state) noexcept {
    if (state == D3D12_RESOURCE_STATE_RENDER_TARGET || state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
      return true;
    constexpr UINT mask = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return state != D3D12_RESOURCE_STATE_COMMON && (static_cast<UINT>(state) & ~mask) == 0;
  }
  // A barrier-only PSR→RT entry is a write, but the instrument draw is still
  // ahead. Only GPU work on this list, or an insertable non-RT after-state,
  // is a finished overwrite we can copy after.
  bool suffix_site(const Slot& slot) const noexcept {
    return slot.seen && slot.wrote && insertable_after(slot.after) && (slot.after != D3D12_RESOURCE_STATE_RENDER_TARGET || !barrier_only_);
  }
  void note_carried_write(Key key, D3D12_RESOURCE_STATES state, UINT operation) noexcept {
    if (!open() || !key.resource || !key.generation)
      return;
    Slot* slot = nullptr;
    for (auto& item : slots_) {
      if (item.key == key) {
        slot = &item;
        break;
      }
      if (item.key.resource == key.resource) {
        invalidate(Refusal::resource_reuse);
        return;
      }
    }
    if (slot && slot->seen && slot->after != state)
      return;
    if (!slot) {
      for (auto& item : slots_)
        if (!item.key.resource) {
          slot = &item;
          break;
        }
      if (!slot) {
        invalidate(Refusal::capacity);
        return;
      }
      slot->key = key;
    }
    if (!slot->seen) {
      slot->first_before = state;
      slot->after = state;
      slot->seen = true;
    }
    slot->wrote = true;
    if (slot->prefix_exit && slot->prefix_blocker == UINT_MAX)
      slot->prefix_blocker = operation;
    slot->prefix_exit = false;
  }
  bool open() noexcept {
    if (!known_ || closed_) {
      invalidate(Refusal::closed_mutation);
      return false;
    }
    return true;
  }
  std::array<Slot, maximum_resources> slots_{};
  std::uint64_t recording_{};
  bool known_{}, closed_{}, active_pass_{}, barrier_only_{true};
  bool prefix_interference_{};
  UINT first_gpu_work_{UINT_MAX}, prefix_blocker_{UINT_MAX};
  Refusal refusal_{Refusal::unobserved};
};
}  // namespace taxi_camera::standalone
