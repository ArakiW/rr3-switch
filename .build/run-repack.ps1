# Rebuild every deliverable for one stamp, in order, and stop at the first
# failure.
#
# WHY THIS EXISTS
# ---------------
# The three packagers were run by hand, from three different places, with
# paths hard-wired to different days.  One repack left the full archive
# stale (see the fingerprint logic in pack-zip.ps1) and the application zip
# was never rebuilt at all, because the chain died on the step before it.
#
# So the order is fixed here: the full archive and its parts first, then the
# application-only zip, then the join check on the parts.
#
#   pack-zip      -> dist\rr3-switch-<stamp>.zip  +  dist\parts-<stamp>\
#   pack-app-v2   -> dist\rr3-switch-app-<stamp>.zip
#   join_check.py -> concatenates the parts and compares them with the zip

param(
    [string]$Stamp = '20261103',
    [switch]$SkipJoinCheck
)

$ErrorActionPreference = 'Stop'

$root = 'E:\AI\RealRacing3-switch'
$sw = [System.Diagnostics.Stopwatch]::StartNew()

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ''
    Write-Host ("=== {0} ===" -f $Name)
    $t = [System.Diagnostics.Stopwatch]::StartNew()
    & $Body
    $t.Stop()
    Write-Host ("--- {0} done in {1:N0}s" -f $Name, $t.Elapsed.TotalSeconds)
}

Step 'pack-zip' {
    & (Join-Path $root '.build\pack-zip.ps1') -Stamp $Stamp
}
Step 'pack-app-v2' {
    & (Join-Path $root '.build\pack-app-v2.ps1') -Stamp $Stamp
}
if (-not $SkipJoinCheck) {
    Step 'join-check' {
        # THE STAMP GOES WITH IT.  The checker used to have the day hard-wired
        # in it, so the chain proved that the OLD parts joined to the OLD
        # archive and said nothing about the ones it had just written.
        & python (Join-Path $root '.build\join_check.py') --stamp $Stamp
        if ($LASTEXITCODE -ne 0) { throw ("join_check failed (exit {0})" -f $LASTEXITCODE) }
    }
}

$sw.Stop()
Write-Host ''
Write-Host ("REPACK COMPLETE : {0} in {1:N0}s" -f $Stamp, $sw.Elapsed.TotalSeconds)
