# THE FIX, MEASURED IN THE TWO CELLS THAT FAILED.
#
# WHY THESE TWO ROUNDS AND NOT OTHERS.
#
# The 2x2 on 2026-11-09 (all four cells, same card, same assets, same
# emulator, one variable each) was:
#
#                census ON            census OFF
#   Nov-4 NRO    m1  WORKS           m3  FAILS
#   Nov-9 NRO    m2  FAILS           q163 FAILS
#
# TWO INDEPENDENT FAULTS, not one:
#
#   * m1 vs m3 is the SAME BINARY with the census off and on, so census=off
#     breaks the guest on its own: the guest never reaches the race.
#   * m1 vs m2 is the SAME CENSUS SETTING with the Nov-9 code and the Nov-4
#     code, so the Nov-9 code breaks the guest on its own.
#
# The Nov-9 code's own log says what it does to a guest thread that waits on
# a condition variable: it takes the global registry lock once per SECOND
# while holding the guest's own mutex, and the census sampler holds that same
# lock while it pauses and resumes every guest thread.  And the signal a
# host condvar drops when nobody is waiting is dropped for good.
#
# The fix: take the lock ONCE per wait, and REMEMBER a signal that arrives
# while nobody is waiting so that the next waiter can collect it.
#
# So the two rounds that matter are the two cells that FAILED, and the
# measurement is the same in both: did the guest's race-metrics object ever
# come alive?
#
# Usage: powershell -File .build\run-condwait-verify.ps1 -Seconds 400
param(
    [int]$Seconds = 400
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

$nro = Join-Path $root 'native-switch\rr3-native-rehost.nro'
if (-not (Test-Path -LiteralPath $nro -PathType Leaf)) {
    throw "no built NRO at $nro"
}
$hash = (Get-FileHash -LiteralPath $nro -Algorithm SHA256).Hash
Write-Host ("=== the fixed NRO: {0} ===" -f $hash.Substring(0, 16))

Write-Host '=== input mode: autopilot ==='
& (Join-Path $root '.build\deploy-input-mode.ps1') -Mode autopilot

$rounds = @(
    @{ tag = 'v1-fixed-census-off'; censusOff = $true },
    @{ tag = 'v2-fixed-census-on';  censusOff = $false }
)

foreach ($r in $rounds) {
    Write-Host ''
    Write-Host ('=== {0} ===' -f $r.tag)
    if ($r.censusOff) {
        & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $r.tag -Seconds $Seconds -CensusOff -Nro $nro
    }
    else {
        & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $r.tag -Seconds $Seconds -Nro $nro
    }
    Write-Host ('--- {0} ---' -f $r.tag)
    python (Join-Path $root '.build\metrics-summary.py') $r.tag
}

Write-Host ''
Write-Host 'VERIFY DONE'
