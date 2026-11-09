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
    [switch]$CensusOff,
    # The NRO that must actually RUN, when it is not the current build.
    #
    # WHY THIS EXISTS.  MEASURED 2026-11-09: an A/B that deployed an OLD NRO
    # onto the card and then called this script ran the NEW NRO anyway, and
    # the experiment was silently worthless.  Two scripts copy the build's
    # NRO onto the card -- deploy-card-romfs.ps1 and Test-OnEden.ps1 -- and
    # BOTH of them did it AFTER the experiment's own deploy.  So the chosen
    # NRO has to go on the card after both of them, and Test-OnEden has to
    # be told not to copy.
    #
    # The card's NRO hash is verified afterwards, because an A/B that cannot
    # say which binary ran is not an A/B.
    [string]$Nro = ''
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

$cardNro = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-native-rehost.nro'
$wantHash = ''

if ($Nro -ne '') {
    Write-Host '=== deploy the chosen NRO (AFTER the build copy) ==='
    Assert-EdenIdle
    & (Join-Path $root '.build\deploy-nro.ps1') -From $Nro
    $wantHash = (Get-FileHash -LiteralPath $Nro -Algorithm SHA256).Hash
}

# THE GATE ON THE EXPERIMENT.  The card's NRO is what runs, so its hash is
# the one thing that has to be right; everything else in the round is
# downstream of it.
$gotHash = (Get-FileHash -LiteralPath $cardNro -Algorithm SHA256).Hash
Write-Host ("card NRO sha256: {0}" -f $gotHash)
if ($wantHash -ne '' -and $gotHash -ne $wantHash) {
    throw ("the card's NRO is not the one that was chosen: {0} != {1}" -f $gotHash, $wantHash)
}

Write-Host '=== eden ==='
Assert-EdenIdle
$edenArgs = @{
    EmulatorRoot = 'E:\Eden-rr3'
    EmulatorExe = 'eden-rr3.exe'
    EmulatorName = 'eden-rr3'
    Seconds = $Seconds
    Tag = $Tag
}
if ($Nro -ne '') {
    # The NRO is already on the card; Test-OnEden must not overwrite it.
    $edenArgs['NoDeploy'] = $true
}
& (Join-Path $root 'tools\Test-OnEden.ps1') @edenArgs

Write-Host 'ROUND COMPLETE'
