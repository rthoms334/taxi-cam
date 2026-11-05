#include "../../src/bridge/d3d12_bridge.hpp"
#include "../support/graphics_fixture.hpp"

namespace {
using namespace taxi_camera;
using namespace taxi_camera::testing;
namespace win = taxi_camera::standalone;
namespace runtime = taxi_camera::scene_runtime;
struct Recording {
  Reference<ID3D12CommandAllocator> allocator;
  Reference<ID3D12GraphicsCommandList> list;
  void create(ID3D12Device* device) {
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "Application allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())),
          "Application list");
  }
  void reset() {
    check(allocator->Reset(), "Drained allocator reset");
    check(list->Reset(allocator.get(), nullptr), "Observed application Reset");
  }
  void close() { check(list->Close(), "Application Close"); }
};
bool matches_pixel(const unsigned char* pixel, std::array<unsigned char, 4> expected) {
  for (unsigned channel = 0; channel < 4; ++channel)
    if (std::abs(int(pixel[channel]) - int(expected[channel])) > 1)
      return false;
  return true;
}
void run(bool warp, unsigned fbw_count, bool common, bool mixed_exit, bool first_list) {
  const auto& profile = fbw_count ? profiles::A380 : profiles::IniA380;
  const unsigned count = fbw_count ? fbw_count : 8;
  const auto display_state = common ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "Factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "WARP adapter");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    check(device->QueryInterface(IID_PPV_ARGS(messages.put())), "Debug messages");
  GradientGenerator generator(device.get());
  Reference<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC qd{};
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Application queue");
  Reference<ID3D12QueryHeap> timestamps;
  Reference<ID3D12Resource> marker_upload, marker_readback;
  constexpr UINT64 Marker = 0x5145525950524546ull;
  if (mixed_exit) {
    const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, 0};
    check(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(timestamps.put())), "Earlier native timestamp heap");
    auto buffer = texture_description(1, 1, DXGI_FORMAT_UNKNOWN);
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(Marker);
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD), readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(marker_upload.put())),
          "Earlier buffer copy source");
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(marker_readback.put())),
          "Earlier buffer copy destination");
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    check(marker_upload->Map(0, &none, &mapped), "Upload non-camera ordering marker");
    std::memcpy(mapped, &Marker, sizeof(Marker));
    const D3D12_RANGE written{0, sizeof(Marker)};
    marker_upload->Unmap(0, &written);
  }
  // Every display, descriptor, and application recording object predates hook
  // installation. RT entry and actual draw use separate lists. The realistic
  // mixed case has earlier native timestamp/buffer work, then all RT exits,
  // then pixel consumers. The earlier commands cannot access the RT texture;
  // rejecting every prior GPU command would recreate the live attachment bug.
  std::array<Recording, 4> recordings;
  for (auto& recording : recordings)
    recording.create(device.get());
  Reference<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Pre-hook RTV heap");
  std::array<Reference<ID3D12Resource>, 8> targets, readbacks;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvs{};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 8> footprints{};
  std::array<UINT64, 8> bytes{};
  auto description = texture_description(768, 1024, fbw_count ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_TYPELESS);
  description.MipLevels = static_cast<UINT16>(profile.mips);
  for (unsigned i = 0; i < count; ++i) {
    create_texture(device.get(), description, targets[i].put());
    rtvs[i] = {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{i} * device->GetDescriptorHandleIncrementSize(hd.Type)};
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(targets[i].get(), &view, rtvs[i]);
    generator.record(recordings[1].list.get(), rtvs[i], 768, 1024, false, 0, 0);
    transition(recordings[1].list.get(), targets[i].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, display_state);
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprints[i], nullptr, nullptr, &bytes[i]);
    auto buffer = texture_description(1, 1, DXGI_FORMAT_UNKNOWN);
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes[i];
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(readbacks[i].put())),
          "Application pixel consumer");
  }
  for (auto& recording : recordings)
    recording.close();
  ID3D12CommandList* pre_hook[]{recordings[1].list.get()};
  queue->ExecuteCommandLists(1, pre_hook);
  require(drain_copy_queue(queue.get(), device.get()), "Pre-hook rendering completed");
  require(win::initialize_graphics(device.get()), win::graphics_status().error);
  win::set_aircraft_profile(profile.id);
  require(win::pfd_inventory().empty(), "Displays and descriptors genuinely predate hooks");
  win::set_graphics_observation_demand(true);
  const auto key = win::graphics_status().device;
  std::array<bool, 8> selected{};
  // partial: the selected displays repaint only this rectangle, in this
  // colour, like an instrument that redraws what changed. views: the selected
  // displays draw through these instead of their pre-hook views.
  const auto record_frame = [&](bool discover_all, bool overwrite, const D3D12_RECT* partial = nullptr, const float* colour = nullptr,
                                const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8>* views = nullptr) {
    for (auto& recording : recordings)
      recording.reset();
    if (mixed_exit) {
      recordings[3].list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
      recordings[3].list->CopyBufferRegion(marker_readback.get(), 0, marker_upload.get(), 0, sizeof(Marker));
    }
    const unsigned active = fbw_count == 3 && !discover_all ? 2 : count;
    for (unsigned i = 0; i < active; ++i) {
      transition(recordings[0].list.get(), targets[i].get(), display_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      if (partial && selected[i])
        recordings[1].list->ClearRenderTargetView(rtvs[i], colour, 1, partial);
      else
        generator.record(recordings[1].list.get(), views && selected[i] ? (*views)[i] : rtvs[i], 768, 1024, false, 0, 0);
      transition(recordings[mixed_exit ? 3 : 2].list.get(), targets[i].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, display_state);
    }
    for (unsigned i = 0; i < count; ++i) {
      auto* consumer = recordings[3].list.get();
      if (overwrite && selected[i]) {
        transition(consumer, targets[i].get(), display_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float colour[]{.7f, .1f, .9f, 1};
        consumer->ClearRenderTargetView(rtvs[i], colour, 0, nullptr);
        transition(consumer, targets[i].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, display_state);
      }
      transition(consumer, targets[i].get(), display_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION source{targets[i].get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
      D3D12_TEXTURE_COPY_LOCATION destination{readbacks[i].get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
      destination.PlacedFootprint = footprints[i];
      consumer->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      transition(consumer, targets[i].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, display_state);
    }
    for (auto& recording : recordings)
      recording.close();
  };
  const auto execute = [&] {
    ID3D12CommandList* batch[]{recordings[0].list.get(), recordings[1].list.get(), recordings[2].list.get(), recordings[3].list.get()};
    // The consumer copies pixels inside THIS original batch. A later private
    // submission cannot change the readback and therefore cannot pass the test.
    if (first_list) {
      // The application deliberately submits its producer separately. The
      // next original Execute begins with the mixed exit/consumer at index 0,
      // so no preceding list in that call can host the private patch.
      queue->ExecuteCommandLists(2, batch);
      queue->ExecuteCommandLists(1, batch + 3);
    } else
      queue->ExecuteCommandLists(4, batch);
    require(drain_copy_queue(queue.get(), device.get()), "Original batch pixel consumer completed");
    check(device->GetDeviceRemovedReason(), "GPU health");
    if (mixed_exit) {
      void* mapped = nullptr;
      const D3D12_RANGE range{0, sizeof(Marker)};
      check(marker_readback->Map(0, &range, &mapped), "Read earlier native buffer work");
      UINT64 actual{};
      std::memcpy(&actual, mapped, sizeof(actual));
      const D3D12_RANGE none{0, 0};
      marker_readback->Unmap(0, &none);
      require(actual == Marker, "Earlier unrelated buffer copy survives native forwarding and patch insertion");
    }
  };
  record_frame(true, false);
  execute();
  const auto start = GetTickCount64();
  for (unsigned window = 0; window < 5; ++window) {
    record_frame(false, false);
    execute();
    win::discover_pfds(start + UINT64{window} * 1000);
  }
  auto inventory = win::pfd_inventory();
  require(inventory.size() == count, "Complete pre-hook display inventory recovered through barriers");
  std::sort(inventory.begin(), inventory.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  const std::array<std::uint64_t, taxi_camera::MaxDisplaySides> expected =
      fbw_count ? std::array{inventory[1].id, inventory[0].id, std::uint64_t{0}} : std::array{inventory[7].id, inventory[5].id, std::uint64_t{0}};
  // Displays are routed by the names their panel marks give them.
  win::set_named_targets(expected);
  win::discover_pfds(start + 5000);
  const auto ids = win::target_ids();
  require(ids == expected, "Named side mapping works without manual assignment");
  for (unsigned i = 0; i < count; ++i) {
    selected[i] = inventory[i].id == ids[0] || inventory[i].id == ids[1];
    require(inventory[i].draws == 0, "Opaque pre-hook RTVs remain unassociated; split submission proves attachment");
  }
  require(inventory[0].submission_activity > 0, "Submitted exits count separately from unavailable native draws");
  std::uint64_t checked = 0;
  // Display snapshot with no camera or calibration patch ready. An unrouted
  // display is read at a proven site in the original batch; the image is the
  // native gradient, with the alpha byte dropped.
  {
    using Result = win::DisplaySnapshotResult;
    require(win::request_display_snapshot(UINT64_MAX, GetTickCount64()) == Result::unavailable, "Unknown snapshot texture refused");
    require(win::poll_display_snapshot(GetTickCount64()).result == Result::none, "Refused snapshot leaves nothing pending");
    const auto unrouted = std::find_if(inventory.begin(), inventory.end(), [&](const auto& c) { return c.id != ids[0] && c.id != ids[1]; });
    runtime::service();  // The bridge's control tick prepares the copy packets outside submission.
    if (fbw_count == 3) {
      // The third FBW display is only sampled, never drawn, in these frames:
      // with no proven RT exit there is no copy site, so nothing is planned.
      require(unrouted != inventory.end() && win::request_display_snapshot(unrouted->id, GetTickCount64()) == Result::pending,
              "Idle snapshot request accepted");
      const auto idle_plans = win::graphics_status().queue_patch_plans;
      record_frame(false, false);
      execute();
      runtime::service();
      require(win::graphics_status().queue_patch_plans == idle_plans &&
                  win::poll_display_snapshot(GetTickCount64()).result == Result::pending,
              "A display without a proven exit was snapshotted");
    }
    const auto snapshot_id = unrouted != inventory.end() && fbw_count != 3 ? unrouted->id : inventory[0].id;
    require(win::request_display_snapshot(snapshot_id, GetTickCount64()) == Result::pending, "Snapshot request accepted");
    require(win::poll_display_snapshot(GetTickCount64()).result == Result::pending, "Snapshot waits for a copy site");
    const auto plans = win::graphics_status().queue_patch_plans;
    record_frame(false, false);
    execute();
    require(win::graphics_status().queue_patch_plans > plans, "Snapshot-only batch was planned");
    runtime::service();
    auto poll = win::poll_display_snapshot(GetTickCount64());
    if (poll.result != Result::ready)
      std::fprintf(stderr, "Snapshot poll: result=%s id=%llu/%llu %ux%u format=%u copies=%llu\n", win::display_snapshot_name(poll.result),
                   static_cast<unsigned long long>(poll.id), static_cast<unsigned long long>(snapshot_id), poll.width, poll.height,
                   poll.format, static_cast<unsigned long long>(runtime::snapshot(key).capture.display_copies));
    require(poll.result == Result::ready && poll.id == snapshot_id && poll.width == 768 && poll.height == 1024 &&
                poll.format == static_cast<unsigned>(description.Format),
            "Snapshot completed after its covering fence");
    require(poll.image.width == 768 && poll.image.height == 1024, "Snapshot within the edge limit keeps its size");
    bool correct = true;
    for (const auto [x, y] : {std::pair{0u, 0u}, std::pair{767u, 1023u}, std::pair{384u, 512u}, std::pair{100u, 900u}}) {
      const auto* bgr = poll.image.bgr.data() + (std::size_t{y} * 768 + x) * 3;
      correct &= matches_pixel(std::array<unsigned char, 4>{bgr[2], bgr[1], bgr[0], 64}.data(),
                               {static_cast<unsigned char>(std::lround(255.f * x / 767)),
                                static_cast<unsigned char>(std::lround(255.f * y / 1023)), 51, 64});
      ++checked;
    }
    require(correct, "Snapshot holds the display's native pixels");
    require(win::poll_display_snapshot(GetTickCount64()).result == Result::none, "Snapshot result is returned once");
  }
  // Full lower-trim and unselected-display oracle; selected upper display has
  // palette checks for calibration and fixed interior samples for camera.
  const auto pixels = [&](bool calibration, bool overwrite) {
    for (unsigned i = 0; i < count; ++i) {
      void* mapped = nullptr;
      const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes[i])};
      check(readbacks[i]->Map(0, &range, &mapped), "Read original-batch consumer pixels");
      bool correct = true;
      for (unsigned y = 0; y < 1024; ++y)
        for (unsigned x = 0; x < 768; ++x) {
          const auto* pixel = static_cast<unsigned char*>(mapped) + SIZE_T{footprints[i].Footprint.RowPitch} * y + x * 4;
          if (overwrite && selected[i])
            correct &= matches_pixel(pixel, {179, 26, 230, 255});
          else if (!selected[i] || y >= 763)
            correct &= matches_pixel(pixel, {static_cast<unsigned char>(std::lround(255.f * x / 767)),
                                             static_cast<unsigned char>(std::lround(255.f * y / 1023)), 51, 64});
          else if (calibration)
            correct &= y == 254  ? matches_pixel(pixel, {255, 255, 255, 255})
                       : y < 254 ? matches_pixel(pixel, {5, 41, 97, 255}) || matches_pixel(pixel, {0, 230, 255, 255})
                                 : matches_pixel(pixel, {31, 82, 8, 255}) || matches_pixel(pixel, {255, 204, 0, 255});
          else if (x >= 380 && x < 388 && (y == 100 || y == 500))
            correct &= matches_pixel(
                pixel, y == 100 ? std::array<unsigned char, 4>{51, 102, 153, 255} : std::array<unsigned char, 4>{153, 51, 102, 255});
          else
            continue;
          ++checked;
        }
      const D3D12_RANGE none{0, 0};
      readbacks[i]->Unmap(0, &none);
      require(correct, "Original batch contains requested patch while preserving native ordering and untouched pixels");
    }
  };
  win::set_calibration(3, 600);
  win::service_display_patches();
  runtime::service();
  record_frame(false, false);
  const auto plans = win::graphics_status().queue_patch_plans;
  execute();
  require(win::graphics_status().queue_patch_plans > plans, "Production queue path planned split-list patches");
  pixels(true, false);
  runtime::service();
  execute();  // Replay exact closed application recordings after pool retirement.
  pixels(true, false);
  runtime::service();
  record_frame(false, true);
  execute();
  pixels(true, true);

  win::set_calibration(0, 600);
  win::set_target_mask(3);
  require(runtime::prepare(key) && runtime::set_display_exposure(key, 0), "Prepare actual camera composition");
  runtime::set_composition(key, profile.composition);
  runtime::manager().begin_source_tracking();
  runtime::manager().set_source_rate(60);
  Reference<ID3D12DescriptorHeap> camera_heap;
  const D3D12_DESCRIPTOR_HEAP_DESC camera_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&camera_hd, IID_PPV_ARGS(camera_heap.put())), "Camera RTVs");
  std::array<Reference<ID3D12Resource>, 2> sources;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> camera_rtvs{};
  Recording camera;
  camera.create(device.get());
  camera.close();
  camera.reset();
  for (unsigned feed = 0; feed < 2; ++feed) {
    const auto pane = profile.camera_panes[feed];
    create_texture(device.get(), texture_description(pane[0], pane[1], DXGI_FORMAT_R8G8B8A8_UNORM), sources[feed].put());
    const D3D12_CPU_DESCRIPTOR_HANDLE view{camera_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                                           SIZE_T{feed} * device->GetDescriptorHandleIncrementSize(camera_hd.Type)};
    device->CreateRenderTargetView(sources[feed].get(), nullptr, view);
    camera_rtvs[feed] = view;
  }
  auto& handoff = scene_handoff();
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {551, 1}, {751, 752},
                          {reinterpret_cast<UINT64>(sources[0].get()), reinterpret_cast<UINT64>(sources[1].get())}),
          "Publish real camera source identities");
  for (unsigned feed = 0; feed < 2; ++feed) {
    const auto colour = feed ? std::array<float, 4>{.6f, .2f, .4f, 1} : std::array<float, 4>{.2f, .4f, .6f, 1};
    generator.record(camera.list.get(), camera_rtvs[feed], profile.camera_panes[feed][0], profile.camera_panes[feed][1], false, 0, feed);
    camera.list->ClearRenderTargetView(camera_rtvs[feed], colour.data(), 0, nullptr);
    transition(camera.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  camera.close();
  ID3D12CommandList* camera_batch[]{camera.list.get()};
  queue->ExecuteCommandLists(1, camera_batch);
  require(drain_copy_queue(queue.get(), device.get()), "Camera source capture completed");
  camera.reset();
  camera.close();
  win::service_display_patches();
  runtime::service();
  if (!runtime::snapshot(key).output)
    std::fprintf(stderr, "Camera service: %s; frames=%llu captures=%llu sources=%llu boundary=%llu/%llu refusal=%s/%s bridge=%s\n",
                 runtime::snapshot(key).message, static_cast<unsigned long long>(runtime::snapshot(key).frames),
                 static_cast<unsigned long long>(runtime::snapshot(key).capture.captures),
                 static_cast<unsigned long long>(runtime::snapshot(key).capture.source_candidates),
                 static_cast<unsigned long long>(runtime::snapshot(key).capture.render_targets[0].matched_boundaries),
                 static_cast<unsigned long long>(runtime::snapshot(key).capture.render_targets[1].matched_boundaries),
                 runtime::snapshot(key).capture.render_targets[0].last_refusal,
                 runtime::snapshot(key).capture.render_targets[1].last_refusal, win::graphics_status().error);
  require(runtime::snapshot(key).frames && runtime::snapshot(key).output, "Real camera pair composed");
  record_frame(false, false);
  execute();
  pixels(false, false);
  runtime::service();
  execute();
  pixels(false, false);
  // Held Close-time stamps. Through views created after Connect the bridge
  // sees the instrument draw the selected displays, so Close could stamp the
  // camera inside the instrument's own list, where nothing can be saved first.
  // These sides run on queue copies: Close holds its stamp, and the queue copy
  // still shows the camera to the original consumer.
  Reference<ID3D12DescriptorHeap> late_heap;
  const D3D12_DESCRIPTOR_HEAP_DESC late_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&late_hd, IID_PPV_ARGS(late_heap.put())), "RTV heap after Connect");
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> late_rtvs{};
  for (unsigned i = 0; i < count; ++i) {
    late_rtvs[i] = {late_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                    SIZE_T{i} * device->GetDescriptorHandleIncrementSize(late_hd.Type)};
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(targets[i].get(), &view, late_rtvs[i]);
  }
  const auto held_before = win::graphics_status();
  record_frame(false, false, nullptr, nullptr, &late_rtvs);
  execute();
  pixels(false, false);
  const auto held = win::graphics_status();
  if (held.shadow_held <= held_before.shadow_held)
    std::fprintf(
        stderr, "Held stamps: held=%llu->%llu end_draws=%llu->%llu copies=%llu->%llu\n",
        static_cast<unsigned long long>(held_before.shadow_held), static_cast<unsigned long long>(held.shadow_held),
        static_cast<unsigned long long>(held_before.recording_end_draws), static_cast<unsigned long long>(held.recording_end_draws),
        static_cast<unsigned long long>(held_before.preferred_copy_stamps), static_cast<unsigned long long>(held.preferred_copy_stamps));
  require(held.shadow_held > held_before.shadow_held && held.recording_end_draws == held_before.recording_end_draws &&
              held.preferred_copy_stamps == held_before.preferred_copy_stamps,
          "Close held its stamp on queue-copy sides, and the queue copy still showed the camera");
  // Display shadow. Under the camera the instrument repaints one rectangle
  // only; the shadow goes back under that repaint and is saved again before
  // the camera copy. When the camera stops, the shadow goes back before the
  // instrument's next partial repaint: the display shows the instrument with
  // both repaints and no camera pixel. A COMMON display has no state to copy
  // in before the instrument's first write, so both repaints land on the
  // camera and the shadow they miss comes back over them.
  const D3D12_RECT under{40, 40, 80, 80}, after{600, 600, 640, 640};
  const float under_colour[]{.1f, .8f, .3f, 1}, after_colour[]{.9f, .5f, .2f, 1};
  record_frame(false, false, &under, under_colour);
  execute();
  pixels(false, false);
  runtime::service();
  const auto shadow = win::graphics_status();
  if (shadow.shadow_modes[0] != 'q' || shadow.shadow_modes[1] != 'q' || shadow.shadow_fallbacks)
    std::fprintf(stderr, "Shadow: modes=%s saves=%llu entry_restores=%llu lost_writes=%llu held=%llu fallbacks=%llu\n", shadow.shadow_modes,
                 static_cast<unsigned long long>(shadow.shadow_saves), static_cast<unsigned long long>(shadow.shadow_entry_restores),
                 static_cast<unsigned long long>(shadow.shadow_lost_writes), static_cast<unsigned long long>(shadow.shadow_held),
                 static_cast<unsigned long long>(shadow.shadow_fallbacks));
  require(shadow.shadow_saves && shadow.shadow_modes[0] == 'q' && shadow.shadow_modes[1] == 'q' && !shadow.shadow_fallbacks,
          "Camera sides ran on queue copies and saved their shadows");
  require(common || shadow.shadow_entry_restores, "The shadow went back under the instrument's writes");
  std::uint64_t lost_by_total = 0;
  for (const auto lost : shadow.shadow_lost_by)
    lost_by_total += lost;
  if (lost_by_total != shadow.shadow_lost_writes || (common && !shadow.shadow_lost_writes))
    for (unsigned i = 0; i < shadow.shadow_lost_by.size(); ++i)
      std::fprintf(stderr, "Shadow lost_by %s=%llu of %llu\n", win::shadow_lost_write_name(static_cast<win::ShadowLostWrite>(i)),
                   static_cast<unsigned long long>(shadow.shadow_lost_by[i]), static_cast<unsigned long long>(shadow.shadow_lost_writes));
  require(lost_by_total == shadow.shadow_lost_writes, "Every missed instrument write has one cause");
  require(!common || (shadow.shadow_lost_writes &&
                      shadow.shadow_lost_by[static_cast<unsigned>(win::ShadowLostWrite::not_insertable)] == shadow.shadow_lost_writes),
          "Writes to a display found in COMMON are missed because nothing can go in before them");
  win::set_target_mask(0);
  win::service_display_patches();
  record_frame(false, false, &after, after_colour);
  execute();
  require(win::graphics_status().shadow_restores >= 2, "Both sides restored their shadows when the camera stopped");
  // The selected displays as the original consumer read them: the native
  // gradient, with the instrument's repaints of `under` and `after` where
  // those were kept, and no camera pixel anywhere.
  const auto restored = [&](bool under_kept, bool after_kept, const char* message) {
    for (unsigned i = 0; i < count; ++i) {
      if (!selected[i])
        continue;
      void* mapped = nullptr;
      const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes[i])};
      check(readbacks[i]->Map(0, &range, &mapped), "Read restored display");
      bool correct = true;
      for (unsigned y = 0; y < 1024; ++y)
        for (unsigned x = 0; x < 768; ++x) {
          const auto* pixel = static_cast<unsigned char*>(mapped) + SIZE_T{footprints[i].Footprint.RowPitch} * y + x * 4;
          const auto inside = [&](const D3D12_RECT& rect) {
            return LONG(x) >= rect.left && LONG(x) < rect.right && LONG(y) >= rect.top && LONG(y) < rect.bottom;
          };
          if (under_kept && inside(under))
            correct &= matches_pixel(pixel, {26, 204, 77, 255});
          else if (after_kept && inside(after))
            correct &= matches_pixel(pixel, {230, 128, 51, 255});
          else
            correct &= matches_pixel(pixel, {static_cast<unsigned char>(std::lround(255.f * x / 767)),
                                             static_cast<unsigned char>(std::lround(255.f * y / 1023)), 51, 64});
          ++checked;
        }
      const D3D12_RANGE none{0, 0};
      readbacks[i]->Unmap(0, &none);
      require(correct, message);
    }
  };
  restored(!common, !common, "The display came back as the instrument left it, without the camera");
  // A camera period whose first save never completes puts nothing back: the
  // pool is not serviced before the camera stops, and a second later the
  // shadow is forgotten with the camera possibly still on the display. The
  // next period there saves the area as it is (a baseline) and restores it
  // again when it stops, instead of never saving on that display again.
  runtime::service();
  win::service_display_patches();  // The completed restore ends that shadow.
  const auto before_dirty = win::graphics_status();
  win::set_target_mask(3);
  win::service_display_patches();
  runtime::service();  // Publishes this generation's camera patches.
  record_frame(false, false);
  execute();
  win::set_target_mask(0);
  win::service_display_patches();
  Sleep(1100);
  record_frame(false, false);
  execute();
  const auto forgotten = win::graphics_status();
  if (forgotten.shadow_restores != before_dirty.shadow_restores || forgotten.shadow_modes[0] != '-')
    std::fprintf(stderr, "Forgotten shadow: modes=%s saves=%llu->%llu restores=%llu->%llu baselines=%llu\n", forgotten.shadow_modes,
                 static_cast<unsigned long long>(before_dirty.shadow_saves), static_cast<unsigned long long>(forgotten.shadow_saves),
                 static_cast<unsigned long long>(before_dirty.shadow_restores), static_cast<unsigned long long>(forgotten.shadow_restores),
                 static_cast<unsigned long long>(forgotten.shadow_baselines));
  require(forgotten.shadow_saves > before_dirty.shadow_saves && forgotten.shadow_restores == before_dirty.shadow_restores &&
              forgotten.shadow_modes[0] == '-' && forgotten.shadow_modes[1] == '-',
          "A shadow whose first save never completed was put back");
  win::set_target_mask(3);
  win::service_display_patches();
  runtime::service();
  record_frame(false, false);
  execute();
  pixels(false, false);
  runtime::service();  // The baseline save completes.
  const auto baseline = win::graphics_status();
  require(baseline.shadow_baselines >= forgotten.shadow_baselines + 2 && baseline.shadow_modes[0] == 'q' && baseline.shadow_modes[1] == 'q',
          "A camera on a display that may hold the camera took a baseline");
  win::set_target_mask(0);
  win::service_display_patches();
  record_frame(false, false, &after, after_colour);
  execute();
  require(win::graphics_status().shadow_restores >= baseline.shadow_restores + 2, "The baseline went back when the camera stopped");
  restored(false, !common, "The baseline came back with the instrument's repaint, without the camera");
  // A dropped first save proves nothing about the display: a packet
  // quarantined after it was forwarded may have run. Another queue's
  // unfinished receipt (a snapshot copy held behind a fence) makes the next
  // display batch escape like issue 97, so the first saves of a clean camera
  // period are planned and dropped. The save after them must be a baseline,
  // not a clean save, and the restore must still bring back no camera pixel.
  runtime::service();
  win::service_display_patches();  // The completed restore ends that shadow.
  Reference<ID3D12CommandQueue> other_queue;
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(other_queue.put())), "Second application queue");
  Reference<ID3D12Fence> queue_hold;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(queue_hold.put())), "Second queue blocker");
  Recording exit_list;
  exit_list.create(device.get());
  exit_list.close();
  exit_list.reset();
  const auto first_selected = static_cast<unsigned>(std::find(selected.begin(), selected.end(), true) - selected.begin());
  transition(exit_list.list.get(), targets[first_selected].get(), display_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  transition(exit_list.list.get(), targets[first_selected].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, display_state);
  exit_list.close();
  require(win::request_display_snapshot(inventory[first_selected].id, GetTickCount64()) == win::DisplaySnapshotResult::pending,
          "Snapshot for the second queue accepted");
  check(other_queue->Wait(queue_hold.get(), 1), "Hold the second queue");
  ID3D12CommandList* held_batch[]{exit_list.list.get()};
  other_queue->ExecuteCommandLists(1, held_batch);
  win::set_target_mask(3);
  win::service_display_patches();
  runtime::service();
  const auto escapes = runtime::snapshot(key).capture.cross_queue_escapes;
  const auto before_drop = win::graphics_status();
  record_frame(false, false);
  execute();
  require(runtime::snapshot(key).capture.cross_queue_escapes > escapes && win::graphics_status().shadow_saves > before_drop.shadow_saves,
          "The first saves were planned in a batch that escaped");
  check(queue_hold->Signal(1), "Release the second queue");
  require(drain_copy_queue(other_queue.get(), device.get()), "Second queue completed");
  runtime::service();
  record_frame(false, false);
  execute();
  const auto after_drop = win::graphics_status();
  {
    const auto escaped = static_cast<unsigned>(taxi_camera::pfd_submission::DropSource::escaped);
    if (after_drop.shadow_drop_sources[escaped] < before_drop.shadow_drop_sources[escaped] + 2)
      for (unsigned i = 0; i < after_drop.shadow_drop_sources.size(); ++i)
        std::fprintf(stderr, "Shadow drops %s=%llu->%llu\n",
                     taxi_camera::pfd_submission::drop_source_name(static_cast<taxi_camera::pfd_submission::DropSource>(i)),
                     static_cast<unsigned long long>(before_drop.shadow_drop_sources[i]),
                     static_cast<unsigned long long>(after_drop.shadow_drop_sources[i]));
    require(after_drop.shadow_drop_sources[escaped] >= before_drop.shadow_drop_sources[escaped] + 2,
            "The first saves of an escaped batch were counted as escaped");
  }
  require(after_drop.shadow_baselines >= before_drop.shadow_baselines + 2 && after_drop.shadow_modes[0] == 'q' &&
              after_drop.shadow_modes[1] == 'q',
          "After a dropped first save the next save was not a baseline");
  pixels(false, false);
  runtime::service();
  win::poll_display_snapshot(GetTickCount64());
  win::set_target_mask(0);
  win::service_display_patches();
  record_frame(false, false, &after, after_colour);
  execute();
  require(win::graphics_status().shadow_restores >= after_drop.shadow_restores + 2, "The shadow went back after a dropped first save");
  restored(false, !common, "After a dropped first save the display came back without the camera");
  runtime::service();
  win::service_display_patches();
  // A camera period whose first copy runs on another queue: an Execute there
  // that leaves the display alone carries the camera over the instrument's
  // last frame, and its save is on that queue. The instrument's writes on the
  // application queue must still get the shadow back under them (it is taken
  // again there), and the stop must restore it there, not wait for the other
  // queue. A copy is carried only over a state a display list's exit left: a
  // COMMON display, or the separate placement's last list (the consumer's
  // copy round trip), leaves none.
  if (!common && mixed_exit) {
    Recording idle_list;
    idle_list.create(device.get());
    idle_list.close();
    idle_list.reset();
    idle_list.close();
    record_frame(false, false);  // The instrument's frame the camera covers first.
    execute();
    win::set_target_mask(3);
    win::service_display_patches();
    runtime::service();
    const auto before_other = win::graphics_status();
    ID3D12CommandList* idle_batch[]{idle_list.list.get()};
    other_queue->ExecuteCommandLists(1, idle_batch);
    require(drain_copy_queue(other_queue.get(), device.get()), "Second queue camera copy completed");
    require(win::graphics_status().shadow_saves >= before_other.shadow_saves + 2, "The camera's first copies saved on the second queue");
    runtime::service();
    record_frame(false, false);
    execute();
    pixels(false, false);
    runtime::service();
    record_frame(false, false, &under, under_colour);
    execute();
    runtime::service();
    const auto rebound = win::graphics_status();
    if (rebound.shadow_entry_restores < before_other.shadow_entry_restores + 2)
      std::fprintf(stderr, "Other-queue shadow: saves=%llu->%llu entry_restores=%llu->%llu lost_writes=%llu->%llu\n",
                   static_cast<unsigned long long>(before_other.shadow_saves), static_cast<unsigned long long>(rebound.shadow_saves),
                   static_cast<unsigned long long>(before_other.shadow_entry_restores),
                   static_cast<unsigned long long>(rebound.shadow_entry_restores),
                   static_cast<unsigned long long>(before_other.shadow_lost_writes),
                   static_cast<unsigned long long>(rebound.shadow_lost_writes));
    require(rebound.shadow_entry_restores >= before_other.shadow_entry_restores + 2,
            "A shadow saved on another queue went back under the application queue's writes");
    win::set_target_mask(0);
    win::service_display_patches();
    record_frame(false, false, &after, after_colour);
    execute();
    require(win::graphics_status().shadow_restores >= rebound.shadow_restores + 2,
            "The camera's stop restored the shadow on the application queue");
    restored(true, true, "After a first copy on another queue the display came back without the camera");
    runtime::service();
    win::service_display_patches();
  }
  handoff.stop_scene();
  runtime::reset_feed(key);
  UINT64 errors = 0;
  if (messages.get())
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0;
      check(messages->GetMessage(i, nullptr, &size), "Debug size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(messages->GetMessage(i, message, &size), "Debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error %u: %s\n", message->ID, message->pDescription);
      }
    }
  require(!errors, "No D3D12 validation error");
  std::printf(
      "PASS split submission %s profile=%u candidates=%u exit=%s placement=%s: automatic mapping, calibration/camera in original batch, "
      "replay, "
      "later overwrite, %llu pixel checks; debug=%d errors=%llu.\n",
      warp ? "WARP" : "hardware", profile.id, count, common ? "COMMON" : "SRV",
      first_list   ? "before-first"
      : mixed_exit ? "before-mixed"
                   : "after-barriers",
      static_cast<unsigned long long>(checked), debug_enabled, static_cast<unsigned long long>(errors));
}
}  // namespace
int main(int argc, char** argv) {
  try {
    bool warp = false, common = false, mixed_exit = false, first_list = false;
    unsigned fbw_count = 0;
    for (int i = 1; i < argc; ++i)
      if (std::strcmp(argv[i], "--warp") == 0)
        warp = true;
      else if (std::strcmp(argv[i], "--common") == 0)
        common = true;
      else if (std::strcmp(argv[i], "--mixed-exit") == 0)
        mixed_exit = true;
      else if (std::strcmp(argv[i], "--first-list") == 0)
        mixed_exit = first_list = true;
      else if (std::strcmp(argv[i], "--fbw") == 0)
        fbw_count = 2;
      else if (std::strcmp(argv[i], "--fbw-three") == 0)
        fbw_count = 3;
      else
        return 2;
    run(warp, fbw_count, common, mixed_exit, first_list);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL split submission: %s\n", error.what());
    return 1;
  }
}
