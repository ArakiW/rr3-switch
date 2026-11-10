# Set a key in the card's rr3-settings.txt (the game's OWN settings file).
#
# WHY: the port's own overhead is now about a millisecond a frame (see
# docs/OPEN-ISSUES.md section 9.39), and what is left of the frame is the
# GUEST's own work -- its rasterisation at the anti-aliasing level the card
# asks for.  AA_LEVEL = 4 means four samples per pixel, and that is the
# knob that decides how much of the frame is the guest's.
#
# Usage: powershell -File .build\set-guest-setting.ps1 -Key AA_LEVEL -Value 0
param(
    [Parameter(Mandatory = $true)][string]$Key,
    [Parameter(Mandatory = $true)][string]$Value,
    [string]$Path = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-settings.txt'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Path)) {
    throw "settings file missing: $Path"
}

$bytes = [System.IO.File]::ReadAllBytes($Path)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
$before = ($text -split "`n" | Where-Object { $_ -match ('^\s*' + [regex]::Escape($Key) + '\s*=') }) -join '; '

$pattern = '(?m)^\s*' + [regex]::Escape($Key) + '\s*=.*$'
if ($text -match $pattern) {
    $text = [regex]::Replace($text, $pattern, ($Key + ' = ' + $Value))
}
else {
    $text = $text.TrimEnd() + "`n" + $Key + ' = ' + $Value + "`n"
}
[System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))

$after = ($text -split "`n" | Where-Object { $_ -match ('^\s*' + [regex]::Escape($Key) + '\s*=') }) -join '; '
Write-Host ($Key + ": '" + $before + "' -> '" + $after + "'")
