# THE 2x2 MATRIX: WAS IT THE CODE, OR WAS IT census=off?
#
# WHY THIS EXISTS.  The Nov-4 rounds (q145, q147) found the guest's
# race-metrics object alive at f=540 and drove the car.  EVERY Nov-9 round
# found it NEVER (rej=the-members-are-not-live, lapd=-1), ran 3-5x slower,
# and re-entered the race screens over and over.
#
# TWO THINGS CHANGED BETWEEN THOSE ROUNDS, and this matrix separates them:
#
#   1. THE CODE.  Commits 456f43c3 + 4a8cd99d changed guestPthreadCondWait
#      (a signal epoch, a ledger, a 120 s give-up), 4dfe4694/7a13ef1b added
#      a no-context GL observer, and b8151e47 reworked the asset fallback and
#      the console release in main.cpp.
#
#   2. THE CENSUS.  The card's sdmc:/switch/rr3-census.conf said "census=off"
#      for every Nov-9 round.  The Nov-4 rounds ran with the census ARMED.
#      The census is the part of the port that touches the guest's threads
#      every frame (it SAMPLES by pausing them -- q138 measured 7,071 guest
#      threads paused and never resumed).
#
# So the four cells are:
#
#   m1  Nov-4 NRO, census ON   <- a replica of the known-good Nov-4 round,
#                                 on today's card.  If this FAILS, the card
#                                 or the emulator changed since Nov 4 and no
#                                 amount of code reading will explain it.
#   m2  Nov-9 NRO, census ON   <- isolates the census against q163, which was
#                                 the same code with census OFF.
#   m3  Nov-4 NRO, census OFF  <- isolates the census against m1.
#
# q163 IS the fourth cell (Nov-9 NRO, census OFF) and it is already measured.
#
# Usage: powershell -File .build\run-regression-matrix.ps1 -Seconds 400
param(
    [int]$Seconds = 400
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

$newNro = Join-Path $root 'native-switch\rr3-native-rehost.nro'
$oldNro = Join-Path $root 'dist\rr3-native-rehost-20261104.nro'

$rounds = @(
    @{ tag = 'm1-nov4-census-on';  nro = $oldNro; censusOff = $false },
    @{ tag = 'm2-nov9-census-on';  nro = $newNro; censusOff = $false },
    @{ tag = 'm3-nov4-census-off'; nro = $oldNro; censusOff = $true }
)

Write-Host '=== input mode: autopilot (the port''s own closed loop) ==='
& (Join-Path $root '.build\deploy-input-mode.ps1') -Mode autopilot

foreach ($r in $rounds) {
    Write-Host ''
    Write-Host ('=== {0} ===' -f $r.tag)
    Write-Host ('    nro: {0}' -f $r.nro)
    if ($r.censusOff) {
        & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $r.tag -Seconds $Seconds -CensusOff -Nro $r.nro
    }
    else {
        & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $r.tag -Seconds $Seconds -Nro $r.nro
    }
    Write-Host ('--- {0} ---' -f $r.tag)
    python (Join-Path $root '.build\metrics-summary.py') $r.tag
}

Write-Host ''
Write-Host 'MATRIX DONE'
