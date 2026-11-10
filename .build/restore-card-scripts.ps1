# Restore the card's control scripts from the repository's shipping copies.
#
# WHY: the 60 FPS experiments wrote to the card's rr3-settings.txt and
# rr3-screen.txt, and a replacement written by a script can lose a line break
# or append a line to the wrong one.  The repository's copies ARE the shipping
# ones (they are what the packager puts in the release zip), so restoring from
# them is exact and needs no guessing.
#
# Usage: powershell -File .build\restore-card-scripts.ps1
$ErrorActionPreference = 'Stop'

$root = 'E:\AI\RealRacing3-switch'
$src = Join-Path $root 'native-switch\card'
$dst = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'

foreach ($name in @('rr3-settings.txt', 'rr3-screen.txt')) {
    $from = Join-Path $src $name
    $to = Join-Path $dst $name
    if (-not (Test-Path -LiteralPath $from)) {
        Write-Host ("missing in the repository: " + $name)
        continue
    }
    Copy-Item -LiteralPath $from -Destination $to -Force
    $srcHash = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
    $dstHash = (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash
    Write-Host ("{0,-18} restored  sha={1}  {2}" -f $name, $dstHash.Substring(0, 16),
        $(if ($srcHash -eq $dstHash) { 'MATCH' } else { 'MISMATCH' }))
}

Write-Host ''
Write-Host 'the card''s effective lines now:'
foreach ($name in @('rr3-settings.txt', 'rr3-screen.txt')) {
    $path = Join-Path $dst $name
    $active = @(Get-Content -LiteralPath $path | Where-Object { $_ -match '^\s*[A-Za-z]' })
    Write-Host ("  " + $name + ": " + $(if ($active.Count -gt 0) { $active -join ' | ' } else { '(no active overrides)' }))
}
