# Set the per-call logging switch on the emulator card.
#
# WHY: the `percall` switch (default 0) gates the port's own per-call trace.
# Four rounds in a row (fps2, fps3, yield1, fps4) did not reach a race while
# the switch was off, and two rounds with it on (chunk2, chunk3) did -- and
# the frames show the front end itself was fine.  This script exists to make
# the A/B one key away, on the CARD, without hand-editing a file there.
#
# Usage: powershell -File .build\set-percall.ps1 -On
param(
    [switch]$On,
    [switch]$Off
)

$ErrorActionPreference = 'Stop'
$conf = 'E:\Eden-rr3\user\sdmc\switch\rr3-gfx.conf'

if (-not (Test-Path -LiteralPath $conf)) {
    throw "card conf missing: $conf"
}

$text = [System.IO.File]::ReadAllText($conf)
$value = if ($On) { '1' } else { '0' }
$text = [regex]::Replace($text, '(?m)^percall\s*=.*$', "percall = $value")
if ($text -notmatch '(?m)^percall\s*=') {
    $text = $text.TrimEnd() + "`npercall = $value`n"
}
[System.IO.File]::WriteAllText($conf, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("percall = " + $value + "   (" + $conf + ")")
