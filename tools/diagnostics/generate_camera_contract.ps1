param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Artifacts,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$ImageCapture,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$StoreArchive,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$MotionArchive,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$IdentityEvidence,
    [string]$Output = (Join-Path $PSScriptRoot '../../src/camera/camera_contract_model.cpp'),
    [string]$Receipt
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Artifacts = [IO.Path]::GetFullPath($Artifacts)
if (!$Receipt) { $Receipt = Join-Path $Artifacts 'generated-contract-receipt.json' }
if (!(Test-Path -LiteralPath $IdentityEvidence -PathType Leaf)) { throw 'An explicit, existing static identity evidence file is required.' }
$inputs = [Collections.Generic.Dictionary[string,string]]::new()
function Read-Input([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $inputs[$full] = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    return [IO.File]::ReadAllText($full)
}
function Read-Json([string]$Path) { return (Read-Input $Path | ConvertFrom-Json) }
function Bytes-Literal($Bytes) {
    return '{' + (($Bytes | ForEach-Object { '0x{0:x2}' -f $_ }) -join ',') + '}'
}
function Has-Property($Node, [string]$Name) { return $null -ne $Node.PSObject.Properties[$Name] }
$evidence = Read-Json (Join-Path $Artifacts 'profile-evidence.json')
$indexed = @(Read-Json (Join-Path $Artifacts 'profile-image-indexed-operand-slots.json'))
$capture = Read-Json $ImageCapture
if (!$capture.read_only -or !$capture.target_verified -or !$capture.inventory.complete -or !$capture.inventory.relocations_valid -or
    !$capture.inventory.runtime_functions_valid -or !$capture.inventory.image.valid) { throw 'Image capture is incomplete or unverified.' }
if ($evidence.required_ranges -ne 31 -or $indexed.Count -ne 24) { throw 'The reviewed profile evidence is incomplete.' }
$sections = $capture.inventory.image.sections
$first = Read-Json (Join-Path $Artifacts 'targeted-capture-01.json')
$second = Read-Json (Join-Path $Artifacts 'targeted-capture-02-code.json')
if (!$first.read_only -or !$first.target_verified -or !$second.read_only -or !$second.target_verified) { throw 'Targeted captures are unverified.' }
foreach ($part in @($first, $second)) {
    if ($part.timestamp -ne $capture.inventory.image.timestamp -or $part.image_size -ne $capture.inventory.image.size) {
        throw 'Offline generation inputs refer to different observed image layouts.'
    }
}
$requests = @($first.requests) + @($second.requests)
$ranges = @($evidence.ranges) + @([pscustomobject]@{
    store_rva = 57402512; steam_rva = 57413504; bytes = 132
    decoded_path = 'targeted-comparison-02/57402512.selected-decoded.json'
    store_instructions_path = 'targeted-comparison-02/57402512.store-normalized.json'
})
# These numbers name reviewed input observations, not runtime lookup gates.
# Generated code contains only symbolic roles and relative operand offsets.
$functionRoles = [ordered]@{
    initialize_descriptor = 17640240; activate_entry = 17641776; create_entry = 17646976
    erase_entry = 17646000; set_position = 66324928; set_up = 66859376
    set_target = 66859344; set_fov = 66859296; update_view = 66825216
    refresh_output = 66809728; manager_update = 17648544
}
$layout = @(Read-Json (Join-Path $Artifacts 'image-layout-mapping.json'))
$identity = $null
$identityBounds = $null
$identityLiterals = $null
if (Test-Path -LiteralPath $IdentityEvidence) {
    $identity = Read-Json $IdentityEvidence
    if ($identity.schema -ne 1) { throw 'Unknown static identity evidence schema.' }
    $identityCapture = Read-Json (Join-Path $Artifacts $identity.capture_path)
    $identityBounds = Read-Json (Join-Path $Artifacts $identity.bounds_path)
    $identityLiterals = Read-Json (Join-Path $Artifacts $identity.literals_path)
    if (@($identity.bindings | Where-Object name -eq 'scene_model_vtable').Count) {
        if (!(Has-Property $identity 'live_proof_path') -or !(Has-Property $identity 'proof_reader_path')) { throw 'Attached model identity lacks live graph provenance.' }
        $liveProof = Read-Input (Join-Path $Artifacts $identity.live_proof_path)
        [void](Read-Input (Join-Path $Artifacts $identity.proof_reader_path))
        $node = @($identity.bindings | Where-Object name -eq 'scene_node_vtable')
        $model = @($identity.bindings | Where-Object name -eq 'scene_model_vtable')
        if ($node.Count -ne 1 -or $model.Count -ne 1 -or
            $liveProof -notmatch ('read_only=true analyst_selected_roots=true native_calls=false process_id={0} graph_valid=1 aircraft_present=1 component_present=1 stage=complete graph_error= scene_complete=1 scene_error= scene_read_bytes=\d+ node_vtable_rva={1} model_vtable_rva={2} ' -f $identityCapture.process_id, $node[0].steam_rva, $model[0].steam_rva)) {
            throw 'Attached model identity was not proved through the complete live scene graph.'
        }
    }
    if (!$identityCapture.read_only -or !$identityCapture.target_verified -or !$identityBounds.read_only -or !$identityBounds.target_verified -or
        !$identityLiterals.read_only -or !$identityLiterals.target_verified -or $identityCapture.process_id -ne $identityBounds.pid -or
        $identityCapture.timestamp -ne $capture.inventory.image.timestamp -or $identityCapture.image_size -ne $capture.inventory.image.size -or
        $identityLiterals.timestamp -ne $identityCapture.timestamp -or $identityLiterals.image_size -ne $identityCapture.image_size) {
        throw 'Static identity evidence is incomplete or refers to different images.'
    }
    $requests += @($identityCapture.requests)
    if (Has-Property $identity 'extra_code_captures') {
        foreach ($path in $identity.extra_code_captures) {
            $extra = Read-Json (Join-Path $Artifacts $path)
            if (!$extra.read_only -or !$extra.target_verified -or $extra.timestamp -ne $identityCapture.timestamp -or $extra.image_size -ne $identityCapture.image_size) { throw 'Aircraft class code evidence is unverified.' }
            $requests += @($extra.requests)
        }
    }
    if (Has-Property $identity 'extra_bounds_paths') {
        foreach ($path in $identity.extra_bounds_paths) {
            $extra = Read-Json (Join-Path $Artifacts $path)
            if (!$extra.read_only -or !$extra.target_verified -or $extra.pid -ne $identityCapture.process_id) { throw 'Aircraft class function bounds are unverified.' }
            $identityBounds.functions += @($extra.functions)
        }
    }
    $ranges += @($identity.ranges)
    $indexed += @(Read-Json (Join-Path $Artifacts $identity.indexed_proof_path))
    foreach ($binding in $identity.bindings) {
        if ($binding.name -notin @('scene_node_vtable', 'scene_model_vtable', 'aircraft_facade_vtable', 'aircraft_controller_vtable', 'aircraft_selected_vtable') -or !$binding.evidence) { throw 'Unreviewed identity binding role.' }
        if ($binding.name.StartsWith('aircraft_')) {
            $field = $binding.name.Replace('aircraft_', '') + '_rva'
            if (!$liveProof -or $liveProof -notmatch ('(?:^|\s){0}={1}(?:\s|$)' -f $field, $binding.steam_rva)) { throw 'Aircraft static class identity lacks a matching rechecked live graph.' }
        }
        $role = @($layout | Where-Object name -eq $binding.name)
        if ($role.Count -ne 1 -or $role[0].steam_rva -or $role[0].store_rva -ne $binding.store_rva) { throw 'Conflicting identity binding.' }
        $role[0].steam_rva = $binding.steam_rva
    }
}
$roleNames = @{}
foreach ($entry in $functionRoles.GetEnumerator()) { $roleNames[[long]$entry.Value] = $entry.Key }
foreach ($entry in $layout | Where-Object steam_rva -gt 0) {
    if (!$roleNames.ContainsKey([long]$entry.store_rva)) { $roleNames[[long]$entry.store_rva] = $entry.name }
}
$roleNames[[long]57402512] = 'manager_constructor'
$roleNames[[long]17642240] = 'setup_entry'
$roleNames[[long]57971319] = 'manager_initialization_window'
$symbols = [Collections.Generic.List[object]]::new()
$byAddress = @{}
function Section-Kind([long]$Rva, [long]$Extent) {
    if ($Rva -eq 0) { return 'image_base' }
    $matches = @($sections | Where-Object { $Rva -ge $_.rva -and $Rva + $Extent -le [long]$_.rva + $_.size })
    if ($matches.Count -ne 1) { throw "No unique captured section for target $Rva+$Extent" }
    [long]$flags = $matches[0].flags
    if (($flags -band 0x40000000L) -eq 0 -or ($flags -band 0x02000000L)) { throw 'Unreadable or discardable target.' }
    if ($flags -band 0x20000000L) {
        if ($flags -band 0x80000000L) { throw 'Writable executable target.' }
        return 'code'
    }
    if ($flags -band 0x80000000L) { return 'writable_data' }
    return 'read_only_data'
}
function Symbol([long]$Rva, [string]$Name, [long]$Extent = 1) {
    if ($byAddress.ContainsKey($Rva)) {
        $id = $byAddress[$Rva]
        $symbols[$id].extent = [Math]::Max($symbols[$id].extent, $Extent)
        if ((Section-Kind $Rva $symbols[$id].extent) -ne $symbols[$id].kind) { throw 'Inconsistent target section.' }
        return $id
    }
    $id = $symbols.Count
    if (!$Name) { $Name = if ($Rva -eq 0) { 'image_base' } else { 'reference_' + $id.ToString('D3') } }
    $kind = Section-Kind $Rva $Extent
    $symbols.Add([pscustomobject]@{ name = $Name; kind = $kind; extent = $Extent; rva = $Rva })
    $byAddress[$Rva] = $id
    return $id
}
$code = [Collections.Generic.List[object]]::new()
foreach ($range in $ranges) {
    $name = if (Has-Property $range 'semantic_name') { $range.semantic_name } elseif ($roleNames.ContainsKey([long]$range.store_rva)) { $roleNames[[long]$range.store_rva] } else { 'code_' + $code.Count.ToString('D2') }
    $id = Symbol $range.steam_rva $name $range.bytes
    if ($symbols[$id].kind -ne 'code') { throw 'Template is outside static code.' }
    $decodedPath = Join-Path $Artifacts $range.decoded_path
    $decoded = Read-Json $decodedPath
    $crossBuild = $range.store_rva -ne 0
    $normal = if ($crossBuild) { @(Read-Json (Join-Path $Artifacts $range.store_instructions_path)) } else {
        if (!(Has-Property $range 'evidence_kind') -or $range.evidence_kind -ne 'steam_static_identity_chain') { throw 'No reviewed instruction provenance.' }
        @($decoded.instructions | ForEach-Object { [pscustomobject]@{rva=$_.rva;offset=$_.rva-$range.steam_rva;size=$_.size;text=($_.text.Trim()-replace '\s+',' ')} })
    }
    $binPath = if (Has-Property $range 'binary_path') { Join-Path $Artifacts $range.binary_path } else { $decodedPath.Replace('.selected-decoded.json', '.selected.bin') }
    $inputs[[IO.Path]::GetFullPath($binPath)] = (Get-FileHash -LiteralPath $binPath -Algorithm SHA256).Hash
    [byte[]]$bytes = [IO.File]::ReadAllBytes($binPath)
    if ($decoded.begin -ne $range.steam_rva -or $decoded.end -ne $range.steam_rva + $range.bytes -or $bytes.Length -ne $range.bytes -or
        $decoded.instructions.Count -ne $normal.Count) { throw 'Decoded/template extent mismatch.' }
    $source = @($requests | Where-Object { $_.kind -eq 'code' -and $_.rva -eq $range.steam_rva -and $_.size -ge $range.bytes })
    if ($source.Count -ne 1 -or [Convert]::ToHexString($bytes) -cne $source[0].hex.Substring(0, 2 * $bytes.Length).ToUpperInvariant()) {
        throw 'Selected binary differs from its verified capture.'
    }
    # The seven-byte aircraft_controller_method is not an independent seed.
    # Vtable and caller relations select that copy, so regeneration keeps 8.
    $code.Add([pscustomobject]@{ symbol = $id; range = $range; bytes = $bytes; decoded = $decoded.instructions; store = $normal; cross_build=$crossBuild; operands = [Collections.Generic.List[object]]::new(); minimum_seed = 8 })
}
# Bind every reference into another reviewed range to that range's symbol plus
# an addend. An independently named alias must not weaken this relationship.
function Target([long]$Rva, [long]$Extent = 1) {
    $inside = @($code | Where-Object { $Rva -ge $_.range.steam_rva -and $Rva -lt $_.range.steam_rva + $_.range.bytes })
    if ($inside.Count -gt 1) { throw 'Overlapping reviewed code ranges.' }
    if ($inside.Count -eq 1) { return [pscustomobject]@{symbol = $inside[0].symbol; addend = $Rva - $inside[0].range.steam_rva} }
    $named = @($layout | Where-Object { $_.steam_rva -gt 0 -and $_.steam_rva -eq $Rva })
    $name = if ($named.Count -eq 1) { $named[0].name } else { '' }
    if ($named.Count -eq 1) { $Extent = [Math]::Max($Extent, $(if ($name -eq 'activation_disable_mask') { 16 } else { 8 })) }
    return [pscustomobject]@{symbol = (Symbol $Rva $name $Extent); addend = 0}
}
function Signed-Bytes([long]$Value, [int]$Width) {
    if ($Width -eq 1) {
        if ($Value -lt -128 -or $Value -gt 127) { throw 'Short displacement overflow.' }
        return ,([byte[]]@([byte]($Value -band 255)))
    }
    if ($Width -ne 4 -or $Value -lt [int]::MinValue -or $Value -gt [int]::MaxValue) { throw 'Displacement overflow.' }
    return ,([BitConverter]::GetBytes([int]$Value))
}
function Find-Field($Bytes, [int]$Offset, [int]$Size, $Needle) {
    $matches = [Collections.Generic.List[int]]::new()
    for ($i = $Offset; $i -le $Offset + $Size - $Needle.Length; ++$i) {
        $same = $true
        for ($j = 0; $j -lt $Needle.Length; ++$j) { if ($Bytes[$i + $j] -ne $Needle[$j]) { $same = $false; break } }
        if ($same) { $matches.Add($i) }
    }
    if ($matches.Count -ne 1) { throw "Decoded displacement encoding is ambiguous at instruction $Offset" }
    return $matches[0]
}
$observedTargets = @{}
foreach ($item in $code) {
    $cursor = 0
    for ($n = 0; $n -lt $item.decoded.Count; ++$n) {
        $ins = $item.decoded[$n]; $saved = $item.store[$n]
        $offset = [int]($ins.rva - $item.range.steam_rva); $size = [int]$ins.size
        $text = $ins.text.Trim() -replace '\s+', ' '
        if ($offset -ne $cursor -or $size -lt 1 -or $size -gt 15 -or $saved.offset -ne $offset -or $saved.size -ne $size) { throw 'Instruction boundary mismatch.' }
        $cursor += $size
        $rip = [regex]::Match($text, '(?<disp>-?\d+)\(%rip\)')
        $branch = [regex]::Match($text, '^(?<op>callq?|j[a-z]+|loop[a-z]*) (?<disp>-?\d+)$')
        $field = -1; $width = 0; $target = 0; $kind = 'pc_relative'
        if ($rip.Success) {
            [long]$disp = $rip.Groups['disp'].Value
            $target = [long]$ins.rva + $size + $disp
            $width = 4; $field = Find-Field $item.bytes $offset $size (Signed-Bytes $disp 4)
            if ($field -le $offset -or ($item.bytes[$field - 1] -band 0xc7) -ne 5) { throw 'RIP field is not preceded by mod00/rm101 ModRM.' }
        } elseif ($branch.Success) {
            [long]$disp = $branch.Groups['disp'].Value
            $target = [long]$ins.rva + $size + $disp
            if ($size -eq 5 -and $item.bytes[$offset] -in @(0xe8, 0xe9)) { $width = 4; $field = $offset + 1 }
            elseif ($size -eq 6 -and $item.bytes[$offset] -eq 0x0f -and $item.bytes[$offset + 1] -ge 0x80 -and $item.bytes[$offset + 1] -le 0x8f) { $width = 4; $field = $offset + 2 }
            elseif ($size -eq 2 -and ($item.bytes[$offset] -eq 0xeb -or ($item.bytes[$offset] -ge 0x70 -and $item.bytes[$offset] -le 0x7f) -or ($item.bytes[$offset] -ge 0xe0 -and $item.bytes[$offset] -le 0xe3))) { $width = 1; $field = $offset + 1 }
            else { throw "Unreviewed relative control-flow encoding: $text" }
            $needle = Signed-Bytes $disp $width
            for ($j = 0; $j -lt $width; ++$j) { if ($item.bytes[$field + $j] -ne $needle[$j]) { throw 'Relative opcode/operand disagrees with decoder.' } }
        }
        $indexedHere = @($indexed | Where-Object { $_.store_function -eq $item.range.store_rva -and $_.instruction_offset -eq $offset })
        if ($indexedHere.Count) {
            if ($indexedHere.Count -ne 1 -or $field -ge 0) { throw 'Overlapping address kinds.' }
            $entry = $indexedHere[0]
            if (!$entry.must_image_base_proven -or $entry.instruction_bytes -ne $size -or $entry.instruction -cne $text) { throw 'Image-base proof does not match instruction.' }
            $field = Find-Field $item.bytes $offset $size (Signed-Bytes $entry.steam_target_rva 4)
            if ($field -ne $entry.operand_offset -or $field -lt $offset + 2 -or ($item.bytes[$field - 2] -band 0xc7) -ne 0x84) { throw 'Image RVA field is not an indexed mod10/SIB displacement.' }
            $kind = 'image_rva'; $width = 4; $target = [long]$entry.steam_target_rva
        }
        # Recheck the semantic comparison independently of the receipt's summary
        # flags. Only reviewed address roles may differ from archived assembly.
        $expectedText = $saved.text
        $actualText = $text
        [long]$storeTarget = -1
        if ($rip.Success) {
            $old = [regex]::Match($expectedText, '(?<disp>-?\d+)\(%rip\)')
            if (!$old.Success) { throw 'Store instruction has a different addressing mode.' }
            $storeTarget = [long]$saved.rva + $size + [long]$old.Groups['disp'].Value
            $expectedText = $expectedText.Replace($old.Value, '<address>(%rip)')
            $actualText = $actualText.Replace($rip.Value, '<address>(%rip)')
        } elseif ($branch.Success) {
            $old = [regex]::Match($expectedText, '^(?<op>callq?|j[a-z]+|loop[a-z]*) (?<disp>-?\d+)$')
            if (!$old.Success) { throw 'Store instruction has a different control-flow operand.' }
            $storeTarget = [long]$saved.rva + $size + [long]$old.Groups['disp'].Value
            $expectedText = $old.Groups['op'].Value + ' <address>'
            $actualText = $branch.Groups['op'].Value + ' <address>'
        } elseif ($indexedHere.Count) {
            $storeTarget = [long]$entry.store_target_rva
            $expectedText = $expectedText.Replace([string]$storeTarget, '<image-rva>')
            $actualText = $actualText.Replace([string]$target, '<image-rva>')
        }
        if ($expectedText -cne $actualText) { throw "Non-address instruction semantics changed in $($item.range.store_rva)+$offset" }
        if ($storeTarget -ge 0 -and $item.cross_build) {
            if ($observedTargets.ContainsKey($storeTarget) -and $observedTargets[$storeTarget] -ne $target) { throw 'Conflicting cross-function target correspondence.' }
            $observedTargets[$storeTarget] = $target
            $oldLocal = $storeTarget -ge $item.range.store_rva -and $storeTarget -lt $item.range.store_rva + $item.range.bytes
            $newLocal = $target -ge $item.range.steam_rva -and $target -lt $item.range.steam_rva + $item.range.bytes
            if ($kind -eq 'pc_relative' -and ($oldLocal -ne $newLocal -or ($oldLocal -and $storeTarget - $item.range.store_rva -ne $target - $item.range.steam_rva))) {
                throw 'Local instruction topology changed.'
            }
        }
        if ($field -lt 0) { continue }
        # Internal branches and RIP references retain their exact original
        # displacement bytes and local instruction topology.
        if ($kind -eq 'pc_relative' -and $target -ge $item.range.steam_rva -and $target -lt $item.range.steam_rva + $item.range.bytes) { continue }
        $destination = Target $target
        $item.operands.Add([pscustomobject]@{offset = $field; width = $width; pc_offset = $(if ($kind -eq 'image_rva') { 0 } else { $offset + $size }); kind = $kind; target_symbol = $destination.symbol; addend = $destination.addend})
    }
    if ($cursor -ne $item.bytes.Length) { throw 'Decoded instructions do not cover template.' }
    foreach ($operand in $item.operands) { for ($n = 0; $n -lt $operand.width; ++$n) { $item.bytes[$operand.offset + $n] = 0 } }
}
$constants = [Collections.Generic.List[object]]::new()
foreach ($anchor in $capture.inventory.anchors) {
    if (!$byAddress.ContainsKey([long]$anchor.rva)) { continue }
    $bytes = [Text.Encoding]::ASCII.GetBytes($anchor.text + [char]0)
    $id = Symbol $anchor.rva '' $bytes.Length
    $constants.Add([pscustomobject]@{symbol = $id; bytes = $bytes})
}
$mask = @($second.requests | Where-Object kind -eq 'mask')
if ($mask.Count -ne 1 -or $mask[0].hex -cne '01000000000000000000000000000000') { throw 'Reviewed activation constant changed.' }
$constants.Add([pscustomobject]@{symbol = (Symbol $mask[0].rva 'activation_disable_mask' 16); bytes = [Convert]::FromHexString($mask[0].hex)})
if ($identity) {
    foreach ($constant in $identity.constants) {
        $literal = [Text.Encoding]::ASCII.GetBytes($constant.text + [char]0)
        if ([Convert]::ToHexString($literal) -cne $constant.hex.ToUpperInvariant()) { throw 'Static identity literal does not match declared bytes.' }
        $observed = @($identityLiterals.requests | Where-Object { $_.rva -eq $constant.rva -and $_.size -ge $literal.Length })
        if ($observed.Count -ne 1 -or $observed[0].hex.Substring(0, 2*$literal.Length).ToUpperInvariant() -cne [Convert]::ToHexString($literal) -or !$byAddress.ContainsKey([long]$constant.rva)) {
            throw 'Static identity literal is unobserved or disconnected from the instruction graph.'
        }
        $constants.Add([pscustomobject]@{symbol=(Symbol $constant.rva '' $literal.Length);bytes=$literal})
    }
}
$pointers = [Collections.Generic.List[object]]::new()
if ($identity -and (Has-Property $identity 'pointer_relations')) {
    $pointerCapture = Read-Json (Join-Path $Artifacts $identity.pointer_capture_path)
    if (!$pointerCapture.read_only -or !$pointerCapture.target_verified -or $pointerCapture.timestamp -ne $identityCapture.timestamp -or $pointerCapture.image_size -ne $identityCapture.image_size) { throw 'Static method slots lack verified capture provenance.' }
    foreach ($relation in $identity.pointer_relations) {
        $owner = @($layout | Where-Object name -eq $relation.owner_name)
        $target = @($layout | Where-Object name -eq $relation.target_name)
        if ($owner.Count -ne 1 -or $target.Count -ne 1 -or !$owner[0].steam_rva -or !$target[0].steam_rva -or
            $relation.offset -lt 0 -or $relation.offset -gt 2040 -or $relation.offset % 8 -or
            !$byAddress.ContainsKey([long]$owner[0].steam_rva) -or !$byAddress.ContainsKey([long]$target[0].steam_rva)) { throw 'Unbound or unaligned static method relation.' }
        $at = [long]$owner[0].steam_rva + $relation.offset
        $observed = @($pointerCapture.requests | Where-Object { $_.kind -eq 'pointers' -and $at -ge $_.rva -and $at + 8 -le [long]$_.rva + $_.size -and ($at - $_.rva) % 8 -eq 0 })
        if ($observed.Count -ne 1 -or $observed[0].target_rvas[($at - $observed[0].rva) / 8] -ne $target[0].steam_rva) { throw 'Static method relation disagrees with captured slot.' }
        $ownerId = Symbol $owner[0].steam_rva $relation.owner_name ($relation.offset + 8)
        $targetId = $byAddress[[long]$target[0].steam_rva]
        if ($symbols[$ownerId].kind -ne 'read_only_data' -or $symbols[$targetId].kind -ne 'code') { throw 'Static method relation has incorrect section kinds.' }
        $pointers.Add([pscustomobject]@{owner_symbol=$ownerId;offset=$relation.offset;target_symbol=$targetId})
    }
}
# Preserve archived real RUNTIME_FUNCTION observations, including split unwind
# spans and the manager window's negative offset into its enclosing function.
$runtimeFunctions = [Collections.Generic.List[object]]::new()
function Visit($Node) {
    if ($null -eq $Node -or $Node -is [string] -or $Node -is [ValueType]) { return }
    if ($Node -is [array]) { foreach ($part in $Node) { Visit $part }; return }
    if ((Has-Property $Node 'function_begin_rva') -and (Has-Property $Node 'function_end_rva')) {
        if ($Node.function_begin_rva -gt 0 -and $Node.function_end_rva -gt $Node.function_begin_rva) {
            $runtimeFunctions.Add([pscustomobject]@{begin = [long]$Node.function_begin_rva; end = [long]$Node.function_end_rva})
        }
    }
    foreach ($p in $Node.PSObject.Properties) { if ($p.Name -ne 'instructions') { Visit $p.Value } }
}
foreach ($file in Get-ChildItem -LiteralPath $StoreArchive -Filter '*1.8.16.0*.json' -File) { Visit (Read-Json $file.FullName) }
$aaEvidence = Read-Input (Join-Path $MotionArchive 'aa-commands-02.txt')
$bodyEvidence = Read-Input (Join-Path $MotionArchive 'body-node-user-methods-03.txt')
if ($bodyEvidence -notmatch '(?m)^body_handle_code rva=55966368 bytes=60 .*relocation_checked=1 repeated_match=1\r?$') {
    throw 'Body-handle instruction provenance is incomplete.'
}
$aaBoundary = [regex]::Match($aaEvidence, '(?m)^function request=69960016 begin=(?<begin>\d+) end=(?<end>\d+) bytes=455 truncated=0 error=\r?$')
if (!$aaBoundary.Success -or [long]$aaBoundary.Groups['end'].Value - [long]$aaBoundary.Groups['begin'].Value -ne 455) {
    throw 'AA runtime-function provenance is incomplete.'
}
$boundaries = [Collections.Generic.List[object]]::new()
foreach ($item in $code) {
    if (Has-Property $item.range 'boundary_bytes') {
        $fresh = @($identityBounds.functions | Where-Object requested -eq $item.range.steam_rva)
        if ($fresh.Count -ne 1 -or $fresh[0].begin -ne $item.range.steam_rva -or $fresh[0].end-$fresh[0].begin -ne $item.range.boundary_bytes -or
            $item.range.bytes -ne $item.range.boundary_bytes) { throw 'Static identity constructor/function bounds disagree.' }
        $boundaries.Add([pscustomobject]@{symbol=$item.symbol;begin_addend=0;bytes=$item.range.boundary_bytes;required_pdata=$true})
        continue
    }
    [long]$begin = $item.range.store_rva; [long]$end = $begin + $item.range.bytes
    $spans = @($runtimeFunctions | Where-Object { $_.begin -lt $end -and $_.end -gt $begin } | Sort-Object begin, end -Unique)
    if (!$spans.Count) {
        # Body accessor and AA evidence are captured in explicit text reports.
        if ($begin -eq 69960016) { $spans = @([pscustomobject]@{begin = [long]$aaBoundary.Groups['begin'].Value; end = [long]$aaBoundary.Groups['end'].Value}) }
        elseif ($begin -in @(66859344, 66859376, 66859296, 56030192, 55874752, 55914960, 55966368, 70721312, 67705600, 80718912)) {
            # Reviewed complete leaves/tail bodies: no call, stack reference,
            # or push/pop. Their existing member accesses and terminal control
            # flow remain exact. A present runtime-function entry is checked by
            # the caller; no metadata absence is invented by this annotation.
            foreach ($instruction in $item.decoded) {
                if ($instruction.text -match '\b(call|push|pop|enter|leave)|%[er]sp\b|%[er]bp\b') { throw 'Reviewed leaf unexpectedly touches stack or calls.' }
            }
        }
        else { throw "No archived unwind or reviewed leaf evidence for $begin" }
    }
    if (!$spans.Count) { $boundaries.Add([pscustomobject]@{symbol = $item.symbol; begin_addend = 0; bytes = $item.range.bytes; required_pdata = $false}); continue }
    $cursor = $begin
    foreach ($span in $spans) {
        if ($span.begin -gt $cursor) { throw "Gap in archived unwind coverage for $begin" }
        $cursor = [Math]::Max($cursor, $span.end)
        $boundaries.Add([pscustomobject]@{symbol = $item.symbol; begin_addend = $span.begin - $begin; bytes = $span.end - $span.begin; required_pdata = $true})
    }
    if ($cursor -lt $end) { throw "Incomplete archived unwind coverage for $begin" }
}
$cpp = [Text.StringBuilder]::new()
[void]$cpp.AppendLine('// Generated by tools/diagnostics/generate_camera_contract.ps1; do not edit by hand.')
[void]$cpp.AppendLine('// Reviewed instruction templates; operand bytes contain no observed build addresses.')
[void]$cpp.AppendLine('#include "camera_contract_model.hpp"')
[void]$cpp.AppendLine('#include <limits>')
[void]$cpp.AppendLine('namespace taxi_camera::native_camera::camera_contract_model {')
[void]$cpp.AppendLine('const relocatable::ContractModel& model() {')
[void]$cpp.AppendLine('static const relocatable::ContractModel value = [] { relocatable::ContractModel out;')
foreach ($s in $symbols) { [void]$cpp.AppendLine(('out.symbols.push_back({{"{0}",relocatable::SectionKind::{1},{2}}});' -f $s.name, $s.kind, $s.extent)) }
foreach ($c in $code) {
    if ($c.minimum_seed -ne 8) { throw "Generated template $($symbols[$c.symbol].name) must keep minimum_seed 8." }
    [void]$cpp.AppendLine(('out.code.push_back({{{0},{1},{{' -f $c.symbol, (Bytes-Literal $c.bytes)))
    foreach ($o in $c.operands) { [void]$cpp.AppendLine(('{{{0},{1},{2},relocatable::AddressKind::{3},{4},{5}}},' -f $o.offset, $o.width, $o.pc_offset, $o.kind, $o.target_symbol, $o.addend)) }
    [void]$cpp.AppendLine(('}},{0}}});' -f $c.minimum_seed))
}
foreach ($constant in $constants) { [void]$cpp.AppendLine(('out.constants.push_back({{{0},0,{1}}});' -f $constant.symbol, (Bytes-Literal $constant.bytes))) }
foreach ($pointer in $pointers) { [void]$cpp.AppendLine(('out.pointers.push_back({{{0},{1},{2}}});' -f $pointer.owner_symbol, $pointer.offset, $pointer.target_symbol)) }
[void]$cpp.AppendLine('return out; }(); return value; }')
[void]$cpp.AppendLine('const std::vector<FunctionBoundary>& boundaries() { static const std::vector<FunctionBoundary> value = {')
foreach ($b in $boundaries) { [void]$cpp.AppendLine(('{{{0},{1},{2},{3}}},' -f $b.symbol, $b.begin_addend, $b.bytes, $b.required_pdata.ToString().ToLowerInvariant())) }
[void]$cpp.AppendLine('}; return value; }')
[void]$cpp.AppendLine('std::uint32_t symbol_index(std::string_view name) noexcept { const auto& symbols=model().symbols; for(std::size_t i=0;i<symbols.size();++i) if(symbols[i].name==name) return static_cast<std::uint32_t>(i); return std::numeric_limits<std::uint32_t>::max(); }')
[void]$cpp.AppendLine('bool bind(const std::vector<std::uint32_t>& symbols, CameraFunctions& functions, CameraImageLayout& layout) noexcept { if(symbols.size()!=model().symbols.size()) return false; CameraFunctions next_functions{}; CameraImageLayout next_layout{};')
foreach ($entry in $functionRoles.GetEnumerator()) {
    $item = @($code | Where-Object { $_.range.store_rva -eq $entry.Value })
    if ($item.Count -ne 1) { throw 'Missing public camera function role.' }
    [void]$cpp.AppendLine(('if(!symbols[{0}]) return false; next_functions.{1}=symbols[{0}];' -f $item[0].symbol, $entry.Key))
}
foreach ($entry in $layout | Where-Object steam_rva -gt 0) {
    if (!$byAddress.ContainsKey([long]$entry.steam_rva)) { throw "Unbound known layout role: $($entry.name)" }
    [void]$cpp.AppendLine(('if(!symbols[{0}]) return false; next_layout.{1}=symbols[{0}];' -f $byAddress[[long]$entry.steam_rva], $entry.name))
}
[void]$cpp.AppendLine('functions=next_functions; layout=next_layout; return true; }')
[void]$cpp.AppendLine('} // namespace taxi_camera::native_camera::camera_contract_model')
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
[void](Read-Input (Join-Path $repo 'dependencies.json'))
[void](Read-Input (Join-Path $repo 'ci/toolchain.ps1'))
[void](Read-Input (Join-Path $repo '.clang-format'))
[void](Read-Input (Join-Path $repo 'src/camera/camera_contract_model.hpp'))
. (Join-Path $repo 'ci/toolchain.ps1')
$toolchain = Get-TaxiToolchain $repo
# Use the canonical source filename for main-header sorting and root style even
# when the caller writes an ignored review output with a different filename.
# Format before touching the destination so a formatter failure leaves it intact.
$formatted = @($cpp.ToString() | & (Join-Path $toolchain 'clang-format.exe') `
    ('--style=file:' + (Join-Path $repo '.clang-format')) `
    ('--assume-filename=' + (Join-Path $repo 'src/camera/camera_contract_model.cpp')))
if ($LASTEXITCODE -ne 0) { throw 'Generated source formatting failed.' }
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),
    (($formatted -join [Environment]::NewLine).TrimEnd() + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
$inputs[[IO.Path]::GetFullPath($PSCommandPath)] = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
$receiptData = [ordered]@{
    schema = 1; purpose = 'Reproducible offline generation provenance; not runtime admission criteria.'
    template_count = $code.Count; template_bytes = ($code | ForEach-Object { $_.bytes.Length } | Measure-Object -Sum).Sum
    symbol_count = $symbols.Count; operands = ($code | ForEach-Object { $_.operands.Count } | Measure-Object -Sum).Sum
    image_rva_operands = $indexed.Count; constants = $constants.Count; boundaries = $boundaries.ToArray()
    pointer_relations = $pointers.ToArray()
    identity_scope = $(if ($identity) { $identity.scope } else { 'No scene identity extension.' })
    template_provenance = @($code | ForEach-Object { [pscustomobject]@{symbol=$_.symbol;cross_build_comparison=$_.cross_build;observed_begin=$_.range.steam_rva;bytes=$_.range.bytes} })
    symbols = $symbols.ToArray(); inputs = @($inputs.GetEnumerator() | Sort-Object Key | ForEach-Object { [pscustomobject]@{path=$_.Key;sha256=$_.Value} })
    output = [IO.Path]::GetFullPath($Output); output_sha256 = (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash
}
$receiptData | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $Receipt -Encoding utf8
[pscustomobject]@{templates=$code.Count;symbols=$symbols.Count;constants=$constants.Count;output=$Output;receipt=$Receipt}|ConvertTo-Json
