# ReShade device compatibility

Issue [#21](https://github.com/rthoms334/taxi-cam/issues/21) exposed a graphics delivery failure with ReShade 6.8: capture/composition could succeed while every terminal PFD write was refused. ReShade's command-list `Close` is a proxy endpoint and can run add-on callbacks before forwarding. Treating it as the native recording boundary would permit later draws to overwrite the camera or consume its final bindings. ReShade compatibility remains experimental; device unwrapping does not establish compatibility with every preset, add-on or injector combination.

The bridge resolves the bootstrap device through ReShade's optional reference-counted `IID_UnwrappedObject` COM extension before installing its existing device, command-list and queue hooks. Unwrapping is COM-only: Taxi Cam queries that GUID, retains canonical COM identities while traversing up to four proxy layers, and never walks private ReShade object layouts or links a ReShade SDK. The native layer receives descriptors and submitted lists after ReShade translates them, and its `Close` follows ReShade's callbacks. The native executable-image endpoint guard remains unchanged. The identifier comes from [ReShade 6.8's COM declarations](https://github.com/crosire/reshade/blob/v6.8.0/source/com_utils.hpp).

Resolution is bounded to four proxy layers, retains canonical COM identities while traversing, and rejects cycles, malformed results and unexpected errors. Only `E_NOINTERFACE` terminates an absent extension. The endpoint must still expose the public device interface and meet the existing Device10 requirement. Device ownership checks also resolve the device returned by `GetDevice`, because ReShade can return its proxy even from a native resource.

## Load and hook ordering

The tested startup order loads ReShade before Taxi Cam initializes its graphics hooks. Keep ReShade on its normal simulator startup path. Connect can be requested at the main menu or in a loaded flight; an early connection observes original display creation, while a late connection must recover the existing bindings described below. Starting the companion early does not itself prove attachment succeeded; check its connection and bridge status.

The required command-list order is:

1. The application calls ReShade's proxy.
2. ReShade performs its callbacks and forwards to the native command list.
3. Taxi Cam's native `Close` hook appends an eligible final PFD write.
4. The saved D3D12 runtime `Close` closes the recording.

This follows [ReShade 6.8's `Close` implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/d3d12/d3d12_command_list.cpp). Taxi Cam does not install this final write above the ReShade proxy or ask users to reorder ReShade effects. The saved forward must still be executable image code in `D3D12Core.dll`, `d3d12.dll` or the D3D12 debug layer `D3D12SDKLayers.dll`. If that proof fails, terminal drawing is refused and `close_forward_refused` increases; independently proved texture copies retain their own checks. Disabling this guard would not make an unknown hook chain safe. After graphics initialization, `bridge.log` records `Command-list Close endpoint: module=… location=… native=… device_proxied=…`. `module` is the basename of the DLL behind the saved forward (no path), `location` is `system` for the Windows system directory, `other` for anywhere else (for example an injector `dxgi.dll` in the simulator folder) or `non_image` for code outside any loaded module, and `device_proxied` reports whether the reported device was a proxy. While cameras are ready with no PFD writes and this proof has failed, the status names that module instead of reporting a pending write opportunity.

The bridge pins its device and hook modules for the process lifetime. Arbitrary injector order, injecting/reloading ReShade after graphics initialization and live hook replacement have not been validated. Restart MSFS after changing the graphics-injector setup; restarting only the companion does not unload or reinstall the bridge already inside MSFS.

## Frame generation and ReShade add-ons

Issue [#27](https://github.com/rthoms334/taxi-cam/issues/27) is a presentation deadlock with ReShade **MFG Unlock** (DLSS-G multi-frame override) and similar frame-generation helpers. Those add-ons submit extra command lists on the same native queue from a helper thread while Present is still inside the application's `ExecuteCommandLists`. Taxi Cam must not hold that queue's submit lock across another thread's submit: a waiter there blocks DXGI and freezes the simulator.

Observed Wait/Signal pairing still serializes on that queue. A helper that cannot take the lock reports its exact batch without acquiring the manager mutex, then forwards the original call. Source-only uncertainty invalidates capture evidence while leaving the device available. Any escaped bridge-owned work retains its separate quarantine/lifetime guards. Same-thread re-entry still runs the timeline Signal so an already-queued Wait is retired. Unrelated application batches release the lock before the original submit. Only fully observed unrelated cross-queue batches bypass busy submission serialization; source-affecting and unknown recordings, snapshot writes and output readers retain their ordering guard.

Private camera composition also defers when submission admission is busy, retaining its input leases for a later service iteration. It must not hold up PFD discovery or turn routine queue contention into a permanently failed output device. These checks do not remove native API calls or all CPU serialization.

This keeps the presentation chain moving. It does not prove that camera pixels reach the PFD while frame generation, ReShade add-ons or DLSS-G are active. If capture stays paused at "waiting for verified GPU state", collect the bridge log. Disabling ReShade/MFG remains the confirmed local workaround; this change is a hang fix, not live MFG acceptance.

## Startup timing

The bridge observes resource creation and later render-target-view creation. After a late attach it also admits already-created display textures on barrier or copy use, and associates a unique render-target entry with the next single OM target on that recording. The bounded association window waits for distinct resource associations across the profile's display set, including all eight iniBuilds A380 displays. An empty inventory has a three-second admission budget; a nonempty inventory has a ten-second association budget. Applying an aircraft profile rebuilds eligibility and restarts the window, including for resources first seen under the previous profile. During the window each resource is classified once: a fixed seen set of 65,536 pointers remembers classified resources, live registry entries skip classification through the lock-free resource index, and placed-footprint copy locations (buffers under the D3D12 contract) are never classified. A pointer that cannot be described yet, or a texture the registry could not hold, is retried on its next use, and a creation that misses the registry lock forgets its address so its first use is still classified. Reconnect windows still cannot end early because of existing descriptor aliases. Once complete or expired, recording hooks retain only the inactive flag check.

Recovered application descriptors retain an unknown format. Camera delivery and calibration use an explicit compatible bridge-owned typed RTV rather than treating a typeless allocation as a typed application view. A recovered association alone does not authorize a write: terminal output needs an actual render-target transition in that recording, and boundary copies retain the existing native transition, recording and pass guards. Binding hints are consumed or discarded at OM, Reset, Close and list retirement; stale resource incarnations and ambiguous entries remain refused. Automatic PFD discovery and side-selection rules are unchanged.

The user verified A350 operation on 2026-09-18. A subsequent loaded-flight iniBuilds A380 session discovered all eight display textures without their older opaque RTV bindings; camera capture and composition continued with zero PFD writes. An isolated GPU reproduction confirmed the separate-command-list failure for FBW's five-mip displays as well.

The bridge now handles this legacy-barrier case without reconstructing opaque descriptors. After validating the complete submitted batch, it can insert at most two prepared copy lists immediately after a fully observed, closed, barrier-only display RT exit. The original lists keep their order in one native `ExecuteCommandLists` call. The copy restores the exact explicit SRV or COMMON state, touches only the selected base-mip rectangle, and retains target/patch leases through the manager's covering fence. Unknown recordings or downstream Close endpoints, suspended/resuming or unsupported passes, aliases, split/enhanced barriers and uncertain generations refuse this path. Packet allocation and reset happen on the service thread; queue callbacks perform no shader preparation, allocator reset or CPU wait.

When all matching display draw counts are zero, verified submitted RT completions supply a separate activity counter to the existing automatic ranking thresholds and side ordering. Switching between activity sources reseeds the detector; manual selection remains authoritative. Hardware and WARP regressions verify iniBuilds eight-display and FBW two/three-display cases, calibration and camera pixels read later in the original batch, replay, later overwrites and preserved surrounding pixels. These local results do not establish live MSFS behaviour of the revised build; the earlier simulator failure remains historical evidence.

The bridge does not bootstrap a D3D11On12 device or install native D3D11 draw detours. Native D3D12 vtable updates use atomic pointer replacement without process-wide thread suspension. Connect remains available in a loaded flight.

Pure D3D12 pre-existing descriptors still require creation evidence or an unambiguous same-recording association; cross-list timing cannot authorize a guessed descriptor map. Overview says **waiting for cockpit displays** only while the PFD list is empty. A nonzero capture/composition count cannot establish successful PFD routing or presentation. See [PFD-copy diagnostics](runtime-reference.md#pfd-copy-diagnostics) for the separate delivery counters.

## Presentation stall with ReShade's immediate list

ReShade 6.8 stays enabled. Its injected `dxgi.dll` flushes an immediate command list on the native queue from inside proxy `ExecuteCommandLists`, then CPU-waits until ReShade's own fence is signaled. That fence is queued after Taxi Cam's `queue->Wait` on the bridge timeline. Taxi Cam now queues that `Wait` only for a receipt on a different queue from the previous timeline `Signal` (see [render-thread freeze guards](render-thread-freeze-guards.md)), so the fence can queue behind it only after such a queue change. The release below is unchanged. A presentation-watchdog trip (`presentation_and_sim_frame_stalled`) used to close the submission gate and release bridge resources while that GPU wait stayed queued, so the flush never returned and the simulator stayed frozen. `gated` stayed 0 because the stuck thread never reached another submit.

Closing the gate now CPU-signals the timeline value already passed to that `Wait`, on the watchdog thread, without taking a bridge mutex (`released_waits` counts each Signal that runs). That is the submission-gate wait release: the ReShade flush can finish once the GPU passes the timeline point. A repeated close, or a close after the value has already completed, does not signal again. Opening the gate does not signal. Read-only depth and stencil render-pass flags (`D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_DEPTH`, `D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_STENCIL`) no longer mark the recording invalid. A second `EndRenderPass` of an ordinary pass that already closed is forwarded and is not `PassState`. Command-list pass identity follows `IID_UnwrappedObject`, so the proxy and the native list share one pass. `PRESERVE_LOCAL` access still invalidates the recording.

These checks do not establish live MSFS behaviour. Shader output and PFD pixels with ReShade 6.8.0 still need a simulator session.

## Validation

`build.ps1 -Validate` runs the bounded COM-chain tests, genuine WARP device/resource identity tests and normal hardware/WARP graphics suite. `smoke-test.ps1` verifies the resulting native delivery artifacts.

To exercise actual ReShade forwarding with an existing trusted local DLL after that build:

```powershell
./tests/graphics/reshade_validation.ps1 -ReShadeDll 'C:\path\to\dxgi.dll'
```

Use `-WarpOnly` on machines without a hardware adapter. The script copies the supplied DLL and graphics harness into fresh ignored `build/reshade-validation/` directories. It records binary hashes, logs and case results. `--require-proxy` requires distinct reported/native device identities, so a run without an active wrapper fails. Application objects and calls continue through ReShade; only bridge-owned work uses the resolved device.

The cases exercise real proxy queue/descriptor forwarding, captured source images and composition, exact surrounding PFD pixels across UNORM/sRGB/mip/alpha combinations, terminal writes, query refusal, proved copies and closed-list replay. The harness creates some queue/list objects before bridge initialization. Display textures and views are now created before `initialize_graphics` and must appear in inventory after the first cockpit-style RT transition and OM bind. That proves local learn-on-use, not live MSFS late-Connect behaviour.

Each proxy-suite receipt identifies the exact ReShade DLL and harness tested. Results apply to those binaries; rerun the suite when either changes. The ordinary native build and smoke checks do not automatically run this optional proxy suite.

These local tests do not establish in-simulator routing, motion, overlay interaction or aircraft startup behavior. Those remain separate live acceptance checks with the intended ReShade configuration. Neither recorded draw counts nor successful local GPU tests prove that a simulator frame was presented.

## Manager inspection and recovery

Repeated failures for an already-pending manager-inspection stop reason preserve the original recovery deadline and bounded attempt count. Retrying still requires a fresh guarded inspection and valid pose; persistent mismatches continue to refuse activation.

The first failed query, read or endpoint comparison records its stage, region index, metadata field, expected/observed values and Windows error. Ordinary diagnostics redact private addresses. These values come from the existing checks without additional memory reads. They identify the failed observation; the retry change does not by itself explain or eliminate a metadata mismatch.

Camera startup can still be refused after device unwrapping succeeds: a first pose check may pass while the repeated check in the descriptor initializer becomes unavailable before any camera is created.

A temporary pose refusal now enters bounded recovery only when the initializer failed before any native camera creation, no owned IDs remain, and the original Start request is still current. The failed controller request is cleared without native callbacks. Each retry retains both full pose checks, fresh manager validation and the existing three-attempt limit. Partial pairs, native create failures, descriptor-contract failures and fatal pose errors remain refused. Background preparation can stay pending during this clean waiting state, and the underlying inspection detail is retained in the stop diagnostic.

An inspection failure before the first body calibration must also allow calibration to make progress after the manager becomes valid again. Recovery may perform the existing guarded pose inspection for a clean, empty controller while fresh public telemetry requests calibration. Waiting for those samples does not consume a retry attempt. A fully validated pose is still required before the retry is admitted, and Stop or a superseding Start prevents the earlier recovery from requesting creation. The retained stop detail describes the original interruption; it is not evidence that each later observer pass encountered the same failure.

Camera startup recovery is independent of native device unwrapping. [Camera memory inspection](architecture.md#3-position-and-render-the-two-cameras) preserves fresh allocation/page checks, exact reads and the ownership checks required for activation. It does not repair missing PFD identities or relax the final native `Close` requirement.
