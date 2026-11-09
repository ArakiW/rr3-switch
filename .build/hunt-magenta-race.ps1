# Walk the guest's event rotation until the magenta race comes back, keeping
# every round's start state so it can be reproduced.
#
# WHY.  MEASURED 2026-11-09: the guest advances to the NEXT EVENT after each
# race.  The NASCAR round (m9) showed the magenta AI opponents; the next
# round (m10) was Hockenheim and had none.  m9 and m10 asked for exactly the
# same 41 asset paths, so the ONLY difference was the event -- and the state
# that produced m9 was gone, because the baseline was overwritten every
# round.
#
# ⇒ So: each round keeps its own state (guest-state.ps1 -Tag), and after
# every round the frames are scanned for magenta and the round's race is
# identified from the guest's own screens.
#
# Usage: powershell -File .build\hunt-magenta-race.ps1 -Rounds 4
param(
    [int]$Rounds = 4,
    [int]$Seconds = 400,
    [string]$Nro = ''
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'

# THE BINARY IS PINNED, NOT A PATH.
#
# A path would let a rebuild change what the later rounds run, which is
# exactly the kind of uncontrolled variable this hunt exists to remove: the
# rounds have to differ ONLY in how many races have happened, which is the
# event.  So the NRO is copied to a name that carries its hash, and the
# rounds run that copy.
if ($Nro -eq '') {
    $built = Join-Path $root 'native-switch\rr3-native-rehost.nro'
    $hash = (Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash.Substring(0, 16)
    $Nro = Join-Path $root (".build\pinned-{0}.nro" -f $hash)
    Copy-Item -LiteralPath $built -Destination $Nro -Force
}
Write-Host ("=== the pinned binary: {0} ===" -f $Nro)

Write-Host '=== input mode: autopilot, blind navigation (the path that reaches a race) ==='
& (Join-Path $root '.build\deploy-ui-requests-blind.ps1')

for ($i = 1; $i -le $Rounds; $i++) {
    $tag = "hunt{0}" -f $i
    Write-Host ''
    Write-Host ("=== {0} ===" -f $tag)
    # THE SAME START STATE FOR EVERY ROUND, so the ONLY thing that changes is
    # how many races have happened -- which is the event.
    & (Join-Path $root '.build\guest-state.ps1') -Restore -Tag 'baseline'
    & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $tag -Seconds $Seconds -CensusOff `
        -Nro $nro

    # WHAT DID THIS ROUND LOOK LIKE?
    $run = Get-ChildItem -LiteralPath (Join-Path $root '.build\eden-runs') -Directory |
        Where-Object { $_.Name -match [regex]::Escape($tag) } | Select-Object -First 1
    if (-not $run) { Write-Host '   (no archive)'; continue }
    $summary = Join-Path $run.FullName 'run-summary.txt'
    if (Test-Path -LiteralPath $summary) {
        Get-Content -LiteralPath $summary |
            Select-String -Pattern 'start lights|race screen' |
            ForEach-Object { Write-Host ("   {0}" -f ($_.Line -replace '^\s+', '')) }
    }
    & (Join-Path $root '.build\scan-magenta.py') $tag
    # THE RACE'S OWN NAME, from the guest's own screens.
    $log = Join-Path $run.FullName 'rr3-native-rehost.guest-view.log'
    if (Test-Path -LiteralPath $log) {
        Select-String -LiteralPath $log -Pattern 'EventMapScreen_STREAM_GROUP_[A-Za-z_]+_card' |
            ForEach-Object { $_.Matches } | ForEach-Object { $_.Value } |
            Sort-Object -Unique | Select-Object -First 6 | ForEach-Object { Write-Host ("   event card: {0}" -f $_) }
    }
    # THE NEXT ROUND STARTS FROM THIS ROUND'S END STATE.
    & (Join-Path $root '.build\guest-state.ps1') -Snapshot -Tag 'baseline'
}

Write-Host ''
Write-Host 'HUNT DONE'
