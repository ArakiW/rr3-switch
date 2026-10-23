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

# The framebuffer PNGs the port writes into the app directory. They are the
# only instrument that shows WHAT the guest drew, so they are archived like
# the logs are -- and they are cleared first, because a stale capture from a
# previous run would be indistinguishable from a current one.
Get-ChildItem -LiteralPath $appDir -Filter 'frame-*.png' -File -ErrorAction SilentlyContinue |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }

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
# How long the guest-view log has been growing still, in seconds.
$lastGrowthElapsed = 0
# The no-log rule. WHY 12 TICKS AND NOT 3.
#
# The guest-view log's growth is the only liveness signal this harness has,
# and it is NOT a heartbeat. The reaper writes one line per 1 s slice, but
# only while its condvar wait TIMES OUT -- every detached guest thread wakes
# the reaper instead (pthread_bridge.cpp:5536-5549, `waitedNanos` resets to
# 0 on a wake), so a guest that churns threads goes quiet for long stretches
# while it is very much alive. The measured consequence in the archives:
# 20261021-184122-...-manifest2, 20261021-184611-...-manifest3 and
# 20261021-185316-...-manifest4 were ALL ended by the old 3-tick (30 s) rule
# at ~120 s, with Eden's CPU time still climbing linearly (13.9 s -> 155.5 s
# over 110 s in manifest2) and the guest log flat from t=80 s onward.
#
# 12 ticks = 120 s, comfortably longer than the longest observed quiet
# window, and it still ends a genuinely wedged guest instead of running it
# to the wall clock.
$stableTickLimit = 12
$tickSeconds = 10
# Why the run ended, recorded in the archive so a short run explains itself.
$exitReason = 'wall-clock deadline reached'
# The host CPU series is the spin-vs-deadlock discriminator. An in-guest
# watchdog thread cannot do this job on Eden (a thread created through the
# guest pthread bridge never gets scheduled out of svcSleepThread there, so it
# produces no heartbeats and mimics the very hang it was meant to detect).
# Here a spin shows as a steady climb and a deadlock collapses toward zero.
$cpuSeries = [System.Collections.Generic.List[object]]::new()
try {
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds $tickSeconds
        $live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        if (-not $live) {
            Write-Host 'eden    : exited on its own'
            $exitReason = 'eden exited on its own'
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
            if ($stableTicks -ge $stableTickLimit) {
                # Say how long the log was ACTUALLY stable, not a hard-coded
                # number. A reader comparing archives needs the real quiet
                # window, and the old fixed "30s" text could not distinguish
                # "quiet for exactly the threshold" from "quiet for 10 minutes
                # with the rule firing late".
                $stableFor = $elapsed - $lastGrowthElapsed
                $exitReason = "guest log stable for ${stableFor}s (no growth since t=${lastGrowthElapsed}s)"
                Write-Host ("        : log stable for {0}s (no growth since t={1}s, {2} ticks of {3}s), guest has stopped making progress" -f `
                    $stableFor, $lastGrowthElapsed, $stableTicks, $tickSeconds)
                break
            }
        } else {
            $stableTicks = 0
            $lastGrowthElapsed = $elapsed
        }
        $lastSize = $size
    }
}
finally {
    Get-Process -Id $process.Id -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
}
# Wall-clock length of the run, measured from the emulator's own start time
# to the moment the harness stopped it. Recorded in the archive because the
# single most important fact about a run -- how long it lasted -- was
# previously only recoverable by subtracting timestamps in the CPU series.
$runEnd = Get-Date
$runSeconds = [int]((New-TimeSpan -Start $process.StartTime -End $runEnd).TotalSeconds)
$finalLogBytes = if (Test-Path $guestLog) { (Get-Item $guestLog).Length } else { 0 }

# --- 4. capture --------------------------------------------------------------
$archive = Join-Path $archiveRoot $label
New-Item -ItemType Directory -Force -Path $archive | Out-Null
$cpuSeries | Export-Csv -NoTypeInformation -Path (Join-Path $archive 'eden-cpu-series.csv')

# HOW MUCH ROOM IS LEFT ON THE CARD. Eden runs a VIRTUAL SD card: a FAT32
# filesystem of FIXED capacity over the loose sdmc directory. The host disk
# having 120 GB free says nothing about it, and the symptom is an errno 12
# (ENOSPC) deep inside a file write -- which reads like a port bug and is
# not one. Measured 2026-10-24: a stale SECOND copy of the seven romfs
# volumes (9.63 GB, left behind by an older deploy layout, referenced by
# nothing) filled the volume, the guest's asset downloads started failing
# with ENOSPC, and Eden exited on its own at 42 s and 55 s. Deleting the
# duplicate freed the space.
#
# So the card's usage is measured and archived, every run, whether or not
# anything went wrong.
$cardFiles = @(Get-ChildItem -LiteralPath $sdmcSwitch -Recurse -File -ErrorAction SilentlyContinue)
$cardBytes = ($cardFiles | Measure-Object -Property Length -Sum).Sum
if ($null -eq $cardBytes) { $cardBytes = 0 }
$staleVolBytes = 0
$staleVolDir = Join-Path $appDir 'rr3-native-rehost'
if (Test-Path -LiteralPath $staleVolDir -PathType Container) {
    $staleVolBytes = ((Get-ChildItem -LiteralPath $staleVolDir -Filter '*.romfs' -File -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum)
    if ($null -eq $staleVolBytes) { $staleVolBytes = 0 }
}
Write-Host ("card     : {0} files, {1:N0} bytes on the SD card{2}" -f `
    $cardFiles.Count, $cardBytes, `
    $(if ($staleVolBytes -gt 0) { " -- WARNING: {0:N0} bytes of STALE duplicate romfs in {1}" -f $staleVolBytes, $staleVolDir } else { '' }))

foreach ($marker in $markers) {
    if (Test-Path $marker -PathType Leaf) {
        Copy-Item -LiteralPath $marker -Destination (Join-Path $archive (Split-Path $marker -Leaf)) -Force
    }
}

# THE GUEST LOG MUST BE ACCOUNTED FOR, NOT JUST COUNTED.
#
# "guest log 54178341 bytes, 0 lines" is what archive
# 20261024-040807-eden-touch3 reported, and it was a lie of omission: the
# 54 MB file could not be COPIED (Eden still held it open), so the archive
# contained no log at all and every count derived from it was zero. A
# reader would have concluded "the guest logged nothing for 600 seconds",
# which is a completely different fact from "the log could not be read".
#
# So the copy is verified, and the summary says WHICH state it is in:
#   archived   the log is in the archive and the counters come from it
#   missing    the log was not copied; every count below is void
#   empty      the log was copied and is genuinely empty
$guestLogArchived = 'missing'
$guestLogBytes = 0
$guestLogLines = 0
$archivedGuestLog = Join-Path $archive (Split-Path $guestLog -Leaf)
if (Test-Path $archivedGuestLog -PathType Leaf) {
    $guestLogArchived = 'archived'
    $guestLogBytes = (Get-Item $archivedGuestLog).Length
    $guestLogLines = ([regex]::Matches([IO.File]::ReadAllText($archivedGuestLog), "`n")).Count
    if ($guestLogBytes -eq 0) { $guestLogArchived = 'empty' }
} else {
    Write-Host ("warn    : the guest log could NOT be copied into the archive ({0} bytes were on the card). Every count derived from it is void." -f $finalLogBytes)
}

# The framebuffer captures. They go into the archive under frames/ so that a
# reader can SEE what the guest drew instead of reading a colour triple.
# The count is recorded in the summary below, because "the archive has no
# pictures" and "the port wrote no pictures" are different failures.
$frameDir = Join-Path $archive 'frames'
$framePngs = @(Get-ChildItem -LiteralPath $appDir -Filter 'frame-*.png' -File -ErrorAction SilentlyContinue |
    Sort-Object Name)
if ($framePngs.Count -gt 0) {
    New-Item -ItemType Directory -Force -Path $frameDir | Out-Null
    foreach ($png in $framePngs) {
        Copy-Item -LiteralPath $png.FullName -Destination (Join-Path $frameDir $png.Name) -Force
    }
    Write-Host ("frames  : {0} PNG(s) captured -> {1}" -f $framePngs.Count, $frameDir)
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
# Eden's emulator log is a CAPPED file, not a per-run file. Measured on every
# recent archive: eden_log.txt stops growing at ~105,830,000 bytes (100.93
# MiB), the last line is COMPLETE rather than truncated, and its timestamps
# stop at t=37-45 s of a run that lasts 60-300 s. Eden's writer simply stops
# once the file passes the cap. The archived copy therefore holds only the
# FIRST ~40 s of the run, and every count taken from it -- "Unmapped Read64"
# above all -- is a count over that window, not over the run. The
# edenLogFirstTs/edenLogLastTs fields in the census and in run-summary.txt
# exist so that a reader can see the window instead of having to discover it.
#
# eden_log.txt.old.txt is NOT the earlier part of this run: Eden creates and
# truncates it at startup (measured: 0 bytes, lastWrite == the run's start
# time), so it is archived for completeness and explicitly labelled as such.
$edenLogOld = "$edenLog.old.txt"
if (Test-Path $edenLogOld -PathType Leaf) {
    Copy-Item -LiteralPath $edenLogOld -Destination (Join-Path $archive 'eden_log.txt.old.txt') -Force
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
    runSeconds   = $runSeconds
    runTicks     = $cpuSeries.Count
    exitReason   = $exitReason
    guestLogBytes = $guestLogBytes
    guestLogLines = 0
    # WHICH state the log is in: archived / empty / missing. A count taken
    # from a log that could not be read is void, and the summary has to say
    # so instead of printing a confident zero.
    guestLogArchived = $guestLogArchived
    guestLogOnCardBytes = $finalLogBytes
    edenLogLines  = 0
    # Initialised to a real value so that an archive always SHOWS the number
    # rather than a blank: "0 TrackDesc::load failures" is evidence, and a
    # missing key is not. Every one of these is overwritten below when the
    # corresponding line is present in the log.
    lastGuestLogOrdinal = 0
    guestLogAnyLines = 0
    gfxStatsLines   = 0
    gfxFrames       = 0
    gfxCalls        = 0
    gfxDraws        = '0/0'
    # How many framebuffer PNGs the run produced. Zero means the port wrote no
    # pictures, which is a DIFFERENT failure from "the pictures are in the
    # archive but show the wrong thing" -- so the number is recorded even
    # when it is zero.
    framePngs       = 0
    # The SD card's usage. Eden's virtual SD is a FIXED-capacity FAT32 over
    # the loose sdmc directory, so the host disk's free space is not the
    # number that matters -- this one is.
    cardBytes       = 0
    cardFiles       = 0
    staleVolBytes   = 0
    trackDescFailures = 0
    trackDescLastNames = ''
    edenLogFirstTs  = '-'
    edenLogLastTs   = '-'
    edenLogOldLines = 0
    # MILESTONES. The guest's own progress markers, counted, so a reader never
    # has to grep a 20 MB log to learn whether the run got past the load.
    #
    # WHY A SEPARATE BLOCK AND NOT "MainMenu": the literal string "MainMenu" is
    # a WEAK instrument. It was 2 in an old build because of a legacy flat name
    # and 0 in a newer one, and it stays 0 even when the front end IS up -- the
    # guest never logs that word. The milestones below are the strings the guest
    # actually prints when it reaches each stage, so a count of 0 there means
    # "did not reach", not "the instrument is quiet".
    msLoadComplete      = 0   # "Total load time to end of load scene_DoIncrementalLoad fns"
    msMd5Mismatch       = 0   # "didn't match its MD5"
    msAssetComplete     = 0   # "ASSET COMPLETE"
    msCannotOpenDl      = 0   # "Cannot open downloaded asset file."
    msDownloadFailed    = 0   # "Download failed - incomplete Download."
    msStat22            = 0   # "unknown error stat(22)"
    msAssetListsAdded   = 0   # "Assets are added"
    msTwosquare         = 0   # "gui/processed/main_menu/main_menu_twosquare.gui" in the PORT trace
    msMainMenuLayouts   = 0   # "gui/processed/main_menu/" in the PORT trace
    # The pixel readback: the most honest "is there anything on screen"
    # instrument, because it measures pixels rather than a string.
    pixelReadbackLines  = 0
    pixelReadbackFrame  = 0
    pixelsLit           = 0
    pixelsTotal         = 0
    pixelBins           = ''
    msEventMapScreen    = 0   # "EventMapScreen"
    msMainMenuLiteral   = 0   # "MainMenu"    (WEAK -- see the note above)
    msAstFilesOnCard    = 0   # *.ast on the card: the physical proof the .ast round trip works
    # The last front-end gate reading, verbatim. This is THE measurement that says
    # which of the three gates in the guest's front-end entry (0xa679a0) is
    # closed, and it is a long line -- so it is copied verbatim instead of being
    # squeezed into a counter.
    feGateLast          = '(none)'
    loadStateLast       = '(none)'
}
$probes = [ordered]@{}
# The counters come from the ARCHIVED COPY, not from the live file on the
# card. The live file can be locked by a running emulator (the copy then
# fails and the archive has no log at all -- archive
# 20261024-040807-eden-touch3) and it can still be growing. The archived
# copy is a verified, stable snapshot, and the summary records whether it
# exists.
if ($guestLogArchived -eq 'archived') {
    $guestText = [IO.File]::ReadAllText($archivedGuestLog)
    $summary.guestLogLines = $guestLogLines
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
                     'phase=menu-gate:', 'phase=masset-gate:',
                     'phase=fe-gate:', 'phase=loadstate:',
                     'phase=manifest-build', 'phase=manifest-asset',
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
    # The LAST guest-log-any ordinal: how many times the guest called its own
    # logging function by the time the run ended. This is the run's "how far
    # did the guest get" counter that does not depend on the render loop, and
    # it is what tells a short run apart from a long one at a glance -- the
    # guest-log ordinal keeps climbing through the quiet stretches that make
    # the guest-view log look dead.
    $allOrdinals = [regex]::Matches($guestText, 'phase=guest-log-any ordinal=(\d+)')
    if ($allOrdinals.Count -gt 0) {
        $summary.lastGuestLogOrdinal = [int]$allOrdinals[$allOrdinals.Count - 1].Groups[1].Value
        $summary.guestLogAnyLines = $allOrdinals.Count
    }
    # The LAST gfx-stats line carries the frame counter, the GL call counter
    # and the draw counter. Recorded here so a reader does not have to grep a
    # 40 MB log to learn how many frames the run rendered.
    $gfxLines = [regex]::Matches($guestText, 'phase=gfx-stats[^\r\n]*')
    if ($gfxLines.Count -gt 0) {
        $gfxLast = $gfxLines[$gfxLines.Count - 1].Value
        $summary.gfxStatsLines = $gfxLines.Count
        $m = [regex]::Match($gfxLast, 'frames=(\d+)')
        if ($m.Success) { $summary.gfxFrames = [int]$m.Groups[1].Value }
        $m = [regex]::Match($gfxLast, 'calls=(\d+)')
        if ($m.Success) { $summary.gfxCalls = [int]$m.Groups[1].Value }
        $m = [regex]::Match($gfxLast, 'draws=(\d+)/(\d+)')
        if ($m.Success) { $summary.gfxDraws = "$($m.Groups[1].Value)/$($m.Groups[2].Value)" }
    }
    # THE LAST PIXEL READBACK: how much of the 1280x720 back buffer is lit.
    #
    # This is the most honest "is there anything on screen" instrument the
    # project has, because it measures the pixels rather than a string. It
    # is what finally settled the front end: archive
    # 20261024-010947-eden-shot2 reads
    #   LATE frame=3000 full 1280x720 lit=921600 of 921600
    #   px0=7f8a46ff quads[r_bins=226703,285945,125911,283041]
    #   pts=[818c48 7f8080 03438b 182c43 0c0d0f]
    # i.e. EVERY pixel lit, four populated red bins, five probe points of
    # five different colours -- a real image, not a flat fill. A source
    # comment in guest_view_trace.cpp still claims the opposite ("lit=0 of
    # 921600 ... the guest really has nothing drawn"); that comment
    # describes an archive from before the fixes and is stale.
    #
    # A run with lit=0 and one with lit=921600 look nothing alike on
    # screen, and no string counter can tell them apart.
    $rbLines = [regex]::Matches($guestText, 'phase=view-pixel-readback[^\r\n]*')
    if ($rbLines.Count -gt 0) {
        $rbLast = $rbLines[$rbLines.Count - 1].Value
        $summary.pixelReadbackLines = $rbLines.Count
        $m = [regex]::Match($rbLast, 'lit=(\d+) of (\d+)')
        if ($m.Success) {
            $summary.pixelsLit = $m.Groups[1].Value
            $summary.pixelsTotal = $m.Groups[2].Value
        }
        $m = [regex]::Match($rbLast, 'r_bins=(\d+),(\d+),(\d+),(\d+)')
        if ($m.Success) { $summary.pixelBins = ($m.Groups[1].Value + ',' + $m.Groups[2].Value + ',' + $m.Groups[3].Value + ',' + $m.Groups[4].Value) }
        $m = [regex]::Match($rbLast, 'frame=(\d+)')
        if ($m.Success) { $summary.pixelReadbackFrame = [int]$m.Groups[1].Value }
    }
    # The guest's own "TrackDesc::load unable to load track desc file: '<name>'"
    # lines. Counted and the names kept, because the names the guest asks for
    # are the evidence for the corrupt track-name table. MEASURED on archive
    # 20261021-185316-eden-massetgate-manifest4: 163 of 163 distinct names
    # the guest asked for are the name the volume ships with its FIRST 16
    # BYTES REMOVED (e.g. it asked for "Tracks/data/-154.bin.nct" where the
    # volume ships "monza-jnr-sunset-154.bin.nct"). The names are therefore a
    # TAIL of the real name, not a name with a stray prefix.
    $trackDesc = [regex]::Matches($guestText, "TrackDesc::load unable to load track desc file: '([^']*)'")
    if ($trackDesc.Count -gt 0) {
        $summary.trackDescFailures = $trackDesc.Count
        $summary.trackDescLastNames = (($trackDesc | ForEach-Object { $_.Groups[1].Value } | Select-Object -Last 10) -join ' ')
    }
    # THE MILESTONE COUNTERS.
    #
    # Each of these is a string the guest itself prints when it reaches a stage,
    # so 0 means "the run did not reach that stage" and not "the probe is
    # quiet". The list is deliberately short and each entry answers a question
    # that has actually been asked of a run:
    #
    #   msLoadComplete   -- did the incremental-load scene finish?
    #   msMd5Mismatch    -- did a downloaded asset fail the guest's own md5 check?
    #   msAssetComplete  -- how many assets passed that check?
    #   msCannotOpenDl   -- could the guest re-open the file it had just written?
    #   msStat22         -- did the guest hit a path the port could not resolve?
    #   msTwosquare      -- did the guest ask for the front-end menu layout?
    #   msEventMapScreen -- did the guest ever name the front-end screen?
    #   msMainMenuLiteral-- WEAK, kept only so the change in it stays visible.
    $summary.msLoadComplete    = [regex]::Matches($guestText, 'Total load time to end of load').Count
    $summary.msMd5Mismatch     = [regex]::Matches($guestText, "didn't match its MD5").Count
    $summary.msAssetComplete   = [regex]::Matches($guestText, 'ASSET COMPLETE').Count
    $summary.msCannotOpenDl    = [regex]::Matches($guestText, 'Cannot open downloaded asset file').Count
    $summary.msDownloadFailed  = [regex]::Matches($guestText, 'Download failed - incomplete Download').Count
    $summary.msStat22          = [regex]::Matches($guestText, 'stat\(22\)').Count
    $summary.msAssetListsAdded = [regex]::Matches($guestText, 'Assets are added').Count
    # THE FRONT-END METRIC. The old one counted the literal "twosquare" in
    # the GUEST'S OWN log, and it could never be non-zero: the guest does
    # not write layout names into its log. Layout names appear in the PORT's
    # trace (map-app / stat-layout), because that is where the port is the
    # one that sees them.
    #
    # Measured 2026-10-24: forty-plus runs reported "front-end layout
    # opened 0" while the screen was plainly up. The framebuffer capture
    # from archive 20261024-010947-eden-shot2 shows the front end rendered
    # -- a 3D track scene, the status bar, the main menu's white panel --
    # with main_menu_career_page.gui and main_menu_race_now.gui both loaded,
    # while the guest's own log had never once said "twosquare". The metric
    # was not measuring the menu; it was measuring a string the guest does
    # not emit. It now counts the PORT's trace of the layout being opened,
    # which is the event that means anything.
    $summary.msTwosquare = [regex]::Matches(
        $guestText, 'gui/processed/main_menu/main_menu_twosquare\.gui').Count
    $summary.msMainMenuLayouts = [regex]::Matches(
        $guestText, 'gui/processed/main_menu/').Count
    $summary.msEventMapScreen  = [regex]::Matches($guestText, 'EventMapScreen').Count
    $summary.msMainMenuLiteral = [regex]::Matches($guestText, 'MainMenu').Count
    # The last front-end gate line and the last loading-state line, verbatim.
    # "ctrl=ok" is the probe's own positive control; "ctrl=fail" invalidates
    # the whole line, so the line has to be read together with its control.
    $feLines = [regex]::Matches($guestText, 'phase=fe-gate:[^\r\n]*')
    if ($feLines.Count -gt 0) {
        $summary.feGateLast = $feLines[$feLines.Count - 1].Value
    }
    $lsLines = [regex]::Matches($guestText, 'phase=loadstate:[^\r\n]*')
    if ($lsLines.Count -gt 0) {
        $summary.loadStateLast = $lsLines[$lsLines.Count - 1].Value
    }
}
# The .ast files on the card are the PHYSICAL check that the download cache
# round trip works: the guest writes "<asset>.ast" on the card and reads it
# back. If the count is 0 after a run in which the guest downloaded assets,
# the write did not persist -- which no log line can show as clearly.
try {
    $summary.msAstFilesOnCard = @(Get-ChildItem -LiteralPath $appDir -Recurse -File -Filter '*.ast' -ErrorAction SilentlyContinue).Count
} catch {
    $summary.msAstFilesOnCard = -1
}
if (Test-Path (Join-Path $archive 'eden_log.txt.tail') -PathType Leaf) {
    $edenText = [IO.File]::ReadAllText((Join-Path $archive 'eden_log.txt.tail'))
    $summary.edenLogLines = ([regex]::Matches($edenText, "`n")).Count
    foreach ($p in @('alert-pump: started', 'alert-pump: thread-body-enter',
                     'alert-pump: pre-lock', 'alert-pump: wait-result',
                     'alert-pump: reached-drain', 'alert-pump: wake-wakeup',
                     'cloudcell-pump: queue-lock', 'Unmapped Read',
                     'Unmapped Write', 'Unmapped Read64')) {
        $probes["eden:$p"] = ([regex]::Matches($edenText, [regex]::Escape($p))).Count
    }
    # The Eden log's own timestamp span. Eden's log file is capped, so for a
    # run longer than the cap the archived copy covers only the FIRST part of
    # the run and every count taken from it is a count over that window, not
    # over the run. Recording the span makes that visible in the archive
    # instead of leaving it to be discovered.
    $ts = [regex]::Matches($edenText, '(?m)^\[\s*(\d+\.\d+)\]')
    if ($ts.Count -gt 0) {
        $summary.edenLogFirstTs = $ts[0].Groups[1].Value
        $summary.edenLogLastTs = $ts[$ts.Count - 1].Groups[1].Value
    }
}
if (Test-Path (Join-Path $archive 'eden_log.txt.old.txt') -PathType Leaf) {
    $summary.edenLogOldLines = ([regex]::Matches(
        [IO.File]::ReadAllText((Join-Path $archive 'eden_log.txt.old.txt')), "`n")).Count
}
$summaryPath = Join-Path $archive 'instrument-census.txt'
$summary.framePngs = $framePngs.Count
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

# --- 4c. run summary ----------------------------------------------------------
# A short run must be obvious from the archive ITSELF, without somebody
# subtracting timestamps in the CPU series or interpreting a counter. This
# file answers "how long did this run last and why did it end" in one read.
$runSummaryPath = Join-Path $archive 'run-summary.txt'
@(
    "run summary: $label"
    ("generated : {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    ''
    ('label          {0}' -f $label)
    ('emulator       {0}' -f $EmulatorName)
    ('nro            {0} bytes  sha256={1}' -f $built.Length, (Get-FileHash -LiteralPath $deployedNro -Algorithm SHA256).Hash)
    ('nro mtime      {0}' -f $built.LastWriteTime)
    ''
    ('wall clock     {0} s   (requested {1} s, {2} ticks of {3} s)' -f $runSeconds, $Seconds, $cpuSeries.Count, $tickSeconds)
    ('ended because  {0}' -f $exitReason)
    ('guest log      {0} bytes, {1} lines   (archived={2}; on the card at the end: {3} bytes)' -f `
        $summary.guestLogBytes, $summary.guestLogLines, $summary.guestLogArchived, $summary.guestLogOnCardBytes)
    ('last guest-log ordinal  {0}   ({1} guest-log-any lines)' -f $summary.lastGuestLogOrdinal, $summary.guestLogAnyLines)
    ('frames         {0}' -f $summary.frames)
    ('gfx-stats      {0} lines; frames={1} calls={2} draws={3}' -f `
        $summary.gfxStatsLines, $summary.gfxFrames, $summary.gfxCalls, $summary.gfxDraws)
    ('TrackDesc::load failures  {0}' -f $summary.trackDescFailures)
    ('frame PNGs      {0}   (the pictures of what the guest drew; see frames/)' -f $summary.framePngs)
    ('pixels lit      {0} of {1}   (the last 1280x720 back-buffer readback; r_bins={2}; frame={3})' -f `
        $summary.pixelsLit, $summary.pixelsTotal, $summary.pixelBins, $summary.pixelReadbackFrame)
    ('eden log       {0} lines, spans t={1}..{2} s of emulator time (capped file: a count taken from it is a count over that window, not over the run)' -f `
        $summary.edenLogLines, $summary.edenLogFirstTs, $summary.edenLogLastTs)
    ('eden log old   {0} lines (Eden''s startup-created file; it is NOT the earlier part of this run)' -f $summary.edenLogOldLines)
    ''
    'milestones'
    ('  load scene finished      {0}   ("Total load time to end of load")' -f $summary.msLoadComplete)
    ('  md5 mismatch             {0}   (want 0)' -f $summary.msMd5Mismatch)
    ('  ASSET COMPLETE           {0}' -f $summary.msAssetComplete)
    ('  cannot open downloaded   {0}   (want 0)' -f $summary.msCannotOpenDl)
    ('  download failed          {0}   (want 0)' -f $summary.msDownloadFailed)
    ('  stat(22)                 {0}   (want 0)' -f $summary.msStat22)
    ('  asset lists added        {0}' -f $summary.msAssetListsAdded)
    ('  .ast files on the card   {0}   (want > 0 when assets were downloaded)' -f $summary.msAstFilesOnCard)
    ('  front-end layout opened  {0}   ("gui/processed/main_menu/main_menu_twosquare.gui" in the PORT trace -- want > 0 for the main menu panel)' -f $summary.msTwosquare)
    ('  main menu layouts open   {0}   (gui/processed/main_menu/ -- want > 0 once the front end is up)' -f $summary.msMainMenuLayouts)
    ('  EventMapScreen           {0}   (want > 0 for the menu)' -f $summary.msEventMapScreen)
    ('  "MainMenu" literal       {0}   (WEAK instrument, see the note in this script)' -f $summary.msMainMenuLiteral)
    ''
    'front-end gate (last reading; ctrl=ok is the probe positive control)'
    ('  {0}' -f $summary.feGateLast)
    ''
    'loading state (last reading)'
    ('  {0}' -f $summary.loadStateLast)
) | Set-Content -LiteralPath $runSummaryPath -Encoding ascii
Write-Host ("summary : run-summary.txt  (wall={0}s end={1})" -f $runSeconds, $exitReason)

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
