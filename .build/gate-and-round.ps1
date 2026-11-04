# Gate, then one Eden round, in that order, with the gate's exit code obeyed.
#
# WHY A SCRIPT: the round measures a build, and a round that measures a build
# the gate rejected is worthless.  Running them by hand is how that happens.
#
# Neither step may be piped or redirected: the gate's stderr carries its
# output, and a pipe swallows it.
#
# Usage: powershell -File .build\gate-and-round.ps1 -Tag q147 -Seconds 400
param(
    [string]$Tag = 'q147-racingline',
    [int]$Seconds = 400,
    [switch]$CensusOff
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

Write-Host '=== gate ==='
& (Join-Path $root '.build\gate.ps1')
if ($LASTEXITCODE -ne 0) {
    throw ("the gate failed (exit {0}); no round was run" -f $LASTEXITCODE)
}

Write-Host ''
Write-Host '=== round ==='
& (Join-Path $root '.build\run-pinned-round.ps1') -Tag $Tag -Seconds $Seconds -CensusOff:$CensusOff

Write-Host 'GATE AND ROUND COMPLETE'
