# Snapshot and restore the guest's PERSISTENT STATE -- PER ROUND.
#
# WHY.  MEASURED 2026-11-09: the guest advances to the NEXT EVENT after each
# race, so WHICH event a round races is a function of how many races have
# happened since the save was made.  The NASCAR round (m9, which showed the
# magenta AI opponents) and the Hockenheim round (m10) asked for exactly the
# same 41 asset paths, and the only difference between them was the event --
# which is why comparing them proved nothing.
#
# ⇒ A single baseline is not enough.  The baseline is OVERWRITTEN every
# round, so the state that produced a given event is gone by the next round,
# and the one round that shows the fault cannot be reproduced.
#
# ⇒ So the state is kept PER ROUND, named after the round, and can be
# restored by name.  A round that shows something is then a round anybody can
# run again, with the same event, on a different build.
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
#   powershell -File .build\guest-state.ps1 -Snapshot -Tag m9-nov4
#   powershell -File .build\guest-state.ps1 -Restore  -Tag m9-nov4
#   powershell -File .build\guest-state.ps1 -Restore            (the default one)
param(
    [switch]$Snapshot,
    [switch]$Restore,
    [string]$Tag = ''
)

$ErrorActionPreference = 'Stop'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$root = 'E:\AI\RealRacing3-switch\.build\guest-state'

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

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

# THE DEFAULT BASELINE is what the untagged form uses.  The per-round copies
# are named after the round they belong to.
$dir = if ($Tag -ne '') { Join-Path $root $Tag } else { Join-Path $root 'baseline' }

if ($Snapshot) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $n = 0
    foreach ($rel in $files) {
        $src = Join-Path $app $rel
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
        $dst = Join-Path $dir ($rel -replace '\\', '__')
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $n++
    }
    Write-Host ("guest state : snapshotted {0} files to {1}" -f $n, $dir)
    exit 0
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        throw ("no guest state at {0} -- take one with -Snapshot -Tag <name> first" -f $dir)
    }
    $n = 0
    foreach ($rel in $files) {
        $src = Join-Path $dir ($rel -replace '\\', '__')
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
        $dst = Join-Path $app $rel
        $parent = Split-Path -Parent $dst
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $parent | Out-Null
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $n++
    }
    Write-Host ("guest state : restored {0} files from {1}" -f $n, $dir)
    exit 0
}

Write-Host 'usage: guest-state.ps1 -Snapshot|-Restore [-Tag <name>]'
Write-Host ("known states: {0}" -f ((Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Name }) -join ', '))
