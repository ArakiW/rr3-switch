# Clean the DEBUG RESIDUE off a card before it goes on a real console.
#
# WHY THIS EXISTS.  MEASURED 2026-11-09: the console froze with the guest's
# onViewChangedJNI never returning.  The progress file's last line was
# "view-sync-blocked-cond-wait" -- the primitive, not the object.  The
# instrument that names the object (the census, which prints
# obj/waits/signals/deficit/wait_caller) was OFF, because this card carried
# an rr3-census.conf left over from an emulator round:
#
#     sdmc:/switch/rr3-census.conf   ->   census=off
#
# The release package does not ship that file, so it was never going to be
# overwritten -- it just SURVIVED, and it turned the one instrument that could
# have explained the freeze into silence.  A card is a moving target: it
# accumulates files across dozens of experiments, and the console inherits
# every one of them.
#
# WHAT THIS DOES.  Reports, and removes, the files on the card that exist
# only because of a measurement round:
#
#   rr3-census.conf        census=off / census=on -- the census kill switch
#   rr3-probe.conf         the probe's kill switch
#   rr3-native-rehost.crash-*.txt   one per boot; hundreds of them
#   rr3-native-rehost.egl-probe.log  overwritten every boot
#   rr3-native-rehost.guest-view.log  40 MB on this card
#   rr3-native-rehost.teardown-repair.log  5.4 MB on this card
#
# It does NOT touch rr3-gfx.conf (that one is the shipped configuration and
# has to stay), the control scripts, or the NRO.
#
# The census is turned back ON here, deliberately: the fix that makes a
# freeze self-explaining lives in the port (the waiter writes its own
# ledger), so the census is no longer the only path to the answer, and
# leaving it off keeps the card cheap.
#
# Usage: powershell -File .build\clean-card-residue.ps1
#        powershell -File .build\clean-card-residue.ps1 -WhatIf
#        powershell -File .build\clean-card-residue.ps1 -CardRoot 'F:\'

param(
    [string]$CardRoot = 'E:\Eden-rr3\user\sdmc\switch',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $CardRoot -PathType Container)) {
    throw "card root not found: $CardRoot"
}

Write-Host ("card root: {0}" -f $CardRoot)

# The kill switches: report them, and make sure the census is ON.
$confFiles = @('rr3-census.conf', 'rr3-probe.conf')
foreach ($name in $confFiles) {
    $path = Join-Path $CardRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Host ("  {0}: absent" -f $name)
        continue
    }
    $text = (Get-Content -LiteralPath $path -Raw)
    $active = ($text -split "`n" |
        Where-Object { $_.Trim() -ne '' -and -not $_.Trim().StartsWith('#') } |
        ForEach-Object { $_.Trim() })
    Write-Host ("  {0}: {1}" -f $name, ($active -join ' | '))
}

# The census kill switch is a MEASUREMENT setting, not a shipped setting.
# On a console it has to be ON: the census is what turns "the guest is
# parked on something" into "the guest is parked on THIS object".
$censusPath = Join-Path $CardRoot 'rr3-census.conf'
if (-not $WhatIf) {
    [System.IO.File]::WriteAllText(
        $censusPath, "census=on`n", (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  -> wrote {0} (census=on)" -f $censusPath)
}
else {
    Write-Host ("  -> would write {0} (census=on)" -f $censusPath)
}

# The per-boot debris: hundreds of crash markers and multi-megabyte logs.
$patterns = @(
    'rr3-native-rehost.crash-*.txt',
    'rr3-native-rehost.egl-probe.log',
    'rr3-native-rehost.guest-boot.progress',
    'rr3-native-rehost.guest-view.log',
    'rr3-native-rehost.teardown-repair.log',
    'rr3-native-rehost.guarded-action.pending'
)

$removed = 0
$freedBytes = 0
foreach ($pattern in $patterns) {
    $matches = @(Get-ChildItem -LiteralPath $CardRoot -Filter $pattern -File -ErrorAction SilentlyContinue)
    if ($matches.Count -eq 0) {
        continue
    }
    $bytes = ($matches | Measure-Object -Property Length -Sum).Sum
    Write-Host ("  {0}: {1} file(s), {2:N0} bytes" -f $pattern, $matches.Count, $bytes)
    if (-not $WhatIf) {
        foreach ($m in $matches) {
            & 'C:\Users\L\.minimax\bin\mavis-trash.cmd' -- $m.FullName | Out-Null
            $removed++
        }
    }
    $freedBytes += $bytes
}

Write-Host ("")
Write-Host ("removed {0} file(s), {1:N0} bytes{2}" -f
    $removed, $freedBytes, $(if ($WhatIf) { ' (WhatIf: nothing was removed)' } else { '' }))
Write-Host "kept: rr3-gfx.conf, the control scripts and the NRO"
