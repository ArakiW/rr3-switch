<#
.SYNOPSIS
    Package the port as an NSP.

.DESCRIPTION
    WHY AN NSP AND NOT AN NRO.  An NSP installs as a TITLE -- through
    hbloader or an NSP installer -- and launches from the Home menu.  An
    NRO has to be launched by a homebrew menu every time, and it is a file
    that the user has to put in the right place by hand.

    WHY THIS SCRIPT INSTEAD OF libnx's `%.nsp` RULE.  libnx's rule builds
    the PFS0 from the DIRECTORY `exefs`, so the container's entries are
    named `main` and `main.npdm` -- that is an EXEFS image, not an NSP.
    An NSP's entries are `exefs/main` and `exefs/main.npdm`, i.e. the
    directory layout has to be inside the container.  So the three steps
    are done here, where the staging layout is visible:

        elf2nso    <elf>              -> <stage>\exefs\main
        npdmtool   <descriptor>       -> <stage>\exefs\main.npdm
        build_pfs0 <stage>            -> <out>.nsp

    WHAT IS IN THE NSP.  The executable and its meta, and nothing else.
    The RomFS is NOT in there, on purpose: npdmtool cannot describe a
    RomFS section, and the port does not need one.  Every port asset is
    read through resolvePortAssetPath(), which falls back from
    "romfs:/X" to "sdmc:/switch/rr3-native-rehost/X", and the guest's own
    libraries and the UI faces are already on the card there.  The game's
    data -- the seven volumes and the .depot tree, about 11.8 GiB -- stays
    on the SD card too, where it has always been.

    THE TITLE ID is 0x010000000000F00D.  It is in the range homebrew uses
    and it is not a retail title, so it cannot collide with something the
    user has installed.

.PARAMETER Out
    Where to write the NSP.  Defaults to native-switch\rr3-native-rehost.nsp.

.PARAMETER ProjectRoot
    The repository root.  Defaults to the parent of this script's folder.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\Pack-NativeSwitchNsp.ps1
#>
param(
    [string]$Out = '',
    [string]$ProjectRoot = ''
)

$ErrorActionPreference = 'Stop'

if ($ProjectRoot -eq '') {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$native = Join-Path $ProjectRoot 'native-switch'
if ($Out -eq '') {
    $Out = Join-Path $native 'rr3-native-rehost.nsp'
}

# --- the tools -----------------------------------------------------------------
# The project-local toolchain first, then DEVKITPRO.  Same rule as
# Build-NativeSwitch.ps1, so the two agree on which build produced the
# artifacts.
$toolBins = @()
$localBin = Join-Path $ProjectRoot '.tools\devkitPro\msys64\opt\devkitpro\tools\bin'
if (Test-Path -LiteralPath $localBin) {
    $toolBins += $localBin
}
if ($env:DEVKITPRO) {
    $toolBins += (Join-Path $env:DEVKITPRO 'tools\bin')
}

function Resolve-Tool([string]$name) {
    foreach ($dir in $toolBins) {
        $candidate = Join-Path $dir $name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw ("{0} was not found in: {1}" -f $name, ($toolBins -join ', '))
}

$elf2nso = Resolve-Tool 'elf2nso.exe'
$npdmtool = Resolve-Tool 'npdmtool.exe'
$pfs0Writer = Join-Path $ProjectRoot 'tools\make-pfs0.py'
if (-not (Test-Path -LiteralPath $pfs0Writer -PathType Leaf)) {
    throw ("the PFS0 writer is not there: {0}" -f $pfs0Writer)
}

$elf = Join-Path $native 'rr3-native-rehost.elf'
if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) {
    throw ("the ELF is not there: {0} -- build the port first" -f $elf)
}
$descriptor = Join-Path $native 'rr3-nsp.json'
if (-not (Test-Path -LiteralPath $descriptor -PathType Leaf)) {
    throw ("the NSP descriptor is not there: {0}" -f $descriptor)
}

# --- the staging layout -------------------------------------------------------
# exefs/main and exefs/main.npdm are what the container's entries have to be
# called, so the staging tree is the container's layout.
$stage = Join-Path $native 'nsp-stage'
$exefs = Join-Path $stage 'exefs'
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $exefs | Out-Null

$main = Join-Path $exefs 'main'
$meta = Join-Path $exefs 'main.npdm'

& $elf2nso $elf $main
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $main -PathType Leaf)) {
    throw 'elf2nso failed.'
}
& $npdmtool $descriptor $meta
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $meta -PathType Leaf)) {
    throw 'npdmtool failed.'
}
# The container is written here and not by devkitPro's build_pfs0, because
# build_pfs0 packs only the TOP LEVEL of its input directory and prints
# "Directories aren't supported, skipping..." for a subdirectory -- so it
# cannot produce the "exefs/main" entry names an NSP needs.
& python $pfs0Writer $stage $Out
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Out -PathType Leaf)) {
    throw 'the PFS0 writer failed.'
}

# --- the check that matters ----------------------------------------------------
# "the tool said it worked" is not a statement about the container.  Read the
# PFS0 back and assert the two entries are named what an NSP needs them
# named, because a container whose entries are `main` and `main.npdm` is an
# EXEFS image and installs as nothing.
$bytes = [System.IO.File]::ReadAllBytes($Out)
if ($bytes.Length -lt 0x40 -or
    [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'PFS0') {
    throw 'the NSP is not a PFS0 container.'
}
$count = [System.BitConverter]::ToUInt32($bytes, 4)
$table = 0x10
$stringBase = $table + ($count * 0x18)
$names = @()
for ($index = 0; $index -lt $count; $index++) {
    $base = $table + ($index * 0x18)
    $stringOffset = [System.BitConverter]::ToUInt64($bytes, $base + 0x10)
    $end = $stringBase + [int]$stringOffset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
    $name = [System.Text.Encoding]::UTF8.GetString($bytes, $stringBase + [int]$stringOffset, $end - ($stringBase + [int]$stringOffset))
    $names += $name
}
foreach ($required in @('exefs/main', 'exefs/main.npdm')) {
    if ($names -notcontains $required) {
        throw ("the NSP is missing '{0}' -- it has: {1}" -f $required, ($names -join ', '))
    }
}

$info = Get-Item -LiteralPath $Out
Write-Host ("NSP      : {0}" -f $Out)
Write-Host ("size     : {0:N0} bytes" -f $info.Length)
Write-Host ("sha256   : {0}" -f (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash)
Write-Host ("entries  : {0}" -f ($names -join ', '))
Write-Host ("title id : {0}" -f ((Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json).title_id))
