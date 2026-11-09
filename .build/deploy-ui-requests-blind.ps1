# The BLIND navigation, for the one comparison that needs it.
#
# WHY.  Round q145 (2026-11-04, the 10.35 % reference) navigated the front end
# with the pad script's blind confirm press, and its rr3-ui.txt asked for
# BTN_RACE -- a control that does NOT exist on the event map, so the port
# tapped NOTHING.  Every 2026-11-09 round has the port TAPPING
# MAP_EVENT_CARD_BTN, which selects whatever card is under the guest's red
# glove; the race then starts 900 frames later than it did in q145, and the
# car stops making progress 450 frames into the race.
#
# ⇒ So this installs q145's navigation exactly: the same held pad script and
# an rr3-ui.txt that asks for BTN_RACE and nothing else.  The only difference
# from round m6 is the ONE line below.
#
# Usage: powershell -File .build\deploy-ui-requests-blind.ps1
$ErrorActionPreference = 'Stop'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'

$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

$requests = @(
    '# THE BLIND NAVIGATION, exactly as q145 had it: the port taps nothing',
    '# on the event map, and the pad script''s held confirm press does the',
    '# navigating.  This is the A/B arm for "did the port''s own navigation',
    '# change which event raced?".',
    'BTN_RACE'
)
[System.IO.File]::WriteAllText((Join-Path $app 'rr3-ui.txt'),
                               ($requests -join "`n") + "`n",
                               (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("ui requests: {0} control (BTN_RACE only -- the blind navigation)" -f
            ($requests | Where-Object { $_ -notmatch '^#' }).Count)
