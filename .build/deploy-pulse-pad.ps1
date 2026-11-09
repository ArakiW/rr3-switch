# Install the PULSING confirm-button script on the emulator card.
#
# WHY THIS EXISTS.  MEASURED 2026-11-09 (rounds m2, m3, v1): the guest
# froze on a modal "you received 50 gold" popup -- the panel with the coin
# icon, the empty text area and the red glove parked at its top right
# corner (archive 20261109-155529-eden-rr3-m3-nov4-census-off/frames/
# frame-005200.png) -- and never reached the race.  All three rounds drew
# EXACTLY 10,394 things in 400 s: the same deterministic stop.
#
# The popup is PERSISTED by the guest, so it survives between rounds: once
# it is on screen, every later round starts stuck on it.
#
# WHY A HELD BUTTON CANNOT DISMISS IT: the autopilot pad script is a state
# timeline and the line "[at 600] ... b=0x001" holds the guest's BTN_A down
# from frame 600 to the end of the round.  There is never a release, so the
# guest's UI never sees a BUTTON-DOWN TRANSITION after the popup appears,
# and a modal dialog that waits for a press waits forever.
#
# The pulse keeps the button held for most of the cycle and releases it for
# a few frames, so the guest gets a fresh press every cycle.
#
# Usage: powershell -File .build\deploy-pulse-pad.ps1
param(
    [switch]$Undo
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$pad = Join-Path $app 'rr3-pad.txt'
$pulse = Join-Path $root 'native-switch\card\rr3-pad-autopilot-pulse.txt'
$plain = Join-Path $root 'native-switch\card\rr3-pad-autopilot.txt'

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

if ($Undo) {
    $source = $plain
    $what = 'the plain hold script'
}
else {
    $source = $pulse
    $what = 'the pulsing script'
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "no script at $source"
}
Copy-Item -LiteralPath $source -Destination $pad -Force
Write-Host ("pad     : {0}  ({1} bytes, {2})" -f $pad, (Get-Item $pad).Length, $what)
Write-Host ("          the confirm button is released every cycle so the guest")
Write-Host ("          sees a fresh press; the reward popup cannot wedge it.")
