#include "add_diffuse_observe.hpp"

#include "../graphics/add_diffuse_interest.hpp"
#include "add_diffuse_slot.hpp"
#include "local_memory.hpp"
#include "view_pool.hpp"

#include <atomic>
#include <cstdio>

namespace taxi_camera::native_camera {
namespace {
struct Staged {
  bool known = false;
  std::uint64_t owned0 = 0;
  std::uint64_t owned1 = 0;
};

Staged& staged() noexcept {
  static thread_local Staged value;
  return value;
}

add_diffuse::Interest from_slot(const engine_camera::AddDiffuseSlot& slot) noexcept {
  add_diffuse::Interest interest;
  interest.inspected = slot.complete;
  interest.present = slot.present;
  interest.resource_present = slot.resource_present;
  interest.bit49 = slot.bit49;
  interest.format = slot.format;
  interest.width = slot.width;
  interest.height = slot.height;
  interest.mips = slot.mips;
  interest.resource = slot.resource_address;
  return interest;
}

void publish_view(unsigned role, LocalMemoryReader& reader, std::uint64_t view, std::uint64_t primary) noexcept {
  if (!view || view == primary) {
    add_diffuse::Interest absent;
    absent.inspected = true;
    add_diffuse::publish_interest(role, absent);
    return;
  }
  reader.reset_budget();
  const auto slot = engine_camera::inspect_add_diffuse_slot(reader, view);
  if (!slot.complete) {
    char line[160];
    std::snprintf(line, sizeof(line), "ADD_DIFFUSE view=%s inspection_failed error=%s", add_diffuse::role_name(role),
                  slot.error && slot.error[0] ? slot.error : "unknown");
    add_diffuse::log_line(line);
    return;
  }
  add_diffuse::publish_interest(role, from_slot(slot));
}
}  // namespace

void stage_add_diffuse_owned(std::uint64_t owned0, std::uint64_t owned1) noexcept {
  staged().known = true;
  staged().owned0 = owned0;
  staged().owned1 = owned1;
}

void flush_add_diffuse(std::uint64_t renderer) noexcept {
  const bool known = staged().known;
  const auto owned0 = staged().owned0;
  const auto owned1 = staged().owned1;
  staged() = {};
  if (!renderer)
    return;
  static std::atomic<std::uint64_t> last{0};
  const auto now = GetTickCount64();
  auto previous = last.load(std::memory_order_relaxed);
  if (now - previous < 1000)
    return;
  if (!last.compare_exchange_strong(previous, now, std::memory_order_relaxed))
    return;
  LocalMemoryReader reader;
  const auto pool = engine_camera::inspect_view_pool(reader, renderer);
  if (!pool.valid || !pool.slots[0].view_address) {
    add_diffuse::log_line("ADD_DIFFUSE primary view pool unavailable");
    return;
  }
  reader.reset_budget();
  const auto primary = engine_camera::inspect_add_diffuse_slot(reader, pool.slots[0].view_address);
  if (!primary.complete) {
    char line[160];
    std::snprintf(line, sizeof(line), "ADD_DIFFUSE view=primary inspection_failed error=%s",
                  primary.error && primary.error[0] ? primary.error : "unknown");
    add_diffuse::log_line(line);
  } else {
    add_diffuse::publish_interest(add_diffuse::kPrimaryRole, from_slot(primary));
  }
  if (!known)
    return;
  publish_view(1, reader, owned0, pool.slots[0].view_address);
  publish_view(2, reader, owned1, pool.slots[0].view_address);
}

}  // namespace taxi_camera::native_camera
