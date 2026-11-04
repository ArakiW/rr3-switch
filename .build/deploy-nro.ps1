# Put a specific NRO on the emulator card.
#
# WHY: the game FROZE at frame 690 right after the race started (the first
# time the race-metrics object was ever found), and the port's own watchdog
# says the sampler thread stopped ("sampler_last_tick=9", "recoveries=1",
# and the sampler-flush lines report "phase=in-guest control_in_guest=0/5111"
# -- the sampler paused the guest's threads and never resumed them).
#
# The pursuit fix (the look-ahead direction and the handedness test) runs in
# exactly that path, so it has to be A/B'd: the OLD NRO with the SAME touch
# script.  If the freeze happens there too, the freeze is not the pursuit
# fix.
#
# Usage: powershell -File .build\deploy-nro.ps1 -From <path-to-nro>

param(
    [Parameter(Mandatory = $true)]
    [string]$From
)

$ErrorActionPreference = 'Stop'

$target = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-native-rehost.nro'

if (-not (Test-Path -LiteralPath $From -PathType Leaf)) {
    throw "no such NRO: $From"
}

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

$before = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
Copy-Item -LiteralPath $From -Destination $target -Force
$after = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash

Write-Host ("card NRO: {0}" -f $before.Substring(0, 16))
Write-Host ("       ->  {0}   ({1:N0} bytes)" -f $after.Substring(0, 16), (Get-Item $target).Length)
Write-Host ("source     : {0}" -f $From)
