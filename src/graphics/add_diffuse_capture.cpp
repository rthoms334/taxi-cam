#include "add_diffuse_capture.hpp"

#include "../hooks/render_boundary_observer.hpp"
#include "add_diffuse_histogram.hpp"
#include "add_diffuse_interest.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace taxi_camera::add_diffuse {
namespace {
constexpr std::uint64_t kCaptureGapMs = 2000;
constexpr std::uint64_t kFrameGapMs = 10000;
constexpr std::uint64_t kMaximumBytes = 8ull * 1024 * 1024;

enum class Phase { idle, recorded, retiring, failed };

struct Packet {
  SRWLOCK lock = SRWLOCK_INIT;
  Phase phase = Phase::idle;
  unsigned role = 0;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  bool armed = false;
  ID3D12Device* device = nullptr;
  ID3D12Resource* source = nullptr;
  ID3D12Resource* readback = nullptr;
  ID3D12Fence* fence = nullptr;
  std::uint64_t signal_value = 0;
  UINT width = 0;
  UINT height = 0;
  UINT row_pitch = 0;
  std::uint64_t buffer_size = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::uint64_t last_attempt[kRoleCount]{};
  std::uint64_t last_frame = 0;
};

Packet& packet() noexcept {
  static Packet value;
  return value;
}

struct Exclusive {
  SRWLOCK& lock;
  explicit Exclusive(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
  ~Exclusive() { ReleaseSRWLockExclusive(&lock); }
};

bool supported(DXGI_FORMAT format) noexcept {
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R11G11B10_FLOAT;
}

void drop_source(Packet& item) noexcept {
  if (item.source) {
    item.source->Release();
    item.source = nullptr;
  }
  item.list = nullptr;
  item.armed = false;
  item.queue = nullptr;
}

bool due(const Packet& item, unsigned role, std::uint64_t now) noexcept {
  return interest_resource(role) != 0 && now - item.last_attempt[role] >= kCaptureGapMs;
}

bool selected_role(const Packet& item, unsigned role, std::uint64_t now) noexcept {
  if (!due(item, role, now))
    return false;
  unsigned best = role;
  auto oldest = item.last_attempt[role];
  for (unsigned index = 0; index < kRoleCount; ++index) {
    if (!due(item, index, now))
      continue;
    if (item.last_attempt[index] < oldest) {
      oldest = item.last_attempt[index];
      best = index;
    }
  }
  return best == role;
}

bool prepare_readback(Packet& item, ID3D12Device* device, std::uint64_t bytes) noexcept {
  if (item.readback && item.device == device && item.buffer_size == bytes && item.fence)
    return true;
  if (item.phase != Phase::idle)
    return false;
  if (item.readback) {
    item.readback->Release();
    item.readback = nullptr;
  }
  if (item.fence) {
    item.fence->Release();
    item.fence = nullptr;
  }
  if (item.device && item.device != device) {
    item.device->Release();
    item.device = nullptr;
  }
  if (!item.device) {
    device->AddRef();
    item.device = device;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&item.readback))))
    return false;
  if (FAILED(device->CreateFence(item.signal_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&item.fence)))) {
    item.readback->Release();
    item.readback = nullptr;
    return false;
  }
  item.buffer_size = bytes;
  return true;
}

void copy_region(ID3D12GraphicsCommandList* list,
                 ID3D12Resource* source,
                 ID3D12Resource* destination,
                 const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) noexcept {
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = source;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};
  to.pResource = destination;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

bool record(ID3D12GraphicsCommandList* list, std::uint64_t generation, ID3D12Resource* resource, bool enhanced) noexcept {
  unsigned role = 0;
  if (!list || !resource || !engine_hook::render_boundary::recording_allows_injection(list, generation) ||
      !interest_role(reinterpret_cast<std::uint64_t>(resource), role))
    return false;
  auto& item = packet();
  const Exclusive guard(item.lock);
  if (item.phase == Phase::failed || item.phase != Phase::idle)
    return false;
  const auto now = GetTickCount64();
  if (!selected_role(item, role, now))
    return false;
  item.last_attempt[role] = now;
  const auto desc = resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
      !supported(desc.Format) || desc.Width < 1 || desc.Width > 8192 || desc.Height < 1 || desc.Height > 8192 ||
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
    log_line("ADD_DIFFUSE capture skipped: unsupported live resource shape");
    return false;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || !device)
    return false;
  if (FAILED(device->GetDeviceRemovedReason())) {
    device->Release();
    return false;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  std::uint64_t bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  if (!bytes || bytes > kMaximumBytes ||
      footprint.Footprint.RowPitch < desc.Width * (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u)) {
    device->Release();
    log_line("ADD_DIFFUSE capture skipped: copy footprint out of bounds");
    return false;
  }
  if (!prepare_readback(item, device, bytes)) {
    device->Release();
    log_line("ADD_DIFFUSE capture skipped: readback allocation failed");
    return false;
  }
  device->Release();
  if (enhanced) {
    auto* extended = static_cast<ID3D12GraphicsCommandList7*>(list);
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    barrier.SyncAfter = D3D12_BARRIER_SYNC_COPY;
    barrier.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    barrier.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    barrier.pResource = resource;
    barrier.Subresources.NumMipLevels = barrier.Subresources.NumArraySlices = barrier.Subresources.NumPlanes = 1;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &barrier;
    extended->Barrier(1, &group);
    copy_region(list, resource, item.readback, footprint);
    barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
    barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    barrier.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    extended->Barrier(1, &group);
  } else {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    copy_region(list, resource, item.readback, footprint);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
  }
  resource->AddRef();
  item.source = resource;
  item.list = list;
  item.role = role;
  item.width = static_cast<UINT>(desc.Width);
  item.height = desc.Height;
  item.row_pitch = footprint.Footprint.RowPitch;
  item.format = desc.Format;
  item.phase = Phase::recorded;
  return true;
}

std::uint8_t tone(float value) noexcept {
  if (!std::isfinite(value) || value <= 0)
    return 0;
  const float mapped = value / (1.f + value);
  const auto byte = static_cast<int>(mapped * 255.f + 0.5f);
  return static_cast<std::uint8_t>(byte > 255 ? 255 : byte);
}

bool save_primary_frame(const void* pixels,
                        UINT width,
                        UINT height,
                        UINT row_pitch,
                        DXGI_FORMAT format,
                        wchar_t* path,
                        std::size_t path_count) noexcept {
  wchar_t directory[32768]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
  if (!n || n >= 32000 || !path || path_count < 64)
    return false;
  const auto directory_length = std::wcslen(directory);
  const wchar_t folder[] = L"\\Taxi Cam";
  const wchar_t tail[] = L"\\Taxi Cam\\add-diffuse-primary.bmp";
  if (directory_length + (sizeof(tail) / sizeof(wchar_t)) > path_count)
    return false;
  std::wmemcpy(path, directory, directory_length);
  std::wmemcpy(path + directory_length, folder, sizeof(folder) / sizeof(wchar_t));
  CreateDirectoryW(path, nullptr);
  std::wmemcpy(path + directory_length, tail, sizeof(tail) / sizeof(wchar_t));
  const auto row = (static_cast<std::size_t>(width) * 3u + 3u) & ~std::size_t{3};
  const auto image = row * height;
  if (image > kMaximumBytes)
    return false;
  auto* file_bytes = static_cast<std::uint8_t*>(std::malloc(54 + image));
  if (!file_bytes)
    return false;
  std::memset(file_bytes, 0, 54 + image);
  file_bytes[0] = 'B';
  file_bytes[1] = 'M';
  const auto total = static_cast<std::uint32_t>(54 + image);
  std::memcpy(file_bytes + 2, &total, 4);
  const std::uint32_t pixel_offset = 54;
  std::memcpy(file_bytes + 10, &pixel_offset, 4);
  const std::uint32_t header = 40;
  std::memcpy(file_bytes + 14, &header, 4);
  std::memcpy(file_bytes + 18, &width, 4);
  auto signed_height = static_cast<std::int32_t>(height);
  std::memcpy(file_bytes + 22, &signed_height, 4);
  const std::uint16_t planes = 1;
  const std::uint16_t bits = 24;
  std::memcpy(file_bytes + 26, &planes, 2);
  std::memcpy(file_bytes + 28, &bits, 2);
  const auto raw_size = static_cast<std::uint32_t>(image);
  std::memcpy(file_bytes + 34, &raw_size, 4);
  const auto* rows = static_cast<const std::uint8_t*>(pixels);
  for (UINT y = 0; y < height; ++y) {
    auto* destination = file_bytes + 54 + static_cast<std::size_t>(height - 1 - y) * row;
    const auto* source = rows + static_cast<std::size_t>(y) * row_pitch;
    for (UINT x = 0; x < width; ++x) {
      float red = 0, green = 0, blue = 0;
      if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t channels[4]{};
        std::memcpy(channels, source + static_cast<std::size_t>(x) * 8u, sizeof(channels));
        red = half_to_float(channels[0]);
        green = half_to_float(channels[1]);
        blue = half_to_float(channels[2]);
      } else {
        std::uint32_t packed = 0;
        std::memcpy(&packed, source + static_cast<std::size_t>(x) * 4u, sizeof(packed));
        red = tiny_float(packed & 0x7ffu, 6);
        green = tiny_float((packed >> 11) & 0x7ffu, 6);
        blue = tiny_float(packed >> 22, 5);
      }
      destination[static_cast<std::size_t>(x) * 3] = tone(blue);
      destination[static_cast<std::size_t>(x) * 3 + 1] = tone(green);
      destination[static_cast<std::size_t>(x) * 3 + 2] = tone(red);
    }
  }
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool wrote = false;
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    wrote = WriteFile(file, file_bytes, total, &written, nullptr) && written == total;
    CloseHandle(file);
  }
  std::free(file_bytes);
  return wrote;
}

void publish_histogram(unsigned role,
                       DXGI_FORMAT format,
                       UINT width,
                       UINT height,
                       const Histogram& histogram,
                       const wchar_t* frame) noexcept {
  char path[520] = "-";
  if (frame && frame[0])
    WideCharToMultiByte(CP_UTF8, 0, frame, -1, path, static_cast<int>(sizeof(path)), nullptr, nullptr);
  char line[1100];
  std::snprintf(line, sizeof(line),
                "ADD_DIFFUSE histogram view=%s format=%u %s size=%ux%u samples=%u nonzero=%u nonfinite=%u max=%g "
                "dark=%u dim=%u mid=%u bright=%u lamp=%u spike=%u frame=%s",
                role_name(role), static_cast<unsigned>(format), format_name(static_cast<std::uint32_t>(format)), width, height,
                histogram.samples, histogram.nonzero, histogram.nonfinite, static_cast<double>(histogram.max_luminance), histogram.dark,
                histogram.dim, histogram.mid, histogram.bright, histogram.lamp, histogram.spike, path);
  log_line(line);
}
}  // namespace

void note_legacy_exit(ID3D12GraphicsCommandList* list, std::uint64_t generation, ID3D12Resource* resource) noexcept {
  record(list, generation, resource, false);
}

void note_enhanced_exit(ID3D12GraphicsCommandList7* list, std::uint64_t generation, ID3D12Resource* resource) noexcept {
  record(list, generation, resource, true);
}

UINT diagnostic_targets(ID3D12Resource** targets, UINT capacity) noexcept {
  if (!targets || !capacity)
    return 0;
  UINT count = 0;
  for (unsigned role = 0; role < kRoleCount && count < capacity; ++role) {
    const auto handle = interest_resource(role);
    if (!handle)
      continue;
    auto* resource = reinterpret_cast<ID3D12Resource*>(handle);
    bool duplicate = false;
    for (UINT index = 0; index < count; ++index)
      duplicate = duplicate || targets[index] == resource;
    if (!duplicate)
      targets[count++] = resource;
  }
  return count;
}

bool arm_submission(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept {
  if (!queue || !count || !lists)
    return false;
  auto& item = packet();
  const Exclusive guard(item.lock);
  if (item.phase != Phase::recorded || !item.list)
    return false;
  for (UINT index = 0; index < count; ++index) {
    if (lists[index] == item.list) {
      item.armed = true;
      item.queue = queue;
      return true;
    }
  }
  return false;
}

void after_submission(ID3D12CommandQueue* queue) noexcept {
  auto& item = packet();
  const Exclusive guard(item.lock);
  if (!item.armed || item.phase != Phase::recorded || item.queue != queue || !item.fence || item.signal_value == UINT64_MAX)
    return;
  ++item.signal_value;
  if (FAILED(queue->Signal(item.fence, item.signal_value))) {
    item.phase = Phase::failed;
    item.armed = false;
    log_line("ADD_DIFFUSE capture quarantined: producer Signal failed");
    return;
  }
  item.phase = Phase::retiring;
  item.armed = false;
}

void submission_refused(ID3D12CommandQueue* queue) noexcept {
  auto& item = packet();
  const Exclusive guard(item.lock);
  if (!item.armed || item.queue != queue)
    return;
  item.phase = Phase::failed;
  item.armed = false;
  log_line("ADD_DIFFUSE capture quarantined: submission refused after recording");
}

void note_list_reset(ID3D12GraphicsCommandList* list) noexcept {
  auto& item = packet();
  const Exclusive guard(item.lock);
  if (item.phase == Phase::recorded && item.list == list) {
    drop_source(item);
    item.phase = Phase::idle;
  }
}

void poll() noexcept {
  auto& item = packet();
  Histogram histogram{};
  unsigned role = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT width = 0;
  UINT height = 0;
  UINT row_pitch = 0;
  void* copy = nullptr;
  std::uint64_t copy_bytes = 0;
  bool save = false;
  {
    const Exclusive guard(item.lock);
    if (item.phase != Phase::retiring || !item.fence || !item.readback)
      return;
    const auto completed = item.fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
      item.phase = Phase::failed;
      log_line("ADD_DIFFUSE capture quarantined: device removed");
      return;
    }
    if (completed < item.signal_value)
      return;
    void* mapped = nullptr;
    if (FAILED(item.readback->Map(0, nullptr, &mapped)) || !mapped) {
      item.phase = Phase::failed;
      log_line("ADD_DIFFUSE capture quarantined: readback map failed");
      return;
    }
    const bool decoded = item.format == DXGI_FORMAT_R16G16B16A16_FLOAT
                             ? histogram_rgba16f(mapped, item.width, item.height, item.row_pitch, histogram)
                             : histogram_r11g11b10(mapped, item.width, item.height, item.row_pitch, histogram);
    role = item.role;
    format = item.format;
    width = item.width;
    height = item.height;
    row_pitch = item.row_pitch;
    const auto now = GetTickCount64();
    save = decoded && role == kPrimaryRole && (item.last_frame == 0 || now - item.last_frame >= kFrameGapMs);
    if (save) {
      copy_bytes = item.buffer_size;
      copy = std::malloc(static_cast<std::size_t>(copy_bytes));
      if (!copy)
        save = false;
      else {
        std::memcpy(copy, mapped, static_cast<std::size_t>(copy_bytes));
        item.last_frame = now;
      }
    }
    item.readback->Unmap(0, nullptr);
    drop_source(item);
    item.phase = Phase::idle;
    if (!decoded)
      log_line("ADD_DIFFUSE histogram decode failed");
    else if (!save)
      publish_histogram(role, format, width, height, histogram, nullptr);
  }
  if (!save || !copy)
    return;
  wchar_t path[32768]{};
  const bool wrote = save_primary_frame(copy, width, height, row_pitch, format, path, 32768);
  std::free(copy);
  publish_histogram(role, format, width, height, histogram, wrote ? path : L"write_failed");
}

}  // namespace taxi_camera::add_diffuse
