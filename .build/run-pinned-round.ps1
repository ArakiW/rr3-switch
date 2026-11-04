# One Eden round with a given census setting, on the current build.
#
# WHY A SEPARATE SCRIPT: the census A/B needs two rounds that differ in ONE
# thing, and the second round has to start from the same build as the first.
# This script takes the census setting as an argument so that the two rounds
# of an A/B cannot accidentally differ in anything else, and so that a later
# round can be run with the same setting by name.
#
# It does NOT build: the build is the slow part and it is the same for both
# halves of an A/B, so the caller builds once and this script deploys and
# runs.  The NRO that goes on the card is the build output, and the script
# prints its hash so the archive says WHICH build the round measured.
#
# Usage: powershell -File .build\run-pinned-round.ps1 -Tag q145-pinned -Seconds 400 -CensusOff
param(
    [string]$Tag = 'q145-pinned',
    [int]$Seconds = 400,
    [switch]$CensusOff
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

function Assert-EdenIdle {
    $running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
    }
}

Assert-EdenIdle

Write-Host '=== census conf ==='
if ($CensusOff) {
    & (Join-Path $root '.build\deploy-census-conf.ps1') -Off
}
else {
    & (Join-Path $root '.build\deploy-census-conf.ps1')
}

Write-Host '=== deploy the build ==='
Assert-EdenIdle
& (Join-Path $root '.build\deploy-card-romfs.ps1')

Write-Host '=== eden ==='
Assert-EdenIdle
& (Join-Path $root 'tools\Test-OnEden.ps1') -EmulatorRoot 'E:\Eden-rr3' `
    -EmulatorExe eden-rr3.exe -EmulatorName eden-rr3 -Seconds $Seconds -Tag $Tag

Write-Host 'ROUND COMPLETE'
