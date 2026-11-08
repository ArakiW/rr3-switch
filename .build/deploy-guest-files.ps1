# Put the guest's own files next to the NRO, the way a user would from their
# own copy of the application.
#
# WHY: the CLEAN build ships no EA material -- the RomFS holds only the OFL
# bootstrap face, libc++ and the font licence.  The guest's own files have to
# be found NEXT TO THE NRO on the card, so this is what makes the clean
# loader testable at all.
#
# It copies:
#   the three guest DSOs            -> sdmc:/switch/rr3-native-rehost/
#   the game's UI faces             -> sdmc:/switch/rr3-native-rehost/ui/
#   the .depot files                -> sdmc:/switch/rr3-native-rehost/original/.depot/
#
# The .depot and the faces are the ones the build script would have staged
# into the RomFS, so this reproduces exactly what a clean build omits.
#
# Usage: powershell -File .build\deploy-guest-files.ps1
param(
    [string]$CardApp = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
)

$ErrorActionPreference = 'Stop'
$root = 'E:\AI\RealRacing3-switch'

$dsos = @(
    (Join-Path $root 'reverse\native\arm64\libRealRacing3.so'),
    (Join-Path $root 'reverse\native\arm64\libNimble.so'),
    (Join-Path $root 'reverse\native\arm64\libfmodex.so')
)

Write-Host '=== the guest DSOs ==='
foreach ($src in $dsos) {
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        throw "missing: $src"
    }
    $dst = Join-Path $CardApp (Split-Path $src -Leaf)
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Host ("  {0,-22} {1,12:N0} bytes" -f (Split-Path $src -Leaf), (Get-Item -LiteralPath $dst).Length)
}

Write-Host ''
Write-Host '=== the UI faces ==='
# The game's faces live in the .depot area (tools\stage_rr3_fonts.py puts them
# there); the build script copies them OUT of it into the RomFS' ui\ folder.
# The clean loader wants them next to the NRO instead, in that same ui\ shape.
$faceSrc = Join-Path $root 'analysis\rr3-ui-source\files\.depot'
$faceDst = Join-Path $CardApp 'ui'
$faceNames = @(
    'ConduitITCPro-Regular.otf', 'ConduitITCPro-Bold.otf',
    'ConduitITCPro-Medium.otf', 'ConduitITCPro-ExtraBold.otf',
    'EurostileLTStd.otf', 'EurostileLTStd-Bold.otf',
    'EurostileLTStd-BoldEx2.otf', 'EurostileLTStd-BoldOblique.otf',
    'EurostileLTStd-Demi.otf', 'EurostileLTStd-DemiOblique.otf',
    'EurostileLTStd-Ex2.otf', 'HelveticaNeueBold.ttf', 'LCD.ttf',
    'minion_pro_italic.otf', 'myriadp0.otf', 'r3_symbols.ttf'
)
New-Item -ItemType Directory -Force -Path $faceDst | Out-Null
$faceCount = 0
foreach ($name in $faceNames) {
    $src = Join-Path $faceSrc $name
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $faceDst $name) -Force
        $faceCount++
    }
}
Write-Host ("  {0} face(s) -> {1}" -f $faceCount, $faceDst)
if ($faceCount -lt 2) {
    throw ("only {0} face(s) found; the two Conduit faces are required" -f $faceCount)
}

Write-Host ''
Write-Host '=== the .depot files ==='
$depotSrc = Join-Path $root 'analysis\rr3-ui-source\files\.depot'
$depotDst = Join-Path $CardApp 'original\.depot'
if (-not (Test-Path -LiteralPath $depotSrc -PathType Container)) {
    throw "missing: $depotSrc"
}
New-Item -ItemType Directory -Force -Path $depotDst | Out-Null
$copied = 0
$bytes = 0
Get-ChildItem -LiteralPath $depotSrc -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($depotSrc.Length + 1)
    $dst = Join-Path $depotDst $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
    $copied++
    $bytes += $_.Length
}
Write-Host ("  {0} file(s), {1:N0} bytes -> {2}" -f $copied, $bytes, $depotDst)

Write-Host ''
Write-Host 'the card now has the layout the clean loader expects.'
