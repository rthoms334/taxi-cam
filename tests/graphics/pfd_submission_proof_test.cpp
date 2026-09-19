#include "../../src/graphics/pfd_submission_proof.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using Proof = taxi_camera::standalone::PfdSubmissionProof;
constexpr Proof::Key left{0x1000, 10}, right{0x2000, 20}, replacement{0x1000, 11};
constexpr auto rt = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr auto psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto nsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
unsigned checks{};
void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
Proof exit_recording(std::uint64_t generation = 1, Proof::Key key = left) {
  Proof proof;
  proof.reset(generation, true);
  proof.observe_legacy(key, rt, psr, 0);
  proof.close(generation, true);
  return proof;
}
bool admitted(const Proof& proof, std::uint64_t generation = 1, Proof::Key key = left) {
  const Proof::Recording batch[]{{&proof, generation}};
  return static_cast<bool>(Proof::batch_candidate(batch, 1, 0, key));
}
void lifecycle() {
  Proof proof;
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(1, true);
  require(!admitted(proof), "Pre-existing unobserved recording admitted");
  proof.reset(1, false);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(1, true);
  require(!admitted(proof), "Unobserved Reset admitted");
  proof.reset(2, true);
  proof.observe_legacy(left, rt, psr, 0);
  require(!admitted(proof, 2), "Open list admitted");
  proof.close(2, true);
  require(admitted(proof, 2), "Observed Reset/Close failed admission");
  for (unsigned replay = 0; replay < 20; ++replay)
    require(admitted(proof, 2), "Immutable completed recording lost replay eligibility");
  require(!admitted(proof, 1), "Older generation admitted");
  require(!admitted(proof, 3), "Future generation admitted");
  proof.reset(3, true);
  require(!admitted(proof, 2), "Reset reused a previous receipt");
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(3, false);
  require(!admitted(proof, 3), "Failed Close admitted");
  proof.reset(4, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(3, true);
  proof.close(4, true);
  require(!admitted(proof, 4), "Mismatched Close resurrected invalid evidence");
  proof.reset(5, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(5, true);
  proof.gpu_work();
  require(!admitted(proof, 5), "Mutation after Close retained admission");
  proof.reset(5, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(5, true);
  require(!admitted(proof, 5), "Reused recording generation admitted");
  proof.reset(std::numeric_limits<std::uint64_t>::max(), true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(std::numeric_limits<std::uint64_t>::max(), true);
  require(admitted(proof, std::numeric_limits<std::uint64_t>::max()), "Last unique generation failed");
  proof.reset(0, true);
  proof.reset(1, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(1, true);
  require(!admitted(proof), "Generation wrap introduced ABA admission");
}
void exact_states_and_keys() {
  for (const auto after : {D3D12_RESOURCE_STATE_COMMON, psr, nsr, static_cast<D3D12_RESOURCE_STATES>(psr | nsr)})
    for (const UINT subresource : {0u, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES}) {
      Proof proof;
      proof.reset(1, true);
      proof.observe_legacy(left, rt, after, subresource);
      proof.observe_legacy(right, rt, after, subresource);
      proof.close(1, true);
      const Proof::Recording batch[]{{&proof, 1}};
      const auto candidate = Proof::batch_candidate(batch, 1, 0, left);
      require(candidate && candidate.key == left && candidate.first_before == rt && candidate.state_after == after &&
                  candidate.subresource == subresource,
              "Base/ALL RT exit lost exact copy restoration state");
      require(admitted(proof, 1, right), "Independent second target lost");
      require(!admitted(proof, 1, replacement), "Resource address reuse matched stale incarnation");
    }
  for (const auto after : {D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ, rt}) {
    Proof proof;
    proof.reset(1, true);
    proof.observe_legacy(left, rt, after, 0);
    proof.close(1, true);
    require(!admitted(proof), "Unsupported final state admitted");
  }
  for (const Proof::Key key : {Proof::Key{0, 10}, Proof::Key{0x1000, 0}}) {
    auto proof = exit_recording(1, key);
    require(!admitted(proof, 1, key), "Missing native incarnation admitted");
  }
  for (const UINT partial : {1u, 4u, 5u}) {
    Proof proof;
    proof.reset(1, true);
    proof.observe_legacy(left, rt, psr, partial);
    proof.close(1, true);
    require(!admitted(proof), "Nonbase FBW mip admitted as base-mip state");
  }
}
void effects_and_work() {
  for (const bool before : {false, true}) {
    Proof proof;
    proof.reset(1, true);
    if (before)
      proof.gpu_work();
    proof.observe_legacy(left, rt, psr, 0);
    if (!before)
      proof.gpu_work();
    proof.close(1, true);
    require(proof.complete(1) && !admitted(proof), "Draw/copy/clear/query work admitted insertion site");
  }
  for (const auto flags : {D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY}) {
    Proof proof;
    proof.reset(1, true);
    proof.observe_legacy(left, rt, psr, 0, flags);
    proof.observe_legacy(left, rt, psr, 0);
    proof.close(1, true);
    require(!admitted(proof), "Later full barrier erased split uncertainty");
  }
  for (const bool invalidate_before : {false, true}) {
    Proof proof;
    proof.reset(1, true);
    if (invalidate_before)
      proof.invalidate();  // Alias/wildcard/unknown/enhanced/unobserved work.
    proof.observe_legacy(left, rt, psr, 0);
    if (!invalidate_before)
      proof.invalidate();
    proof.close(1, true);
    require(!admitted(proof), "Unknown/alias effect resurrected within recording");
  }
  Proof proof;
  proof.reset(1, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.observe_legacy(left, psr, D3D12_RESOURCE_STATE_COMMON, 0);
  proof.close(1, true);
  require(!admitted(proof), "Later barrier retained stale exit state");
  proof.reset(2, true);
  proof.observe_legacy(left, psr, rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
  proof.observe_legacy(left, rt, psr, 0);
  proof.close(2, true);
  const auto candidate = proof.candidate(left, 2);
  require(candidate && candidate.first_before == psr && candidate.state_after == psr,
          "Complete entry/exit state chain failed replay metadata");
  proof.reset(3, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.observe_legacy(left, rt, psr, 0);  // StateBefore disagrees with prior StateAfter.
  proof.close(3, true);
  require(!admitted(proof, 3), "Contradictory later transition became last-wins evidence");
  proof.reset(4, true);
  proof.observe_legacy(left, rt, psr, 0);
  proof.observe_legacy(replacement, rt, psr, 0);
  proof.close(4, true);
  require(!admitted(proof, 4, replacement), "Reused resource address replaced recorded incarnation");
  proof.reset(5, true);
  for (std::size_t i = 0; i < Proof::maximum_resources + 1; ++i)
    proof.observe_legacy({0x1000 + i, 10 + i}, rt, psr, 0);
  proof.close(5, true);
  require(!admitted(proof, 5), "Resource capacity overflow silently truncated proof");
  proof.reset(6, true);
  for (std::size_t i = 0; i < Proof::maximum_resources; ++i)
    proof.observe_legacy({0x1000 + i, 10 + i}, rt, psr, 0);
  proof.close(6, true);
  for (std::size_t i = 0; i < Proof::maximum_resources; ++i) {
    const auto indexed = proof.candidate(i, 6);
    require(indexed && indexed.key == Proof::Key{0x1000 + i, 10 + i}, "Bounded indexed discovery lost eligible display");
  }
  require(!proof.candidate(Proof::maximum_resources, 6), "Indexed discovery exceeded resource capacity");
  for (const bool later_work : {false, true}) {
    proof.reset(later_work ? 8 : 7, true);
    proof.observe_legacy(left, rt, D3D12_RESOURCE_STATE_COMMON, 0);
    if (later_work)
      proof.gpu_work();  // Could implicitly promote COMMON to another state.
    else
      proof.observe_legacy(left, D3D12_RESOURCE_STATE_COMMON, psr, 0);
    proof.close(later_work ? 8 : 7, true);
    require(!admitted(proof, later_work ? 8 : 7), "Later work/barrier retained stale explicit COMMON exit");
  }
  proof.reset(9, true);
  proof.close(9, true);
  require(!admitted(proof, 9), "Empty list guessed implicit COMMON state");
  proof.reset(10, true);
  proof.observe_legacy(left, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON, 0);
  proof.close(10, true);
  require(!admitted(proof, 10), "Non-RT COMMON transition admitted as completed display output");
}
void whole_batch_passes() {
  auto exit = exit_recording();
  for (const auto flags : {D3D12_RENDER_PASS_FLAG_NONE, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES}) {
    Proof producer;
    producer.reset(1, true);
    producer.begin_pass(flags);
    producer.gpu_work();
    producer.end_pass();
    producer.close(1, true);
    Proof consumer;
    consumer.reset(1, true);
    consumer.gpu_work();
    consumer.close(1, true);
    const Proof::Recording batch[]{{&producer, 1}, {&exit, 1}, {&consumer, 1}};
    require(Proof::batch_allows(batch, 3), "Complete ordinary producer pass rejected whole batch");
    require(static_cast<bool>(Proof::batch_candidate(batch, 3, 1, left)), "Ordinary producer blocked proven barrier-list insertion");
    require(!Proof::batch_candidate(batch, 3, 0, left) && !Proof::batch_candidate(batch, 3, 2, left),
            "Producer/consumer GPU list became insertion site");
  }
  for (const auto flags :
       {D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS, D3D12_RENDER_PASS_FLAG_RESUMING_PASS,
        static_cast<D3D12_RENDER_PASS_FLAGS>(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS | D3D12_RENDER_PASS_FLAG_RESUMING_PASS),
        static_cast<D3D12_RENDER_PASS_FLAGS>(8)}) {
    Proof pass;
    pass.reset(1, true);
    pass.begin_pass(flags);
    pass.end_pass();
    pass.close(1, true);
    for (const bool prefix : {false, true}) {
      const Proof::Recording batch[]{{prefix ? &pass : &exit, 1}, {prefix ? &exit : &pass, 1}};
      require(!Proof::batch_candidate(batch, 2, prefix ? 1 : 0, left),
              "Prefix suspension/future resumption or unknown pass flags admitted copy");
    }
  }
  Proof suspended, resumed;
  suspended.reset(1, true);
  suspended.begin_pass(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
  suspended.end_pass();
  suspended.close(1, true);
  resumed.reset(1, true);
  resumed.begin_pass(D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
  resumed.end_pass();
  resumed.close(1, true);
  const Proof::Recording sandwich[]{{&suspended, 1}, {&exit, 1}, {&resumed, 1}};
  require(!Proof::batch_candidate(sandwich, 3, 1, left), "Suspended/pass-free/resumed sandwich admitted GPU copy");
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    Proof pass;
    pass.reset(1, true);
    if (invalid == 0)
      pass.end_pass();
    else {
      pass.begin_pass(D3D12_RENDER_PASS_FLAG_NONE, invalid != 1);
      if (invalid == 2)
        pass.begin_pass(D3D12_RENDER_PASS_FLAG_NONE);
      if (invalid == 3)
        pass.observe_legacy(left, rt, psr, 0);
      if (invalid != 4)
        pass.end_pass();
    }
    pass.close(1, true);
    const Proof::Recording batch[]{{&pass, 1}, {&exit, 1}};
    require(!Proof::batch_candidate(batch, 2, 1, left), "Malformed/local-access/unclosed pass admitted batch");
  }
}
void bounds_and_unknown_lists() {
  auto exit = exit_recording();
  Proof unknown;
  const Proof::Recording valid[]{{&exit, 1}};
  require(!Proof::batch_allows(nullptr, 1) && !Proof::batch_allows(valid, 0), "Missing batch admitted");
  require(!Proof::batch_allows(valid, Proof::maximum_batch + 1), "Oversized batch read beyond bounded metadata");
  require(!Proof::batch_candidate(valid, 1, 1, left), "Out-of-bounds insertion index admitted");
  for (const Proof::Recording missing :
       {Proof::Recording{}, Proof::Recording{&unknown, 1}, Proof::Recording{&exit, 0}, Proof::Recording{&exit, 2}})
    for (const bool prefix : {false, true}) {
      const Proof::Recording batch[]{prefix ? missing : valid[0], prefix ? valid[0] : missing};
      require(!Proof::batch_candidate(batch, 2, prefix ? 1 : 0, left), "Unknown/stale prefix or suffix ignored");
    }
  std::array<Proof::Recording, Proof::maximum_batch> maximum{};
  maximum.fill(valid[0]);
  require(Proof::batch_allows(maximum.data(), maximum.size()), "Exact fixed batch capacity rejected");
}
void leading_exit_prefix() {
  for (const auto after : {D3D12_RESOURCE_STATE_COMMON, psr, static_cast<D3D12_RESOURCE_STATES>(psr | nsr)})
    for (const UINT subresource : {0u, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES}) {
      Proof proof;
      proof.reset(1, true);
      proof.observe_legacy(right, psr, rt, 0);  // Another texture's leading barrier is harmless.
      proof.observe_legacy(left, rt, after, subresource);
      proof.gpu_work();  // The original list consumes the PFD after its exit.
      proof.close(1, true);
      const Proof::Recording batch[]{{&proof, 1}};
      const auto prefix = proof.prefix_candidate(left, 1);
      const bool common_after = after == D3D12_RESOURCE_STATE_COMMON;
      // COMMON promotes to RT on the first write. A prefix overlay would then
      // be replaced by the native instrument for that stamp frame.
      require(Proof::batch_allows(batch, 1) && !proof.candidate(left, 1) && !proof.prefix_candidate(right, 1),
              "Later GPU work accidentally relaxed existing tail admission");
      require(static_cast<bool>(prefix) == !common_after &&
                  (!prefix || (prefix.key == left && prefix.recording == 1 && prefix.first_before == rt)),
              "Leading full RT exit followed by GPU work lost exact prefix restoration state");
      require(static_cast<bool>(proof.activity_candidate(left, 1)) && proof.activity_candidate(left, 1).first_before == rt,
              "Later overwrite revoked prefix insertion and also lost leading-exit activity");
      if (common_after)
        continue;
      const auto indexed = proof.prefix_candidate(1u, 1);
      require(indexed && indexed.key == left && !proof.prefix_candidate(Proof::maximum_resources, 1),
              "Indexed prefix lookup lost identity or exceeded resource bounds");
      for (unsigned replay = 0; replay < 3; ++replay)
        require(static_cast<bool>(proof.prefix_candidate(left, 1)), "Immutable prefix lost replay eligibility");
      require(!proof.prefix_candidate(replacement, 1) && !proof.prefix_candidate(left, 2),
              "Prefix accepted stale resource or recording incarnation");
      proof.reset(2, true);
      require(!proof.prefix_candidate(left, 1) && !proof.prefix_candidate(left, 2), "Reset retained an earlier leading exit");
    }
  for (unsigned preceding = 0; preceding < 3; ++preceding) {
    Proof proof;
    proof.reset(1, true);
    if (preceding == 0)
      proof.gpu_work();
    else if (preceding == 1)
      proof.observe_legacy(left, psr, rt, 0);
    else {
      proof.begin_pass(D3D12_RENDER_PASS_FLAG_NONE);
      proof.end_pass();
    }
    proof.observe_legacy(left, rt, psr, 0);
    proof.close(1, true);
    require(!proof.prefix_candidate(left, 1), "Earlier GPU work/pass/target transition was forgotten by prefix admission");
  }
  Proof later_barriers;
  later_barriers.reset(1, true);
  later_barriers.observe_legacy(left, rt, psr, 0);
  later_barriers.gpu_work();
  later_barriers.observe_legacy(left, psr, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
  later_barriers.close(1, true);
  const auto first = later_barriers.prefix_candidate(left, 1);
  require(first && first.first_before == rt && !later_barriers.candidate(left, 1),
          "Later barrier overwrote the immutable first-exit fact or revived a tail copy");
  for (const bool observed : {false, true}) {
    Proof proof;
    proof.reset(1, observed);
    proof.observe_legacy(left, rt, psr, 0);
    require(!proof.prefix_candidate(left, 1), "Open prefix recording admitted");
    proof.close(1, !observed);
    require(!proof.prefix_candidate(left, 1), "Unobserved Reset or failed Close admitted prefix");
  }
  for (const UINT partial : {1u, 4u}) {
    auto proof = Proof{};
    proof.reset(1, true);
    proof.observe_legacy(left, rt, psr, partial);
    proof.close(1, true);
    require(!proof.prefix_candidate(left, 1), "Lower mip transition inferred base-mip prefix state");
  }
  for (unsigned uncertainty = 0; uncertainty < 6; ++uncertainty) {
    Proof proof;
    proof.reset(1, true);
    proof.observe_legacy(left, rt, psr, 0);
    if (uncertainty < 3)
      proof.invalidate();  // Caller-observed alias, enhanced barrier, or unknown work.
    else if (uncertainty == 3)
      proof.observe_legacy(right, rt, psr, 0, D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
    else if (uncertainty == 4)
      proof.observe_legacy(replacement, psr, rt, 0);
    else {
      proof.begin_pass(D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
      proof.end_pass();
    }
    proof.close(1, true);
    require(!proof.prefix_candidate(left, 1), "Future uncertainty resurrected an earlier prefix opportunity");
  }
  auto exit = exit_recording();
  for (const auto flags : {D3D12_RENDER_PASS_FLAG_NONE, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS, D3D12_RENDER_PASS_FLAG_RESUMING_PASS}) {
    Proof pass;
    pass.reset(1, true);
    pass.begin_pass(flags);
    pass.gpu_work();
    pass.end_pass();
    pass.close(1, true);
    for (const bool before : {false, true}) {
      const Proof::Recording batch[]{{before ? &pass : &exit, 1}, {before ? &exit : &pass, 1}};
      const bool admitted_prefix = Proof::batch_allows(batch, 2) && static_cast<bool>(exit.prefix_candidate(left, 1));
      require(admitted_prefix == (flags == D3D12_RENDER_PASS_FLAG_NONE),
              "Whole-batch prefix failed ordinary pass or ignored past/future suspension");
    }
  }
  Proof unknown;
  for (const Proof::Recording missing : {Proof::Recording{&unknown, 1}, Proof::Recording{&exit, 2}}) {
    const Proof::Recording batch[]{{&exit, 1}, missing};
    require(!Proof::batch_allows(batch, 2), "Prefix ignored unknown or stale later recording");
  }
}
void state_disjoint_prefix_work() {
  // These operations cannot access the admitted base mip while it remains in
  // RT state. They are still GPU work, so none may create a tail opportunity.
  constexpr UINT disjoint[]{14, 15, 16, 17, 18, 19, 47, 49, 50, 52, 53, 54, 64, 66, 72, 73, 74, 76};
  constexpr UINT interfering[]{0, 12, 13, 48, 60, 61, 79};
  for (const UINT operation : disjoint)
    for (const auto after : {D3D12_RESOURCE_STATE_COMMON, static_cast<D3D12_RESOURCE_STATES>(psr | nsr)}) {
      Proof proof;
      require(proof.first_gpu_work() == UINT_MAX && proof.prefix_blocker() == UINT_MAX, "Fresh proof invented GPU diagnostics");
      proof.reset(1, true);
      proof.state_disjoint_work(operation);
      proof.observe_legacy(left, rt, after, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
      proof.close(1, true);
      const auto prefix = proof.prefix_candidate(left, 1);
      const Proof::Recording batch[]{{&proof, 1}};
      require(Proof::batch_allows(batch, 1) && prefix && prefix.first_before == rt && prefix.state_after == after,
              "State-disjoint work before explicit RT exit blocked exact prefix restoration");
      require(!proof.candidate(left, 1), "State-disjoint work accidentally admitted a tail copy");
      require(proof.first_gpu_work() == operation && proof.prefix_blocker() == UINT_MAX,
              "State-disjoint GPU work became an RT blocker or lost its operation diagnostic");
      proof.reset(2, true);
      require(proof.first_gpu_work() == UINT_MAX && proof.prefix_blocker() == UINT_MAX && !proof.prefix_candidate(left, 1),
              "Reset retained previous work diagnostics or prefix evidence");
    }
  for (const UINT operation : interfering)
    for (const bool first : {false, true}) {
      Proof proof;
      proof.reset(1, true);
      if (!first)
        proof.state_disjoint_work(53);
      proof.gpu_work(operation);
      proof.state_disjoint_work(15);
      proof.observe_legacy(left, rt, psr, 0);
      proof.close(1, true);
      require(proof.complete(1) && !proof.prefix_candidate(left, 1) && !proof.candidate(left, 1),
              "Earlier RT writer/unknown work was erased by state-disjoint work");
      require(proof.first_gpu_work() == (first ? operation : 53) && proof.prefix_blocker() == operation,
              "First GPU operation and first RT blocker were conflated");
    }
  Proof mixed;
  mixed.reset(1, true);
  mixed.state_disjoint_work(53);
  mixed.observe_legacy(left, rt, psr, 0);
  mixed.gpu_work(48);  // Later drawing/clearing does not precede the proved first exit.
  mixed.close(1, true);
  const auto sample_only = mixed.prefix_candidate(left, 1);
  require(sample_only && sample_only.first_before == rt && sample_only.state_after == psr && !mixed.candidate(left, 1),
          "Later sample-only GPU work revoked a leading RT-exit prefix");
  mixed.reset(2, true);
  mixed.state_disjoint_work(53);
  mixed.observe_legacy(left, rt, psr, 0);
  mixed.gpu_work(48);
  mixed.observe_legacy(left, psr, rt, 0);
  mixed.observe_legacy(left, rt, D3D12_RESOURCE_STATE_COMMON, 0);
  mixed.close(2, true);
  require(!mixed.prefix_candidate(left, 2) && !mixed.candidate(left, 2) && mixed.activity_candidate(left, 2),
          "Later writable return to RT kept a prefix overlay that the instrument can overwrite");
  require(mixed.first_gpu_work() == 53 && mixed.prefix_blocker() == 48, "Later RT writer diagnostic was lost");
  mixed.state_disjoint_work(14);
  require(!mixed.complete(2) && !mixed.prefix_candidate(left, 2) && mixed.refusal() == Proof::Refusal::closed_mutation,
          "State-disjoint work after Close retained immutable recording permission");
  for (unsigned uncertainty = 0; uncertainty < 7; ++uncertainty) {
    Proof proof;
    proof.reset(1, true);
    proof.state_disjoint_work(53);
    if (uncertainty == 0)
      proof.observe_legacy(left, psr, rt, 0);  // First state is not RT at list entry.
    if (uncertainty == 1) {
      proof.begin_pass(D3D12_RENDER_PASS_FLAG_NONE);
      proof.end_pass();
    }
    proof.observe_legacy(left, rt, psr, 0);
    if (uncertainty == 2)
      proof.observe_legacy(left, rt, psr, 0);  // Contradicts the first exit.
    if (uncertainty == 3)
      proof.invalidate(Proof::Refusal::barrier_uncertainty);  // Alias/split.
    if (uncertainty == 4)
      proof.invalidate(Proof::Refusal::enhanced_barrier);
    if (uncertainty == 5)
      proof.observe_legacy(replacement, psr, rt, 0);
    if (uncertainty == 6) {
      proof.begin_pass(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
      proof.end_pass();
    }
    proof.close(1, true);
    require(!proof.prefix_candidate(left, 1), "State-disjoint work weakened first-state, pass, alias or incarnation guards");
    if (uncertainty == 1)
      require(proof.first_gpu_work() == 53 && proof.prefix_blocker() == 68,
              "Ordinary pass entry failed to retain the preceding query and pass blocker separately");
  }
}
void later_list_overwrite() {
  constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Proof prefix;
  prefix.reset(1, true);
  prefix.observe_legacy(left, rt, psr, 0);
  prefix.gpu_work(48);
  prefix.close(1, true);
  require(prefix.prefix_candidate(left, 1) && !prefix.overwrote(left, 1) && !prefix.suffix_candidate(left, 1),
          "Sample-only work after RT→SRV invented a later overwrite");
  Proof compute;
  compute.reset(1, true);
  compute.observe_legacy(left, psr, uav, 0);
  compute.compute_work(14);
  compute.observe_legacy(left, uav, psr, 0);
  compute.close(1, true);
  require(!compute.prefix_candidate(left, 1) && compute.overwrote(left, 1) && compute.suffix_candidate(left, 1).state_after == psr,
          "UAV TAA write failed to become a suffix overlay site");
  const Proof::Recording batch[]{{&prefix, 1}, {&compute, 1}};
  const auto overlay = Proof::batch_overlay(batch, 2, left);
  require(overlay && !overlay.before && overlay.list == 1 && overlay.candidate.state_after == psr,
          "Prefix overlay was kept in front of a later UAV write of the same display");
  require(static_cast<bool>(prefix.prefix_candidate(left, 1)), "Later-list scan mutated the earlier prefix recording");
  Proof only_prefix = prefix;
  const Proof::Recording prefix_only[]{{&only_prefix, 1}};
  const auto kept = Proof::batch_overlay(prefix_only, 1, left);
  require(kept && kept.before && kept.list == 0, "Single-list prefix overlay was lost");
  Proof same;
  same.reset(1, true);
  same.observe_legacy(left, rt, psr, 0);
  same.observe_legacy(left, psr, uav, 0);
  same.observe_legacy(left, uav, psr, 0);
  same.close(1, true);
  require(!same.prefix_candidate(left, 1) && same.suffix_candidate(left, 1).state_after == psr && same.activity_candidate(left, 1),
          "Same-list UAV return kept a prefix stamp or lost leading-exit activity");
  Proof dispatch_uav;
  dispatch_uav.reset(1, true);
  dispatch_uav.observe_legacy(left, rt, uav, 0);
  dispatch_uav.compute_work(14);
  dispatch_uav.close(1, true);
  require(!dispatch_uav.prefix_candidate(left, 1) && dispatch_uav.suffix_candidate(left, 1).state_after == uav &&
              dispatch_uav.prefix_blocker() == 14,
          "Dispatch on an explicit UAV display kept a prefix overlay");
  Proof dispatch_before;
  dispatch_before.reset(1, true);
  dispatch_before.compute_work(14);
  dispatch_before.observe_legacy(left, rt, psr, 0);
  dispatch_before.close(1, true);
  require(dispatch_before.prefix_candidate(left, 1) && dispatch_before.prefix_blocker() == UINT_MAX,
          "Dispatch before the first RT exit blocked a still-valid prefix");
  Proof common_write;
  common_write.reset(1, true);
  common_write.observe_legacy(left, rt, D3D12_RESOURCE_STATE_COMMON, 0);
  common_write.gpu_work(48);
  common_write.close(1, true);
  require(!common_write.prefix_candidate(left, 1) && !common_write.suffix_candidate(left, 1),
          "COMMON plus later GPU work invented an after-state copy");
  const Proof::Recording flashed[]{{&prefix, 1}, {&common_write, 1}};
  require(!Proof::batch_overlay(flashed, 2, left), "Prefix overlay was kept when a later list overwrote without an insertable after-state");
  Proof enter;
  enter.reset(1, true);
  enter.observe_legacy(left, psr, rt, 0);
  enter.close(1, true);
  Proof leave;
  leave.reset(1, true);
  leave.observe_legacy(left, rt, psr, 0);
  leave.close(1, true);
  require(enter.overwrote(left, 1) && !enter.suffix_candidate(left, 1) && leave.candidate(left, 1),
          "Barrier-only RT entry became a suffix site or lost the later tail exit");
  const Proof::Recording split[]{{&enter, 1}, {&leave, 1}};
  const auto tail = Proof::batch_overlay(split, 2, left);
  require(tail && !tail.before && tail.list == 1 && tail.candidate.state_after == psr,
          "Split RT entry/exit placed the overlay on the entry list instead of after the exit");
  Proof carried;
  carried.reset(1, true);
  carried.note_render_target_write(left, 48);
  carried.close(1, true);
  require(carried.suffix_candidate(left, 1).state_after == rt && !carried.prefix_candidate(left, 1),
          "Carried RT clear or draw did not become a suffix after that write");
  const Proof::Recording flashed_white[]{{&prefix, 1}, {&carried, 1}};
  const auto after_clear = Proof::batch_overlay(flashed_white, 2, left);
  require(after_clear && !after_clear.before && after_clear.list == 1 && after_clear.candidate.state_after == rt,
          "Queue copy stayed in front of a later RT clear that has no new barrier");
  const Proof::Recording after_uav[]{{&compute, 1}, {&carried, 1}};
  const auto cover_clear = Proof::batch_overlay(after_uav, 2, left);
  require(cover_clear && !cover_clear.before && cover_clear.list == 1, "UAV suffix stayed in front of a later carried RT write");
  Proof contradicted;
  contradicted.reset(1, true);
  contradicted.observe_legacy(left, rt, psr, 0);
  contradicted.note_render_target_write(left, 12);
  contradicted.close(1, true);
  require(contradicted.prefix_candidate(left, 1) && !contradicted.suffix_candidate(left, 1),
          "A draw after an RT exit invented an RT suffix or dropped the leading prefix");
  Proof uav_clear;
  uav_clear.reset(1, true);
  uav_clear.note_unordered_access_write(left, 50);
  uav_clear.close(1, true);
  require(uav_clear.suffix_candidate(left, 1).state_after == uav && !uav_clear.prefix_candidate(left, 1),
          "Carried UAV clear did not become a suffix");
}
}  // namespace

int main() {
  try {
    lifecycle();
    exact_states_and_keys();
    effects_and_work();
    whole_batch_passes();
    bounds_and_unknown_lists();
    leading_exit_prefix();
    state_disjoint_prefix_work();
    later_list_overwrite();
    std::printf("PASS PFD submission proof: %u lifecycle, final-state, GPU-work, whole-batch pass and bound checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL PFD submission proof: %s\n", error.what());
    return 1;
  }
}
