# Camera entry contract observations

The captured code distinguishes **creating an entry with a new ID** from **rebinding an existing entry**. Creation can return an ID before setup completes. These observations describe internal data flow; they do not establish a complete C++ ABI, callable interface, ownership contract or rendering guarantee.

## Evidence and notation

All addresses below are **decimal RVAs** in the observed MSFS 2024 **1.8.16.0** main image. Its recorded `SizeOfImage` is 235963904 and PE timestamp is 1787653788. These observations are build-specific, and those metadata fields are not a cryptographic identity.

Evidence comes from saved, ignored artifacts now retained under `build/archives/source-layout/discovery/`:

| Artifact | Evidence used here |
| --- | --- |
| `msfs-1.8.16.0-camera-entry-state.json` | Creation range 17646976..17647554 and rebinding range 17644576..17645126; respectively 578 and 550 bytes, fully decoded with no errors |
| `msfs-1.8.16.0-camera-method-contexts.json` | Setup 17642240..17644563 and manager update 17648544..17652594 |
| `msfs-1.8.16.0-camera-view-setup.json` | Activation gate, view association methods, opaque-key lookup and material-handle routine |
| `msfs-1.8.16.0-explicit-view-setup.json` | Free-view index finder 66864720..66864902, flag helper 66802672..66802730, and refreshed view-association/projection prefixes |
| `msfs-1.8.16.0-camera-view-types.json` | Repeated view contexts plus exact `Node_Z`, `Camera_Z`, `Material_Z`, `Bitmap_Z` and diagnostic literals |
| `msfs-1.8.16.0-camera-renderer-bridge.json` | Creation caller 61358400..61360122, plus initial runtime-function ranges for container helper 17639168 and string helper 17641040 |
| `msfs-1.8.16.0-camera-resource-records.json` | Contiguous continuation ranges completing the observed container helper through 17639535 and string helper through 17641214 |
| `msfs-1.8.16.0-camera-descriptor-pose.json` | Complete descriptor initializer 17640240..17640370 and initial Node update range 66324928..66325017 |
| `msfs-1.8.16.0-camera-init-position.json` | Node update continuations 66325017..66325163 and 66325163..66325212; the request for 66325201 repeats the latter range |
| `msfs-1.8.16.0-camera-pose-types-and-removal.json` | Name-table growth/rehash 17647568..17648044; this routine is not entry removal |
| `msfs-1.8.16.0-camera-resource-cleanup.json` | ID-entry collection cleanup 57383776..57383948 and name-index cleanup 57381088..57381267 |
| `msfs-1.8.16.0-camera-record-create.json` | Complete entry-payload destructor 17640768..17641028 |
| `msfs-1.8.16.0-camera-destruction-callers.json` | Direct-call context from 17646000..17646961 into the payload destructor at instruction 17646904 |
| `msfs-1.8.16.0-camera-native-resource-and-erase.json` | Complete individual-entry erase routine 17646000..17646961 |
| `msfs-1.8.16.0-camera-renderer-create-cleanup-methods.json` | Four-word service metadata capture: renderer vtable+104 resolves to 70351808 |
| `msfs-1.8.16.0-camera-owner-and-viewport-release.json` | Initial renderer-release runtime-function range 70351808..70351844; it falls through beyond this capture |
| `msfs-1.8.16.0-camera-viewport-release-parts.json` | Three contiguous ranges covering renderer release 70351808..70351934, including both early-return branches |

Range ends are exclusive. A complete PE runtime-function range is not necessarily a complete logical method: the view range starting at 66825216 falls through and branches outside its own range. The container and string helper bodies below required combining contiguous captures before their full visible behavior could be described. This document does not interpret uncaptured continuations.

`M` means the manager pointer, `D` the creation descriptor pointer, `E` a collection entry, and `P` the view record used by setup. These are explanatory names, not recovered class definitions. Register arguments below are the values explicitly prepared or consumed in the captured code; they do not prove the full declared signature.

## Entry creation: 17646976

The routine retains incoming `RCX` as `M` and incoming `RDX` as `D`.

For the descriptor name, **dword `D+32 == 0` selects text beginning at `D`**; a **nonzero** value selects the pointer loaded from **qword `D+0`**. It tests the selected text's first byte. This establishes the branch condition, not a standard-library string type or its full storage/lifetime rules.

If the name is nonempty, the routine calls `17639952` with `RCX=M+128`, `RDX=&temporary`, and `R8=D`. A nonzero qword at `temporary+16` causes an immediate **RAX=0** return. This is consistent with rejecting a name already in the manager's name index. It does not return the existing entry's ID on that path.

For an accepted name, including an empty name, the routine:

1. Determines whether current global/context references permit immediate setup. In particular, a null global slot at RVA 173790384 clears that readiness condition.
2. **Increments qword `M+168`** at instruction 17647295.
3. Calls `17639168` with `RCX=M+88` and `RDX=&M+168`, retaining its returned entry pointer as `E`.
4. Populates `E` from `D` and stores the counter at `E+16`.
5. For a nonempty name, calls `17639536(M+128, D, &E+16)`, consistent with adding the name-to-ID association.
6. Calls setup `17642240(M, current counter)` only when its earlier readiness condition holds.
7. Returns **qword `E+16`**, without checking setup's completion byte `E+8`.

The incremented key and populated result establish a new-ID creation path, rather than an update of an existing entry selected by the caller. The captured caller does not check `17639168`'s returned pointer before writing fields. The counter's wraparound and synchronization rules are unknown.

Combining **17639168..17639369**, **17639369..17639515** and **17639515..17639535** reveals the container operation. Let `T=M+88`:

- A null bucket pointer at `T+8` or zero bucket count at `T+4` calls **17648048** with `max(256, old_bucket_count * 4)`, using the observed unsigned 32-bit operations.
- The helper hashes the supplied qword key, selects a bucket, compares entries' qword+0, and follows tagged +296 links. A matching key branches directly to the epilogue with **RAX equal to the existing entry**.
- For a missing key, it takes a reusable entry from **T+16** if present, advancing that list through entry+296 with the low bit cleared. Otherwise it calls **17641328(T+24)** to obtain an entry.
- It increments **DWORD T+0**, writes the supplied key to **qword E+0**, and calls **17640384(E+8)** to initialize the entry payload.
- It inserts the entry at the end of the observed bucket chain, or sets the bucket head when the chain is empty. If `T+0 > 4 * T+4`, it calls **17648048** again with `max(256, 4 * bucket_count)`.
- It returns **E in RAX**.

This proves lookup-or-insert behavior, including reuse-list handling, key storage, payload initialization and bucket linkage. The creation caller normally supplies its newly incremented counter key. The deeper allocation/reuse implementation, payload defaults in **17640384**, growth mechanics and failure behavior remain unverified.

**An entry is still created when immediate setup is unavailable.** A returned ID therefore establishes neither `E+8 == 1` nor a usable output texture. The manager update separately retries setup for entries whose completion byte is zero.

### One engine caller: 61358400

The 1722-byte captured caller retains incoming `RCX` as an owner `O` and incoming `RDX` as a request record `C`. No concrete class or configuration schema name for either object is established by this capture.

It obtains temporary name text through **61360256**, with a stack output address in `RCX` and the incoming request pointer still in `RDX`. It searches the owner's **656-byte-stride** record array at `O+2240`, with count at `O+2236`. Its existing-record name comparison also requires a nonnull field at record+648. These operations establish owner/request bookkeeping; they do not identify the record's public type or the executing thread.

The camera-entry branch has three visible conditions:

- DWORD **C+4 is not 2**; value 2 takes a separate earlier path.
- **Byte C+8 has bit 0x20 or 0x40 set**; neither bit selects a different path.
- Qword **C+440 is zero**; a nonzero value bypasses the new entry request.

At 61359636, it calls **17640240** with a stack descriptor address `D` in `RCX`. It then sets **D+44=1**, followed by one of these configurations:

| Request flags | Explicit descriptor construction |
| --- | --- |
| `C+8 & 0x20` | Writes **D+40=0** and copies sixteen bytes from **C+288..303** into **D+48..63** |
| Otherwise, `C+8 & 0x40` | Writes **D+40=1** and copies NUL-terminated text from the member at **C+304** into **D+64** |

Bit **0x20 takes precedence** if both bits are present. The mode-one source text is inline at `C+304` when DWORD **C+432 == 0**, otherwise it is loaded through qword **C+304**. The destination selects inline versus indirect storage through DWORD **D+192**, and the caller grows it when needed before copying and terminating the text. This supplies the immediate source of the mode-zero opaque key and mode-one name, but not the producer or semantics of those request fields.

There is **no explicit write to the descriptor's first name member after 17640240** in this caller. The initializer is now verified to leave that name empty, so this producer requests **unnamed entries** and bypasses creation's optional duplicate-name lookup and name-to-ID insertion. The earlier temporary request name does not become `D+0` in the observed path.

At 61359872, the caller loads a service/owner pointer from global slot RVA **173790440**, checks it for null, loads the cached manager pointer at **service+2496**, and checks that for null. At **61359903**, it calls **17646976(manager, D)** and stores returned **RAX at C+440**. The cache offset matches the earlier engine initialization observation, which placed the resolved manager at owner+2496. This confirms retrieval through that cache, not a lifetime or thread-safety guarantee.

The caller then passes the descriptor's indirect text buffers to **65127104** for cleanup when their selectors are nonzero: **D+64** through DWORD **D+192**, and **D+0** through DWORD **D+32**. These are temporary-descriptor cleanup operations. They do not remove the newly created manager entry. The caller continues owner-record bookkeeping through **61353136**, and returns a pointer into the owner's 656-byte-stride array rather than returning the camera-entry ID itself.

The camera branch uses direct calls and a stack descriptor. No explicit thread handoff, frame callback or per-entry deletion call is visible there. That observation does not establish which engine thread invokes the caller or what its surrounding lifecycle guarantees are.

### Observed descriptor fields

The complete initializer **17640240..17640370** explicitly writes these defaults and returns `D`:

| Field | Default written |
| --- | --- |
| BYTE `D+0` and DWORD `D+32` | Zero: empty inline first name |
| BYTE `D+64` and DWORD `D+192` | Zero: empty inline second name |
| DWORD `D+40` | Zero: mode zero |
| BYTE `D+44` | One |

It does **not** write the opaque key at `D+48..63`, clear the complete inline buffers, or initialize every intervening byte. The highest explicit field write covers **D+192..195**, establishing a minimum accessible extent of **196 bytes**, not a complete `sizeof`, alignment or packing contract. The observed mode-zero caller supplies the key afterward; its mode-one branch makes no explicit key write.

| Descriptor offset | Observed access and destination | Interpretation limit |
| --- | --- | --- |
| `D+0` | Inline name text when dword `D+32` is zero; otherwise qword pointer to name text | String representation is only partially observed |
| `D+32` | DWORD controlling that selection | Do not assume it is a length or capacity |
| `D+40` | DWORD copied to `E+24` at 17647312 | Selects distinct update paths, including observed values 0 and 1; no complete enum recovered |
| `D+44` | BYTE copied to `E+72` at 17647349 | Controls an additional setup flag operation described below |
| `D+48..63` | Sixteen bytes copied to `E+112..127` at 17647373 | Mode-zero code uses them as an opaque lookup key, not as a pose |
| `D+64` | Address passed to `17641040`, with destination `E+128` | Source storage selector is DWORD **D+192**; rebinding later reads text from `E+128` |

Creation also copies the selected first name text into `E+32` through `2650432`. The second text member's helper is now captured across **17641040..17641091**, **17641091..17641200**, and **17641200..17641214**. It selects inline source/destination text when the respective DWORD+128 is zero, otherwise selects the indirect pointer from qword+0. Equal selected text pointers return the destination member unchanged.

For distinct pointers, the helper measures the NUL-terminated source length. It uses **128** as the inline destination capacity, or the nonzero destination DWORD+128 as the capacity. When the measured length is not smaller than that capacity, it calls **4228864(destination member, length, R8D=0)**. It then reselects destination storage, calls **122292176(destination text, source text, length)**, explicitly appends a NUL byte, and returns the destination member address in RAX. This is a text-copy operation, rather than retaining the descriptor's source text pointer. Growth allocation/failure semantics and the complete member initialization contract remain unknown.

Unlisted descriptor bytes, total size, alignment, complete destruction requirements and ownership are not established. The initializer's explicit field defaults do not establish all of those requirements; this table remains insufficient to manufacture a descriptor for an external call.

### The 128-bit value is an opaque key

In mode zero, update passes `E+112` to `31727952`. That routine compares the input's four DWORDs with candidate fields at offsets 72, 76, 80 and 84. A match selects the corresponding record from another array with a **96-byte stride**; failure returns null. The update code consumes three-double vectors at returned offsets 0, 24 and 48.

Consequently, `D+48..63` identifies another engine record in this path. It is not evidence for directly supplying position, orientation, a matrix or a UUID. The key producer, record lifetime and coordinate conventions remain unknown. Mode one's separate plane/projection calculations do not supply a validated descriptor contract either.

## Setup state and the `E+72` flag

Setup `17642240` looks up `E` by key in the collection whose bucket metadata is at `M+92/+96`. It returns early for a missing entry or nonzero completion byte `E+8`, and checks additional global/context conditions. On its normal path it eventually writes **byte `E+8=1` at 17644330**, after output/reference setup and the mode-one callback registration sequence.

### Existing view capacity: 66864720

The fresh **66864720..66864902** capture identifies a **free-index finder over eight existing view slots**, not a view allocator. It obtains the cached renderer from global RVA **173790384**, with the existing lazy-service wrapper path if needed, and examines the pointers in **renderer+2752** at indices **0 through 7**.

For each `P`, it inspects the association handle at **P+72**. A null control pointer, a mismatch between **DWORD P+80** and **DWORD control+28**, or a null control payload at **control+0** makes that slot available. The routine returns the first such index in EAX; if all eight associations are valid, it returns **-1**. It does not consume its incoming RCX as a world/container argument, allocate a view, expand the pool, reserve a slot, or register an association. It assumes the existing pool and its eight view pointers can be dereferenced.

Setup calls the finder at **17642687**, sign-extends its result at **17642692**, and indexes **renderer+2752** at **17642714** with **no intervening -1 check**. A future experiment must therefore check capacity in the same engine execution phase before requesting creation, and must refuse an exhausted pool. An earlier asynchronous capacity snapshot would not close this unchecked path. The finder itself supplies no reservation or synchronization guarantee.

The selected slot becomes occupied according to that predicate only later, when setup calls **66823664(P, &worldNodeHandle)** at **17643819**. That method replaces the association at **P+72** and calls the new associated object's virtual slot **+160** with the view ID from **P+64**; it uses virtual slot **+168** for the previous association. Those calls are registration/unregistration candidates, but their complete semantics and per-frame renderer consumption remain unverified. The created camera's Node is separately associated at **P+104** through **66823440**. These operations do not establish a new view allocation or a rendering guarantee.

### Setup flag operations

The flag copied from `D+44` has these exact visible effects in setup:

- At 17642882, setup calls `66802672(P, &temporary)` after constructing a two-qword argument from image slots 176053056 and 176053064, OR-ing the first qword with `0x1000200020` (bits 5, 21 and 36; the [architecture notes](architecture.md) describe bit 36's effect on the per-view render driver).
- At 17642887, it ANDs qword `P+48` with `0xffffffffbffef2ff`, clearing bits selected by `0x40010d00`.
- If **byte `E+72 != 0`**, instructions 17642901..17642919 copy sixteen bytes from static RVA **130434112** and call `66802672(P, &copy)` again.

The fresh **66802672..66802730** capture establishes the helper's flag-combination operation exactly: it **ORs argument qword+0 into P+48 and argument qword+8 into P+56**. It clears neither flag word. If **bit 60 of the input first qword** is set, it additionally loads the double at image RVA **173800016**, calls **69685968**, and stores the returned float at **P+136**. The callee's meaning and this side effect's purpose remain unknown. No registration or render dispatch is visible in the helper itself.

The numeric contents and meanings of the copied masks are not established here. In particular, **D+44 is not an established activation flag** and is not proven to select mirror mode, scenery inclusion or aircraft inclusion. Its observed initializer default is one; that is evidence of the existing caller's configuration, not a complete flag contract.

### Mode values other than zero and one

Setup checks **E+24 == 1** at instructions **17642600** and **17644014**. Other values bypass mode one's extra service/callback steps while continuing the shared view, Node_Z, Camera_Z and material setup, subject to the common checks.

Manager update makes a separate decision at **17649647..17649661**: zero branches to **17651999**, the opaque-key pose path; one enters the plane/projection path; all other values branch to **17652194** and then common temporary cleanup. Thus **mode 2 skips both observed pose-update branches**. Shared work before that dispatch, including view-dimension updates and reference checks, can still occur.

The common post-dispatch path releases a temporary handle; it contains no shared pose setter or view-projection refresh call. The **66825216** call at **17650439** belongs to mode one's branch. Setup calls that helper once at **17643827**, but the captured code does not establish an external caller's required refresh or rendering schedule.

This makes mode 2 a candidate for an explicitly controlled pose experiment that avoids the mode-zero key lookup, not a demonstrated supported custom mode. Creation with that mode, direct Node/Camera pose controls and activation remain **uninvoked** by this work. A future experiment would need verified setup completion, an owned valid view slot, the appropriate engine execution phase and established pose conventions before using those controls. The observed request caller constructs only modes zero and one. Camera_Z defaults, other engine writers and per-frame render scheduling remain unverified.

### View dimensions

Setup loads the primary view's pair at offsets **+16/+20** at instruction **17642788** and copies it into the selected view's **+16/+20**, **+24/+28** and **+32/+36** through **17642837**. No captured descriptor field selects a per-entry resolution.

Manager fields **M+80/+84 cache the primary-view dimensions**; they are not established custom-resolution controls. Update refreshes them from the primary view at **17649284/17649292** and marks the dimension change at **17649296**. For a ready entry, that change causes the cached pair to be copied into its view's three dimension pairs at **17649490..17649529**, before mode dispatch. Mode 2 receives this conditional synchronization but bypasses mode one's additional dimension writes and **66825216** refresh. Changing M+80/+84 externally would conflict with their observed cache role; independently sized outputs and correct projection refresh remain unresolved.

### Node position update: 66324928

The setup and update paths call **66324928** with a resolved Node reference in `RCX` and a three-component vector address in `RDX`. Let those values be `N` and `Q`. Combining the initial range and its two continuations now covers **66324928..66325212: 284 distinct bytes**, including the return path.

If **qword N+368 is null**, the routine compares qwords **Q+0, Q+8 and Q+16** with **N+112, N+120 and N+128**. These are bitwise comparisons, not floating-point equality tests. If all three match, it returns without the later flag writes. Otherwise it copies **24 bytes from Q into N+112..135** and proceeds to those flag writes.

If **P=*(N+368) is nonnull**, the routine calls **66319248** with `RCX=P`; `RDX` still contains `Q`, although the callee's complete signature is not established. Treat its returned pointer as `T`. It reads the input as doubles `x=Q[0]`, `y=Q[8]`, `z=Q[16]`, applies the following affine transform, and stores the resulting three doubles at **N+112, N+120 and N+128**:

| Stored component | Arithmetic observed; all `T[...]` indices are byte offsets containing doubles |
| --- | --- |
| X | `x*T[0] + y*T[32] + z*T[64] + T[96]` |
| Y | `x*T[8] + y*T[40] + z*T[72] + T[104]` |
| Z | `x*T[16] + y*T[48] + z*T[80] + T[112]` |

The transform branch does not compare the transformed result with the previous stored value before flagging it. At **66325163**, both that branch and the changed direct-copy branch **OR 0x10 into qword N+56**. They then follow the linked chain beginning at **N+368**, **OR 1 into each linked object's qword+56**, and advance through that object's +368 until null.

This strongly supports a parent/ancestor relationship and dirty-flag propagation. The exact meanings of the flags, whether `66319248` supplies an inverse transform, and the world/local coordinate direction remain unverified. No homogeneous divide or independent orientation update occurs in this routine. The initial equality shortcut is available only when N+368 is null; an unchanged transformed position still follows the flagging path. These complete instructions establish the operation, but not its required engine thread, hierarchy invariants, rendering phase or permission for external invocation.

## Existing-entry rebinding: 17644576

This routine explicitly consumes `RCX=M`, `RDX=entry lookup key`, and a source pointer in `R8`. It searches the existing `M+92/+96` hash collection, comparing the key against qword `E+0` and following `E+296` links with their low tag bit cleared. A missing entry returns without insertion or counter changes.

For a found entry, it performs the following operations:

1. Builds a temporary reference from **incoming source+400** through `66273728`. The source's concrete type is unknown.
2. Resets the reference at **E+264**, using `66271872` followed by `66273728` with source address RVA 175997848.
3. Reads text beginning at **E+128** when dword **E+256 == 0**; otherwise loads the text pointer from qword **E+128**. It hashes the text with the same table-based 64-bit hash used elsewhere in setup.
4. Calls **66311568** with `RCX=resolved source`, `RDX=&text hash`, `R8=E+264`, and **R9B=1**. The callee's full signature and the flag's meaning are unknown.
5. Tests the resulting reference: qword `E+264` must be nonnull, its control record's DWORD+28 must match DWORD `E+272`, and its payload qword+0 must be nonnull.
6. For a valid reference, calls `17641776(M, E+16, true)` and stores **E+280=1**. Otherwise it calls that routine with `false` and stores **E+280=0**.

Thus this routine replaces an existing entry's named-object association and adjusts an activation gate. **E+280 records the reference-validation outcome**; its value is stored independently of the activation call's return. The rebinding routine's final cleanup calls also mean its own return register is not established as a boolean result.

There is no `E+8` readiness check in this routine. Its caller must therefore supply additional lifecycle invariants that have not yet been recovered.

### Activation gate: 17641776

This helper finds an existing entry, uses signed DWORD **E+76** to select `P` from the engine's view pool, and returns AL=1 after either visible branch. A nonzero input flag directly clears **bit zero of qword `P+48`** and performs no other visible update to the view. A zero flag instead calls `66802672(P, &copy)` using sixteen bytes from static RVA **130434096**, so its operation is the OR-mask behavior and optional bit-60 side effect described above. Missing global/context or entry returns AL=0.

The helper does not check **E+8** or visibly validate the selected pool index. A future caller must require completed setup and a valid, owned **E+76** view index before requesting activation. Its direct flag operation supports an activation/visibility-gate interpretation; neither AL=1 nor clearing bit zero proves that a rendered view becomes available immediately.

## Teardown evidence and remaining removal path

The captured manager method **57390544..57390617**, table slot three, has a deleting-destructor pattern. It retains incoming `RCX=M` and the low 32 bits of incoming `RDX`, calls **57381088(M+128)** to clean the name index, calls **57383776(M+88)** to clean the ID-entry collection, then calls **70016608(M)**. When bit zero of the retained incoming flag is set, it calls **65116208** with `RCX=M` and **EDX=176**. It returns `M`. This is whole-manager destruction, not an operation to remove one ID.

The newly captured collection routines establish a more specific cleanup path:

| Routine and incoming argument | Observed sequence |
| --- | --- |
| **57383776**, `RCX=T=M+88` | Walks bucket pointer `T+8` and count `T+4`; for each live entry calls **17640768 with RCX=E+8**; follows tagged **E+296** links; calls **57416928(T+24, DL=1)**; frees nonnull bucket storage `T+8` through **65127104**; calls **57416928(T+24, EDX=0)** |
| **57381088**, `RCX=T=M+128` | Walks its buckets; frees each entry's indirect text pointer through **65127104** only when DWORD **entry+32 != 0**; follows tagged **entry+48** links; calls **57415632(T+24, DL=1)**; frees nonnull bucket storage `T+8`; calls **57415632(T+24, EDX=0)** |

This establishes **17640768(E+8)** as the live-entry payload cleanup call made during whole-manager destruction. The allocator/pool helper interpretations for 57416928 and 57415632 follow their argument location and call order; their complete behavior is not yet captured here. The observed collection cleanup does not independently establish how pending entries are cancelled or unlinked during normal operation.

The complete **17640768..17641028** payload destructor consumes `RCX=Q=E+8` and releases members in this order:

| Order | Entry-relative field and observed operation |
| --- | --- |
| 1 | **E+288** token: when nonnull, atomically subtracts one from DWORD token+0; when the previous count is one, calls **4086784(token+8)**, calls **65116208(token, EDX=80)** and clears E+288 |
| 2 | **E+264** association handle: passes its address to **66271872** three times, then **66251296** |
| 3 | **E+128** second text member: if DWORD **E+256 != 0**, passes its indirect pointer to **65127104** |
| 4 | **E+96** Node handle: passes its address to **66271872** three times, then **66251296** |
| 5 | **E+80** material handle: passes its address to **66271872** three times, then **66251296** |
| 6 | **E+32** first text member: if DWORD **E+64 != 0**, passes its indirect pointer to **65127104** |

The token field is not explicitly cleared when its prior reference count is greater than one; the payload is being destroyed. The repeated handle-helper calls are recorded literally because their complete reference/deletion semantics are not established here. This body has no explicit viewport-pool release, hash unlink or entry-storage free operation; treating it alone as the complete entry-removal API would omit those responsibilities.

### Individual-entry erasure: 17646000

The complete **17646000..17646961** routine consumes **RCX=M** and **RDX=the qword entry ID by value**. It hashes that ID, searches the collection at **M+92/+96**, compares against qword **E+0**, and follows tagged **E+296** links. Missing bucket storage, a zero bucket count or a missing entry returns without removal.

After finding an entry, it tests global cache slot **173790384** at instruction **17646203**. **A null cache causes an immediate return without erasing the entry, including when E+8 is zero.** It does not provide unconditional pending-entry cancellation during renderer teardown.

For an entry whose completion byte **E+8 is nonzero**, it performs these additional operations before unlinking anything:

1. Obtains the renderer through **4164736(wrapper at 173790320)** and selects `P` from renderer+2752 using signed DWORD **E+76** as the pointer-array index. No explicit index bounds check is visible in this routine.
2. Builds a temporary reference from static source **175997848** through **66273728**, then calls **66823664(P, &temporary)**. That association method replaces the handle at **P+72**, including calls concerning the previous association.
3. Repeats that temporary-reference construction and calls **66823440(P, &temporary)**, replacing the handle at **P+104**. The temporary references are then released through the observed handle helpers. This sequence is consistent with detaching the view's associations; the source record's contents and all helper side effects are not independently established by this capture.
4. Obtains the renderer again and invokes the method at **byte offset 104 of its vtable**, with **RCX=renderer** and **RDX=P**, at instruction **17646438**. The subsequent four-word service metadata capture resolves that target to **70351808**, through vtable **134482536**, pointer slot **134482640**. It marks and queues the view as described below; eventual destruction and GPU completion guarantees remain unverified.

When **E+8 is zero**, it skips those view operations and continues with erasure, provided the renderer cache check passed. The common erasure sequence is:

1. Selects the first name at **E+32**, inline when DWORD **E+64 == 0**, otherwise through its indirect pointer. For a nonempty name, it calls **17639952** with **RCX=M+128**, a temporary lookup output in **RDX**, and **R8=E+32**.
2. If that name-index entry exists, decrements its table count, unlinks it from the **+48** chain, frees its indirect key text when its DWORD+32 selector is nonzero, and pushes the name-index entry onto the table's reuse list at **M+144** with a tagged +48 link. A missing name-index entry does not prevent the subsequent ID-entry erasure.
3. Decrements **DWORD M+88** at **17646761** and unlinks `E` from its bucket or predecessor's **+296** link, preserving the link tag.
4. Calls **17640768(E+8)** at **17646904** to destroy the payload in the order listed above.
5. Pushes the entry onto the ID collection's reuse list: writes a tagged link to **E+296** and stores **E at M+104** at **17646933**. The entry allocation is retained for reuse rather than explicitly freed on this path.

This establishes individual ID erasure, its view-operation ordering, name-index cleanup and payload destruction. It does **not** establish a return-status contract: the routine has no explicit boolean result, and early returns leave incidental register values. It also contains no visible collection lock or renderer fence. Permitted caller/thread phase, behavior of the renderer vtable method, outstanding callback execution and safe GPU-resource retirement still require their own evidence. Calling the payload destructor alone would omit the view operations and both collection updates.

### Renderer release queue: 70351808

The initial **36-byte runtime-function range 70351808..70351844** was insufficient by itself: it falls through, and both early-return branches target **70351929**. Combining the later **85-byte range 70351844..70351929** and **five-byte range 70351929..70351934** now covers **126 distinct bytes**, including the return path.

With incoming `RCX=renderer` and `RDX=P`, the method returns immediately for a null `P` or nonzero **BYTE P+23688**. Otherwise it writes **P+23688=1**, sign-extends **DWORD P+64**, and selects the corresponding pointer slot in the array at **renderer+2752**. The pointer eventually appended is loaded from that pool slot, rather than taken directly from the original `P` argument. No explicit index check or comparison between the pool pointer and `P` is present here.

Let `Q=renderer+2784`. The remaining operations are:

1. If **DWORD Q+0 == 0**, call **70350304** with **RCX=Q** and **EDX=DWORD Q+4 + 32**, then write **Q+0=32**. This is consistent with growing the queue by 32 slots; the helper's allocation and failure behavior are not captured here.
2. Load the selected view-pool pointer and store it at **`*(Q+8) + 8 * DWORD(Q+4)`**.
3. Increment **DWORD Q+4** and decrement **DWORD Q+0**, then return.

These operations establish deferred queueing with a duplicate-request flag. The method does **not** clear the view-pool slot, free `P`, release its members or wait for a GPU fence. Removing the manager entry therefore does not prove that the queued view has completed retirement. The consumer captured below establishes a later cleanup phase; its thread synchronization, stale pool indices and allocation-failure handling are not established by these routines alone.

### Renderer release consumer: 70350848 (`0x4317800`)

A subsequent read-only capture from the same verified MSFS image covers the complete **70350848..70351796** routine (948 bytes), selected by **renderer vtable+120**. Its first loop consumes a second queue at **renderer+2768**, with count at **+2772** and data pointer at **+2776**. For each queued `P`, it reads the diagnostic index from **P+64**, replaces both view associations through the same association helpers, clears a flag in **P+48**, and calls another view helper with a zero second argument. It then clears **BYTE P+23688** at RVA **70351119** (`0x431790f`).

Only diagnostic indices **27 and above** proceed through pool-slot clearing and the destructor/free-list path. The eight slots used by the camera availability finder retain their `P` objects. Clearing the marker therefore denotes completion of this view-cleanup phase, not destruction of those pooled objects or a GPU-fence guarantee.

After the loop, the routine adds the consumed count back to spare capacity, zeros that count, and swaps both complete 16-byte queue headers at **renderer+2768** and **renderer+2784**. Newly enqueued releases therefore reach the consumer through a buffer swap. An empty enqueue queue alone does not establish retirement; its entries may be in the other buffer. There is also an interval after a view's marker is cleared but before its queue entry is removed from the published count.

The allocation finder examines association availability, without testing the release marker or either queue. A detached view can thus be the first native-free slot while pending cleanup would subsequently clear newly rebound associations. Creation admission must preserve the native finder's ordering and establish that the first required free slots have a clear marker and appear in neither queue. Selecting later clean slots in a separate count cannot change which slots native creation will actually use. Observations must be bounded and reread; these inspections detect changes rather than creating a lifetime reservation. The consumer capture establishes this reuse hazard, not that it was the exact cause of a particular simulator crash.

By comparison, **17647568..17648044** consumes a table in `RCX` and a new bucket count in `EDX`. It writes the count at table+4, resizes the table+8 storage to eight bytes per bucket through **65127280**, zeroes the added bucket region through **122292192**, hashes existing strings through **2520832**, and relinks entries through **+48**. Those operations identify name-table growth/rehashing. They do not destroy an entry payload, erase one ID or release a view.

The manager update's failure and completion paths also do not supply removal: the calls to **66271872** and **66251296** near **17651951..17652505** receive addresses of temporary stack handles, then iteration continues by reading **E+296**. The captured update does not decrement the entry count or unlink those entries. The creation caller's post-call **65127104** operations similarly free temporary descriptor strings, as described above.

Mode-one setup exposes the same token-release pattern later seen in the payload destructor: replacing **E+288** releases the previous pointer with an atomic reference-count decrement. If the previous count was one, it calls **4086784(old_pointer+8)** and **65116208(old_pointer, EDX=80)**. This is consistent with releasing a callback/connection token. Its disconnect behavior and synchronization with a callback already executing remain unverified. It is not permission to free E+288 directly or to substitute activation for removal.

## Known producer and output chain

The recovered chain is an entry request, deferred or immediate setup, internal object association, and a material handle:

| Stage | Captured evidence |
| --- | --- |
| Request producer | `61358400` configures a temporary descriptor from request flags and fields, retrieves the cached manager from service+2496, and saves the returned ID at request+440 |
| Entry request | `17646976` obtains a fresh counter key, populates `E`, and optionally calls setup |
| Setup objects | `17642240` hashes verified type literals **Node_Z** at 129524228 and **Camera_Z** at 129524296, then calls `66843280` with the type hash and formatted camera-to-texture label hash |
| Type resolution/creation | `66843280` resolves a signed 16-bit type index, returns an empty 16-byte result for a negative index, otherwise delegates to `66257872` using the registered type record |
| View associations | The Node_Z reference is associated with `P` and stored at **E+96**; the view's diagnostic ID, DWORD **P+64**, is copied to **E+76** |
| Entry output reference | `66809728(P)` returns **the address `P+144`**, an embedded material handle; setup copies that reference into **E+80** |
| Material and slots | The routine lazily creates **Material_Z** under `ViewportRenderedMaterial VP%d`, then creates/associates **Bitmap_Z** references for diagnostic slots listed below |

The bitmap slot offsets are relative to the **resolved material object**, not the entry or view:

| Diagnostic name | Material handle offset | Slot |
| --- | ---: | ---: |
| `VIEWPORT_MATERIAL_DIFFUSE VP%d` | 520 | 0 |
| `VIEWPORT_MATERIAL_DEPTH_STENCIL VP%d` | 712 | 12 |
| `VIEWPORT_MATERIAL_ADD_DIFFUSE VP%d` | 664 | 9 |

These references use control-record/generation checks. Neither **E+80** nor the return of **66809728** is a raw bitmap or `ID3D12Resource` pointer. The observed construction path does not prove a completed render or usable GPU contents.

## Contracts still required for implementation

- **Request schema and input ownership:** concrete owner/request types, the caller of 61358400, complete descriptor size/alignment/destruction requirements, valid mode/flag combinations, and the upstream source of request+288 and request+304. One producer and its descriptor's explicit defaults are now observed; their complete contract is not.
- **Thread and frame phase:** where creation, rebinding and removal are permitted; synchronization around the manager counter, containers and view pool.
- **Removal and cancellation:** individual erasure, payload destruction, renderer vtable+104 queueing and the vtable+120 consumer are now captured. Their permitted caller/thread phase, synchronization with consumers, callback disconnection completion, reference-helper semantics and GPU retirement still require independent evidence. Erasure refuses to proceed when the renderer cache is null, even for a pending entry; a shutdown cancellation path is not established.
- **Camera updates:** usable position/orientation/projection inputs, coordinate systems, timing and preservation of scenery/aircraft visibility for two independent taxi views.
- **Rendering:** which stage schedules and completes each view, whether the output includes the complete required scene, and behavior when the main camera does not see taxi-camera geometry.
- **GPU handoff:** conversion from an engine Bitmap_Z reference to a supported native resource, lifetime, format, resource states, fences and the point at which PFD sampling is safe.
- **Failure semantics:** allocation failure, partial setup rollback, stale-key handling, scene transitions, device loss and build changes.

The separate calibration diagnostic demonstrates writes to the upper PFD region. It does not supply an engine camera view or resolve these remaining contracts.
