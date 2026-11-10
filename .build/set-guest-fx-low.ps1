# Turn the guest's own expensive per-frame work off, in one go.
#
# WHY: the port's own share of the frame is now about a millisecond, and what
# is left is the GUEST's per-frame work (its Java/C++ method bodies inside the
# JNI dispatch, plus its rasterisation).  MEASURED 2026-11-10: the anti-
# aliasing level is NOT that cost (AA_LEVEL 4 -> 0 did not make the frame
# faster: tt median 72 -> 83 ms).  So the next candidates are the guest's
# other quality knobs, and the efficient way to find them is a big hammer
# first: turn them ALL off, and if the frame time moves, bisect.
#
# THE KEYS ARE THE GUEST'S OWN (rr3-settings.txt documents them as the
# per-profile overrides): the full-screen pass, the particles, the dynamic
# reflections, the shadow maps, the interior LOD, the damage, the anisotropic
# filtering, the high-res menu, and the mipmap levels to skip.
#
# Usage: powershell -File .build\set-guest-fx-low.ps1
$ErrorActionPreference = 'Stop'

$settings = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-settings.txt'
$off = @(
    'USE_FULL_SCREEN_EFFECTS',
    'USE_PARTICLE_EFFECTS',
    'DYNAMIC_CAR_REFLECTIONS',
    'EXTERIOR_SHADOWS_MENU',
    'EXTERIOR_SHADOWS_OPPONENT',
    'EXTERIOR_SHADOWS_PLAYER',
    'CAR_SHADOW_MAPS',
    'CAR_INTERIOR_SHADOW_MAPS',
    'CAR_INTERIOR_LOD',
    'FULL_OPPONENT_DAMAGE',
    'USE_ANISOTROPIC_FILTERING',
    'HIGH_RES_MENU'
)

$text = [System.IO.File]::ReadAllText($settings, [System.Text.Encoding]::UTF8)
foreach ($key in $off) {
    $pattern = '(?m)^\s*' + [regex]::Escape($key) + '\s*=.*$'
    if ($text -match $pattern) {
        $text = [regex]::Replace($text, $pattern, ($key + ' = false'))
    }
    else {
        $text = $text.TrimEnd() + "`n" + $key + ' = false' + "`n"
    }
}
[System.IO.File]::WriteAllText($settings, $text, (New-Object System.Text.UTF8Encoding($false)))

$after = ($text -split "`n" | Where-Object { $_ -match '^\s*[A-Z_]+\s*=\s*(false|true|\d+)\s*$' })
Write-Host ("the guest's quality overrides now:" + "`n  " + ($after -join "`n  "))
