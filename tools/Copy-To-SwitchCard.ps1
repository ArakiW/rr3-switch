<#
.SYNOPSIS
    Install an RR3 Native Rehost release package onto a real Switch SD card.

.DESCRIPTION
    This package is SELF-CONTAINED: the app, the control scripts, the seven
    data volumes (~9.5 GiB) and the loose .depot tree (~1.6 GiB) all ship in
    it, so this script copies out of THIS package and needs no other source.

    The card root is the folder that CONTAINS `switch\`.  Everything goes to
    `switch\rr3-native-rehost\`, except one file that goes to `switch\`:
    `rr3-gfx.conf`.

    WHY rr3-gfx.conf IS INSTALLED AND NOT DELETED
    ----------------------------------------------
    It is the port's emulator opt-in file, read once per process, and it
    carries THREE different kinds of settings:

      * which backend: an absent file, or ANY value other than null/0/on,
        selects Mesa (nouveau/DRM, NVN on Maxwell).  Only a literal
        "backend=null" selects the stub backend that never draws.
      * how the guest behaves: ccdisp / ccloginok / ccsocialok / objid.
        With these off, the guest spins on its cloud-sync queue and never
        reaches the race -- that was measured, one key at a time.
      * where the port may touch the hardware: nofb, nogo, mkdiridem.

    So the correct action on a console is to INSTALL the shipped file.  The
    one thing that really does cause a black screen is a leftover
    "backend=null" from an old emulator setup; this script overwrites it.

.PARAMETER Card
    The SD card's root -- the folder that CONTAINS `switch\`.

.PARAMETER PackageRoot
    Where this package lives.  Defaults to the folder this script is in,
    which is the package root when the script was shipped inside one.

.PARAMETER SkipVolumes
    Do not copy the seven data volumes (the game will have no assets).
    Use when they are already on the card.

.PARAMETER SkipDepot
    Do not copy the loose .depot tree (the game will have no assets).

.PARAMETER Profile
    full (default) installs everything in the package.

    mini installs everything EXCEPT two of the data volumes, for a card that
    is too small for the lot:

      * gui-hd.romfs (1.55 GiB) -- the 2048x1536 GUI assets.  In the
        measured run it was consulted 272 times against gui-sd's 2,321, and
        every one of those was the 2048x1536 set: the loading splash.  The
        game runs without it; the splash falls back to the 960x640 set.
      * vehicles-modern.romfs (1.37 GiB) -- the 2021-and-later cars.  It was
        consulted ZERO times in the measured run, because the car being
        driven was not one of them.

    That is 2.92 GiB saved and the card goes from 9.56 GiB to 6.64 GiB.
    What you lose: the 2048x1536 splash art, and the modern cars.

.PARAMETER OptionalTree
    Also unpack optional\card-depot-full.zip onto the card.  It is the
    COMPLETE loose tree -- the 10,413 files the measured run did not read.
    Off by default: it is 1.6 GiB and 10,413 files, which is the opposite of
    what a small card wants.  Turn it on only if the game reports a missing
    asset.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Copy-To-SwitchCard.ps1 -Card F:\

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Copy-To-SwitchCard.ps1 -Card F:\ -Profile mini
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Card,
    [string]$PackageRoot = '',
    [switch]$SkipVolumes,
    [switch]$SkipDepot,
    [ValidateSet('full', 'mini')]
    [string]$Profile = 'full',
    [switch]$OptionalTree
)

$ErrorActionPreference = 'Stop'

if ($PackageRoot -eq '') {
    $PackageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$packageApp = Join-Path $PackageRoot 'switch\rr3-native-rehost'
$packageConf = Join-Path $PackageRoot 'switch\rr3-gfx.conf'

if (-not (Test-Path -LiteralPath $Card -PathType Container)) {
    Write-Error "Card not found: $Card"
    exit 1
}
if (-not (Test-Path -LiteralPath $packageApp -PathType Container)) {
    Write-Error "package is broken: $packageApp is missing"
    exit 1
}

$appDir = Join-Path (Join-Path $Card 'switch') 'rr3-native-rehost'
New-Item -ItemType Directory -Force -Path $appDir | Out-Null

$scriptNames = @('rr3-touch.txt', 'rr3-drive.txt', 'rr3-pad.txt', 'rr3-action.txt',
    'rr3-screen.txt', 'rr3-settings.txt', 'rr3-ui.txt', 'rr3-open.txt',
    'rr3-gamepad.txt')
$volumeNames = @('core.romfs', 'gui-hd.romfs', 'gui-sd.romfs', 'tracks.romfs',
    'vehicles-classic.romfs', 'vehicles-mid.romfs', 'vehicles-modern.romfs')

# THE VOLUMES THE "mini" PROFILE LEAVES BEHIND, and why.  Both numbers are
# from the measured run (archive 20261102-141035-eden-rr3-q118), not from a
# guess: the log records every volume the port consulted.
$miniOmits = @('gui-hd.romfs', 'vehicles-modern.romfs')
$wantedVolumes = if ($Profile -eq 'mini') {
    @($volumeNames | Where-Object { $miniOmits -notcontains $_ })
} else {
    $volumeNames
}

# ---- 0. preflight ------------------------------------------------------------
$fail = @()
foreach ($name in @('rr3-native-rehost.nro', 'rr3.romfs')) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageApp $name) -PathType Leaf)) {
        $fail += "package file missing: switch\rr3-native-rehost\$name"
    }
}
foreach ($name in $scriptNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageApp $name) -PathType Leaf)) {
        $fail += "package file missing: switch\rr3-native-rehost\$name"
    }
}
if (-not $SkipVolumes) {
    foreach ($name in $wantedVolumes) {
        if (-not (Test-Path -LiteralPath (Join-Path $packageApp "data\$name") -PathType Leaf)) {
            $fail += "package volume missing: switch\rr3-native-rehost\data\$name"
        }
    }
}
if (-not (Test-Path -LiteralPath $packageConf -PathType Leaf)) {
    $fail += "package file missing: switch\rr3-gfx.conf"
}
if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Host ("preflight : {0} app files, {1} scripts, {2} volumes, 1 conf -- all present" -f `
    2, $scriptNames.Count, $(if ($SkipVolumes) { 0 } else { $wantedVolumes.Count }))
if ($Profile -eq 'mini') {
    Write-Host ("profile   : mini -- {0} left off ({1:N2} GiB saved)" -f `
        ($miniOmits -join ', '), 2.92)
}

# ---- 1. the app --------------------------------------------------------------
foreach ($name in @('rr3-native-rehost.nro', 'rr3.romfs')) {
    $src = Join-Path $packageApp $name
    $dst = Join-Path $appDir $name
    $want = (Get-FileHash $src -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $got = (Get-FileHash $dst -Algorithm SHA256).Hash
    if ($got -ne $want) {
        Write-Error "copy verification failed: $name"
        exit 3
    }
    Write-Host ("copied    : {0,-24} {1,14:N0} bytes  sha256={2}" -f `
        $name, (Get-Item $dst).Length, $got.Substring(0, 16))
}

# ---- 2. the control scripts --------------------------------------------------
foreach ($name in $scriptNames) {
    $src = Join-Path $packageApp $name
    $dst = Join-Path $appDir $name
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Host ("copied    : {0,-24} {1,14:N0} bytes" -f $name, (Get-Item $dst).Length)
}

# ---- 3. the data volumes -----------------------------------------------------
if ($SkipVolumes) {
    Write-Host "data      : SKIPPED by request (the game will have no assets)"
}
else {
    $sourceData = Join-Path $packageApp 'data'
    $targetData = Join-Path $appDir 'data'
    New-Item -ItemType Directory -Force -Path $targetData | Out-Null
    # EVERY file in data\, not just the seven volumes: the directory also
    # carries rr3-data-manifest.json, which says what is inside the volumes.
    # Copying only the named volumes left the console without it -- caught by
    # the installer's own positive control (2,594 bytes missing).
    $dataFiles = @(Get-ChildItem -LiteralPath $sourceData -File | Sort-Object Name)
    foreach ($file in $dataFiles) {
        if ($wantedVolumes -notcontains $file.Name) {
            Write-Host ("omitted   : {0,-24} {1,14:N0} bytes (the mini profile)" -f `
                $file.Name, $file.Length)
            continue
        }
        $dst = Join-Path $targetData $file.Name
        $srcLen = $file.Length
        if ((Test-Path -LiteralPath $dst -PathType Leaf) -and
            (Get-Item $dst).Length -eq $srcLen) {
            Write-Host ("present   : {0,-24} {1,14:N0} bytes (skipped)" -f $file.Name, $srcLen)
            continue
        }
        Write-Host ("copying   : {0,-24} {1,14:N0} bytes ..." -f $file.Name, $srcLen)
        Copy-Item -LiteralPath $file.FullName -Destination $dst -Force
        $dstLen = (Get-Item $dst).Length
        if ($dstLen -ne $srcLen) {
            Write-Error "data copy verification failed: $($file.Name) ($srcLen -> $dstLen)"
            exit 3
        }
        Write-Host ("copied    : {0,-24} {1,14:N0} bytes" -f $file.Name, $dstLen)
    }
    $missing = $wantedVolumes | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $targetData $_) -PathType Leaf)
    }
    if ($missing) {
        Write-Error ("volumes missing on the card: {0}" -f ($missing -join ', '))
        exit 3
    }
}

# ---- 4. the loose trees -------------------------------------------------------
#
# .depot, cache and doc.
#
# /E IS LOAD-BEARING.  robocopy without /E copies only the TOP LEVEL: the
# tree's files live in subdirectories (Tracks\, vehicles\, gui_assets\,
# audio\, ...), so a non-recursive copy leaves the console with a .depot that
# has the root files and none of the assets.  The file count is verified
# afterwards, so this cannot fail silently.
#
# THE PACKAGE CARRIES THE SUBSET THE PORT ACTUALLY READS (measured: 133
# distinct card files in a full run), not the whole 10,546-file tree, so
# this is a small copy on purpose.
if ($SkipDepot) {
    Write-Host "depot     : SKIPPED by request (the game will have no assets)"
}
else {
    foreach ($dir in @('.depot', 'cache', 'doc')) {
        $sourceTree = Join-Path $packageApp $dir
        if (-not (Test-Path -LiteralPath $sourceTree -PathType Container)) {
            continue
        }
        $targetTree = Join-Path $appDir $dir
        $srcCount = @(Get-ChildItem $sourceTree -Recurse -File -Force).Count
        if ($srcCount -eq 0) { continue }
        Write-Host ("copying   : {0} ({1} files) ..." -f $dir, $srcCount)
        & robocopy.exe $sourceTree $targetTree /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) {
            Write-Error "robocopy failed for $dir (code $LASTEXITCODE)"
            exit 4
        }
        $dstCount = @(Get-ChildItem $targetTree -Recurse -File -Force).Count
        if ($dstCount -lt $srcCount) {
            Write-Error ("{0} copy incomplete: {1} files in the package, {2} on the card" -f $dir, $srcCount, $dstCount)
            exit 3
        }
        Write-Host ("copied    : {0,-10} -- {1} files, verified" -f $dir, $dstCount)
    }
}

# ---- 4b. the optional complete tree -------------------------------------------
if ($OptionalTree) {
    $archive = Join-Path $PackageRoot 'optional\card-depot-full.zip'
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        Write-Error "optional\card-depot-full.zip is missing from the package"
        exit 1
    }
    Write-Host ("unpacking  : {0} (this is 10,413 files -- it takes a while)" -f $archive)
    & tar.exe -xf $archive -C $appDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "tar failed (code $LASTEXITCODE)"
        exit 4
    }
    Write-Host ("unpacked  : the complete loose tree, {0} files" -f `
        @(Get-ChildItem $appDir -Recurse -File -Force).Count)
}

# ---- 5. the port's conf ------------------------------------------------------
$confTarget = Join-Path (Join-Path $Card 'switch') 'rr3-gfx.conf'
$leftover = $null
if (Test-Path -LiteralPath $confTarget -PathType Leaf) {
    $leftover = (Get-Content -LiteralPath $confTarget -Raw)
}
Copy-Item -LiteralPath $packageConf -Destination $confTarget -Force
Write-Host ("copied    : switch\rr3-gfx.conf           {0,14:N0} bytes" -f (Get-Item $confTarget).Length)
if ($leftover -and ($leftover -match '(?im)^\s*backend\s*=\s*(null|0|on)\s*$')) {
    Write-Host "note      : the previous rr3-gfx.conf selected the NULL backend; it is replaced."
}
Write-Host "note      : this file is the port's console configuration. It is REQUIRED."
Write-Host "            Do not delete it: ccdisp/ccloginok/ccsocialok/objid decide whether"
Write-Host "            the guest gets past its cloud-sync spin and reaches the race."

# ---- 6. what to do next ------------------------------------------------------
Write-Host ""
Write-Host "Installed. Launch the NRO from the title-bar path:"
Write-Host ("  {0}" -f $appDir)
Write-Host "Expect: the port's loading screen (skipped by design), then the guest's own"
Write-Host "front end, then a race.  Then send back:"
Write-Host ("  {0}" -f (Join-Path $appDir 'rr3-native-rehost.guest-view.log'))
Write-Host "the phase=race-state lines are the ones that matter."
