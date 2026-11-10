# The APP-ONLY package for a given date: the port, the romfs, the control
# scripts, the console configuration and the README.
#
# WHY A SCRIPT AND NOT A ONE-OFF: the package that went to the console on
# 2026-11-10 carried the NRO from 01:23, and the port has been rebuilt and
# measured four times since (the race-state instrument fix, the pursuit's
# control law and two rounds that measured them).  A console test with a stale
# NRO measures the stale NRO, and the package's own bytes have to be
# reproducible, not remembered.
#
# WHAT IS IN IT, AND WHERE EACH PIECE COMES FROM:
#
#   the NRO and the romfs   the CURRENT build (native-switch\), so the package
#                           always carries the build that was gated;
#   the eight control files native-switch\card\, which are the SHIPPING
#                           versions -- the console's rr3-pad.txt has NO events,
#                           because the player's own controller drives the car;
#   rr3-gfx.conf            the card's, because the card is the state that was
#                           actually MEASURED;
#   the two install scripts and the README from tools\.
#
# IT DOES NOT CARRY THE ASSETS: the seven data volumes and the .depot tree are
# the guest's own data, they have not changed, and the console already has
# them.  The full package (12 GiB) is a different script.
#
# Usage: powershell -File .build\pack-app-dated.ps1 -Stamp 20261110
param(
    [string]$Stamp = '20261110'
)

$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\AI\RealRacing3-switch'
$build = Join-Path $projectRoot 'native-switch'
$card = Join-Path $projectRoot 'native-switch\card'
$tools = Join-Path $projectRoot 'tools'
$cardApp = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$cardRoot = 'E:\Eden-rr3\user\sdmc\switch'
$stage = Join-Path $projectRoot (".build\app-stage-{0}" -f $Stamp)
$out = Join-Path $projectRoot ("dist\rr3-switch-app-{0}.zip" -f $Stamp)

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
$appOut = Join-Path $stage 'switch\rr3-native-rehost'
New-Item -ItemType Directory -Force -Path $appOut | Out-Null

# The port itself.
#
# THE NSP IS THE SHIPPABLE ARTIFACT.  It installs as a title and launches
# from the Home menu; the NRO is still in the package because the emulator
# card runs it, and because it carries the RomFS that the NSP cannot (the
# guest's libraries and the UI faces are ALSO on the card, so the port's
# own fallback finds them either way).
$nsp = Join-Path $build 'rr3-native-rehost.nsp'
if (-not (Test-Path -LiteralPath $nsp -PathType Leaf)) {
    throw ("the NSP is not there: {0} -- build the port first" -f $nsp)
}
Copy-Item -LiteralPath $nsp -Destination (Join-Path $stage 'rr3-native-rehost.nsp') -Force
Copy-Item -LiteralPath (Join-Path $build 'rr3-native-rehost.nro') -Destination (Join-Path $appOut 'rr3-native-rehost.nro') -Force
Copy-Item -LiteralPath (Join-Path $build 'rr3.romfs') -Destination (Join-Path $appOut 'rr3.romfs') -Force

# The control scripts, the SHIPPING versions.
foreach ($name in @('rr3-action.txt', 'rr3-drive.txt', 'rr3-gamepad.txt', 'rr3-open.txt',
        'rr3-pad.txt', 'rr3-screen.txt', 'rr3-settings.txt', 'rr3-touch.txt', 'rr3-ui.txt')) {
    Copy-Item -LiteralPath (Join-Path $card $name) -Destination (Join-Path $appOut $name) -Force
}

# The console configuration, from the measured card.
Copy-Item -LiteralPath (Join-Path $cardRoot 'rr3-gfx.conf') -Destination (Join-Path $stage 'switch\rr3-gfx.conf') -Force

# The install scripts and the README.
Copy-Item -LiteralPath (Join-Path $tools 'Copy-To-SwitchCard.ps1') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $tools 'Join-And-Extract.ps1') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $tools 'README-hardware-test.md') -Destination $stage -Force

# The zip.
if (Test-Path -LiteralPath $out) {
    Remove-Item -LiteralPath $out -Force
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $out,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)

$nro = Get-Item -LiteralPath (Join-Path $appOut 'rr3-native-rehost.nro')
$nspInfo = Get-Item -LiteralPath (Join-Path $stage 'rr3-native-rehost.nsp')
Write-Host ("package : {0}" -f $out)
Write-Host ("size    : {0:N1} MiB" -f ((Get-Item -LiteralPath $out).Length / 1MB))
Write-Host ("nsp     : {0} bytes, sha256 {1}" -f $nspInfo.Length,
    (Get-FileHash -LiteralPath $nspInfo.FullName -Algorithm SHA256).Hash.Substring(0, 16))
Write-Host ("nro     : {0} bytes, sha256 {1}" -f $nro.Length,
    (Get-FileHash -LiteralPath $nro.FullName -Algorithm SHA256).Hash.Substring(0, 16))
Write-Host ("build   : {0}" -f (Get-Item -LiteralPath (Join-Path $build 'rr3-native-rehost.nro')).LastWriteTime)
