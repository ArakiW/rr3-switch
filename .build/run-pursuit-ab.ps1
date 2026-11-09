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
# THE HELD SCRIPT, NOT THE PULSE.  MEASURED 2026-11-09 (rounds v3, v4, v5):
# a PULSE produces a button-down TRANSITION, and the event map is a
# scrolling world map, so a pulse WALKS the selection -- which is how the
# guest ended up on a manufacturer cup whose assets are not on the card, and
# stuck on a 0 % progress bar (v4).  A HELD button produces no transition,
# so it never walks the map, and the guest stays on the streamable group it
# started on.
#
# The navigation is done BY NAME instead, through rr3-ui.txt: the port taps
# MAP_EVENT_CARD_BTN on the event map and BTN_RACE on the race start screen.
# With that, round v5 reached the race with the normal 7 start lights and 10
# race screens, and the flip fix took the flips from 7 to 1 and the stalls
# from 5 to 0.
Write-Host '=== input mode: autopilot (held confirm; the port navigates by name) ==='
& (Join-Path $root '.build\deploy-input-mode.ps1') -Mode autopilot

# AND THE SEMANTIC CONTROL REQUESTS, which are what actually navigate.
& (Join-Path $root '.build\deploy-ui-requests.ps1')

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
