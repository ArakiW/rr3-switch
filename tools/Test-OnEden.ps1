<#
.SYNOPSIS
    Run the built RR3 NRO under the Eden emulator and capture the guest log.

.DESCRIPTION
    Wraps the manual loop documented in PROJECT_STATUS.md section 7 into one
    command so every iteration is reproducible:

      1. stash stale recovery markers / logs into a timestamped folder
      2. copy the freshly built NRO into Eden's SDMC
      3. launch Eden on it
      4. wait for the guest to make progress (log stops growing AND a tail
         marker is reached) or until -Seconds elapses
      5. kill Eden, archive the captured log under .build\eden-runs\

    Nothing is deleted: every previous marker and log is moved aside, so a run
    can always be compared against its predecessor.

.PARAMETER Seconds
    How long to let the guest run before capturing. Default 120.

.PARAMETER Tag
    Free-form label recorded in the archive folder name (e.g. the change you
    are testing). Default derived from the NRO timestamp.

.PARAMETER NoDeploy
    Skip the NRO copy and run whatever is already in Eden's SDMC.

.EXAMPLE
    .\Test-OnEden.ps1 -Seconds 120 -Tag permEnf-emulator-shortcircuit
#>
[CmdletBinding()]
param(
    [ValidateRange(10, 1800)]
    [int]$Seconds = 120,
    [string]$Tag = '',
    [switch]$NoDeploy,
    # Which emulator to drive. Eden is the default; CITRON (E:\Citron) is the
    # cross-check, because the guest's socket behaviour -- accept, poll, thread
    # scheduling -- lives in the emulator's HLE and a second implementation is
    # the only honest way to answer "is this the emulator's fault?".
    [string]$EmulatorRoot = 'E:\Eden',
    [string]$EmulatorExe = 'eden.exe',
    [string]$EmulatorName = 'eden'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildNro = Join-Path $projectRoot 'native-switch\rr3-native-rehost.nro'
$archiveRoot = Join-Path $projectRoot '.build\eden-runs'

$edenRoot = $EmulatorRoot
$edenExe = Join-Path $edenRoot $EmulatorExe
$sdmcSwitch = Join-Path $edenRoot 'user\sdmc\switch'
$appDir = Join-Path $sdmcSwitch 'rr3-native-rehost'
$deployedNro = Join-Path $appDir 'rr3-native-rehost.nro'

$markers = @(
    (Join-Path $sdmcSwitch 'rr3-native-rehost.guarded-action.pending'),
    (Join-Path $sdmcSwitch 'rr3-native-rehost.guest-view.log'),
    (Join-Path $sdmcSwitch 'rr3-native-rehost.egl-probe.log'),
    (Join-Path $sdmcSwitch 'rr3-native-rehost.guest-boot.progress'),
    # The teardown repair keeps its OWN log because the guest-view log is
    # written by several threads at once and demonstrably tears -- a good run
    # contains a shredded line mid-file, and another lost a whole line while the
    # producer provably ran. On a log that loses lines, "the repair never fired"
    # and "the repair's line was lost" are the same observation. One writer, one
    # file, no interleaving, so `wc -l` on it is an independent count.
    (Join-Path $sdmcSwitch 'rr3-native-rehost.teardown-repair.log')
)
$guestLog = $markers[1]

if (-not (Test-Path $edenExe -PathType Leaf)) {
    throw "Eden not found at $edenExe"
}

# --- 1. data volumes are the thing that actually takes time to provision ------
if (-not (Test-Path (Join-Path $appDir 'data\core.romfs') -PathType Leaf)) {
    throw "Data volumes missing under $appDir\data. Run tools\Deploy-RR3Data.ps1 first."
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# The emulator goes in the label: a CITRON run and an Eden run of the SAME
# binary are the cross-check, and an archive folder that cannot say which
# emulator produced it is worthless for that comparison.
$label = "$stamp-$EmulatorName"
if ($Tag) { $label += "-$($Tag -replace '[^A-Za-z0-9._-]', '_')" }
$stash = Join-Path $sdmcSwitch "_stale_$stamp"
New-Item -ItemType Directory -Force -Path $stash | Out-Null
New-Item -ItemType Directory -Force -Path $archiveRoot | Out-Null

foreach ($marker in $markers) {
    if (Test-Path $marker -PathType Leaf) {
        Move-Item -LiteralPath $marker -Destination (Join-Path $stash (Split-Path $marker -Leaf)) -Force
    }
}

# --- 2. deploy ---------------------------------------------------------------
if (-not $NoDeploy) {
    if (-not (Test-Path $buildNro -PathType Leaf)) {
        throw "No built NRO at $buildNro. Run tools\Build-NativeSwitch.ps1 first."
    }
    New-Item -ItemType Directory -Force -Path $appDir | Out-Null
    Copy-Item -LiteralPath $buildNro -Destination $deployedNro -Force
}
$built = Get-Item -LiteralPath $deployedNro
Write-Host ("deploy : {0}  {1} bytes  sha256={2}" -f $built.LastWriteTime, $built.Length, (Get-FileHash -LiteralPath $deployedNro -Algorithm SHA256).Hash.Substring(0, 16))

# The emulator opt-in files live on the SD card, NOT in the repository, so nothing
# restores them after a cleanup, a fresh card, or a stray deletion. Without
# rr3-gfx.conf, gfx_backend.cpp silently falls back to its default backend --
# Mesa, the one the real console uses. On Eden that is the wrong default: there
# is no DRM device, so Mesa's eglInitialize never returns and the run wedges at
# "view-egl-init-begin" with zero gfx-proc calls. That failure is indistinguishable
# from a code regression by symptom alone, and it cost a full misdiagnosis once.
# The harness guarantees the file instead of relying on someone remembering it.
$gfxConf = Join-Path $sdmcSwitch 'rr3-gfx.conf'
if (-not (Test-Path $gfxConf -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path $sdmcSwitch | Out-Null
    Set-Content -LiteralPath $gfxConf -Value 'backend=null' -Encoding ascii -NoNewline
    Write-Host "gfxconf : recreated $gfxConf (backend=null) -- it was missing, so the run would have used Mesa and hung in eglInitialize"
}
else {
    $backendLine = (Select-String -LiteralPath $gfxConf -Pattern '^\s*backend\s*=' -ErrorAction SilentlyContinue | Select-Object -First 1).Line
    if (-not $backendLine) { $backendLine = (Get-Content -LiteralPath $gfxConf -Raw).Trim() }
    Write-Host ("gfxconf : {0}   -> {1}" -f $gfxConf, $backendLine)
    if ($backendLine -notmatch 'null') {
        Write-Warning "rr3-gfx.conf does not select the null backend ('$backendLine'). On Eden any other backend has no DRM device and eglInitialize will hang. Expect a run that stops at view-egl-init-begin."
    }
}

Write-Host ("run    : eden.exe <nro>   wait {0}s" -f $Seconds)

# --- 3. run ------------------------------------------------------------------
$process = Start-Process -FilePath $edenExe -ArgumentList "`"$deployedNro`"" -PassThru
# Eden's log is a single rolling buffer on disk, NOT a per-run file. A quiet run
# (e.g. when a mapped guest page silences the fault spam) produces only a few
# hundred KB, and archiving it un-cleared silently mixes in the PREVIOUS run's
# faults. Truncate it before launch so every archived histogram is per-run.
$edenLog = Join-Path $edenRoot 'user\log\eden_log.txt'
# CITRON writes its own log under a different name, and the file is only
# created once the emulator has actually started, so the name is discovered
# rather than assumed. Without this the CITRON run would silently archive an
# EMPTY emulator log and the comparison would be worthless.
if ($EmulatorName -ne 'eden') {
    $candidate = Get-ChildItem (Join-Path $edenRoot 'user\log') -File -Filter '*.txt' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($candidate) { $edenLog = $candidate.FullName }
}
if (Test-Path $edenLog -PathType Leaf) {
    try {
        Clear-Content -LiteralPath $edenLog -ErrorAction Stop
    }
    catch {
        Write-Host "warn    : could not clear eden_log.txt ($($_.Exception.Message)); archived fault counts may include the previous run"
    }
}
$deadline = (Get-Date).AddSeconds($Seconds)
$lastSize = -1
$stableTicks = 0
# The host CPU series is the spin-vs-deadlock discriminator. An in-guest
# watchdog thread cannot do this job on Eden (a thread created through the
# guest pthread bridge never gets scheduled out of svcSleepThread there, so it
# produces no heartbeats and mimics the very hang it was meant to detect).
# Here a spin shows as a steady climb and a deadlock collapses toward zero.
$cpuSeries = [System.Collections.Generic.List[object]]::new()
try {
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 10
        $live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        if (-not $live) {
            Write-Host 'eden    : exited on its own'
            break
        }
        $size = if (Test-Path $guestLog) { (Get-Item $guestLog).Length } else { 0 }
        $stampLine = ''
        if ($size -gt 0) {
            $last = Get-Content $guestLog -Tail 1 -ErrorAction SilentlyContinue
            if ($last) { $stampLine = ($last -split ' ')[0] }
        }
        $elapsed = [int]((New-TimeSpan -Start $process.StartTime -End (Get-Date)).TotalSeconds)
        $cpuSeries.Add([pscustomobject]@{ t = $elapsed; cpu = $live.CPU; logBytes = $size })
        Write-Host ("t={0,4}s cpu={1,7:N1}s log={2,6}B  {3}" -f $elapsed, $live.CPU, $size, $stampLine)
        if ($size -eq $lastSize -and $size -gt 0) {
            $stableTicks++
            if ($stableTicks -ge 3) { Write-Host '        : log stable for 30s, guest has stopped making progress'; break }
        } else {
            $stableTicks = 0
        }
        $lastSize = $size
    }
}
finally {
    Get-Process -Id $process.Id -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
}

# --- 4. capture --------------------------------------------------------------
$archive = Join-Path $archiveRoot $label
New-Item -ItemType Directory -Force -Path $archive | Out-Null
$cpuSeries | Export-Csv -NoTypeInformation -Path (Join-Path $archive 'eden-cpu-series.csv')
foreach ($marker in $markers) {
    if (Test-Path $marker -PathType Leaf) {
        Copy-Item -LiteralPath $marker -Destination (Join-Path $archive (Split-Path $marker -Leaf)) -Force
    }
}
$edenLog = Join-Path $edenRoot 'user\log\eden_log.txt'
if ($EmulatorName -ne 'eden') {
    $candidate = Get-ChildItem (Join-Path $edenRoot 'user\log') -File -Filter '*.txt' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($candidate) { $edenLog = $candidate.FullName }
}
if (Test-Path $edenLog -PathType Leaf) {
    Copy-Item -LiteralPath $edenLog -Destination (Join-Path $archive 'eden_log.txt.tail') -Force
}

# --- 4b. instrument census -----------------------------------------------------
# WHY: the instrument lines in this project are NOT stable run to run. Observed
# on the same binary within minutes of each other, 150 s each:
#   alert-pump wait-result : present in one run, absent in the next
#   fpwalk-*               : 12 lines in one run, 0 in the next
#   census-pc              : 6 lines, then 4
# A single run therefore cannot distinguish "the instrument never fired" from
# "this run it did not fire", and a conclusion drawn from one run has already
# been wrong several times. This summary makes every archive self-describing so
# that N runs can be compared by intersection instead of by picking a round.
#
# Both sinks matter and are different files: guest-facing lines go to the
# guest-view log, while the alert-pump bracket lines use rawLog ->
# svcOutputDebugString and therefore only ever appear in Eden's log. Grepping
# the guest-view log for those is a false negative.
$summary = [ordered]@{
    label        = $label
    seconds      = $Seconds
    guestLogLines = 0
    edenLogLines  = 0
}
$probes = [ordered]@{}
if (Test-Path $guestLog -PathType Leaf) {
    $guestText = [IO.File]::ReadAllText($guestLog)
    $summary.guestLogLines = ([regex]::Matches($guestText, "`n")).Count
    # The trailing space on the last two is load-bearing, not decoration:
    # "phase=guest-thread" is a substring of "phase=guest-thread-start", so the
    # bare name would add the two counts together and the creation line -- the
    # one that carries the creator return address -- could no longer be read off
    # the census. The space only matches the creation line.
    foreach ($p in @('phase=gfx-stats', 'phase=wedge:', 'phase=cloudcell_defer',
                     'phase=cloudcell_pump', 'phase=entering', 'site=alert-pump',
                     'phase=census-pc', 'phase=census-sweep', 'phase=fpwalk-frame',
                     'phase=cloudcell-probe', 'phase=gate-show-message',
                     'phase=apk-res-reach', 'phase=reap-join-before',
                     'phase=render-call-pre', 'phase=render-call-post',
                     'phase=mmap-file-ok', 'phase=mmap-file-fail',
                     'phase=stat-layout',
                     'phase=guest-thread ', 'phase=guest-thread-start ')) {
        $probes["guest:$p"] = ([regex]::Matches($guestText, [regex]::Escape($p))).Count
    }
    # frames= appears many times in a run and only the LAST one is the total.
    # CORRECTED 2026-10-19: this used [regex]::Match, which returns the FIRST hit,
    # and the first hit in a real archive is "fpwalk-end ... frames=1" -- the
    # stack walker's own frame count, not the render loop's. Every census since
    # the field existed therefore reported frames=1 while the run was actually
    # at ~16,000, which reads exactly like "the guest wedged on frame one" and
    # has already been mistaken for that more than once.
    #
    # framesFirst is kept so the artifact that caused the confusion stays
    # visible in the archive instead of being quietly corrected in place.
    $allFrames = [regex]::Matches($guestText, 'frames=(\d+)')
    if ($allFrames.Count -gt 0) {
        $summary.frames = [int]$allFrames[$allFrames.Count - 1].Groups[1].Value
        $summary.framesFirst = [int]$allFrames[0].Groups[1].Value
        $summary.framesMatches = $allFrames.Count
    }
}
if (Test-Path (Join-Path $archive 'eden_log.txt.tail') -PathType Leaf) {
    $edenText = [IO.File]::ReadAllText((Join-Path $archive 'eden_log.txt.tail'))
    $summary.edenLogLines = ([regex]::Matches($edenText, "`n")).Count
    foreach ($p in @('alert-pump: started', 'alert-pump: thread-body-enter',
                     'alert-pump: pre-lock', 'alert-pump: wait-result',
                     'alert-pump: reached-drain', 'alert-pump: wake-wakeup',
                     'cloudcell-pump: queue-lock', 'Unmapped Read',
                     'Unmapped Write')) {
        $probes["eden:$p"] = ([regex]::Matches($edenText, [regex]::Escape($p))).Count
    }
}
$summaryPath = Join-Path $archive 'instrument-census.txt'
@(
    "# instrument census for $label"
    ("# {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    ("# A count of 0 is NOT evidence of absence until several runs are compared."
     )
    ''
    ($summary.GetEnumerator() | ForEach-Object { '{0,-16} {1}' -f $_.Key, $_.Value })
    ''
    ($probes.GetEnumerator() | ForEach-Object { '{0,-46} {1}' -f $_.Key, $_.Value })
) | Set-Content -LiteralPath $summaryPath -Encoding ascii
Write-Host ("census  : instrument-census.txt  (frames={0} guestLines={1} edenLines={2})" -f `
    $summary.frames, $summary.guestLogLines, $summary.edenLogLines)

# The null graphics backend (RR3_GFX) dumps every GLSL source and every binary
# shader blob it is handed to sdmc:/switch/. They are the only way to answer
# "does this game ship GLSL or precompiled shaders", so archive them with the
# run. LastWriteTime is recorded because the dumps survive between runs and a
# stale file must not be mistaken for a fresh one.
$gfxDumps = @(Get-ChildItem -LiteralPath $sdmcSwitch -Filter 'rr3-glsl*' -File -ErrorAction SilentlyContinue |
              Sort-Object Name | Select-Object -First 64)
if ($gfxDumps.Count -gt 0) {
    $dumpDir = Join-Path $archive 'gfx-dumps'
    New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
    $index = foreach ($dump in $gfxDumps) {
        Copy-Item -LiteralPath $dump.FullName -Destination (Join-Path $dumpDir $dump.Name) -Force
        '{0}`t{1}`t{2}' -f $dump.Name, $dump.Length, $dump.LastWriteTime.ToString('yyyyMMdd-HHmmss')
    }
    Set-Content -LiteralPath (Join-Path $archive 'gfx-dump-index.txt') -Value $index -Encoding UTF8
    Write-Host ("gfx dumps: {0} archived to {1}" -f $gfxDumps.Count, $dumpDir)
}

Write-Host ''
Write-Host "archive: $archive"
if (Test-Path $guestLog -PathType Leaf) {
    $lines = Get-Content $guestLog
    Write-Host ("guest log: {0} lines" -f $lines.Count)
    Write-Host '--- tail 30 ---'
    $lines | Select-Object -Last 30 | ForEach-Object { Write-Host "  $_" }
} else {
    Write-Host 'guest log: MISSING (guest never wrote one)'
}
Write-Host ''
Write-Host ("markers stashed at: {0}" -f $stash)
