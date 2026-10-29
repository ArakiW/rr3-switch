<#
.SYNOPSIS
    Build a private, isolated Eden instance for this project, so that another
    project's stress runs cannot kill our emulator or overwrite our logs.

.DESCRIPTION
    WHY THIS EXISTS.

    On 2026-10-29 the run tagged q19 died after 112 seconds and the archive
    was worthless.  The cause was NOT in the port:

      * E:\AI\ani-switch\scripts\eden_stress_run.ps1 runs its own Eden on the
        SAME E:\Eden installation, and before every run it executes

            Get-Process -Name 'eden','eden-cli','Eden','Eden-cli' | Kill

        which matches our emulator by PROCESS NAME and kills it.
      * Both instances share E:\Eden\user\log\eden_log.txt.  Eden truncates
        that file at startup, so the second instance to start DESTROYS the
        first one's log.  Both files in the q19 archive (eden_log.txt.tail
        and eden_log.txt.old.txt) say "Loading aniswitch" and contain zero
        mentions of "rr3" -- they are the other project's log, not ours.

    The archived Eden log of a run is the ONLY record of the emulator's own
    view of that run (unmapped pages, GPU faults, the loaded title).  Losing
    it to an unrelated process makes a run uninterpretable, and it does so
    SILENTLY: the harness just reports whatever log it finds.

    WHAT THIS SCRIPT BUILDS.

    A second Eden installation at E:\Eden-rr3 with:

      1. A DIFFERENT EXECUTABLE NAME.  eden.exe is copied to eden-rr3.exe, so
         the other project's kill-by-name does not match it, and the other
         project's Resolve-EdenRoot (which looks for eden-cli.exe / eden.exe)
         does not find it either.

      2. ITS OWN user\ DIRECTORY, so the emulator log, the crash dumps, the
         NAND, the shader cache and the config state are all separate.

      3. ITS OWN SD CARD COPY, so a frame capture, a guest log or a litter
         directory written by one project can never be mistaken for the
         other's, and so the card's free-space accounting is ours alone.

    The card is a full copy rather than a set of junctions on purpose: the
    guest WRITES to the card (save files, .depot caches, the guest log), and
    a junction would put those writes back into the other project's card.

    The other project's aniswitch application is EXCLUDED from the copy: it
    is not ours, it is 34 MB, and leaving it out keeps the card's free-space
    accounting clean.

.PARAMETER SourceRoot
    The Eden installation to copy from.  Default: E:\Eden

.PARAMETER TargetRoot
    Where to build the private instance.  Default: E:\Eden-rr3

.PARAMETER SkipCard
    Build everything except the SD card copy.  The card is ~11.8 GiB and
    takes the longest; use this when the card is already in place.

.PARAMETER Apply
    Actually copy.  Without it the script only reports what it would do.

.EXAMPLE
    .\Setup-IsolatedEden.ps1 -Apply
#>
[CmdletBinding()]
param(
    [string]$SourceRoot = 'E:\Eden',
    [string]$TargetRoot = 'E:\Eden-rr3',
    [switch]$SkipCard,
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'

# The guest's own application directory on the card.  It carries the game
# data (11.8 GiB) and the control files the port reads.
$appDirName = 'rr3-native-rehost'

function Write-Step([string]$Message) {
    Write-Host ("  {0}" -f $Message)
}

Write-Host "isolated eden: $SourceRoot -> $TargetRoot"

if (-not (Test-Path (Join-Path $SourceRoot 'eden.exe') -PathType Leaf)) {
    throw "No eden.exe under $SourceRoot"
}

if (-not $Apply) {
    Write-Host "dry run only; pass -Apply to build it"
    $srcCard = Join-Path $SourceRoot 'user\sdmc'
    if (Test-Path $srcCard) {
        $bytes = (Get-ChildItem $srcCard -Recurse -File -Force -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
        if ($null -eq $bytes) { $bytes = 0 }
        Write-Host ("  card copy: {0:N0} bytes ({1:N2} GiB)" -f $bytes, ($bytes / 1GB))
    }
    return
}

# --- 1. the executables, under a name the other project will not kill -------
New-Item -ItemType Directory -Force -Path $TargetRoot | Out-Null

foreach ($pair in @(@('eden.exe', 'eden-rr3.exe'), @('eden-cli.exe', 'eden-rr3-cli.exe'))) {
    $src = Join-Path $SourceRoot $pair[0]
    $dst = Join-Path $TargetRoot $pair[1]
    if (-not (Test-Path $src -PathType Leaf)) {
        Write-Step ("skip {0} (not in the source install)" -f $pair[0])
        continue
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Step ("exe    : {0} -> {1} ({2:N0} bytes)" -f $pair[0], $pair[1], (Get-Item $dst).Length)
}

# --- 2. the firmware and the keys -------------------------------------------
# The firmware directory is copied under its OWN name too: the install ships
# a second copy whose name is NOT ASCII ("23.0.0" plus the Chinese word for
# firmware), and a private instance with an empty one starts the GUI and
# never loads the game: measured on 2026-10-29, the first run of E:\Eden-rr3
# produced a 250-line Eden log that stopped at t=0.8 s in the AES noise, a
# guest log of 0 bytes and a CPU of 0.2 cores, and the guest never booted.
#
# The second directory is found by PATTERN, never by its literal name: the
# name contains non-ASCII characters, and PowerShell 5.1 reads a script file
# as GBK while the file system holds UTF-8, so a literal in the script does
# not match the directory on disk.
$firmwareDirs = @()
foreach ($dir in (Get-ChildItem $SourceRoot -Force -Directory -ErrorAction SilentlyContinue)) {
    if ($dir.Name -eq 'firmware' -or $dir.Name -eq 'keys') { continue }
    # The user directory is the emulator's own state, not firmware, and it is
    # handled separately below.  Skipping it here keeps the "alternate
    # firmware" copy from dragging 11 GB of card into the firmware folder.
    if ($dir.Name -eq 'user' -or $dir.Name -eq 'LICENSES') { continue }
    $files = @(Get-ChildItem $dir.FullName -Recurse -File -ErrorAction SilentlyContinue)
    # The alternate firmware copy is the one that holds the .nca/.ncd pairs.
    if (@($files | Where-Object { $_.Extension -eq '.nca' }).Count -gt 0) {
        $firmwareDirs += $dir
    }
}
foreach ($dir in $firmwareDirs) {
    $dst = Join-Path $TargetRoot $dir.Name
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    & robocopy.exe $dir.FullName $dst /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $($dir.Name) (code $LASTEXITCODE)" }
    Write-Step ("copy   : {0} ({1} files)" -f $dir.Name, @(Get-ChildItem $dst -Recurse -File -ErrorAction SilentlyContinue).Count)
}

foreach ($name in @('firmware', 'keys')) {
    $src = Join-Path $SourceRoot $name
    if (-not (Test-Path $src -PathType Container)) { continue }
    $dst = Join-Path $TargetRoot $name
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    & robocopy.exe $src $dst /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $name (code $LASTEXITCODE)" }
    $n = @(Get-ChildItem $dst -Recurse -File -ErrorAction SilentlyContinue).Count
    Write-Step ("copy   : {0} ({1} files)" -f $name, $n)
}
foreach ($name in @('prod.keys', 'LICENSE.txt', 'README.md')) {
    $src = Join-Path $SourceRoot $name
    if (Test-Path $src -PathType Leaf) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $TargetRoot $name) -Force
    }
}

# --- 3. the private user directory ------------------------------------------
# The configuration files hold ABSOLUTE paths into the old installation
# (E:/Eden/user/nand and friends).  They are rewritten so the private
# instance never touches the shared installation's state.
$dstUser = Join-Path $TargetRoot 'user'
foreach ($name in @('config', 'log', 'crash_dumps', 'dump', 'load', 'tas', 'amiibo', 'cache', 'icons', 'play_time', 'screenshots', 'shader', 'nand')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dstUser $name) | Out-Null
}

$srcConfig = Join-Path $SourceRoot 'user\config'
$dstConfig = Join-Path $dstUser 'config'
if (Test-Path $srcConfig -PathType Container) {
    & robocopy.exe $srcConfig $dstConfig /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for the config directory" }
}

# Rewrite the absolute paths.  Eden stores them with forward slashes, and the
# config also carries a "default=" twin for each key, which must be left
# alone: only the effective value line is rewritten.
$rewritten = 0
foreach ($file in @('qt-config.ini', 'sdl2-config.ini')) {
    $path = Join-Path $dstConfig $file
    if (-not (Test-Path $path -PathType Leaf)) { continue }
    $text = [System.IO.File]::ReadAllText($path)
    $from = ($SourceRoot -replace '\\', '/')
    $to = ($TargetRoot -replace '\\', '/')
    $new = $text.Replace($from, $to)
    if ($new -ne $text) {
        [System.IO.File]::WriteAllText($path, $new)
        $rewritten++
        Write-Step ("config : {0} rewritten to {1}" -f $file, $to)
    }
}
if ($rewritten -eq 0) {
    Write-Step "config : no absolute path needed rewriting (the source config is already relative)"
}

Write-Step "user   : private user directory created (config, log, crash_dumps, dump, load, tas, amiibo, cache, icons, play_time, screenshots, shader, nand)"

$srcNand = Join-Path $SourceRoot 'user\nand'
$dstNand = Join-Path $dstUser 'nand'
if (Test-Path $srcNand -PathType Container) {
    & robocopy.exe $srcNand $dstNand /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for the nand directory" }
    Write-Step "nand   : copied"
}

# --- 4. the private SD card --------------------------------------------------
if ($SkipCard) {
    Write-Step "card   : skipped (-SkipCard)"
}
else {
    $srcCard = Join-Path $SourceRoot 'user\sdmc'
    $dstCard = Join-Path $dstUser 'sdmc'
    if (-not (Test-Path $srcCard -PathType Container)) {
        throw "no card at $srcCard"
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $dstCard 'switch') | Out-Null

    # Everything on the card except the other project's application.
    & robocopy.exe $srcCard $dstCard /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 `
        /XD (Join-Path (Join-Path $srcCard 'switch') 'aniswitch') `
        /XF 'aniswitch.nro' 'aniswitch.nacp' | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for the card (code $LASTEXITCODE)" }

    $files = @(Get-ChildItem $dstCard -Recurse -File -Force -ErrorAction SilentlyContinue)
    $bytes = ($files | Measure-Object -Property Length -Sum).Sum
    if ($null -eq $bytes) { $bytes = 0 }
    Write-Step ("card   : copied ({0:N0} files, {1:N0} bytes, {2:N2} GiB)" -f `
        $files.Count, $bytes, ($bytes / 1GB))
}

# --- 5. report ---------------------------------------------------------------
Write-Host ""
Write-Host "the private instance is ready:"
Write-Host ("  exe     : {0}" -f (Join-Path $TargetRoot 'eden-rr3.exe'))
Write-Host ("  card    : {0}" -f (Join-Path (Join-Path $TargetRoot 'user\sdmc\switch') $appDirName))
Write-Host ("  log     : {0}" -f (Join-Path $TargetRoot 'user\log\eden_log.txt'))
Write-Host ""
Write-Host "run the harness against it with:"
Write-Host ("  .\Test-OnEden.ps1 -EmulatorRoot '{0}' -EmulatorExe 'eden-rr3.exe' -EmulatorName 'eden-rr3'" -f $TargetRoot)
