# Two rounds that differ in ONE thing: the guest-thread census.
#
# WHY
# ---
# The frame rate is the black screen's real lever, and it has been measured
# wrong twice already.  MEASURED 2026-11-04 (q142, archive
# 20261104-145617-eden-rr3-q142-corners): every healthy round pegs the CPU at
# 1.44..1.56 cores, but the frames per second ranged from 2.35 (q133) to 11.44
# (q142).  The same CPU budget, a 4.9x spread in frames -- so the extra time
# per frame is WAITING, not computing.
#
# The census is the port's own instrument that touches the guest's threads:
# it sweeps them every 4 s and samples each one.  The kill switch already
# exists ("census=off" in sdmc:/switch/rr3-census.conf) and has NEVER been
# run.  This script runs the pair that finally measures it.
#
# WHAT IS THE SAME IN BOTH ROUNDS
# -------------------------------
# The SAME build (the NRO is deployed once, before both rounds), the same
# card scripts, the same emulator, the same wall-clock budget.  The only
# difference is the census conf file.  That is what makes it an A/B rather
# than two observations.
#
# The rounds also carry the pursuit fix and the log-volume cut, so they are
# the race round too: the guest's own red-glove follower presses the race
# button (MEASURED in q142: "phase=ui-press ... kind=orange x=1105 y=695
# area=8180" at frame 420), so a race starts whether or not the semantic
# BTN_RACE request works.
#
# Usage: powershell -File .build\run-lock-census.ps1 -Seconds 400
param(
    [int]$Seconds = 400
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

function Assert-EdenIdle {
    $running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
    }
}

Write-Host '=== deploy the build ==='
Assert-EdenIdle
& (Join-Path $root '.build\deploy-card-romfs.ps1')

Write-Host '=== deploy the card scripts ==='
& (Join-Path $root 'tools\Deploy-RR3CardScripts.ps1') -EmulatorRoot 'E:\Eden-rr3' -Apply

$rounds = @(
    @{ Tag = 'q143-lock-censusoff'; Census = $true },
    @{ Tag = 'q144-lock-censson';  Census = $false }
)

foreach ($round in $rounds) {
    Write-Host ("=== census {0} -> {1} ===" -f $(if ($round.Census) { 'OFF' } else { 'ON' }), $round.Tag)
    Assert-EdenIdle
    if ($round.Census) {
        & (Join-Path $root '.build\deploy-census-conf.ps1') -Off
    }
    else {
        & (Join-Path $root '.build\deploy-census-conf.ps1')
    }
    & (Join-Path $root 'tools\Test-OnEden.ps1') -EmulatorRoot 'E:\Eden-rr3' `
        -EmulatorExe eden-rr3.exe -EmulatorName eden-rr3 -Seconds $Seconds -Tag $round.Tag
}

Write-Host 'BOTH ROUNDS COMPLETE'
