# Put the guest's settings back the way the card shipped them.
#
# WHY: the 60 FPS experiments turned the guest's quality knobs off (the big
# hammer in set-guest-fx-low.ps1) and the port's view down to 960x640.  Both
# were MEASURED and neither paid: AA_LEVEL 4 -> 0 did not make the frame
# faster (tt median 72 -> 83 ms), the twelve quality knobs off did not either
# (72 -> 70 ms), and halving the resolution did not either (72 -> 77 ms).
# See docs/OPEN-ISSUES.md section 9.40.
#
# So the shipping state is the original one: the guest's profile defaults
# (AndroidHigh) and the port's 1280x720 view.
#
# Usage: powershell -File .build\restore-guest-settings.ps1
$ErrorActionPreference = 'Stop'

$settings = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-settings.txt'
$screen = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-screen.txt'

# 1. The quality overrides go back to being COMMENTS, which is how the card
#    shipped them: the guest then uses its own profile's values.
$text = [System.IO.File]::ReadAllText($settings, [System.Text.Encoding]::UTF8)
$text = [regex]::Replace($text, '(?m)^(\s*)(USE_FULL_SCREEN_EFFECTS|USE_PARTICLE_EFFECTS|DYNAMIC_CAR_REFLECTIONS|EXTERIOR_SHADOWS_MENU|EXTERIOR_SHADOWS_OPPONENT|EXTERIOR_SHADOWS_PLAYER|CAR_SHADOW_MAPS|CAR_INTERIOR_SHADOW_MAPS|CAR_INTERIOR_LOD|FULL_OPPONENT_DAMAGE|USE_ANISOTROPIC_FILTERING|HIGH_RES_MENU)(\s*=\s*false\s*)$', '#$1$2 = false')
$text = [regex]::Replace($text, '(?m)^(\s*AA_LEVEL\s*=\s*)(\d+)\s*$', '$1$2')
[System.IO.File]::WriteAllText($settings, $text, (New-Object System.Text.UTF8Encoding($false)))

# 2. The view goes back to 1280x720.
$stext = [System.IO.File]::ReadAllText($screen, [System.Text.Encoding]::UTF8)
$stext = [regex]::Replace($stext, '(?m)^(\s*view\s*=\s*).*$', '$11280x720')
[System.IO.File]::WriteAllText($screen, $stext, (New-Object System.Text.UTF8Encoding($false)))

Write-Host 'guest settings restored:'
Write-Host ('  ' + ((([System.IO.File]::ReadAllText($settings, [System.Text.Encoding]::UTF8)) -split "`n") | Where-Object { $_ -match '^\s*AA_LEVEL' }) -join '')
Write-Host ('  ' + ((([System.IO.File]::ReadAllText($screen, [System.Text.Encoding]::UTF8)) -split "`n") | Where-Object { $_ -match '^\s*view\s*=' }) -join '')
