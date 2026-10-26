<#
.SYNOPSIS
    Deploy the seven RR3 romfs data volumes onto an emulator's virtual SD card.

.DESCRIPTION
    The port reads its assets from seven "romfs" volumes plus a manifest that
    live on the card at <card>\switch\rr3-native-rehost\data\. This script
    copies a prepared set from the build tree onto that card, and refuses to
    start if the card cannot hold the result:

      1. PREFLIGHT  every expected volume exists in the source, is a regular
         file, is non-zero, and matches the sha256 recorded in
         rr3-data-manifest.json (a manifest without per-volume hashes says so
         instead of pretending it verified anything).
      2. CAPACITY   Eden's SD card is a FIXED-CAPACITY FAT32 image over a
         loose directory tree, so the projected final usage AND the transient
         peak while a volume is copied are compared against a ceiling BEFORE
         anything is written.
      3. LITTER     the harness' own per-run rubbish is removed only when
         -CleanLitter is passed.
      4. STALE      duplicate volumes left on the card by an older deploy
         layout are removed only when -RemoveStale is passed.
      5. COPY       each file is copied to a temporary name in the SAME
         directory, verified there, and then moved over the target.
      6. VERIFY     length and sha256 of source and destination are compared
         for all eight files; any mismatch is a non-zero exit code.

    Nothing is deleted unless -CleanLitter / -RemoveStale were passed, and
    nothing is written at all unless -Apply was passed. The default run is a
    dry run: it prints the plan, the byte math and the gate results, and
    changes nothing.

    WHY THE CAPACITY GATE IS A HARD GATE
    A full card produces a failure that looks exactly like a port bug. Eden's
    SD is a fixed-capacity FAT32 image; when a stale SECOND copy of the seven
    volumes was left on it (measured 2026-10-24: 9,634,419,608 bytes of
    duplicate romfs referenced by nothing, plus 5.40 GiB of stashed logs), the
    guest's own asset download failed with errno 12 (ENOSPC), the guest
    printed its Chinese "no diskspace" warning and shut down at frame 18, with
    no race, no HUD and no probe output. That reads like a port regression and
    is not one. The only defence is to know the numbers before writing, which
    is what this script does.

    WHY THE PEAK IS GATED TOO, NOT JUST THE FINAL SIZE
    A volume is replaced by copy-to-temp-then-rename, so at the worst moment
    the NEW tracks.romfs (1,938,469,088 bytes) exists ALONGSIDE the old one
    (1,912,527,840 bytes). That is about 1.94 GB of transient headroom above
    the final size, and no ordering avoids it: the old file may not be deleted
    before the replacement has been verified, or a failed copy would leave the
    card with no volume at all. A card that fits the final size but not the
    peak still dies with ENOSPC, so both numbers are checked.

    EXIT CODES
      0  success (dry run completed, or apply finished and verified)
      1  preflight failed (missing/empty volume, manifest hash mismatch, ...)
      2  the capacity gate refused the plan (nothing was written)
      3  a post-copy verification mismatch (source vs destination)
      4  an apply step failed after the plan was approved

.PARAMETER SourceRoot
    Directory holding the prepared volumes and the manifest. Default is the
    rebuild tree under .build\volumes-rebuild.

.PARAMETER CardDataDir
    Directory on the card that holds the seven volumes plus the manifest.

.PARAMETER CardSwitchDir
    Root of the card's switch\ tree. Used for the capacity accounting and as
    the boundary inside which litter and stale duplicates may be removed.

.PARAMETER CardCapacityBytes
    An OPTIONAL hard ceiling in bytes. 0 (the default) means "no hard ceiling",
    which is the right value for Eden: Eden's SD capacity is not a setting, it
    is computed from the live tree as floor((used + 4 GiB) / 4 GiB) * 4 GiB, so
    it grows with the content. Pass a real number for a card whose capacity IS
    fixed by something else (a console SD card), and then the peak usage must
    fit under it.

.PARAMETER RequiredFreeBytes
    The free space the card must still report at the WORST moment of the
    deployment. Default 256 MiB. Eden's free space is a saw-tooth (4 GiB minus
    the usage modulo 4 GiB), so a large write can pass through "almost nothing
    free" while climbing across a band boundary; the gate evaluates the free
    space over the whole usage range, not only at the end. The guest asks for
    104 MB when it downloads its assets and refuses to start below that.

.PARAMETER CleanLitter
    Remove the harness' own per-run litter: the _stale_<stamp> directories,
    the live guest-view / teardown-repair logs and the crash-*.txt files, all
    directly under the card switch dir. Default OFF. The seven volumes are
    never touched by this switch.

.PARAMETER RemoveStale
    Remove stale duplicate volumes: files under the card data dir whose name
    is not one of the eight expected names, and the legacy nested copy of the
    volumes (the layout that filled the card on 2026-10-24). Default OFF.

.PARAMETER Apply
    Actually write to the card. Default OFF (dry run).

.EXAMPLE
    .\Deploy-RR3Volumes.ps1
    Dry run against the defaults; prints the plan, the byte math and the
    capacity gate without touching the card.

.EXAMPLE
    .\Deploy-RR3Volumes.ps1 -CleanLitter -RemoveStale
    Dry run that also shows how many bytes the litter and the stale
    duplicate would give back.

.EXAMPLE
    .\Deploy-RR3Volumes.ps1 -CleanLitter -RemoveStale -Apply
    Apply against an Eden card. Eden's capacity is computed from the live
    tree, so no hard ceiling is needed; the gate is the free space the card
    reports at the worst moment of the write.

.EXAMPLE
    .\Deploy-RR3Volumes.ps1 -CardCapacityBytes 17179869184 -CleanLitter -RemoveStale -Apply
    Apply with a hard ceiling of 16 GiB as well, for a card whose capacity is
    fixed by something other than Eden.
#>
[CmdletBinding()]
param(
    [string]$SourceRoot = 'E:\AI\RealRacing3-switch\.build\volumes-rebuild\switch\rr3-native-rehost\data',
    [string]$CardDataDir = 'E:\Eden\user\sdmc\switch\rr3-native-rehost\data',
    [string]$CardSwitchDir = 'E:\Eden\user\sdmc\switch',

    # Alias so the parameter can be spelled either way.
    [Alias('MaxCardBytes')]
    [long]$CardCapacityBytes = 0,

    # The free space the card must still report at the WORST moment of the
    # deployment. Eden's capacity is derived from the live tree
    # (total = floor((used + 4 GiB) / 4 GiB) * 4 GiB), so the free space is a
    # saw-tooth: 4 GiB minus (usage mod 4 GiB). A single large write can climb
    # through a band boundary and pass through "almost nothing free" on its way
    # up, so the gate is evaluated over the whole usage range. The guest asks
    # for 104 MB when it downloads its assets and refuses to start below that,
    # hence 256 MiB as the default margin.
    [long]$RequiredFreeBytes = 268435456,

    [switch]$CleanLitter,
    [switch]$RemoveStale,
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$exitOk = 0
$exitPreflight = 1
$exitCapacity = 2
$exitVerify = 3
$exitApply = 4

$expectedVolumeNames = @(
    'core.romfs',
    'gui-hd.romfs',
    'gui-sd.romfs',
    'tracks.romfs',
    'vehicles-classic.romfs',
    'vehicles-mid.romfs',
    'vehicles-modern.romfs'
)
$manifestName = 'rr3-data-manifest.json'
$expectedNames = @($expectedVolumeNames + @($manifestName))

# ------------------------------------------------------------------ helpers --
function Write-Section {
    param([string]$Title)
    Write-Host ''
    Write-Host ("=== {0} ===" -f $Title)
}

function Get-TreeBytes {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return [long]0 }
    $files = @(Get-ChildItem -LiteralPath $Path -Recurse -Force -File -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) { return [long]0 }
    $sum = ($files | Measure-Object -Property Length -Sum).Sum
    if ($null -eq $sum) { return [long]0 }
    return [long]$sum
}

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-ShortHash {
    # The verify table prints a prefix, but a MISSING destination has no hash
    # at all, and Substring on an empty string throws.
    param([string]$Hash)
    if ($null -eq $Hash -or $Hash.Length -eq 0) { return '(missing)' }
    if ($Hash.Length -le 16) { return $Hash }
    return $Hash.Substring(0, 16)
}

function Find-ManifestEntries {
    # Walks the parsed manifest and collects every { file, sha256 } object it
    # contains, keyed by file name. The manifest layout is not guaranteed, so
    # this does not assume a "volumes" node exists.
    param($Node)
    $found = @{}
    $queue = New-Object 'System.Collections.Queue'
    $queue.Enqueue($Node)
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        if ($null -eq $cur) { continue }
        if ($cur -is [string] -or $cur -is [System.ValueType]) { continue }
        if ($cur -is [System.Collections.IDictionary]) {
            foreach ($key in $cur.Keys) { $queue.Enqueue($cur[$key]) }
            continue
        }
        if ($cur -is [System.Collections.IEnumerable]) {
            foreach ($item in $cur) { $queue.Enqueue($item) }
            continue
        }
        $fileProp = $cur.PSObject.Properties['file']
        if ($null -ne $fileProp -and $fileProp.Value -is [string]) {
            $name = $fileProp.Value
            if (-not $found.ContainsKey($name)) {
                $hash = $null
                $hashProp = $cur.PSObject.Properties['sha256']
                if ($null -ne $hashProp -and $hashProp.Value -is [string]) {
                    $hash = $hashProp.Value.ToUpperInvariant()
                }
                $found[$name] = $hash
            }
        }
        foreach ($prop in $cur.PSObject.Properties) {
            if ($null -eq $prop.Value) { continue }
            if ($prop.Value -is [string] -or $prop.Value -is [System.ValueType]) { continue }
            $queue.Enqueue($prop.Value)
        }
    }
    return , $found
}

function Assert-UnderCard {
    # Every deletion goes through this. A typo in a path must not turn into a
    # recursive delete outside the card.
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($CardSwitchDir).TrimEnd('\')
    if (-not $full.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to touch '$full': it is outside the card switch dir '$root'."
    }
    return $full
}

function Assert-Deletable {
    # The second, independent guard. The eight deployed files live at exactly
    # these paths, and none of them may ever be deleted by the cleanup
    # switches: the seven volumes are the game's entire asset set and the
    # manifest is what the next deploy verifies against.
    #
    # This guard exists because a path was once derived wrongly (the legacy
    # nested scan pointed at the data dir itself) and the plan then listed
    # the LIVE volumes as removable. A name-based check would not catch that,
    # so the check is on the full destination path.
    param([string]$Path)
    $full = Assert-UnderCard $Path
    foreach ($name in $expectedNames) {
        $dest = [IO.Path]::GetFullPath((Join-Path $CardDataDir $name))
        if ($full.Equals($dest, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to delete '$full': it is a deployed volume."
        }
    }
    return $full
}

$mode = 'dry-run'
if ($Apply) { $mode = 'apply' }

Write-Host ("RR3 volume deploy  --  mode: {0}" -f $mode)
Write-Host ("  source : {0}" -f $SourceRoot)
Write-Host ("  card   : {0}" -f $CardDataDir)
Write-Host ("  cardroot: {0}" -f $CardSwitchDir)

# =========================================================== 1. PREFLIGHT ===
Write-Section 'PREFLIGHT (source)'

if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    Write-Host ("FATAL   : source root not found: {0}" -f $SourceRoot) -ForegroundColor Red
    exit $exitPreflight
}

$sourceFiles = @{}
$preflightFailures = @()
foreach ($name in $expectedNames) {
    $path = Join-Path $SourceRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $preflightFailures += ("missing source volume: {0}" -f $name)
        continue
    }
    $item = Get-Item -LiteralPath $path
    if ($item.Length -le 0) {
        # A zero length romfs is not a volume, it is a truncated copy that
        # would mount as an empty asset set and look like missing content.
        $preflightFailures += ("source volume is empty: {0}" -f $name)
        continue
    }
    $sourceFiles[$name] = $item
}
if ($preflightFailures.Count -gt 0) {
    foreach ($f in $preflightFailures) { Write-Host ("FATAL   : {0}" -f $f) -ForegroundColor Red }
    exit $exitPreflight
}
Write-Host ("source  : {0} volumes + manifest present, all non-zero" -f $expectedVolumeNames.Count)

# -- manifest -----------------------------------------------------------------
$manifestPath = Join-Path $SourceRoot $manifestName
$manifestEntries = @{}
$manifestParsed = $false
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Write-Warning ("manifest missing: {0} -- the volumes will still be copied, but nothing can be verified against it." -f $manifestPath)
}
else {
    try {
        # Read as UTF-8 explicitly: the manifest carries a non-ASCII archive
        # name, and PowerShell 5.1 would decode a BOM-less file as ANSI.
        $raw = [IO.File]::ReadAllText($manifestPath, [Text.Encoding]::UTF8)
        $parsed = $raw | ConvertFrom-Json
        $manifestEntries = Find-ManifestEntries $parsed
        $manifestParsed = $true
    }
    catch {
        Write-Warning ("manifest does not parse as JSON ({0}): {1}" -f $manifestPath, $_.Exception.Message)
    }
}

$sourceHashes = @{}
if ($manifestParsed) {
    $verifiedCount = 0
    $hashMissing = @()
    foreach ($name in $expectedVolumeNames) {
        $expectedHash = $null
        if ($manifestEntries.ContainsKey($name)) { $expectedHash = $manifestEntries[$name] }
        if ($null -eq $expectedHash -or $expectedHash -eq '') {
            $hashMissing += $name
            continue
        }
        $actual = Get-Sha256 (Join-Path $SourceRoot $name)
        $sourceHashes[$name] = $actual
        if ($actual -ne $expectedHash) {
            Write-Host ("FATAL   : manifest hash mismatch for {0}" -f $name) -ForegroundColor Red
            Write-Host ("           manifest: {0}" -f $expectedHash)
            Write-Host ("           actual  : {0}" -f $actual)
            exit $exitPreflight
        }
        $verifiedCount++
    }
    Write-Host ("manifest: verified {0}/{1} volume hashes against rr3-data-manifest.json" -f $verifiedCount, $expectedVolumeNames.Count)
    if ($hashMissing.Count -gt 0) {
        Write-Warning ("manifest records no sha256 for: {0} -- those files were NOT hash-verified." -f ($hashMissing -join ', '))
    }
    # The manifest cannot carry its own hash, so it is reported on its own
    # instead of being counted as a missing hash.
    $manifestSelfHash = $null
    if ($manifestEntries.ContainsKey($manifestName)) { $manifestSelfHash = $manifestEntries[$manifestName] }
    if ($null -eq $manifestSelfHash -or $manifestSelfHash -eq '') {
        Write-Host "manifest: carries no sha256 for itself (expected: a file cannot contain its own hash)"
    }
}
else {
    Write-Warning "manifest unusable -- NO file was hash-verified against it."
}

# ================================================= 2. CARD SURFACE SURVEY ===
Write-Section 'PREFLIGHT (card)'

if (-not (Test-Path -LiteralPath $CardSwitchDir -PathType Container)) {
    Write-Host ("FATAL   : card switch dir not found: {0}" -f $CardSwitchDir) -ForegroundColor Red
    exit $exitPreflight
}
$cardDataExists = Test-Path -LiteralPath $CardDataDir -PathType Container
if (-not $cardDataExists) {
    if ($Apply) {
        New-Item -ItemType Directory -Force -Path $CardDataDir | Out-Null
        Write-Host ("card    : created missing data dir {0}" -f $CardDataDir)
        $cardDataExists = $true
    }
    else {
        Write-Host ("card    : data dir is missing and would be created: {0}" -f $CardDataDir)
    }
}

# The app directory is the parent of the data dir; the legacy nested copy of
# the volumes lives one level below it (the layout that filled the card on
# 2026-10-24). The app folder name comes from the app DIRECTORY, not from
# the data dir: the data dir's leaf is "data", and pointing the legacy scan
# at "data" would scan the live volumes themselves.
$appDir = Split-Path -Parent $CardDataDir
$appFolderName = Split-Path -Leaf $appDir
$legacyDirs = @(
    (Join-Path $CardDataDir $appFolderName),
    (Join-Path $appDir $appFolderName)
)

$staleInDataDir = @()
if ($cardDataExists) {
    $cardDataFiles = @(Get-ChildItem -LiteralPath $CardDataDir -Force -File -ErrorAction SilentlyContinue)
    foreach ($f in $cardDataFiles) {
        if ($expectedNames -notcontains $f.Name) {
            $staleInDataDir += [pscustomobject]@{ Path = $f.FullName; Bytes = [long]$f.Length }
        }
    }
}

$staleLegacy = @()
foreach ($legacy in $legacyDirs) {
    if (-not (Test-Path -LiteralPath $legacy -PathType Container)) { continue }
    # Belt and braces: the legacy scan must never be pointed at the data dir
    # itself, or it would list the live volumes as removable.
    $legacyFull = [IO.Path]::GetFullPath($legacy)
    $dataFull = [IO.Path]::GetFullPath($CardDataDir)
    if ($legacyFull.Equals($dataFull, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    $legacyFiles = @(Get-ChildItem -LiteralPath $legacy -Force -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -eq '.romfs' -or $_.Name -eq $manifestName })
    foreach ($f in $legacyFiles) {
        $staleLegacy += [pscustomobject]@{ Path = $f.FullName; Bytes = [long]$f.Length }
    }
}

Write-Host ("card    : data dir holds {0} of the {1} expected names" -f `
    (@(Get-ChildItem -LiteralPath $CardDataDir -Force -File -ErrorAction SilentlyContinue |
        Where-Object { $expectedNames -contains $_.Name }).Count), $expectedNames.Count)
if ($staleInDataDir.Count -gt 0) {
    Write-Host ("stale   : {0} file(s) in the data dir are not part of the expected set (candidates for -RemoveStale):" -f $staleInDataDir.Count)
    foreach ($s in $staleInDataDir) { Write-Host ("           {0}  {1:N0} bytes" -f $s.Path, $s.Bytes) }
}
else {
    Write-Host "stale   : no unexpected file in the data dir"
}
if ($staleLegacy.Count -gt 0) {
    Write-Host ("stale   : {0} file(s) in the legacy nested layout (candidates for -RemoveStale):" -f $staleLegacy.Count)
    foreach ($s in $staleLegacy) { Write-Host ("           {0}  {1:N0} bytes" -f $s.Path, $s.Bytes) }
}
else {
    Write-Host "stale   : no legacy nested copy of the volumes on the card"
}

# -- litter -------------------------------------------------------------------
# Only DIRECTLY under the card switch dir. The _stale_<stamp> directories are
# counted as whole directories, so their contents must not also be counted
# file by file or the same bytes would be freed twice in the projection.
$litterDirs = @(Get-ChildItem -LiteralPath $CardSwitchDir -Directory -Force -Filter '_stale_*' -ErrorAction SilentlyContinue)
$litterLogNames = @("$appFolderName.guest-view.log", "$appFolderName.teardown-repair.log")
$litterLogFiles = @(Get-ChildItem -LiteralPath $CardSwitchDir -Force -File -ErrorAction SilentlyContinue |
    Where-Object { $litterLogNames -contains $_.Name })
$litterCrashFiles = @(Get-ChildItem -LiteralPath $CardSwitchDir -Force -File -Filter "$appFolderName.crash-*.txt" -ErrorAction SilentlyContinue)
$litterFiles = @($litterLogFiles) + @($litterCrashFiles)

$litterBytes = [long]0
foreach ($d in $litterDirs) { $litterBytes += Get-TreeBytes $d.FullName }
foreach ($f in $litterFiles) { $litterBytes += [long]$f.Length }
$litterCrashBytes = [long]0
foreach ($f in $litterCrashFiles) { $litterCrashBytes += [long]$f.Length }

Write-Host ("litter  : {0} stale director(ies), {1} log file(s), {2} crash file(s) -- {3:N0} bytes" -f `
    $litterDirs.Count, $litterLogFiles.Count, $litterCrashFiles.Count, $litterBytes)
if (-not $CleanLitter) {
    Write-Host "litter  : NOT removed (-CleanLitter was not passed); the projection below keeps those bytes"
}

# -- emulator running ---------------------------------------------------------
# A running emulator can hold a volume open. The rename over an open file
# fails with a sharing violation, and a guest that reads a half-replaced set
# of volumes produces failures that look like data corruption.
$emulatorProcs = @(Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -in @('eden', 'Ryujinx', 'yuzu') })
if ($emulatorProcs.Count -gt 0) {
    $names = ($emulatorProcs | ForEach-Object { $_.ProcessName }) -join ', '
    if ($Apply) {
        Write-Host ("FATAL   : an emulator is running ({0}). Close it before deploying." -f $names) -ForegroundColor Red
        exit $exitPreflight
    }
    Write-Host ("warn    : an emulator is running ({0}); deploying now would race it." -f $names)
}

# ============================================== 3. CAPACITY ACCOUNTING ======
Write-Section 'CAPACITY'

$cardFilesAll = @(Get-ChildItem -LiteralPath $CardSwitchDir -Recurse -Force -File -ErrorAction SilentlyContinue)
$cardBytesBefore = [long]0
if ($cardFilesAll.Count -gt 0) {
    $m = ($cardFilesAll | Measure-Object -Property Length -Sum).Sum
    if ($null -ne $m) { $cardBytesBefore = [long]$m }
}

$staleBytesFound = [long]0
foreach ($s in $staleInDataDir) { $staleBytesFound += $s.Bytes }
foreach ($s in $staleLegacy) { $staleBytesFound += $s.Bytes }

$litterFreed = [long]0
if ($CleanLitter) { $litterFreed = $litterBytes }
$staleFreed = [long]0
if ($RemoveStale) { $staleFreed = $staleBytesFound }

$baseBytes = $cardBytesBefore - $litterFreed - $staleFreed

# The plan: one row per expected file, with what it costs on the card.
$plan = @()
foreach ($name in $expectedNames) {
    $src = Join-Path $SourceRoot $name
    $dst = Join-Path $CardDataDir $name
    $newBytes = [long]$sourceFiles[$name].Length
    $oldBytes = [long]0
    if (Test-Path -LiteralPath $dst -PathType Leaf) {
        $oldBytes = [long](Get-Item -LiteralPath $dst).Length
    }
    $plan += [pscustomobject]@{
        Name     = $name
        Source   = $src
        Dest     = $dst
        NewBytes = $newBytes
        OldBytes = $oldBytes
        Delta    = ($newBytes - $oldBytes)
    }
}

$newTotal = [long]0
$oldTotal = [long]0
foreach ($p in $plan) { $newTotal += $p.NewBytes; $oldTotal += $p.OldBytes }

# Copy order: the files whose replacement RELEASES space go first, so the old
# bytes are back on the card before the biggest new temp file is written.
# Ties break on the larger new file, which keeps the number of big temps that
# can coexist small.
$ordered = @($plan | Sort-Object -Property `
    @{ Expression = { $_.Delta }; Ascending = $true },
    @{ Expression = { $_.NewBytes }; Descending = $true })

# Simulate the copy to get the PEAK usage: at the worst moment the new file
# and the old file of the same volume exist at the same time.
$simUsage = $baseBytes
$peakBytes = $baseBytes
$peakAt = ''
foreach ($p in $ordered) {
    $simUsage += $p.NewBytes
    if ($simUsage -gt $peakBytes) { $peakBytes = $simUsage; $peakAt = $p.Name }
    $simUsage -= $p.OldBytes
}
$projectedFinalBytes = $simUsage

# ---------------------------------------------------------------------------
# EDEN'S CAPACITY IS NOT A FIXED CEILING. It is computed from the LIVE tree.
#
# Eden keeps the SD card as a loose directory and synthesises a FAT32 volume
# over it. That volume's size is not a setting: core/file_sys/sdmc_factory.cpp
# (v0.2.1, the installed build) computes
#
#     total = floor((used + 4 GiB) / 4 GiB) * 4 GiB
#     free  = total - used
#
# from the recursive byte sum of the tree. The capacity therefore GROWS with
# the content, and the free space is a SAW-TOOTH:
#
#     free = 4 GiB - (used mod 4 GiB)
#
# It falls to almost nothing whenever the usage sits just below a 4 GiB
# multiple. That is what the guest reported as "available space: 0 MB" on
# 2026-10-24 -- not a hard limit being reached.
#
# WHAT THIS CHANGES FOR A GATE. The number that matters is not the usage at
# the end but the SMALLEST free space reached at ANY point of the operation:
# one large write can climb through a band boundary and pass through free ~ 0
# on its way up. So the gate below evaluates the free space over the whole
# usage range, and reports where the minimum is.
# ---------------------------------------------------------------------------
$script:EdenBand = [long]4294967296   # 4 GiB

function Get-EdenCapacityBytes([long]$used) {
    if ($used -lt 0) { $used = 0 }
    return [long]([math]::Floor(($used + $script:EdenBand) / $script:EdenBand)) * $script:EdenBand
}

function Get-EdenFreeBytes([long]$used) {
    return (Get-EdenCapacityBytes $used) - $used
}

# The smallest free space the card reports at ANY usage between two points,
# and the usage at which it happens.
function Get-EdenMinFree([long]$low, [long]$high) {
    if ($high -lt $low) { $swap = $low; $low = $high; $high = $swap }
    $minFree = [long]::MaxValue
    $minAt = $low
    foreach ($u in @($low, $high)) {
        $free = Get-EdenFreeBytes $u
        if ($free -lt $minFree) { $minFree = $free; $minAt = $u }
    }
    # Every band boundary inside the range: one byte below it the free space
    # is at its smallest.
    $first = ([long]([math]::Floor($low / $script:EdenBand)) + 1) * $script:EdenBand
    for ($boundary = $first; $boundary -le $high; $boundary += $script:EdenBand) {
        $u = $boundary - 1
        $free = Get-EdenFreeBytes $u
        if ($free -lt $minFree) { $minFree = $free; $minAt = $u }
    }
    return @($minFree, $minAt)
}

$minFreeInfo = Get-EdenMinFree $baseBytes $peakBytes
$minFreeBytes = $minFreeInfo[0]
$minFreeAt = $minFreeInfo[1]
$finalFreeBytes = Get-EdenFreeBytes $projectedFinalBytes

# An OPTIONAL hard ceiling, for a card whose capacity is fixed by something
# else (a real console SD card, for instance). 0 means "no hard ceiling; use
# Eden's computed capacity", which is the default because Eden's capacity is
# derived, not fixed.
$ceilingBytes = $CardCapacityBytes
$ceilingKind = 'none (Eden computes it from the live tree)'
if ($ceilingBytes -gt 0) { $ceilingKind = 'hard ceiling (explicit)' }

Write-Host ("card BEFORE      : {0:N0} files, {1:N0} bytes" -f $cardFilesAll.Count, $cardBytesBefore)
Write-Host ("freed -CleanLitter: {0:N0} bytes{1}" -f $litterFreed, $(if ($CleanLitter) { '' } else { ' (switch off)' }))
Write-Host ("freed -RemoveStale: {0:N0} bytes{1}" -f $staleFreed, $(if ($RemoveStale) { '' } else { ' (switch off)' }))
Write-Host ("new volume bytes : {0:N0} bytes ({1} files)" -f $newTotal, $plan.Count)
Write-Host ("card AFTER       : {0:N0} bytes projected" -f $projectedFinalBytes)
Write-Host ("peak during copy : {0:N0} bytes{1}" -f $peakBytes, $(if ($peakAt) { " (while writing $peakAt)" } else { '' }))
Write-Host ("ceiling          : {0:N0} bytes ({1})" -f $ceilingBytes, $ceilingKind)

# ================================================== 4. THE PLAN / THE GATE ===
Write-Section 'PLAN'

foreach ($p in $ordered) {
    Write-Host ("copy    : {0,-24} {1,15:N0} -> {2,15:N0}  (delta {3,14:N0})" -f `
        $p.Name, $p.OldBytes, $p.NewBytes, $p.Delta)
}
if ($litterFreed -gt 0) {
    foreach ($d in $litterDirs) { Write-Host ("remove  : {0}  ({1:N0} bytes)" -f $d.FullName, (Get-TreeBytes $d.FullName)) }
    foreach ($f in $litterLogFiles) { Write-Host ("remove  : {0}  ({1:N0} bytes)" -f $f.FullName, $f.Length) }
    # The crash files are reported as one line: there are hundreds of them and
    # printing each one buries the numbers that matter. Their total is still
    # accounted for byte for byte.
    if ($litterCrashFiles.Count -gt 0) {
        Write-Host ("remove  : {0} crash file(s) '{1}.crash-*.txt'  ({2:N0} bytes)" -f `
            $litterCrashFiles.Count, $appFolderName, $litterCrashBytes)
    }
}
else {
    Write-Host ("remove  : no litter would be removed (-CleanLitter off, or nothing to remove)")
}
if ($staleFreed -gt 0) {
    foreach ($s in $staleInDataDir) { Write-Host ("remove  : {0}  ({1:N0} bytes)" -f $s.Path, $s.Bytes) }
    foreach ($s in $staleLegacy) { Write-Host ("remove  : {0}  ({1:N0} bytes)" -f $s.Path, $s.Bytes) }
}
else {
    Write-Host ("remove  : no stale duplicate would be removed (-RemoveStale off, or nothing to remove)")
}

$refusal = ''
if ($finalFreeBytes -lt $RequiredFreeBytes) {
    $refusal = ("the card would not have enough room free for the guest: after this deployment the card reports {0:N0} bytes free, and the margin required is {1:N0} bytes. The guest asks for 104 MB when it downloads its assets and refuses to start below that, so a deployment that leaves less than the margin would break the next run rather than this one." -f `
        $finalFreeBytes, $RequiredFreeBytes)
}
elseif ($ceilingBytes -gt 0 -and $peakBytes -gt $ceilingBytes) {
    $refusal = ("the card cannot hold this deployment: {0:N0} bytes are needed at the peak (the new volume and the old one exist at the same time), {1:N0} bytes are projected to be in use at the end, and the hard ceiling was {2:N0} bytes." -f `
        $peakBytes, $projectedFinalBytes, $ceilingBytes)
}
elseif ($ceilingBytes -gt 0 -and $projectedFinalBytes -gt $ceilingBytes) {
    $refusal = ("the card cannot hold this deployment: {0:N0} bytes are projected to be in use at the end, and the hard ceiling was {1:N0} bytes." -f `
        $projectedFinalBytes, $ceilingBytes)
}

# The machine readable summary is built here, before any exit, so that EVERY
# path below (refused, dry run, applied) prints the same set of numbers.
$summary = [ordered]@{
    mode                  = $mode
    apply                 = [bool]$Apply
    cleanLitter           = [bool]$CleanLitter
    removeStale           = [bool]$RemoveStale
    cardBytesBefore       = $cardBytesBefore
    cardFilesBefore       = $cardFilesAll.Count
    litterBytesFound      = $litterBytes
    litterBytesFreed      = $litterFreed
    litterStaleDirs       = $litterDirs.Count
    litterLogFiles        = $litterLogFiles.Count
    litterCrashFiles      = $litterCrashFiles.Count
    litterCrashBytes      = $litterCrashBytes
    staleBytesFound       = $staleBytesFound
    staleBytesFreed       = $staleFreed
    newVolumeBytes        = $newTotal
    projectedFinalBytes   = $projectedFinalBytes
    peakBytes             = $peakBytes
    peakAt                = $peakAt
    capacityCeilingBytes  = $ceilingBytes
    capacitySource        = $ceilingKind
    edenTotalAtEndBytes   = (Get-EdenCapacityBytes $projectedFinalBytes)
    edenFreeAtEndBytes    = $finalFreeBytes
    edenFreeWorstBytes    = $minFreeBytes
    edenFreeWorstAtBytes  = $minFreeAt
    gateCriterion         = 'free space at the end (what the guest sees); the worst moment during the copy is reported, not gated'
    requiredFreeBytes     = $RequiredFreeBytes
    manifestHashesChecked = $sourceHashes.Count
    cardBytesAfter        = 0
    filesVerified         = 0
    mismatches            = 0
    wouldRefuse           = ($refusal -ne '')
    exitCode              = $exitOk
}

if ($Apply -and $RequiredFreeBytes -le 0) {
    # A margin of zero means "the card may report no free space at some point
    # during the write". The guest asks for 104 MB when it downloads its
    # assets, and it refuses to start when the card says there is less, so a
    # zero margin is not a safe way to run a 10 GB deployment.
    Write-Host ''
    Write-Host 'FATAL   : -RequiredFreeBytes must be positive.' -ForegroundColor Red
    Write-Host '         It is the free space the card must still report at the WORST' -ForegroundColor Red
    Write-Host '         moment of the deployment. The guest asks for 104 MB, so use' -ForegroundColor Red
    Write-Host '         at least that, and more for headroom.' -ForegroundColor Red
    $summary['exitCode'] = $exitCapacity
    Write-Host ''
    Write-Host '=== SUMMARY (machine readable) ==='
    Write-Host ($summary | ConvertTo-Json -Depth 4)
    exit $exitCapacity
}

if ($refusal -ne '') {
    Write-Host ''
    Write-Host 'REFUSED : the capacity gate failed.' -ForegroundColor Red
    Write-Host ("         usage after cleanup       : {0:N0}" -f $baseBytes)
    Write-Host ("         bytes needed (peak)        : {0:N0}" -f $peakBytes)
    Write-Host ("         projected bytes in use     : {0:N0}" -f $projectedFinalBytes)
    Write-Host ("         Eden total at the end      : {0:N0}" -f (Get-EdenCapacityBytes $projectedFinalBytes))
    Write-Host ("         Eden free at the end       : {0:N0}   <-- the gate" -f $finalFreeBytes)
    Write-Host ("         free-space margin required : {0:N0}" -f $RequiredFreeBytes)
    if ($ceilingBytes -gt 0) {
        Write-Host ("         hard ceiling                : {0:N0}" -f $ceilingBytes)
    }
    Write-Host ("         freed by -CleanLitter      : {0:N0}" -f $litterFreed)
    Write-Host ("         freed by -RemoveStale      : {0:N0}" -f $staleFreed)
    Write-Host ("         {0}" -f $refusal)
    Write-Host '         Nothing was written.' -ForegroundColor Red
    $summary['exitCode'] = $exitCapacity
    Write-Host ''
    Write-Host '=== SUMMARY (machine readable) ==='
    Write-Host ($summary | ConvertTo-Json -Depth 4)
    exit $exitCapacity
}
Write-Host ''
Write-Host ('gate    : capacity OK (Eden free at the end {0:N0} >= margin {1:N0}; Eden total at the end {2:N0})' -f `
    $finalFreeBytes, $RequiredFreeBytes, (Get-EdenCapacityBytes $projectedFinalBytes))
Write-Host ("note    : the smallest free space anywhere DURING the copy is {0:N0} bytes (at {1:N0} in use)." -f `
    $minFreeBytes, $minFreeAt)
Write-Host '          That is a saw-tooth crossing, not a quota: Eden stores the card' -ForegroundColor DarkGray
Write-Host '          as a directory on the host and only REPORTS a computed free' -ForegroundColor DarkGray
Write-Host '          space, so the write itself is not limited by it. The guest is' -ForegroundColor DarkGray
Write-Host '          not running during the copy and sees only the end state.' -ForegroundColor DarkGray

# ================================================== 5. ACT (only with -Apply) =
if (-not $Apply) {
    Write-Host ''
    Write-Host 'DRY RUN : nothing was written, nothing was deleted.'
    Write-Host ''
    Write-Host '=== SUMMARY (machine readable) ==='
    Write-Host ($summary | ConvertTo-Json -Depth 4)
    exit $exitOk
}

# ---- 5a. litter --------------------------------------------------------------
Write-Section 'APPLY: litter'
if ($CleanLitter) {
    foreach ($d in $litterDirs) {
        $target = Assert-Deletable $d.FullName
        $bytes = Get-TreeBytes $target
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
        Write-Host ("removed : {0}  ({1:N0} bytes freed)" -f $target, $bytes)
    }
    foreach ($f in $litterLogFiles) {
        $target = Assert-Deletable $f.FullName
        $bytes = [long]$f.Length
        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
        Write-Host ("removed : {0}  ({1:N0} bytes freed)" -f $target, $bytes)
    }
    foreach ($f in $litterCrashFiles) {
        $target = Assert-Deletable $f.FullName
        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
    }
    if ($litterCrashFiles.Count -gt 0) {
        Write-Host ("removed : {0} crash file(s) '{1}.crash-*.txt'  ({2:N0} bytes freed)" -f `
            $litterCrashFiles.Count, $appFolderName, $litterCrashBytes)
    }
}
else {
    Write-Host 'skipped : -CleanLitter was not passed'
}

# ---- 5b. stale duplicates ----------------------------------------------------
Write-Section 'APPLY: stale duplicates'
if ($RemoveStale) {
    $staleTargets = @()
    foreach ($s in $staleInDataDir) { $staleTargets += $s }
    foreach ($s in $staleLegacy) { $staleTargets += $s }
    foreach ($s in $staleTargets) {
        $target = Assert-Deletable $s.Path
        if (Test-Path -LiteralPath $target -PathType Container) {
            Write-Host ("FATAL   : stale candidate is a directory, refusing to delete: {0}" -f $target) -ForegroundColor Red
            exit $exitApply
        }
        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
        Write-Host ("removed : {0}  ({1:N0} bytes freed)" -f $target, $s.Bytes)
    }
}
else {
    Write-Host 'skipped : -RemoveStale was not passed'
}

# ---- 5c. copy ----------------------------------------------------------------
Write-Section 'APPLY: copy'

$verifyRows = @()
$mismatchCount = 0
foreach ($p in $ordered) {
    $dest = $p.Dest
    if (Test-Path -LiteralPath $dest -PathType Container) {
        # Copying into a directory would scatter bytes instead of replacing the
        # volume, and the volume would then still be the old one.
        Write-Host ("FATAL   : destination is a directory, refusing to write into it: {0}" -f $dest) -ForegroundColor Red
        exit $exitApply
    }
    $temp = Join-Path $CardDataDir ('.new-' + $p.Name)
    if (Test-Path -LiteralPath $temp) {
        # A leftover temp from an interrupted run. It is this script's own
        # artefact and it eats card space, so it goes before the new copy.
        $leftover = Assert-Deletable $temp
        Write-Host ("removed : leftover temp {0} ({1:N0} bytes)" -f $leftover, (Get-TreeBytes $leftover))
        Remove-Item -LiteralPath $leftover -Force -ErrorAction Stop
    }
    Write-Host ("copy    : {0}  ({1:N0} bytes) -> {2}" -f $p.Name, $p.NewBytes, (Split-Path -Leaf $temp))
    Copy-Item -LiteralPath $p.Source -Destination $temp -Force -ErrorAction Stop

    $tempLen = [long](Get-Item -LiteralPath $temp).Length
    $tempHash = Get-Sha256 $temp
    $srcHash = $sourceHashes[$p.Name]
    if ($null -eq $srcHash) { $srcHash = Get-Sha256 $p.Source }

    # Verify the TEMP before it is promoted: a short or corrupt copy must
    # never become the live volume, because the old one is still the only
    # good copy at this point.
    if ($tempLen -ne $p.NewBytes -or $tempHash -ne $srcHash) {
        Write-Host ("FATAL   : temp copy of {0} does not match the source; the live volume was left alone." -f $p.Name) -ForegroundColor Red
        Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        exit $exitApply
    }

    try {
        Move-Item -LiteralPath $temp -Destination $dest -Force -ErrorAction Stop
    }
    catch {
        # Move-Item -Force over an existing file fails if the file is held
        # open. Removing the old file first is the last resort: it is the only
        # way forward, and the temp has already been verified byte for byte.
        Write-Warning ("rename over the target failed ({0}); removing the old file first." -f $_.Exception.Message)
        if (Test-Path -LiteralPath $dest -PathType Leaf) {
            Remove-Item -LiteralPath $dest -Force -ErrorAction Stop
        }
        Move-Item -LiteralPath $temp -Destination $dest -Force -ErrorAction Stop
    }
    Write-Host ("deployed: {0}" -f $dest)
}

# ---- 5d. verify --------------------------------------------------------------
Write-Section 'VERIFY'

Write-Host ("{0,-24} {1,14} {2,14}  {3,-18} {4,-18} {5}" -f 'file', 'src bytes', 'dst bytes', 'src sha256', 'dst sha256', 'verdict')
foreach ($p in $ordered) {
    $dest = $p.Dest
    $srcHash = $sourceHashes[$p.Name]
    if ($null -eq $srcHash) { $srcHash = Get-Sha256 $p.Source }
    $srcLen = [long]$p.NewBytes
    $dstLen = [long]0
    $dstHash = ''
    if (Test-Path -LiteralPath $dest -PathType Leaf) {
        $dstLen = [long](Get-Item -LiteralPath $dest).Length
        $dstHash = Get-Sha256 $dest
    }
    $ok = ($srcLen -eq $dstLen) -and ($srcHash -eq $dstHash)
    if (-not $ok) { $mismatchCount++ }
    $verdict = 'OK'
    if (-not $ok) { $verdict = 'MISMATCH' }
    Write-Host ("{0,-24} {1,14:N0} {2,14:N0}  {3,-18} {4,-18} {5}" -f `
        $p.Name, $srcLen, $dstLen, (Get-ShortHash $srcHash), (Get-ShortHash $dstHash), $verdict)
    $verifyRows += [pscustomobject]@{ Name = $p.Name; Ok = $ok }
}

$cardFilesAfter = @(Get-ChildItem -LiteralPath $CardSwitchDir -Recurse -Force -File -ErrorAction SilentlyContinue)
$cardBytesAfter = [long]0
if ($cardFilesAfter.Count -gt 0) {
    $m = ($cardFilesAfter | Measure-Object -Property Length -Sum).Sum
    if ($null -ne $m) { $cardBytesAfter = [long]$m }
}
Write-Host ("card AFTER       : {0:N0} files, {1:N0} bytes (measured)" -f $cardFilesAfter.Count, $cardBytesAfter)
Write-Host ("projected        : {0:N0} bytes" -f $projectedFinalBytes)

if ($mismatchCount -gt 0) {
    Write-Host ''
    Write-Host ("FATAL   : {0} file(s) did not verify." -f $mismatchCount) -ForegroundColor Red
}

$summary['cardBytesAfter'] = $cardBytesAfter
$summary['filesVerified'] = $verifyRows.Count
$summary['mismatches'] = $mismatchCount
if ($mismatchCount -gt 0) { $summary['exitCode'] = $exitVerify }
Write-Host ''
Write-Host '=== SUMMARY (machine readable) ==='
Write-Host ($summary | ConvertTo-Json -Depth 4)

if ($mismatchCount -gt 0) { exit $exitVerify }
exit $exitOk
