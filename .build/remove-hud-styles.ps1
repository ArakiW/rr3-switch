# Take the LocalStyles stand-in back OFF the card.
#
# WHY: the experiment failed.  With the per-series
# gui/processed/limited_time_series/common/LocalStyles.gui placed on the card as
# the top-level gui/processed/LocalStyles.gui, the guest stopped reaching the
# race: round hudv1 reported "race screen 0", "start lights 0" and
# "EventMapScreen 0", and the front-end gate showed the guest still at the
# front end at frame 3840.  The round before it, with the same ETC volumes and
# NO stand-in (etcv1), reached a race at frame 4200.
#
# So the per-series style table is NOT a valid stand-in for the top-level one:
# the guest loads it and then cannot build its front end.  The file has to
# come from the Android device that has run the game (docs/OPEN-ISSUES.md 9.20).
#
# A card that carries a wrong file is worse than a card that carries none: the
# game boots into a broken front end instead of booting with the default
# styles.
param(
    [string]$Card = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\.depot\gui\processed'
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $Card 'LocalStyles.gui'
if (Test-Path -LiteralPath $target -PathType Leaf) {
    Remove-Item -LiteralPath $target -Force
    Write-Host ("removed  : {0}" -f $target)
}
else {
    Write-Host "nothing to remove"
}
if (Test-Path -LiteralPath $Card -PathType Container) {
    $left = @(Get-ChildItem -LiteralPath $Card -File -ErrorAction SilentlyContinue)
    if ($left.Count -eq 0) {
        Remove-Item -LiteralPath $Card -Force
        Write-Host ("removed  : {0} (empty)" -f $Card)
    }
    else {
        Write-Host ("left     : {0} file(s) in {1}" -f $left.Count, $Card)
    }
}
Write-Host "The card is back to the state the game can boot in."
