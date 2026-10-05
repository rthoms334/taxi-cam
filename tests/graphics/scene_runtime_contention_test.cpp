#include "../support/graphics_fixture.hpp"
// Exercise service itself and inspect its retained leases. Link the ordinary
// graphics objects except scene_runtime.cpp, which is compiled here.
#include "../../src/graphics/scene_runtime.cpp"

#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace taxi_camera;
using namespace taxi_camera::testing;
namespace runtime = taxi_camera::scene_runtime;
using Output = SceneFrameOutput;

struct DeathProbe final : IUnknown {
  explicit DeathProbe(std::atomic<unsigned>& count) : count_(count) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    *result = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *result = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --refs_;
    if (!remaining) {
      ++count_;
      delete this;
    }
    return remaining;
  }
  std::atomic<ULONG> refs_{1};
  std::atomic<unsigned>& count_;
};
constexpr GUID SessionProbeId{0xf50d3096, 0x4341, 0x479d, {0xb7, 0x76, 0xd9, 0x60, 0x74, 0xaf, 0x23, 0xd1}};

void run(bool warp) {
  Reference<ID3D12Debug> debug;
  const bool debug_enabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  if (debug_enabled)
    debug->EnableDebugLayer();
  Reference<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "Contention factory");
  Reference<IDXGIAdapter> adapter;
  if (warp)
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put())), "Contention WARP");
  Reference<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())), "Contention device");
  Reference<ID3D12InfoQueue> messages;
  if (debug_enabled)
    check(device->QueryInterface(IID_PPV_ARGS(messages.put())), "Contention debug messages");
  constexpr std::uint64_t key = 9201;
  require(runtime::init_device(key, device.get()), "Actual compositor runtime device");
  {
    // prepare compiles every distinct shader once, before it takes the
    // runtime mutex: with the mutex held here, compilation still completes and
    // prepare then waits. Initialization under the mutex compiles nothing.
    using Compositor = CameraCompositorD3D12;
    const auto compositor_shaders = [] {
      auto& cache = Compositor::shader_cache();
      AcquireSRWLockShared(&cache.lock);
      const auto ready = std::count_if(cache.bytecode.begin(), cache.bytecode.end(), [](ID3DBlob* blob) { return blob != nullptr; });
      ReleaseSRWLockShared(&cache.lock);
      return static_cast<std::size_t>(ready);
    };
    require(PfdStampD3D12::shader_compiles() == 0 && compositor_shaders() == 0, "Shader caches are empty in a fresh process");
    std::unique_lock held(runtime::runtime().mutex);
    auto prepared = std::async(std::launch::async, [] { return runtime::prepare(key); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while ((PfdStampD3D12::shader_compiles() < 4 || compositor_shaders() < Compositor::ShaderCount) &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(PfdStampD3D12::shader_compiles() == 4 && compositor_shaders() == Compositor::ShaderCount,
            "prepare compiled its shaders only after taking the runtime mutex");
    require(prepared.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
            "prepare initialized without the runtime mutex");
    held.unlock();
    require(prepared.get(), "Actual compositor runtime");
    const auto& prepared_item = *runtime::find(key);
    require(PfdStampD3D12::shader_compiles() == 4 && prepared_item.output.compositor_->statistics().shader_compiles == 0 &&
                prepared_item.waiting_available && prepared_item.waiting_output.compositor_->statistics().shader_compiles == 0,
            "Initialization under the runtime mutex compiled shaders");
  }
  require(runtime::set_display_exposure(key, 0), "Unmodified validation colours");
  auto& manager = runtime::manager();
  auto& item = *runtime::find(key);
  auto& handoff = scene_handoff();
  require(handoff.register_device(key) != 0, "Handoff device");
  Reference<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC qd{};
  check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "Application queue");
  Reference<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap.put())), "Source RTVs");
  std::array<Reference<ID3D12Resource>, 2> sources;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs{};
  std::array<std::uint64_t, 3> handles{};
  for (UINT feed = 0; feed < 2; ++feed) {
    const auto pane = profiles::A380.camera_panes[feed];
    create_texture(device.get(), texture_description(pane[0], pane[1], DXGI_FORMAT_R8G8B8A8_UNORM), sources[feed].put());
    handles[feed] = reinterpret_cast<std::uint64_t>(sources[feed].get());
    require(handoff.register_resource(key, handles[feed], 501 + feed), "Source identity");
    rtvs[feed] = {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{feed} * device->GetDescriptorHandleIncrementSize(hd.Type)};
    device->CreateRenderTargetView(sources[feed].get(), nullptr, rtvs[feed]);
  }
  handoff.begin_scene();
  require(handoff.publish(handoff.begin_capture(), {73, 1}, {101, 102}, handles), "Camera pair publication");
  struct Recording {
    Reference<ID3D12CommandAllocator> allocator;
    Reference<ID3D12GraphicsCommandList> list;
  };
  std::array<Recording, 6> recordings;
  using Colours = std::array<std::array<unsigned char, 4>, 2>;
  const Colours first{{{51, 102, 153, 255}, {153, 51, 102, 255}}};
  const Colours second{{{204, 153, 51, 255}, {51, 204, 153, 255}}};
  const auto capture = [&](unsigned round, const Colours& colours, bool retire = true) {
    std::array<ID3D12CommandList*, 2> lists{};
    for (UINT feed = 0; feed < 2; ++feed) {
      const auto index = round * 2 + feed;
      auto& recording = recordings[index];
      check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(recording.allocator.put())), "Capture allocator");
      check(device->CreateCommandList(0, qd.Type, recording.allocator.get(), nullptr, IID_PPV_ARGS(recording.list.put())), "Capture list");
      require(manager.register_command_list(recording.list.get(), key, index + 1), "Capture registration");
      std::array<float, 4> colour{};
      for (unsigned channel = 0; channel < 4; ++channel)
        colour[channel] = colours[feed][channel] / 255.f;
      recording.list->ClearRenderTargetView(rtvs[feed], colour.data(), 0, nullptr);
      require(manager.record_render_target_before_transition(recording.list.get(), sources[feed].get(), true, index + 1),
              "Real source capture");
      transition(recording.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(recording.list.get(), sources[feed].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      check(recording.list->Close(), "Close capture");
      lists[feed] = recording.list.get();
    }
    const auto receipt = manager.before_submission(queue.get(), static_cast<UINT>(lists.size()), lists.data());
    require(receipt != 0, "Capture submission receipt");
    queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    manager.after_submission(queue.get(), receipt);
    if (!retire)
      return;
    for (UINT feed = 0; feed < 2; ++feed) {
      const auto index = round * 2 + feed;
      manager.destroy_command_list(recordings[index].list.get(), index + 1);
    }
    require(drain_copy_queue(queue.get(), device.get()), "Captures and retirement fences complete");
  };
  capture(0, first);
  runtime::service();
  require(drain_copy_queue(item.output.queue(), device.get()), "Initial composition completion");
  require(runtime::snapshot(key).output && item.status.frames == 1 && !item.status.failed, "Initial healthy camera output");

  Reference<ID3D12Resource> readback;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = Output::BufferBytes;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto read_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(readback.put())),
        "Output readback");
  Recording consumer;
  check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(consumer.allocator.put())), "Consumer allocator");
  check(device->CreateCommandList(0, qd.Type, consumer.allocator.get(), nullptr, IID_PPV_ARGS(consumer.list.put())), "Consumer list");
  require(manager.register_command_list(consumer.list.get(), key, 20) && manager.register_consumer_recording(consumer.list.get()),
          "Application consumer participates in the real timeline");
  consumer.list->CopyBufferRegion(readback.get(), 0, item.output.buffer(), 0, Output::BufferBytes);
  check(consumer.list->Close(), "Close consumer");
  ID3D12CommandList* executable = consumer.list.get();
  const auto finish_consumer = [&](std::uint64_t receipt) {
    queue->ExecuteCommandLists(1, &executable);
    manager.after_submission(queue.get(), receipt);
    require(drain_copy_queue(queue.get(), device.get()), "Application consumer completed");
  };
  unsigned pixels = 0;
  const auto verify_pixels = [&](const Colours& colours) {
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(Output::BufferBytes)};
    check(readback->Map(0, &range, &mapped), "Map composed output");
    bool correct = true;
    for (UINT feed = 0; feed < 2; ++feed)
      for (UINT dy = 0; dy < 4; ++dy)
        for (UINT dx = 0; dx < 4; ++dx) {
          const UINT x = 380 + dx, y = (feed ? 500 : 100) + dy;
          const auto* pixel = static_cast<const unsigned char*>(mapped) + SIZE_T{y} * Output::RowPitch + x * 4;
          correct &= std::memcmp(pixel, colours[feed].data(), 4) == 0;
          ++pixels;
        }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    require(correct, "Both camera feeds reach output pixels without changing the last good image during contention");
  };

  capture(1, second);
  // Hold a genuine application consumer's before/after receipt on this thread.
  // service runs on its own thread as in the bridge, so TLS cannot bypass the
  // contention. Always complete the receipt before asserting, even on failure.
  const auto held = manager.before_submission(queue.get(), 1, &executable);
  require(held != 0, "Held application submission receipt");
  auto serviced = std::async(std::launch::async, [&] {
    runtime::service();
    const std::array tokens{item.pending[0].token, item.pending[1].token};
    runtime::service();
    bool leased = tokens[0] && tokens[1] && item.pending[0].token == tokens[0] && item.pending[1].token == tokens[1];
    {
      const std::lock_guard lock(manager.mutex_);
      for (const auto token : tokens) {
        bool retained = false;
        for (const auto& packet : manager.packets_)
          retained |= packet.token == token && packet.assigned && packet.leased && !packet.quarantined &&
                      packet.gpu.state() == SceneCaptureD3D12::State::ready;
        leased &= retained;
      }
    }
    const auto status = runtime::snapshot(key);
    return leased && !status.failed && status.output && status.frames == 1 && status.capture.completed == 4 &&
           !status.capture.quarantined && item.output.submissions() == 1 && !item.output.prepared_;
  });
  const bool prompt = serviced.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  finish_consumer(held);
  const bool preserved = serviced.get();
  require(prompt, "Runtime must return while application submission serialization remains held");
  require(preserved, "Repeated deferral must preserve both leases, device health and last good output without GPU submission");
  verify_pixels(first);

  runtime::service();
  require(drain_copy_queue(item.output.queue(), device.get()), "Retried composition completes");
  const auto status = runtime::snapshot(key);
  require(!status.failed && status.output && status.frames == 2 && status.completed_frames == 2 && !status.capture.quarantined &&
              !item.pending[0].token && !item.pending[1].token,
          "Releasing contention composes the retained pair exactly once and returns its leases");
  const auto receipt = manager.before_submission(queue.get(), 1, &executable);
  require(receipt != 0, "Retried output consumer receipt");
  finish_consumer(receipt);
  verify_pixels(second);

  // A full flight reset invalidates publication immediately while old app
  // recordings remain executable and both producer/consumer fences are blocked.
  std::atomic<unsigned> destroyed{};
  for (auto& source : sources) {
    auto* probe = new DeathProbe(destroyed);
    check(source->SetPrivateDataInterface(SessionProbeId, probe), "Source lifetime probe");
    probe->Release();
  }
  Reference<ID3D12Fence> blocked;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(blocked.put())), "Session blocker");
  check(queue->Wait(blocked.get(), 1), "Block actual old-flight producer");
  capture(2, first, false);
  auto reset = std::async(std::launch::async, [&] { return runtime::reset_session(key); });
  const bool reset_prompt = reset.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  if (!reset_prompt)
    check(blocked->Signal(1), "Unblock fixture before reporting reset wait");
  const auto session = reset.get();
  require(reset_prompt && session > 1 && !runtime::snapshot(key).output, "Session invalidation is immediate without GPU wait");
  for (auto& source : sources) {
    source->Release();
    *source.put() = nullptr;
  }
  require(destroyed == 0, "Reset cannot free sources still referenced by executable old captures");
  std::array<ID3D12CommandList*, 2> old{recordings[4].list.get(), recordings[5].list.get()};
  const auto old_receipt = manager.before_submission(queue.get(), 2, old.data());
  require(old_receipt != 0, "Old capture replay retains its receipt after full reset");
  queue->ExecuteCommandLists(2, old.data());
  manager.after_submission(queue.get(), old_receipt);
  for (unsigned index = 4; index < 6; ++index)
    manager.destroy_command_list(recordings[index].list.get(), index + 1);
  runtime::service();
  require(destroyed == 0 && item.status.frames == 2 && !item.status.output,
          "Retirement alone cannot release blocked GPU sources or publish old captures");
  require(runtime::resume_session(key, session) && !manager.register_consumer_recording(consumer.list.get()),
          "Resume cannot admit new reads in a previous-session native recording");
  check(blocked->Signal(1), "Finish old-flight GPU work");
  require(drain_copy_queue(queue.get(), device.get()), "Old replay and producer-retirement fences complete");
  runtime::service();
  runtime::service();
  require(destroyed == 2 && item.status.frames == 2 && !runtime::snapshot(key).output && !item.pending[0].token && !item.pending[1].token,
          "Completed obsolete captures release actual source leases without republishing into the new session");
  check(consumer.allocator->Reset(), "Reset completed old consumer allocator");
  check(consumer.list->Reset(consumer.allocator.get(), nullptr), "Fresh native recording in resumed session");
  manager.successful_reset(consumer.list.get(), 20);
  require(manager.register_consumer_recording(consumer.list.get()), "Actual native Reset admits current-session work");
  check(consumer.list->Close(), "Close fresh consumer");
  manager.destroy_command_list(consumer.list.get(), 20);
  handoff.stop_scene();
  runtime::reset_feed(key);
  {
    // Later start attempts of an initialized or failed device answer at once
    // and compile nothing, even with every compositor shader missing from the
    // cache; one behind a busy mutex still compiles first. The emptied cache
    // slots are restored afterwards (the recompiled blobs are left unreleased).
    using Compositor = CameraCompositorD3D12;
    auto& cache = Compositor::shader_cache();
    const auto cached = [&] {
      AcquireSRWLockShared(&cache.lock);
      const auto ready = std::count_if(cache.bytecode.begin(), cache.bytecode.end(), [](ID3DBlob* blob) { return blob != nullptr; });
      ReleaseSRWLockShared(&cache.lock);
      return static_cast<std::size_t>(ready);
    };
    AcquireSRWLockExclusive(&cache.lock);
    const auto saved = cache.bytecode;
    cache.bytecode = {};
    ReleaseSRWLockExclusive(&cache.lock);
    const auto stamp_compiles = PfdStampD3D12::shader_compiles();
    require(runtime::prepare(key) && cached() == 0, "An initialized device compiled shaders on a later start attempt");
    {
      const std::lock_guard lock(runtime::runtime().mutex);
      item.status.failed = true;
    }
    require(!runtime::prepare(key) && cached() == 0 && PfdStampD3D12::shader_compiles() == stamp_compiles,
            "A failed device compiled its missing shaders on a later start attempt");
    std::unique_lock held(runtime::runtime().mutex);
    auto busy = std::async(std::launch::async, [] { return runtime::prepare(key); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (cached() < Compositor::ShaderCount && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool compiled_first = cached() == Compositor::ShaderCount;
    held.unlock();  // Before any require: the future's destructor waits for prepare.
    require(compiled_first, "A start attempt behind a busy mutex did not compile first");
    require(!busy.get(), "A failed device reported success");
    AcquireSRWLockExclusive(&cache.lock);
    cache.bytecode = saved;
    ReleaseSRWLockExclusive(&cache.lock);
  }
  UINT64 errors = 0;
  if (messages.get())
    for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
      SIZE_T size = 0;
      check(messages->GetMessage(index, nullptr, &size), "Debug message size");
      std::vector<unsigned char> bytes(size);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      check(messages->GetMessage(index, message, &size), "Debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        ++errors;
        std::fprintf(stderr, "D3D12 error %u: %s\n", message->ID, message->pDescription);
      }
    }
  require(errors == 0, "Contention GPU debug validation");
  std::printf(
      "PASS runtime contention %s: prompt retry, retained leases, flight reset/replay/fenced source retirement, old/new output %u pixels, "
      "compile-free later starts; debug=%d errors=%llu.\n",
      warp ? "WARP" : "hardware", pixels, debug_enabled, errors);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--warp") != 0))
      return 2;
    run(argc == 2);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL runtime contention: %s\n", error.what());
    return 1;
  }
}
