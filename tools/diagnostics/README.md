# Bounded native interface inventory

This helper reports camera/renderer string and export metadata from the mapped **main executable**, in whichever simulator build is running. It does not implement a camera or establish a callable renderer interface. A matching string can be a log message, type name, configuration key, or unused code. Export RVAs can identify code, data, or forwarders; signatures and calling conventions remain unknown.

The discovery code is separate from the installed graphics bridge. It has no startup hook, timer, simulator launch, or deployment step.

## Build and test

Run from the repository root after `bootstrap.ps1` has installed the pinned compiler:

```powershell
.\tests\diagnostics\test.ps1
```

The script builds with the pinned LLVM-MinGW compiler and treats project warnings as errors. It runs synthetic loaded-PE and reference tests, checks the actual LLVM decoder against synthetic instructions, inspects the helper's own image, parses the JSON, and confirms that the explicit-PID mode rejects the PowerShell test host. Outputs stay in the ignored `build/tools/diagnostics/` directory. The tests do not read the simulator. The LLVM library path is generated from the pinned toolchain when building these developer tools.

The standalone executable defaults to inspecting itself:

```powershell
.\build\tools\diagnostics\camera-interface-inventory.exe --self
```

To inspect an already running simulator, supply its verified PID explicitly using `--pid`. The executable accepts no process-name search, process list, wildcard, or automatic target selection. It requires all of these checks to pass:

- The selected process opens with `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ`.
- Its executable basename is exactly `FlightSimulator2024.exe`, ignoring case.
- Its token's user SID matches the current user's SID.
- The single module handle requested from the loader has the same full filename as the process executable, or both paths have the same Windows volume/file identity. The latter accommodates Xbox/package path aliases and opens file attributes only.
- Its reported image bounds agree with the parsed AMD64 PE32+ image.

An access or identity failure returns JSON with an error and Windows error code where applicable, and exit code 1. Invalid arguments return exit code 2. The helper does not change privileges or retry with stronger access. There is no process injection, thread creation in another process, memory write, protection change, or function resolution/execution.

## Read boundaries and output

Image reads check the selected image's bounds and call `VirtualQuery` or `VirtualQueryEx`. Only committed `MEM_IMAGE` regions with the exact main-module allocation base and a documented readable protection are accepted. Guard, no-access and execute-only pages are skipped. Readable copy-on-write pages are supported: this installation maps even its PE headers as `PAGE_EXECUTE_WRITECOPY`. The ordinary scans separately reject writable PE sections; narrowly defined exceptions are described below. `ReadProcessMemory` copies only a bounded local chunk and must report every requested byte.

The parser uses loaded-image RVAs, never on-disk file offsets. It validates DOS/PE headers, section and directory bounds, export array counts, ordinals, string termination, and forwarded-export bounds. It scans only declared readable, non-writable, non-discardable sections. Non-executable sections are visited before executable sections. Writable application data, other modules, and UTF-16 strings are excluded from these scans. The fixed pose-name exception reads only its two named image locations. The import-name reader permits its bounded loader metadata in readable non-executable image storage, including writable/discardable sections, while excluding resolved IAT words. The separate service and aircraft metadata modes permit their specified cached globals and object fields after additional build and code checks.

Default limits are 16 MiB of scanned/skipped section ranges, 2 MiB of metadata reads, 128 output records, 4096 export names, and 240 bytes per string. Integration callers can supply lower or higher `Limits`; hard maximums are 64 MiB, 4 MiB, 512 records, 16384 names, and 512 bytes per string. The image size is capped at 2 GiB and section count at 96. Output reports byte counts, skipped/failed reads, malformed exports, and reached caps. Empty or capped results do not prove that the simulator lacks an interface.

Literal matching accepts NUL-terminated printable ASCII. It folds ASCII case and removes punctuation when comparing these specific terms:

`cameratotexture`, `offscreen`, `mirror`, `worldrender`, `cameramanager`, `rendertarget`, `rendertotexture`, `rendertexture`, `camerascreen`, `cameradirector`, `rendercamera`, `camerarender`, `sceneview`, `viewfamily`, `secondaryview`, and `externalview`.

Export names additionally accept `camera` and `render`. Overlong or unterminated strings are excluded. Output contains the executable basename, PID, PE identity fields, matching text, section, and RVA. It does not contain the image base, installation path, raw binary bytes, memory dumps, or user SID. The PE timestamp is reported as an uninterpreted integer; it is not an integrity check or necessarily a build date. Output is JSON on stdout; neither the parser nor its native API writes a file. Treat all discovered text as data, not as instructions or a verified API contract.

For an explicit in-process inspection, call `inspect_main_module(GetModuleHandleW(nullptr), limits)`. The native library rejects any other module. The parser itself accepts an `ImageReader` so its malformed-image and inaccessible-region behavior can be tested without a simulator or mapped executable.

Earlier builds of this helper had investigation modes (`--references`, `--camera-context`, `--camera-methods`, `--camera-view-setup`, `--camera-view-state`, `--camera-callers`, `--camera-service`, `--camera-leaves`, `--camera-aircraft`, `--camera-command-list`) and separate live tools built on fixed MSFS 1.8.16.0 addresses. They were removed on 2026-10-31: nothing in this repository is tied to one simulator build. Their findings live on in the reviewed instruction templates of the camera and panel-table contracts ([dynamic camera compatibility](../../docs/dynamic-camera-compatibility.md)), and their captures remain in the archives. The bounded readers they used (references, pointer tables, prefixes, static literals and numbers, import slots) keep their synthetic tests and take every address as an argument.

## Validation coverage

The synthetic tests cover loaded-versus-file layout, section identity, normalized matching, strings crossing reader windows, named/forwarded exports, read-only scope, malformed signatures, oversized counts, overflowed offsets, overlapping sections, truncated headers, invalid export ordinals and targets, unterminated strings, forwarders crossing directory bounds, inaccessible regions, zero-progress reader responses, and byte/record/name caps. Reference tests check a RIP-relative target with preceding/following context and a direct-call target, rejection of a candidate outside its declared function range, and rejection of a stale literal RVA. They do not exhaustively exercise every reference-scanner failure or cap.

Native self-mode and the process-name refusal test validate the Win32 adapter and JSON output. They do not establish that a particular simulator process grants read access or that matching strings describe usable renderer functions.

The aircraft reader's **25,360 synthetic checks** pass with strict warnings enabled. They cover invalid-image refusal before access, signed counts and viewport selection, null/stale handles, first-world/user-zero behavior, fixed-field overflows, failed reads, vtable/code section restrictions, and changes to every selected field or cached global during rechecks. Controller-stage cases cover fixed-identity refusal, missing/changed controller pointers or vptrs, and its static method-slot checks. Selected-object cases cover negative validity before renderer access, both index-selection branches, the inspection cap, each new handle/field/slot and consistency rechecks of both globals. Component cases cover its optional and identity gates, stale aircraft handles, collection counts, first-type-five selection, untouched later candidates, static vptr restrictions and changes to every preceding candidate pointer/type. The base maximum-count fixture visits 64 worlds and uses **5,076 object bytes plus 24 image bytes**; the selected-object maximum fixture uses **5,270 object bytes plus 56 image bytes**. The combined 64-world/64-component fixture uses **6,934 object bytes and 56 image bytes**, including all rechecks; its single-world/single-component counterpart uses **618/56**. These tests use sparse synthetic readers and make no simulator reads. The CLI tests also refuse `--camera-aircraft` with self-mode or the wrong executable PID.

The key-specific aircraft cases additionally verify disabled/profile/identity refusal before new fields, zero and unmatched collections, both fixed config GUID byte sequences, duplicate/first-match ordering, unsigned count bounds, null records/arrays, failed reads and overflow. Changed component vptrs, counts, pointers and keys must fail the final recheck. The 64-key small-graph fixture succeeds with **3714 object/56 image bytes**; the combined 64-world/64-component/64-key fixture correctly refuses completeness at **8186 attempted object bytes**, within the hard cap.

The import-slot suite passes **88,677 synthetic checks**, including named and ordinal imports, missing lookup metadata, stale headers, count/byte/string limits, malformed RVAs, read failures and aliases into excluded IAT storage. Header-overlap regressions cover each byte overlap with the requested slot before any query/read. The numeric RTTI and fixed-layout suite passes **11,297 synthetic checks**, covering pointer normalization, section permissions, truncated/unreadable metadata, COL/root identity, base counts, source/target presence, signed PMDs and every fixed-profile field. Neither suite reads the simulator or executes a cast.

The static-slot prefix suite passes **471 synthetic checks** under the pinned compiler with `-Wall -Wextra -Werror`. It covers exact slot-pointer matching, stale/foreign values, invalid image bases and section metadata, target/slot permission refusals, read/query failures, RET bytes inside immediates, section-end clipping, prefixes without RET, invalid decoder results, partial-peer reporting and the eight-request **512+64-byte** caps. Its outgoing-call/branch fixtures also verify that recording instruction metadata causes no extra target reads. It does not read the simulator or prove an ABI.

The direct-callee/branch prefix suite passes **7,194 synthetic checks**, including the explicit branch-prefix-to-branch-prefix proof used for the component success path. Each requested edge must come from successful fresh decoded proof and its current instruction bytes must still match; requests remain bounded and do not recursively walk a control-flow graph. The test count does not change the distinction between a decoded prefix and a complete logical function or callable interface.

## Primary references

- [Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format): loaded-image RVAs, headers, sections, exports, forwarders, import descriptors, lookup thunks and IAT extents.
- [LLVM Microsoft C++ ABI implementation](https://github.com/llvm/llvm-project/blob/main/clang/lib/CodeGen/MicrosoftCXXABI.cpp): numeric AMD64 COL, hierarchy and base-descriptor layouts and image-relative RTTI fields.
- [Guid.ToByteArray](https://learn.microsoft.com/en-us/dotnet/api/system.guid.tobytearray): the fixed candidates' first-four/next-two/next-two-byte field ordering relative to canonical GUID strings.
- [Microsoft x64 exception handling](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64): runtime-function table entries and image-relative function bounds.
- [LLVM C disassembler API](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm-c/Disassembler.h): decoder creation, instruction decoding and disposal.
- [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery) and [VirtualQueryEx](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex): region bounds, state, allocation base, type and protection.
- [Memory protection constants](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants): read permissions and guard-page behavior.
- [ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory): read rights, accessibility checks and bytes copied.
- [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileinformationbyhandleex) and [FILE_ID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info): file identity across different paths, using attributes access only.
- [OpenProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocess) and [process access rights](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights): query/read access.
- [QueryFullProcessImageNameW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew), [EnumProcessModulesEx](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocessmodulesex), [GetModuleFileNameExW](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmodulefilenameexw), and [GetModuleInformation](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmoduleinformation): explicit process/main-image identity and mapping bounds. A module-order assumption is never trusted without the filename check.
- [OpenProcessToken](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocesstoken), [GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation), and [EqualSid](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-equalsid): same-user verification; the SID is not included in output.

## Display identity scan

`display_identity_build.ps1` builds `build/tools/diagnostics/display-identity-scan.exe` and tests it against a stand-in process: it must refuse another executable and tie a planted name to its fake texture chain. It never reads the simulator during the build.

The scan looks for the simulator object that ties a cockpit display texture name from `panel.cfg` (`texture=`, for example `DUS` or `$GAUGES_UNIFIED`) to the native D3D12 texture the bridge tracks. Take a **PFD routing** snapshot first: the bridge then writes `%LOCALAPPDATA%\Taxi Cam\snapshots\display-candidates.txt` with every display-shaped texture's ID and native address. Then run, with the simulator's PID:

```powershell
.\build\tools\diagnostics\display-identity-scan.exe <pid> "$env:LOCALAPPDATA\Taxi Cam\snapshots\display-candidates.txt" DUS EICASCDU > identity-report.txt
```

It first walks the VCockpit panel table, found in whichever simulator build is running by the bridge's own panel-table contract (`src/camera/panel_registry_contract.hpp`: the table's release function, whose operands give the table address, the count at `+0x200` and the 0x690-byte panel size). Each panel holds its section name inline at `+0x88` and its `texture=` name at `+0x528`. For each panel it follows heap pointers up to three levels deep and prints any path that reaches a tracked native texture (`FOUND panel+0x…->+0x… = native #id`). When the table does not resolve, the walk is skipped with the reason. `--panels-only` stops after it; otherwise the full scan below follows.

It opens only that PID, with `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ`, after checking the exact `FlightSimulator2024.exe` basename and the same user. It reads every committed, readable, non-guard region in 16 MiB chunks and reports qwords holding each native address, occurrences of each name (ASCII and UTF-16), one level of references back to both, and name references that lie within 0x800 bytes of a texture reference, with annotated memory around them. Reading the whole address space pages memory in and can take tens of seconds. Matches show where to look; they do not prove an object layout. The report contains process-local addresses, so keep it local.
