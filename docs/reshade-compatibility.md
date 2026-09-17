# ReShade device compatibility

Issue [#21](https://github.com/rthoms334/taxi-cam/issues/21) exposed a graphics delivery failure with ReShade 6.8: capture/composition could succeed while every terminal PFD write was refused. ReShade's command-list `Close` is a proxy endpoint and can run add-on callbacks before forwarding. Treating it as the native recording boundary would permit later draws to overwrite the camera or consume its final bindings. ReShade compatibility remains experimental; device unwrapping does not establish compatibility with every preset, add-on or injector combination.

The bridge resolves the bootstrap device through ReShade's optional reference-counted `IID_UnwrappedObject` extension before installing its existing device, command-list and queue hooks. The native layer receives descriptors and submitted lists after ReShade translates them, and its `Close` follows ReShade's callbacks. The native executable-image endpoint guard remains unchanged. The identifier comes from [ReShade 6.8's COM declarations](https://github.com/crosire/reshade/blob/v6.8.0/source/com_utils.hpp); no ReShade SDK or private object layout is required.

Resolution is bounded to four proxy layers, retains canonical COM identities while traversing, and rejects cycles, malformed results and unexpected errors. Only `E_NOINTERFACE` terminates an absent extension. The endpoint must still expose the public device interface and meet the existing Device10 requirement. Device ownership checks also resolve the device returned by `GetDevice`, because ReShade can return its proxy even from a native resource.

## Load and hook ordering

The tested startup order loads ReShade before Taxi Cam initializes its graphics hooks. Keep ReShade on its normal simulator startup path, and start Taxi Cam while MSFS is at the main menu, before loading the flight. This gives the bridge an opportunity to attach before the aircraft creates its display textures and views. Starting the companion early does not itself prove attachment succeeded; check its connection and bridge status.

The required command-list order is:

1. The application calls ReShade's proxy.
2. ReShade performs its callbacks and forwards to the native command list.
3. Taxi Cam's native `Close` hook appends an eligible final PFD write.
4. The saved D3D12 runtime `Close` closes the recording.

This follows [ReShade 6.8's `Close` implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/d3d12/d3d12_command_list.cpp). Taxi Cam does not install this final write above the ReShade proxy or ask users to reorder ReShade effects. The saved forward must still be executable image code in `D3D12Core.dll`, `d3d12.dll` or the D3D12 debug layer `D3D12SDKLayers.dll`. If that proof fails, terminal drawing is refused and `close_forward_refused` increases; independently proved texture copies retain their own checks. Disabling this guard would not make an unknown hook chain safe.

The bridge pins its device and hook modules for the process lifetime. Arbitrary injector order, injecting/reloading ReShade after graphics initialization and live hook replacement have not been validated. Restart MSFS after changing the graphics-injector setup; restarting only the companion does not unload or reinstall the bridge already inside MSFS.

## Frame generation and ReShade add-ons

Issue [#27](https://github.com/rthoms334/taxi-cam/issues/27) is a presentation deadlock with ReShade **MFG Unlock** (DLSS-G multi-frame override) and similar frame-generation helpers. Those add-ons submit extra command lists on the same native queue from a helper thread while Present is still inside the application's `ExecuteCommandLists`. Taxi Cam must not hold that queue's submit lock across another thread's submit: a waiter there blocks DXGI and freezes the simulator.

Observed Wait/Signal pairing still serializes on that queue. A helper that cannot take the lock immediately forwards the original batch, then reports `contended_submission`. Capture invalidates source-state evidence for that submit and keeps the device available. Same-thread re-entry during the original submit is treated the same way: the timeline Signal still runs so an already-queued Wait cannot hang the GPU, but the device is not permanently failed. Unrelated application batches release the lock before the original submit.

This keeps the presentation chain moving. It does not prove that camera pixels reach the PFD while frame generation, ReShade add-ons or DLSS-G are active. If capture stays paused at "waiting for verified GPU state", collect the bridge log. Disabling ReShade/MFG remains the confirmed local workaround; this change is a hang fix, not live MFG acceptance.

## Startup timing

The bridge observes resource creation and later render-target-view creation. After a late attach it also admits already-created display textures the first time a barrier or copy sees them, and it can recover the bound RTV handle when that unique resource is the next OM target. That path is a short one-shot window: each pointer is observed once, then a cheap already-seen check, then the extra work is disabled entirely (one relaxed flag load) after two PFD candidates and their handles exist, or after three seconds. It does not walk the device heap and does not add locks, allocations or logging on the steady-state Present/barrier/copy/bind path.

Starting after the aircraft has loaded can still miss targets when instruments never transition or bind during that window, or when several render targets share one command list before OM. Overview then stays on “waiting for cockpit displays” rather than claiming ready. Keep Taxi Cam running and look again after the next cockpit draw; use **Restart Flight** only if the list stays empty. Restarting the companion does not reconstruct missing GPU objects. A nonzero capture/composition count cannot establish successful PFD routing or presentation. See [PFD-copy diagnostics](runtime-reference.md#pfd-copy-diagnostics) for the separate delivery counters.

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
