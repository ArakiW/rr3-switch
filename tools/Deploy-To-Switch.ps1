<#
.SYNOPSIS
    Stage the port onto a real Switch SD card.

.DESCRIPTION
    The repository has no Switch deployment path. Stage-NativeSwitch.ps1 copies
    into dist/, Install-SwitchAndroid.ps1 and Deploy-RR3Data.ps1 both drive adb
    (they target an Android device, not a console), and dist/ itself is stale:
    dist\rr3-native-rehost-ui-0.18.3\ holds a 41.5 MB NRO from the 0.18.3 era
    while the current build is ~52 MB. Deploying from dist/ silently runs a
    build from weeks ago.

    So this script takes the three things a console actually needs, and refuses
    to run if any of them is wrong:

      1. the NRO straight out of native-switch\ (never dist\)
      2. the data volumes, ~9 GiB, which must already be on the card under
         sdmc:/switch/rr3-native-rehost/data/
      3. NO rr3-gfx.conf -- its absence is what selects Mesa (nouveau/DRM,
         NVN on Maxwell). A leftover backend=null forces the stub backend,
         which answers every GL call from a table and never draws: the game
         would sit on a black screen forever with the real path unused.

.PARAMETER Card
    Path to the SD card's root -- the folder that CONTAINS `switch\`. The Switch
    homebrew loader shows that path in the title bar when the game is launched.

.PARAMETER SkipData
    Only copy the NRO and the RomFS. Use when the data volumes are already on
    the card; the script still verifies they exist unless -SkipData is given.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\Deploy-To-Switch.ps1 -Card F:\
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Card,
    [switch]$SkipData
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$nro = Join-Path $projectRoot 'native-switch\rr3-native-rehost.nro'
$romfs = Join-Path $projectRoot 'native-switch\rr3.romfs'
$dist = Join-Path $projectRoot 'dist'

$switchDir = Join-Path $Card 'switch'
$appDir = Join-Path $switchDir 'rr3-native-rehost'
$dataDir = Join-Path $appDir 'data'

# ---------------------------------------------------------------- preflight --
$fail = @()
if (-not (Test-Path -LiteralPath $nro -PathType Leaf)) {
    $fail += "NRO missing: $nro  (run tools\Build-NativeSwitch.ps1)"
}
if (-not (Test-Path -LiteralPath $romfs -PathType Leaf)) {
    $fail += "RomFS image missing: $romfs  (same build script produces it)"
}
if (-not (Test-Path -LiteralPath $Card -PathType Container)) {
    $fail += "Card not found: $Card"
}
if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Error $_ }
    exit 1
}

# Catch the dist/ trap explicitly rather than letting it happen silently.
# Reported ONCE against the newest stale build: dist/ accumulates one NRO per UI
# version, so a per-file warning repeats a dozen times and buries everything
# else this script has to say.
$currentLen = (Get-Item $nro).Length
$stale = @(Get-ChildItem (Join-Path $dist 'rr3-native-rehost-ui-*') -Recurse -Filter '*.nro' -ErrorAction SilentlyContinue)
$odd = @($stale | Where-Object { $_.Length -ne $currentLen })
if ($odd.Count -gt 0) {
    $newest = $odd | Sort-Object Length -Descending | Select-Object -First 1
    Write-Warning ("dist/ holds {0} older build(s); newest is {1:N0} B vs current {2:N0} B." -f `
        $odd.Count, $newest.Length, $currentLen)
    Write-Warning "Staging from native-switch\, not dist\."
}

New-Item -ItemType Directory -Force -Path $appDir | Out-Null

# ------------------------------------------------------------- 1. NRO + romfs --
Copy-Item -LiteralPath $nro -Destination (Join-Path $appDir 'rr3-native-rehost.nro') -Force
Write-Host ("staged NRO    : {0:N0} bytes" -f (Get-Item $nro).Length)
Copy-Item -LiteralPath $romfs -Destination (Join-Path $appDir 'rr3.romfs') -Force
Write-Host ("staged RomFS  : {0:N0} bytes" -f (Get-Item $romfs).Length)

# ---------------------------------------------------------------- 2. volumes --
if ($SkipData) {
    Write-Host "data volumes : SKIPPED by request"
}
else {
    $vols = @(Get-ChildItem -LiteralPath $dataDir -Filter '*.romfs' -ErrorAction SilentlyContinue)
    if ($vols.Count -eq 0) {
        Write-Error ("no *.romfs under {0} -- the game has no assets to load." -f $dataDir)
        Write-Error ("the seven volumes are ~9.0 GiB; copy them from a prepared card first.")
        exit 1
    }
    $sum = ($vols | Measure-Object Length -Sum).Sum
    Write-Host ("data volumes : {0} files, {1:N1} GiB already on the card" -f $vols.Count, ($sum / 1GB))
    $want = @('core.romfs', 'gui-hd.romfs', 'gui-sd.romfs', 'tracks.romfs',
        'vehicles-classic.romfs', 'vehicles-mid.romfs', 'vehicles-modern.romfs')
    $have = $vols.Name
    $absent = $want | Where-Object { $_ -notin $have }
    if ($absent) {
        Write-Warning ("missing volumes: {0}" -f ($absent -join ', '))
    }
}

# ------------------------------------------------------------------ 3. conf --
$conf = Join-Path $switchDir 'rr3-gfx.conf'
if (Test-Path -LiteralPath $conf -PathType Leaf) {
    Remove-Item -LiteralPath $conf -Force
    Write-Host "removed      : rr3-gfx.conf  (Mesa is now the active backend)"
}
else {
    Write-Host "confirmed    : no rr3-gfx.conf  -> Mesa (nouveau/DRM), the real path"
}

Write-Host ""
Write-Host "Staged. Launch the NRO from the title-bar path on the console:"
Write-Host ("  {0}" -f $appDir)
Write-Host "Expect: main constructors, JNI lifecycle, then Created/Changed/Render."
Write-Host "If the screen stays black, capture sdmc:/switch/rr3-native-rehost/rr3-native-rehost.guest-view.log"
Write-Host "and report it -- unlike Eden, a console run has a working GL path, so a"
Write-Host "guest-side stall there is evidence rather than an emulator artifact."
