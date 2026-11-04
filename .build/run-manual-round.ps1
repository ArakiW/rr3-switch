# The MANUAL CONTROL round: prove the human's input reaches the car.
#
# WHY: every round so far ran with "autopilot = 1", where the port's own
# closed loop owns the steering, the throttle and the brake.  Every
# measurement of "the lap counter" has therefore been a measurement of the
# AUTOPILOT, which is a different question from whether the port is
# PLAYABLE.
#
# This runner does the whole thing in the only order that makes sense:
#
#   1. the gate, because a round that measures a build the gate rejected is
#      worthless;
#   2. the input mode: the card's rr3-gamepad.txt gets "autopilot = 0" (the
#      port does not touch the car) and the card's rr3-pad.txt becomes the
#      weave script (accelerator held, steering woven);
#   3. the round.
#
# The NRO is deployed by the round runner, so the build that is measured is
# the build that was gated.
#
# Usage: powershell -File .build\run-manual-round.ps1 -Tag q148-manual -Seconds 400
param(
    [string]$Tag = 'q148-manual',
    [int]$Seconds = 400,
    [ValidateSet('manual', 'autopilot', 'none')]
    [string]$Mode = 'manual'
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

Write-Host '=== gate ==='
& (Join-Path $root '.build\gate.ps1')
if ($LASTEXITCODE -ne 0) {
    throw ("the gate failed (exit {0}); no round was run" -f $LASTEXITCODE)
}

Write-Host ''
Write-Host ("=== input mode: {0} ===" -f $Mode)
& (Join-Path $root '.build\deploy-input-mode.ps1') -Mode $Mode

Write-Host ''
Write-Host '=== round ==='
& (Join-Path $root '.build\run-pinned-round.ps1') -Tag $Tag -Seconds $Seconds -CensusOff

Write-Host 'ROUND COMPLETE'
