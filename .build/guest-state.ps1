# Snapshot and restore the guest's PERSISTENT STATE.
#
# WHY THIS EXISTS.  MEASURED 2026-11-09: the guest stores the "you have an
# unclaimed reward" popup in its own save data, so the popup survives
# between rounds.  A round that ends a race leaves the popup behind, and
# EVERY LATER ROUND THEN STARTS STUCK ON IT.
#
# The symptom is unmistakable once it has been seen once: three different
# rounds (m2, m3, v1) all drew EXACTLY 10,394 things in 400 s and all
# stopped in the same place, on the same modal panel (archive
# 20261109-155529-eden-rr3-m3-nov4-census-off/frames/frame-005200.png).
#
# ⇒ A ROUND THAT DOES NOT START FROM THE SAME STATE IS NOT A COMPARISON.
# The 2x2 matrix on 2026-11-09 was confounded by exactly this: the rounds
# ran in order, the first one left the popup behind, and the cells after
# it were all measuring the popup instead of the variable they were
# supposed to be isolating.
#
# WHAT IS SAVED.  The guest writes its state into a handful of files under
# sdmc:/switch/rr3-native-rehost/doc/ (and one under .depot/):
#
#     doc/character.2.dat       5.9 MB   the player's profile and tournament
#     doc/player_profile.2.dat  1.2 kB
#     doc/events.dat            262 kB   (in .depot/)
#     doc/loadout.2.dat, doc/nrc.bin.2.dat, doc/gs.2.dat,
#     doc/sv.2.dat, doc/ttss.2.dat, doc/replayCache.2.dat
#
# The assets in .depot/ and the volumes in data/ are NOT state and are NOT
# touched: they are gigabytes and they are the same every round.
#
# Usage:
#   powershell -File .build\guest-state.ps1 -Snapshot   # take the baseline
#   powershell -File .build\guest-state.ps1 -Restore    # put it back
param(
    [switch]$Snapshot,
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$stash = 'E:\AI\RealRacing3-switch\.build\guest-state-baseline'

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

$files = @(
    'doc\character.2.dat',
    'doc\player_profile.2.dat',
    'doc\loadout.2.dat',
    'doc\nrc.bin.2.dat',
    'doc\gs.2.dat',
    'doc\sv.2.dat',
    'doc\ttss.2.dat',
    'doc\replayCache.2.dat',
    'doc\rmFDetails.bin',
    'doc\friendsList.pld',
    'doc\mgrRacerSettings.bin',
    'doc\profile.dat',
    '.depot\events.dat'
)

if ($Snapshot) {
    New-Item -ItemType Directory -Force -Path $stash | Out-Null
    $n = 0
    foreach ($rel in $files) {
        $src = Join-Path $app $rel
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
        $dst = Join-Path $stash ($rel -replace '\\', '__')
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $n++
    }
    Write-Host ("guest state : snapshotted {0} files to {1}" -f $n, $stash)
    exit 0
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $stash -PathType Container)) {
        throw ("no baseline at {0} -- take one with -Snapshot first" -f $stash)
    }
    $n = 0
    foreach ($rel in $files) {
        $src = Join-Path $stash ($rel -replace '\\', '__')
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
        $dst = Join-Path $app $rel
        $parent = Split-Path -Parent $dst
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $parent | Out-Null
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $n++
    }
    Write-Host ("guest state : restored {0} files" -f $n)
    exit 0
}

throw "usage: guest-state.ps1 -Snapshot | -Restore"
