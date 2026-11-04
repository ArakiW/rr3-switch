# Deploy the CURRENT build's romfs (and NRO) onto the Eden SD card.
#
# WHY THIS EXISTS: tools\Deploy-RR3CardScripts.ps1 only installs the eight
# control scripts and rr3-gfx.conf.  The NRO and the romfs were copied by
# hand, so the card's rr3.romfs was still the 2026-10-04 build (43,882,800
# bytes) while the build output was the 2026-11-02 one (44,826,028 bytes).
# Every Eden round since then ran the NEW NRO against the OLD romfs.
#
# This script installs both and verifies the sha256 of each on the card.

$ErrorActionPreference = 'Stop'

$buildRoot = 'E:\AI\RealRacing3-switch\native-switch'
$cardApp   = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'

$files = @('rr3-native-rehost.nro', 'rr3.romfs')

foreach ($name in $files) {
    $src = Join-Path $buildRoot $name
    $dst = Join-Path $cardApp $name
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        throw "missing build output: $src"
    }
    $before = if (Test-Path -LiteralPath $dst -PathType Leaf) {
        "{0} ({1} bytes)" -f (Get-FileHash $dst -Algorithm SHA256).Hash.ToLower().Substring(0, 16),
                             (Get-Item $dst).Length
    } else {
        'absent'
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $want = (Get-FileHash $src -Algorithm SHA256).Hash.ToLower()
    $got  = (Get-FileHash $dst -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $want) {
        throw "MISMATCH after copy: $name"
    }
    Write-Host ("{0,-24} was {1}  ->  now {2} ({3} bytes)" -f `
        $name, $before, $got.Substring(0, 16), (Get-Item $dst).Length)
}

Write-Host 'card app files now match the build output.'
