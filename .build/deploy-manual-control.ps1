# Put the card into MANUAL control and install the weave script.
#
# WHY: every round so far ran with "autopilot = 1", where the port's own
# closed loop owns the steering, the throttle and the brake and the human's
# inputs are overwritten before they reach the guest.  So every measurement
# of "the lap counter" has been a measurement of the AUTOPILOT's driving,
# which is a different question from whether the port is PLAYABLE.
#
# This script switches the card to "autopilot = 0" (the port does not touch
# the car at all) and installs native-switch\card\rr3-pad-manual.txt, which
# holds the accelerator and weaves the steering axis.  A round then shows
# three separate things:
#
#   * the port's gamepad-state line: lx == guest_lx, so the human's input
#     reached the guest;
#   * the car accelerates while BTN_A is held, so the throttle reached the
#     car;
#   * the car's path curves when lx changes, so the steering reached the car.
#
# It also keeps a copy of the card's previous rr3-pad.txt so the change can
# be undone, because the card is the only place the port reads these from.
#
# Usage: powershell -File .build\deploy-manual-control.ps1 [-Restore]
param(
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$gamepad = Join-Path $app 'rr3-gamepad.txt'
$pad = Join-Path $app 'rr3-pad.txt'
$backup = Join-Path $app 'rr3-pad.autopilot.bak'

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $backup)) {
        throw 'there is no saved autopilot pad script to restore'
    }
    Copy-Item -LiteralPath $backup -Destination $pad -Force
    Remove-Item -LiteralPath $backup -Force
    Write-Host ("restored the autopilot pad script: {0}" -f $pad)
}
else {
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $pad -Destination $backup -Force
        Write-Host ("saved the previous pad script: {0}" -f $backup)
    }

    # THE MODE, AND ONLY THE MODE.  Every other line of the mapping is left
    # exactly as it was: the point of the round is the input path, not a
    # remapping.
    $text = Get-Content -LiteralPath $gamepad -Raw
    $before = ($text -split "`n" | Where-Object { $_.Trim() -like 'autopilot*' })
    $text = [regex]::Replace($text, '(?m)^autopilot\s*=.*$', 'autopilot = 0')
    [System.IO.File]::WriteAllText($gamepad, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("gamepad: {0}  ->  autopilot = 0" -f ($before -join '; '))

    $script = Join-Path $root 'native-switch\card\rr3-pad-manual.txt'
    Copy-Item -LiteralPath $script -Destination $pad -Force
    Write-Host ("pad     : {0}  ({1} bytes)" -f $pad, (Get-Item $pad).Length)
    Write-Host 'the port will now NOT touch the car: the script owns the throttle and the steering.'
}
