#include "add_diffuse_interest.hpp"

#include "../shared/rotating_log.hpp"

#include <atomic>
#include <cstdio>
#include <string>

namespace taxi_camera::add_diffuse {
namespace {
struct Slot {
  std::atomic<std::uint64_t> resource{0};
  std::atomic<std::uint64_t> meta{0};
  std::atomic<std::uint64_t> logged{0};
  std::atomic<std::uint64_t> logged_at{0};
};

Slot& slot(unsigned role) noexcept {
  static Slot slots[kRoleCount];
  return slots[role < kRoleCount ? role : 0];
}

std::uint64_t pack(const Interest& interest) noexcept {
  std::uint64_t value = 0;
  value |= interest.inspected ? 1ull : 0;
  value |= interest.present ? 2ull : 0;
  value |= interest.resource_present ? 4ull : 0;
  value |= interest.bit49 ? 8ull : 0;
  value |= (static_cast<std::uint64_t>(interest.format) & 0xffffu) << 4;
  value |= (static_cast<std::uint64_t>(interest.width) & 0xffffu) << 20;
  value |= (static_cast<std::uint64_t>(interest.height) & 0xffffu) << 36;
  value |= (static_cast<std::uint64_t>(interest.mips) & 0xffu) << 52;
  return value;
}

Interest unpack(std::uint64_t meta, std::uint64_t resource) noexcept {
  Interest interest;
  interest.inspected = (meta & 1ull) != 0;
  interest.present = (meta & 2ull) != 0;
  interest.resource_present = (meta & 4ull) != 0;
  interest.bit49 = (meta & 8ull) != 0;
  interest.format = static_cast<std::uint32_t>((meta >> 4) & 0xffffu);
  interest.width = static_cast<std::uint32_t>((meta >> 20) & 0xffffu);
  interest.height = static_cast<std::uint32_t>((meta >> 36) & 0xffffu);
  interest.mips = static_cast<std::uint16_t>((meta >> 52) & 0xffu);
  interest.resource = resource;
  return interest;
}

void remember(unsigned role, const Interest& interest) noexcept {
  char line[640];
  std::snprintf(line, sizeof(line),
                "ADD_DIFFUSE VIEWPORT_MATERIAL_ADD_DIFFUSE slot=9 offset=664 view=%s inspected=%u present=%u resource=%u "
                "format=%u %s size=%ux%u mips=%u bit49=%u",
                role_name(role), interest.inspected ? 1u : 0u, interest.present ? 1u : 0u, interest.resource_present ? 1u : 0u,
                interest.format, format_name(interest.format), interest.width, interest.height, interest.mips, interest.bit49 ? 1u : 0u);
  log_line(line);
}
}  // namespace

void log_line(const char* text) noexcept {
  try {
    if (!text || !text[0])
      return;
    wchar_t directory[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
    if (!n || n >= 32700)
      return;
    std::wstring path(directory);
    path += L"\\Taxi Cam";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\bridge.log";
    char record[1400];
    const auto length = std::snprintf(record, sizeof(record), "%llu %s\r\n", static_cast<unsigned long long>(GetTickCount64()), text);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(record))
      return;
    standalone::append_rotating_log(path, std::string_view(record, static_cast<std::size_t>(length)), standalone::BridgeLogBytes);
  } catch (...) {
  }
}

void publish_interest(unsigned role, const Interest& interest) noexcept {
  if (role >= kRoleCount)
    return;
  auto& item = slot(role);
  const auto meta = pack(interest);
  item.resource.store(interest.resource_present ? interest.resource : 0, std::memory_order_release);
  item.meta.store(meta, std::memory_order_release);
  const auto now = GetTickCount64();
  const auto previous = item.logged.load(std::memory_order_relaxed);
  const auto when = item.logged_at.load(std::memory_order_relaxed);
  if (previous == meta && now - when < 5000)
    return;
  item.logged.store(meta, std::memory_order_relaxed);
  item.logged_at.store(now, std::memory_order_relaxed);
  remember(role, interest);
}

Interest interest(unsigned role) noexcept {
  if (role >= kRoleCount)
    return {};
  const auto& item = slot(role);
  return unpack(item.meta.load(std::memory_order_acquire), item.resource.load(std::memory_order_acquire));
}

std::uint64_t interest_resource(unsigned role) noexcept {
  if (role >= kRoleCount)
    return 0;
  return slot(role).resource.load(std::memory_order_acquire);
}

bool interest_role(std::uint64_t resource, unsigned& role) noexcept {
  if (!resource)
    return false;
  for (unsigned index = 0; index < kRoleCount; ++index) {
    if (slot(index).resource.load(std::memory_order_acquire) == resource) {
      role = index;
      return true;
    }
  }
  return false;
}

}  // namespace taxi_camera::add_diffuse
