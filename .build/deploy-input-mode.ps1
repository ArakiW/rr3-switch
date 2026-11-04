# Put the card into one of the three INPUT MODES, and install the matching
# pad script.
#
# WHY: the three modes are three different questions, and mixing them up is
# how this project spent a round measuring the wrong thing.
#
#   manual   the port does not touch the car.  The player's controller (or
#            the weave script, inside an emulator) drives it.  This is what
#            the console ships, because a port that drives the car itself
#            and ignores the player is not a playable port.
#   autopilot the port's own closed loop owns the steering, the throttle and
#            the brake.  This is the ATTRACT MODE, and it is what the
#            rounds that measure the pursuit use.
#   none     the port does not touch the car and there is no script either:
#            only a physical controller can drive.
#
# MEASURED 2026-11-04 (q149, manual): the axis the human asked for is the
# axis the guest got, the accelerator is the accelerator, the closed loop did
# not overwrite either, and the car moved and turned.
#
# MEASURED 2026-11-04 (q148, the control: autopilot = 0 with the OLD code):
# the autopilot still drove (drive engaged 201/208 samples), because the mode
# only governed the physical path.  That is the bug this script's modes are
# now real.
#
# Usage: powershell -File .build\deploy-input-mode.ps1 -Mode manual
param(
    [ValidateSet('manual', 'autopilot', 'none')]
    [string]$Mode = 'manual'
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$gamepad = Join-Path $app 'rr3-gamepad.txt'
$pad = Join-Path $app 'rr3-pad.txt'

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

switch ($Mode) {
    'manual' {
        $autopilot = '0'
        $script = Join-Path $root 'native-switch\card\rr3-pad-manual.txt'
    }
    'autopilot' {
        $autopilot = '1'
        $script = Join-Path $root 'native-switch\card\rr3-pad-autopilot.txt'
    }
    'none' {
        $autopilot = '0'
        $script = Join-Path $root 'native-switch\card\rr3-pad.txt'
    }
}

# THE MODE, AND ONLY THE MODE.  Every other line of the mapping is left
# exactly as it was: the point of the round is the input path, not a
# remapping.
$text = Get-Content -LiteralPath $gamepad -Raw
$before = ($text -split "`n" | Where-Object { $_.Trim() -like 'autopilot*' })
$text = [regex]::Replace($text, '(?m)^autopilot\s*=.*$', ("autopilot = " + $autopilot))
[System.IO.File]::WriteAllText($gamepad, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("gamepad : {0}  ->  autopilot = {1}" -f ($before -join '; '), $autopilot)

Copy-Item -LiteralPath $script -Destination $pad -Force
Write-Host ("pad     : {0}  ({1} bytes, from {2})" -f $pad, (Get-Item $pad).Length, (Split-Path -Leaf $script))
switch ($Mode) {
    'manual' { Write-Host 'the port will NOT touch the car: the script owns the throttle and the steering.' }
    'autopilot' { Write-Host 'the port''s own closed loop owns the car.' }
    'none' { Write-Host 'the port will NOT touch the car, and there is no script: only a controller drives.' }
}
