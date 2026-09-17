# Dynamic camera compatibility

The camera runtime discovers its required functions and data in the loaded executable. It does not select compatibility from a whole-file hash, timestamp, storefront or list of fixed RVAs. Reviewed instruction templates preserve opcodes, registers, member offsets and local control flow. Only decoded address operands may move, and their destinations must satisfy the model's section and cross-reference constraints.

A match must be unique and complete. Native calls remain disabled if a required function changes, a reference points outside its permitted storage, a class identity is ambiguous, or a reread shows that the evidence changed. The independent PE, relocation, function-boundary, activation-mask and RTTI/vtable checks still apply. Runtime object ownership, generation checks, public aircraft telemetry and GPU synchronization remain separate requirements after discovery.

An update that only moves otherwise compatible code and data can therefore continue to work. An update that changes the reviewed instructions or object layout still needs investigation. Dynamic discovery cannot establish compatibility with an arbitrary future simulator implementation.

## Runtime boundary

Production access still requires the `FlightSimulator2024.exe` process and its actual main executable image. The resolved function addresses and data layout are published together only after the full contract passes. The runtime then verifies the manager update slot, installs its observer and retains that contract for the loaded image's lifetime. It does not rescan every frame or switch to a different contract after the observer is installed.

The resolved layout is passed to manager, aircraft, scene-pose, activation-mask and AA inspections. The fixed values in `observed_store_layout()` remain diagnostic/test defaults; they are not a fallback for an unresolved production image. Method bodies, object member offsets, native signatures and lifecycle rules remain tied to the reviewed ABI. Resolving a vtable or finding matching bytes does not grant ownership of a live object.

Startup discovery and code/metadata scans retain the default full-region image reader and their bounded scan/read budgets. Selected recurring inspections use the explicit page-query mode described in [camera inspection](architecture.md#3-position-and-render-the-two-cameras). That mode still requires the exact main-image allocation and current access to every requested page, retains exact reads and validates saved observations at the inspection endpoint. It changes metadata-query coverage and cost, not the accepted instruction templates or object identities.

## Startup, transitions and AA

Temporary pose or manager unavailability during a clean initial request can enter bounded recovery. A retry needs fresh manager and pose validation, confirmed absence of old owned IDs and an unchanged current request. The first calibration may progress while that retry waits; waiting for calibration does not consume a retry attempt. Partial creation and identity/native-creation failures remain refused. See [manager inspection and recovery](reshade-compatibility.md#manager-inspection-and-recovery) for the exact recovery scope.

An aircraft/profile transition also distinguishes an empty request from a retained camera pair. A request with no native creation can be cancelled without native cleanup. A retained pair must match its manager lifetime, entry IDs and published allocation dimensions before it can be acknowledged and resumed; dimensions from an earlier pair cannot establish the new pair's readiness. Stop or a superseding request cancels the earlier creation intent.

Inherited render, display and output dimensions may differ under DLSS/upscaling. Each inherited pair is bounded independently, while all requested camera dimensions must still match the exact profile pane. Initial sizing and retained-output restoration require closed gates and field rereads; restoration additionally proves the existing output dimensions and does not allocate a replacement. This removes an invalid equal-dimensions assumption, not the remaining DLSS motion/visual limitations described in the [runtime reference](runtime-reference.md#camera-mounts).

Per-view AA suppression uses the resolved override addresses and changes only the reviewed bit in a freshly validated owned view. It preserves the main-view AA setting, both global-override checks and flag rereads. A failed writable-span, snapshot or override check leaves activation refused. Memory-query optimizations do not skip validation when the AA bit is already clear.

## Validation scope

The local suites cover moved instruction/data references, incomplete and ambiguous matches, malformed or changing metadata, static method/vtable relations, resolved-layout use, mixed render/display dimensions and cancellation/recovery races. They also retain the wrong-host runtime refusal. The complete native build and hardware/WARP tests are separate from simulator acceptance.

Simulator acceptance applies to the exact tested binaries, aircraft and AA/overlay configuration; it does not establish arbitrary later builds or configurations. Dynamic camera discovery also does not recover display textures missed by late bridge attachment; see [startup timing](reshade-compatibility.md#startup-timing) and [ReShade hook ordering](reshade-compatibility.md#load-and-hook-ordering).

## Updating the reviewed model

The generated model is checked into source. `tools/diagnostics/generate_camera_contract.ps1` rebuilds it from reviewed captures and decoded evidence; its input receipts and raw captures belong under ignored `build/` directories. The generator and disassembler are development tools, not runtime dependencies. Captured-byte replays and synthetic image tests establish the discovery checks; successful read-only discovery or scene inspection does not establish rendered camera behaviour.

The production templates currently in `src/camera/camera_contract_model.cpp` were generated from **MSFS 2024 1.8.16.0** reviewed captures (PE timestamp `1787653788`, `SizeOfImage` `235963904`). Discovery accepts relocated copies of those reviewed instructions; it does **not** invent a contract for an unobserved build. A newer simulator update that changes the reviewed instruction bytes—such as **MSFS 2024 SU7 beta (for example 1.9.10)**—refuses with `template_not_found` naming the missing semantic templates and annotating the loaded image's PE timestamp, size, checksum and section count. That refusal is expected until a complete capture set from that exact build is reviewed and the model is regenerated. Bridge connection, PFD detection and aircraft profiles can still succeed while discovery refuses.

Capturing a beta or other unreviewed build does **not** require the original maintainer's machine. Any developer with that exact simulator build can collect the reviewed diagnostic archives (image capture, store/motion evidence and static identity), run the generator below into an ignored review path, and open a PR with the receipt plus proposed `camera_contract_model.cpp` replacement. Keep aircraft Lvar and material identifiers exact; do not rewrite historical release receipts.

Regeneration requires explicit paths to all reviewed inputs; a normal build uses the checked-in model and needs none of the local capture archives. Generate into an ignored review location first:

```powershell
./tools/diagnostics/generate_camera_contract.ps1 `
  -Artifacts '<reviewed-evidence-directory>' `
  -ImageCapture '<verified-image.json>' `
  -StoreArchive '<archived-store-evidence>' `
  -MotionArchive '<archived-motion-evidence>' `
  -IdentityEvidence '<complete-static-identity.json>' `
  -Output 'build/validation/generated-camera-contract.cpp' `
  -Receipt 'build/validation/generated-camera-contract-receipt.json'
```

Review the generated changes and receipt before replacing `src/camera/camera_contract_model.cpp`. The legacy `profile.hpp` and `verified_profile.cpp` remain evidence for the separate read-only diagnostic tools; the production bridge does not link the old fingerprint list.
