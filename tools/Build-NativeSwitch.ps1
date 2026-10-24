[CmdletBinding()]
param(
    [ValidateRange(1, 32)]
    [int]$Jobs = 8,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$nativeRoot = Join-Path $projectRoot 'native-switch'
$gameLibrary = Join-Path $projectRoot 'reverse\native\arm64\libRealRacing3.so'
$romfsRoot = Join-Path $nativeRoot 'romfs'
$embeddedLibrary = Join-Path $romfsRoot 'libRealRacing3.so'
$originalDepotRoot = Join-Path $projectRoot 'analysis\rr3-ui-source\files\.depot'
$dependencyNames = @(
    'libNimble.so'
    'libfmodex.so'
    'libc++_shared.so'
)
$allManagedDependencyNames = @(
    'libNimble.so'
    'libfmodex.so'
    'libarcore_sdk_c.so'
    'libarcore_sdk_jni.so'
    'libc++_shared.so'
    'libfuelmetrics.so'
)

if (-not (Test-Path $gameLibrary -PathType Leaf)) {
    throw "Missing game library: $gameLibrary"
}
New-Item -ItemType Directory -Force -Path $romfsRoot | Out-Null
Copy-Item -LiteralPath $gameLibrary -Destination $embeddedLibrary -Force
foreach ($managedName in $allManagedDependencyNames) {
    if ($managedName -notin $dependencyNames) {
        $staleEmbeddedDependency = Join-Path $romfsRoot $managedName
        if (Test-Path $staleEmbeddedDependency -PathType Leaf) {
            Remove-Item -LiteralPath $staleEmbeddedDependency -Force
        }
    }
}
foreach ($dependencyName in $dependencyNames) {
    $dependency = Join-Path $projectRoot "reverse\native\arm64\$dependencyName"
    if (-not (Test-Path $dependency -PathType Leaf)) {
        throw "Missing embedded dependency: $dependency"
    }
    Copy-Item -LiteralPath $dependency -Destination (Join-Path $romfsRoot $dependencyName) -Force
}

# Keep the package's own typography, Simplified Chinese catalog, and authored
# animation tracks in their original relative layout. The bootstrap shell uses
# the original Latin faces immediately; the rehosted game receives the original
# localization and .banim data unchanged once its content VFS is active.
#
# THE GAME ASKS FOR FOURTEEN FACES BY NAME (measured from the guest's own
# log), and the guest's Java measures the UI text with the face it asked
# for. So the port has to measure it with the SAME face: a different face
# has different per-glyph advances, and the laid-out paragraph stops being
# the paragraph the guest asked for.
#
# The two Conduit faces are REQUIRED -- they are the port's universal
# fallback for every face it does not have. The rest are copied when they
# are staged, and their absence is REPORTED rather than fatal, because the
# port then draws that text with the fallback, which is exactly what it did
# before, and the guest log now says which face it really used.
#
# tools\stage_rr3_fonts.py puts the game's own faces in
# analysis\rr3-ui-source\files\.depot (an EA asset area, excluded by
# .gitignore); this script only copies them out of it.
$requiredFontAssets = @(
    'ConduitITCPro-Regular.otf'
    'ConduitITCPro-Bold.otf'
)
$optionalFontAssets = @(
    'ConduitITCPro-Medium.otf'
    'ConduitITCPro-ExtraBold.otf'
    'EurostileLTStd.otf'
    'EurostileLTStd-Bold.otf'
    'EurostileLTStd-BoldEx2.otf'
    'EurostileLTStd-BoldOblique.otf'
    'EurostileLTStd-Demi.otf'
    'EurostileLTStd-DemiOblique.otf'
    'EurostileLTStd-Ex2.otf'
    'HelveticaNeueBold.ttf'
    'LCD.ttf'
    'minion_pro_italic.otf'
    'myriadp0.otf'
)
$fontDestination = Join-Path $romfsRoot 'ui'
New-Item -ItemType Directory -Force -Path $fontDestination | Out-Null
foreach ($assetName in $requiredFontAssets) {
    $assetSource = Join-Path $originalDepotRoot $assetName
    if (-not (Test-Path $assetSource -PathType Leaf)) {
        throw "Missing original UI font: $assetSource"
    }
    Copy-Item -LiteralPath $assetSource -Destination (Join-Path $fontDestination $assetName) -Force
}
$missingOptionalFonts = @()
foreach ($assetName in $optionalFontAssets) {
    $assetSource = Join-Path $originalDepotRoot $assetName
    if (-not (Test-Path $assetSource -PathType Leaf)) {
        $missingOptionalFonts += $assetName
        continue
    }
    Copy-Item -LiteralPath $assetSource -Destination (Join-Path $fontDestination $assetName) -Force
}
if ($missingOptionalFonts.Count -gt 0) {
    Write-Host ("font     : {0} of the game's own faces are NOT staged, so text that asks for them will be drawn with the Conduit fallback: {1}" -f `
        $missingOptionalFonts.Count, ($missingOptionalFonts -join ', '))
    Write-Host ("           stage them with: python tools\stage_rr3_fonts.py <data-pack.zip>")
}
$bootstrapChineseFont = Join-Path $fontDestination 'NotoSansCJKsc-RR3Bootstrap.otf'
$bootstrapFontLicense = Join-Path $romfsRoot 'licenses\Noto-CJK-OFL.txt'
if (-not (Test-Path $bootstrapChineseFont -PathType Leaf)) {
    throw "Missing bootstrap Simplified Chinese font: $bootstrapChineseFont"
}
if (-not (Test-Path $bootstrapFontLicense -PathType Leaf)) {
    throw "Missing bootstrap font license: $bootstrapFontLicense"
}

$originalAssets = @(
    'langs.utf8'
    'text_cn-Hans.txt'
    'text_en.txt'
    'MenuTransitions.xml'
    'Tracks\Menu\processed\menu_intro.banim'
    'Tracks\Menu\processed\menu_transition.banim'
    'Tracks\Menu\processed\menu_carselect_transition.banim'
    'Tracks\Menu\processed\menu_carselect_timetrial_transition.banim'
    'Tracks\Menu\processed\menu_awards.banim'
    'Tracks\Menu\processed\menu_caravailable.banim'
    'Tracks\Menu\processed\menu_carpurchased.banim'
    'Tracks\Menu\processed\menu_carunlock.banim'
    'Tracks\Menu\processed\garage_mycars.banim'
)
$originalDestination = Join-Path $romfsRoot 'original\.depot'
foreach ($relativeAsset in $originalAssets) {
    $assetSource = Join-Path $originalDepotRoot $relativeAsset
    if (-not (Test-Path $assetSource -PathType Leaf)) {
        throw "Missing original UI package asset: $assetSource"
    }
    $assetDestination = Join-Path $originalDestination $relativeAsset
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $assetDestination) | Out-Null
    Copy-Item -LiteralPath $assetSource -Destination $assetDestination -Force
}

$localMsys = Join-Path $projectRoot '.tools\devkitPro\msys64'
$localBash = Join-Path $localMsys 'usr\bin\bash.exe'
$localCygpath = Join-Path $localMsys 'usr\bin\cygpath.exe'
$localRomfsBuilder = Join-Path $localMsys 'opt\devkitpro\tools\bin\build_romfs.exe'
$romfsImage = Join-Path $nativeRoot 'rr3.romfs'

if (Test-Path $localBash -PathType Leaf) {
    if (-not (Test-Path $localRomfsBuilder -PathType Leaf)) {
        throw "build_romfs was not found: $localRomfsBuilder"
    }
    & $localRomfsBuilder $romfsRoot $romfsImage
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $romfsImage -PathType Leaf)) {
        throw 'RomFS image build failed.'
    }
    $msysNativeRoot = (& $localCygpath -u $nativeRoot).Trim()
    $actions = @('source /etc/profile.d/devkit-env.sh')
    if ($Clean) {
        $actions += "make -C '$msysNativeRoot' clean"
    }
    $actions += "make -C '$msysNativeRoot' -j$Jobs"
    & $localBash -lc ($actions -join '; ')
    if ($LASTEXITCODE -ne 0) {
        throw 'Native Switch build failed in the project-local toolchain.'
    }
}
else {
    if (-not $env:DEVKITPRO) {
        throw 'Neither the project-local toolchain nor DEVKITPRO was found.'
    }
    $make = Get-Command make -ErrorAction SilentlyContinue
    if (-not $make) {
        throw 'make was not found.'
    }
    $romfsBuilder = Join-Path $env:DEVKITPRO 'tools\bin\build_romfs.exe'
    if (-not (Test-Path $romfsBuilder -PathType Leaf)) {
        throw 'build_romfs was not found in DEVKITPRO.'
    }
    & $romfsBuilder $romfsRoot $romfsImage
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $romfsImage -PathType Leaf)) {
        throw 'RomFS image build failed.'
    }
    if ($Clean) {
        & $make.Source -C $nativeRoot clean
    }
    & $make.Source -C $nativeRoot "-j$Jobs"
    if ($LASTEXITCODE -ne 0) {
        throw 'Native Switch build failed.'
    }
}

$nro = Join-Path $nativeRoot 'rr3-native-rehost.nro'
if (-not (Test-Path $nro -PathType Leaf)) {
    throw 'Build returned success but the NRO was not created.'
}

Write-Host "Built: $nro" -ForegroundColor Green
