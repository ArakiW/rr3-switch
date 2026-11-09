# Put the SEMANTIC control requests on the card, and the plain (held) pad
# script beside them.
#
# WHY THIS EXISTS.  MEASURED 2026-11-09 (rounds m3, v3, v4):
#
#   m3   the confirm button HELD DOWN.  The guest froze on the modal "you
#       received 50 gold" popup and never reached the race.  The popup is
#       PERSISTED by the guest, so every later round started stuck on it.
#   v3   the confirm button PULSED.  The popup was dismissed and the guest
#       reached the race.
#   v4   the pulse AND the guest's save restored.  The guest ended up on the
#       NASCAR championship's card -- a MANUFACTURER cup -- whose assets are
#       not on the card, and it sat there on a 0 % progress bar for the whole
#       round.
#
# ⇒ THE TWO HALVES OF THAT ARE NOT IN CONFLICT, and the archives show it:
#
#   * A PULSE produces a button-down TRANSITION, and the event map is a
#     scrolling world map: a pulse WALKS the selection, which is how the
#     guest ends up on a manufacturer cup.
#   * A HELD button produces no transition, so it never walks the map -- and
#     the guest stays on the group it started on, the streamable AMATEUR one.
#     (m1 and m3 both held it, and both stayed on the first card.)
#
# ⇒ SO: HOLD the accelerator, and let the PORT do the navigating by NAME.
# The port already has the machinery: rr3-ui.txt is a list of control ids
# and the port taps each one it can find on the current screen
# (ui_control_find / uiControlNextRequest in ui_control_probe.cpp, called
# from guest_view_probe.cpp).  A request whose control is not on the screen
# is SKIPPED, not blocking the others.
#
# ⇒ AND THE IDS ARE NOT GUESSED.  tools/gui_index.py decrypts all 939 of
# the guest's .gui files and indexes their 4,729 id_strings:
#
#   MAP_EVENT_CARD_BTN   on EventMapScreen_STREAM_GROUP_*_card -- the
#                        pressable event card, on EVERY group
#   BTN_RACE              on MainMenu and RaceStartScreen
#   BTN_OK / BTN_CONTINUE on the popup screens
#
# Usage: powershell -File .build\deploy-ui-requests.ps1
$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

# THE REQUESTS.  One control id per line.  The optional "at=<frame>" token
# makes a request wait for the port's frame counter, so a request can be
# aimed at a screen that appears later.
$requests = @(
    '# The controls the port is asked to press, by the GUEST''S OWN id.',
    '# Read once per run from sdmc:/switch/rr3-native-rehost/rr3-ui.txt.',
    '# A request whose control is not on the current screen is skipped.',
    'MAP_EVENT_CARD_BTN',
    'BTN_RACE',
    'BTN_OK',
    'BTN_CONTINUE'
)
[System.IO.File]::WriteAllText((Join-Path $app 'rr3-ui.txt'),
                               ($requests -join "`n") + "`n",
                               (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("ui requests: {0} controls" -f ($requests | Where-Object { $_ -notmatch '^#' }).Count)

# THE PAD SCRIPT: the plain one, which HOLDS the accelerator and therefore
# never walks the event map.  The pulse is what walks it.
Copy-Item -LiteralPath (Join-Path $root 'native-switch\card\rr3-pad-autopilot.txt') `
            -Destination (Join-Path $app 'rr3-pad.txt') -Force
Write-Host ("pad script : {0} bytes (the held script; a pulse walks the map)" -f `
            (Get-Item (Join-Path $app 'rr3-pad.txt')).Length)
