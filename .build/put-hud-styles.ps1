# Put the LocalStyles candidate on the card, as a STAND-IN for the file the
# game generates at runtime.
#
# WHY: the guest reads gui/processed/LocalStyles.gui and it exists in NONE of
# the three data packs, NONE of the 3,870 gui/processed entries in core.romfs
# and NONE of the 338 raw .gui files on the card -- the app generates it in its
# own files directory on first run, so only the Android device that has run the
# game has it (docs/OPEN-ISSUES.md 9.20).  Until that file comes off the
# phone, the HUD falls back to default styles and renders as grey-white
# blocks.
#
# This is an EXPERIMENT, not a fix: the per-series
# gui/processed/limited_time_series/common/LocalStyles.gui (1,014 B, the
# "common" variant) is used as the top-level one, so the guest has SOME style
# table where it had none.  Whether the HUD looks right is a measurement, not
# an assumption: the menu frame is compared with the baseline frame.
#
# The file is placed on the CARD (not in a volume): the port's third read
# candidate is the same path on the SD card, so a card copy is found even
# though the volumes do not have it.
param(
    [string]$Source = 'E:\AI\RealRacing3-switch\.build\LocalStyles_common.gui',
    [string]$Card = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\.depot\gui\processed'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    Write-Error "the candidate is missing: $Source"
    exit 1
}
if (-not (Test-Path -LiteralPath $Card -PathType Container)) {
    New-Item -ItemType Directory -Path $Card | Out-Null
}
$target = Join-Path $Card 'LocalStyles.gui'
Copy-Item -LiteralPath $Source -Destination $target -Force
$bytes = (Get-Item -LiteralPath $target).Length
Write-Host ("placed   : {0} ({1} bytes)" -f $target, $bytes)
Write-Host "The guest will now find a style table where it found none.  Run a"
Write-Host "round and compare the menu frame with the baseline: the HUD should"
Write-Host "no longer be grey-white blocks."
