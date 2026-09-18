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
$fontAssets = @(
    'ConduitITCPro-Regular.otf'
    'ConduitITCPro-Bold.otf'
    'EurostileLTStd-Bold.otf'
)
$fontDestination = Join-Path $romfsRoot 'ui'
New-Item -ItemType Directory -Force -Path $fontDestination | Out-Null
foreach ($assetName in $fontAssets) {
    $assetSource = Join-Path $originalDepotRoot $assetName
    if (-not (Test-Path $assetSource -PathType Leaf)) {
        throw "Missing original UI font: $assetSource"
    }
    Copy-Item -LiteralPath $assetSource -Destination (Join-Path $fontDestination $assetName) -Force
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
