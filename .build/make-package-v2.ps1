<#
.SYNOPSIS
    Build the consolidated release package (v2).

.DESCRIPTION
    WHAT CHANGED, AND WHY IT IS MEASURED
    -------------------------------------
    The previous package shipped the card tree as it was: 10,546 files and
    11.21 GiB, of which 9,873 were a loose `.depot` tree.  On a card that is
    already tight, that is the difference between "it fits" and "it does not
    open".

    So the package ships the tree the port ACTUALLY READS, and nothing else.

    The measurement is the port's own log.  Every read the port serves from
    the card is recorded as

        phase=... served=sdmc:/switch/rr3-native-rehost/<relative path>

    and in a full run (archive 20261102-141035-eden-rr3-q118) there were
    133 DISTINCT card files served -- 92 under .depot, 30 under cache, 10
    under doc, plus .nomedia.  The other 10,413 files on the card were
    never read: the seven data volumes carry 46,574 files and are
    consulted FIRST, so the loose copies on the card are a fallback that
    almost never fires.

    So the package ships:
      * the app (the NRO and the port's romfs),
      * the nine control scripts,
      * the seven data volumes and their manifest,
      * the .depot / cache / doc files that the measured run served, at
        their real relative paths, with their parent directories, because
        the guest WRITES some of them and a write into a directory that does
        not exist fails.

    And it ships, as ONE optional archive, the complete loose tree, so that
    a missing asset can be put back without hunting for it.

    WHAT IT DOES NOT SHIP
    ---------------------
    The card's `apk/` directory (244 files): the guest's apk/res tree is
    served from `rr3core:/files/apk/res` -- 10,812 hits in the measured run
    against 0 hits from the card -- so the card's copy is dead weight.

.PARAMETER Stamp
    The date stamp in the package name.

.PARAMETER RunLog
    The guest-view log(s) the prune list is derived from, separated by
    commas.  Defaults to the last three run archives under
    .build\eden-runs.

    MORE THAN ONE LOG IS BETTER THAN ONE: the prune list is "the files the
    port served FROM THE CARD", and a run that never reaches a given screen
    never asks for that screen's files.  The union of several runs is the
    closest thing to "the files the port can ask for" that a log can tell
    us.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .build\make-package-v2.ps1
#>
[CmdletBinding()]
param(
    [string]$Stamp = '20261102',
    [string]$RunLog = ''
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$cardApp = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$pkg = Join-Path $projectRoot ("dist\rr3-switch-{0}" -f $Stamp)

if (-not (Test-Path -LiteralPath $cardApp -PathType Container)) {
    throw "card app dir missing: $cardApp"
}

# ---- the prune list -----------------------------------------------------------
$logs = @()
if ($RunLog -eq '') {
    $runs = @(Get-ChildItem (Join-Path $projectRoot '.build\eden-runs') -Directory |
        Sort-Object Name | Select-Object -Last 3)
    foreach ($run in $runs) {
        $candidate = Join-Path $run.FullName 'rr3-native-rehost.guest-view.log'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $logs += $candidate }
    }
} else {
    $logs = @($RunLog -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
}
if ($logs.Count -eq 0) {
    throw 'no run archive under .build\eden-runs to derive the prune list from'
}

$cardPrefix = 'sdmc:/switch/rr3-native-rehost/'
$served = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($log in $logs) {
    if (-not (Test-Path -LiteralPath $log -PathType Leaf)) {
        Write-Warning "run log missing, skipped: $log"
        continue
    }
    Write-Host ("prune list : {0}" -f $log)
    foreach ($line in [System.IO.File]::ReadAllLines($log, [System.Text.Encoding]::UTF8)) {
        # EVERY card path the log mentions, not only the ones that were
        # served.
        #
        # THE FIRST PRUNE LIST WAS THE WRONG INSTRUMENT.  It collected the
        # paths the port SERVED from the card, and a file the guest asks for
        # but the port substitutes is never served -- so it was cut.  The
        # guest's r3_symbols.ttf is exactly that case: the log says
        # "req=sdmc:/switch/rr3-native-rehost/.depot/r3_symbols.ttf" and the
        # port answers it with the text face instead, so it never appears in
        # a served= line -- and the shipped package had no symbol font in it
        # at all.  That is a missing-character bug caused by the packaging,
        # not by the port.
        #
        # So the rule is: ANY card path the log mentions is kept.
        $at = 0
        while (($at = $line.IndexOf($cardPrefix, $at)) -ge 0) {
            $start = $at + $cardPrefix.Length
            $end = $line.IndexOf(' ', $start)
            if ($end -lt 0) { $end = $line.Length }
            $rel = $line.Substring($start, $end - $start).Trim()
            # ONLY THE THREE LOOSE TREES.  The log also mentions the card's
            # own top-level files -- the control scripts, and the frame
            # captures the test harness writes there -- and those are not
            # part of the loose tree: the scripts are deployed from the
            # repository and the frames are test artifacts.
            if ($rel -ne '' -and ($rel.StartsWith('.depot/') -or
                                  $rel.StartsWith('cache/') -or
                                  $rel.StartsWith('doc/'))) {
                [void]$served.Add($rel)
            }
            $at = $end
        }
    }
}
Write-Host ("             {0} distinct card paths were mentioned" -f $served.Count)

# ---- the tree -----------------------------------------------------------------
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'switch') | Out-Null
$dstApp = Join-Path $pkg 'switch\rr3-native-rehost'
New-Item -ItemType Directory -Force -Path $dstApp | Out-Null

# ---- 1. the app ---------------------------------------------------------------
# FROM THE BUILD OUTPUT, not from the card: the card's copy can be locked by a
# running emulator, and the build output is the thing that was just tested.
$buildRoot = Join-Path $projectRoot 'native-switch'
foreach ($name in @('rr3-native-rehost.nro', 'rr3.romfs')) {
    $src = Join-Path $buildRoot $name
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        throw "build output missing: $src"
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $dstApp $name) -Force
}
Write-Host ("app        : {0:N1} MiB" -f `
    ((Get-ChildItem $dstApp -File | Measure-Object Length -Sum).Sum / 1MB))

# ---- 2. the control scripts ---------------------------------------------------
# FROM THE REPOSITORY, not from the card: the card's copy may be an older
# edit, and the repository's copy is what the source says.
$scriptSource = Join-Path $projectRoot 'native-switch\card'
$scriptNames = @('rr3-touch.txt', 'rr3-drive.txt', 'rr3-pad.txt', 'rr3-action.txt',
    'rr3-screen.txt', 'rr3-settings.txt', 'rr3-ui.txt', 'rr3-open.txt',
    'rr3-gamepad.txt')
foreach ($name in $scriptNames) {
    Copy-Item -LiteralPath (Join-Path $scriptSource $name) -Destination (Join-Path $dstApp $name) -Force
}
Write-Host ("scripts    : {0}" -f $scriptNames.Count)

# ---- 3. the data volumes ------------------------------------------------------
$dstData = Join-Path $dstApp 'data'
New-Item -ItemType Directory -Force -Path $dstData | Out-Null
foreach ($file in @(Get-ChildItem (Join-Path $cardApp 'data') -File)) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $dstData $file.Name) -Force
}
Write-Host ("data       : {0} files, {1:N1} MiB" -f `
    (Get-ChildItem $dstData -File).Count, `
    ((Get-ChildItem $dstData -File | Measure-Object Length -Sum).Sum / 1MB))

# ---- 4. the live card files, at their real relative paths ----------------------
$liveCount = 0
$liveBytes = 0
foreach ($rel in ($served | Sort-Object)) {
    $source = Join-Path $cardApp ($rel -replace '/', '\')
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $target = Join-Path $dstApp ($rel -replace '/', '\')
    $parent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    Copy-Item -LiteralPath $source -Destination $target -Force
    $liveCount++
    $liveBytes += (Get-Item $source).Length
}
Write-Host ("live       : {0} files, {1:N2} MiB (the ones the measured run read)" -f `
    $liveCount, ($liveBytes / 1MB))

# ---- 5. the optional full tree, as ONE archive --------------------------------
#
# The safety net.  It is NOT installed by default: it is the 10,413 files the
# measured run did not read, and putting them back is a deliberate act.
$optional = Join-Path $pkg 'optional'
New-Item -ItemType Directory -Force -Path $optional | Out-Null
$staging = Join-Path $pkg '.staging'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
foreach ($dir in @('.depot', 'apk')) {
    $from = Join-Path $cardApp $dir
    if (-not (Test-Path -LiteralPath $from -PathType Container)) { continue }
    & robocopy.exe $from (Join-Path $staging $dir) /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $dir (code $LASTEXITCODE)" }
}
$fullArchive = Join-Path $optional 'card-depot-full.zip'
Write-Host 'packing    : optional\card-depot-full.zip ...'
& tar.exe -a -cf $fullArchive -C $staging .
if ($LASTEXITCODE -ne 0) { throw "tar failed (code $LASTEXITCODE)" }
# Empty the staging directory with robocopy /MIR against an empty one, so that
# nothing has to be deleted recursively.
$empty = Join-Path $pkg '.empty'
New-Item -ItemType Directory -Force -Path $empty | Out-Null
& robocopy.exe $empty $staging /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
Remove-Item -LiteralPath $staging -Force
Remove-Item -LiteralPath $empty -Force
Write-Host ("optional   : {0:N1} MiB" -f ((Get-Item $fullArchive).Length / 1MB))

# ---- 6. the port's conf, the installer, the readme ----------------------------
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\rr3-gfx.conf') `
    -Destination (Join-Path $pkg 'switch\rr3-gfx.conf') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\Copy-To-SwitchCard.ps1') `
    -Destination (Join-Path $pkg 'Copy-To-SwitchCard.ps1') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\README-hardware-test.md') `
    -Destination (Join-Path $pkg 'README-hardware-test.md') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\Join-And-Extract.ps1') `
    -Destination (Join-Path $pkg 'Join-And-Extract.ps1') -Force

# ---- 7. the inventory ---------------------------------------------------------
$all = @(Get-ChildItem -LiteralPath $pkg -Recurse -File -Force |
    Where-Object { $_.Name -ne 'checksums.sha256' } | Sort-Object FullName)
$manifest = Join-Path $pkg 'checksums.sha256'
$out = New-Object System.Collections.Generic.List[string]
foreach ($file in $all) {
    $rel = $file.FullName.Substring($pkg.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLower()
    $out.Add(('{0}  {1}' -f $hash, $rel))
}
[System.IO.File]::WriteAllLines($manifest, $out, (New-Object System.Text.UTF8Encoding($false)))

$total = ($all | Measure-Object Length -Sum).Sum
Write-Host ""
Write-Host ("package    : {0}" -f $pkg)
Write-Host ("            {0} files, {1:N2} GiB" -f ($all.Count + 1), ($total / 1GB))
$cardTree = @(Get-ChildItem (Join-Path $pkg 'switch') -Recurse -File -Force)
Write-Host ("card tree  : {0} files, {1:N2} GiB" -f $cardTree.Count, `
    (($cardTree | Measure-Object Length -Sum).Sum / 1GB))
