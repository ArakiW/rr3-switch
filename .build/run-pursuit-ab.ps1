# The pursuit's A/B, as one script.
#
# WHY IT EXISTS.  The pursuit has TWO halves that only make sense together:
# the line the car follows, and how far ahead it aims.  MEASURED 2026-11-04
# (q147) showed them pulling in opposite directions on the racing line: the
# car got ON the right line (0.32 m from it instead of 2.92-8.25 m from the
# centre line) and the lap progress got WORSE (10.35 % -> 5.14 %), because
# the look-ahead was a constant 20 m and 20 m ahead on that line's corners
# is 0.7 rad off the car's path.
#
# So the two keys are independent and the combinations have to be MEASURED,
# not argued about:
#
#   raceline=centre_spline   lookahead=fixed      the old behaviour
#   raceline=centre_spline   lookahead=adaptive   the new default
#   raceline=ai_spline       lookahead=fixed      the q147 combination
#   raceline=ai_spline       lookahead=adaptive   the combination q147 asked for
#
# THE ROUND IS PINNED TO ONE BINARY.  The A/B is about the CONFIGURATION,
# so every cell has to run the same NRO: the script resolves the build's NRO
# once, hashes it, and hands that file to every round, and each round
# verifies the card's hash before the guest starts.  A rebuild in the middle
# of the A/B would otherwise change one cell and make the comparison a lie.
#
# Usage:
#   powershell -File .build\run-pursuit-ab.ps1 -Seconds 400
#   powershell -File .build\run-pursuit-ab.ps1 -Seconds 400 -Only adaptive-ai
param(
    [int]$Seconds = 400,
    [string]$Only = '',
    [switch]$SkipGate,
    [switch]$CensusOn
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

$combos = @(
    @{ name = 'fixed-centre'; raceline = 'centre_spline'; lookahead = 'fixed' },
    @{ name = 'adaptive-centre'; raceline = 'centre_spline'; lookahead = 'adaptive' },
    @{ name = 'fixed-ai'; raceline = 'ai_spline'; lookahead = 'fixed' },
    @{ name = 'adaptive-ai'; raceline = 'ai_spline'; lookahead = 'adaptive' }
)

if (-not $SkipGate) {
    Write-Host '=== gate ==='
    & (Join-Path $root '.build\gate.ps1')
    if ($LASTEXITCODE -ne 0) {
        throw ("the gate failed (exit {0}); no round was run" -f $LASTEXITCODE)
    }
}

# THE ONE BINARY THE WHOLE A/B RUNS.  Resolved once, hashed once.
$nro = Join-Path $root 'native-switch\rr3-native-rehost.nro'
if (-not (Test-Path -LiteralPath $nro -PathType Leaf)) {
    throw "no built NRO at $nro -- build first, or pass -SkipGate with a build you trust"
}
$nroHash = (Get-FileHash -LiteralPath $nro -Algorithm SHA256).Hash
Write-Host ("=== the NRO every cell runs: {0} ===" -f $nroHash.Substring(0, 16))

# THE AUTOPILOT, NOT THE MANUAL SCRIPT: this measures the port's own closed
# loop, which is the thing that has to complete a lap.
#
# THE PULSE.  MEASURED 2026-11-09: with the plain script, which HOLDS the
# guest's BTN_A down for the whole round, the guest never sees a
# button-down TRANSITION, so a modal dialog (the "you received 50 gold"
# popup, which the guest PERSISTS between rounds) waits for a press that
# never comes and the guest sits in the front end forever.  Three rounds
# drew EXACTLY 10,394 things in 400 s and stopped in the same place.  With
# the pulse, the same build and the same card reached the race.
Write-Host '=== input mode: autopilot (pulsing confirm) ==='
& (Join-Path $root '.build\deploy-input-mode.ps1') -Mode autopilot-pulse

foreach ($combo in $combos) {
    if ($Only -ne '' -and $combo.name -ne $Only) {
        continue
    }
    Write-Host ''
    Write-Host ("=== {0}: raceline={1} lookahead={2} ===" -f
        $combo.name, $combo.raceline, $combo.lookahead)
    # THE SAME GUEST STATE AT THE START OF EVERY CELL.  The guest persists
    # its own state, so a cell that starts from a different state is
    # measuring that state and not the variable it was given.
    & (Join-Path $root '.build\guest-state.ps1') -Restore
    python (Join-Path $root '.build\set-pursuit-conf.py') $combo.raceline $combo.lookahead
    if ($LASTEXITCODE -ne 0) {
        throw ("could not set the pursuit configuration for {0}" -f $combo.name)
    }
    $tag = "pursuit-{0}" -f $combo.name
    if ($CensusOn) {
        & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $tag -Seconds $Seconds -Nro $nro
    }
    else {
        & (Join-Path $root '.build\run-pinned-round.ps1') -Tag $tag -Seconds $Seconds -CensusOff -Nro $nro
    }
    Write-Host ''
    Write-Host ("--- {0} ---" -f $combo.name)
    python (Join-Path $root '.build\pursuit-report.py') $tag
}

Write-Host ''
Write-Host ("A/B DONE, all cells on NRO {0}" -f $nroHash.Substring(0, 16))

# THE BASELINE FOR THE NEXT A/B.  Taken after the last round, so that the
# next A/B starts every cell from the state this one ended in -- and not
# from a state one of its own cells produced.
Write-Host '=== snapshot the guest state for the next A/B ==='
& (Join-Path $root '.build\guest-state.ps1') -Snapshot
