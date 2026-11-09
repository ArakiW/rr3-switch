# Assemble the HYBRID volume set: the front end from the DXT pack, the vehicle
# textures from the ETC pack.
#
# WHY A HYBRID, AND WHAT THE PURE ETC REBUILD MEASURED
# ------------------------------------------------------
# The pure ETC rebuild (seven volumes from the ETC pack) does NOT work: round
# etcv1 reported "front-end layout opened 0", "main menu layouts open 0",
# "EventMapScreen 0", "start lights 0" and "race screen 0", while the same
# build on the DXT volumes (round dxtab0) reported 4 / 20 / 193 / 7 / 10 --
# the game reached the race.  The packs are NOT interchangeable: the ETC pack
# carries 12,305 gui_assets entries against the DXT pack's 12,653, and the
# front end cannot be built without them.
#
# BUT the vehicle textures are where the magenta lives, and the guest asks for
# a texture by SPELLING -- pvr, then etc, then dxt, then atc -- and takes the
# first one that exists.  The card's loose .depot has the etc spelling for the
# 82 cars it carries (and those cars render correctly), while the volumes
# carry only the dxt spelling, so every other car falls through to dxt --
# and Mesa's S3TC decode zeroes the green channel of that texture
# (docs/OPEN-ISSUES.md 9.18).
#
# So: the volumes that the FRONT END reads stay on the DXT pack, and the
# volumes that the CARS read are rebuilt from the ETC pack.  Every volume is
# taken from a build that produced it, so the manifest's sha256 for each one is
# the one the build recorded, and the merge re-verifies it.
#
# The vehicle volumes contain the car's MESHES as well as its textures, and
# the meshes are the same bytes in both packs (the compression format of the
# textures is what differs), so swapping the whole volume keeps the geometry
# and changes the texture format.
param(
    [string]$DxtPackage = 'E:\AI\RealRacing3-switch\dist\rr3-native-rehost-complete-dxt',
    [string]$EtcPackage = 'E:\AI\RealRacing3-switch\dist\rr3-native-rehost-complete-etc',
    [string]$OutPackage = 'E:\AI\RealRacing3-switch\dist\rr3-native-rehost-complete-hybrid'
)

$ErrorActionPreference = 'Stop'

$dxtData = Join-Path $DxtPackage 'switch\rr3-native-rehost\data'
$etcData = Join-Path $EtcPackage 'switch\rr3-native-rehost\data'
foreach ($dir in @($dxtData, $etcData)) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        Write-Error "missing a build's data directory: $dir"
        exit 1
    }
}

# THE HYBRID IS A NEW DIRECTORY.  The pure DXT build stays intact, because it is
# the fallback: the ETC-only volumes were measured to break the front end, and
# the card must be able to go back to a set that boots.
$outData = Join-Path $OutPackage 'switch\rr3-native-rehost\data'
New-Item -ItemType Directory -Force -Path $outData | Out-Null

$dxtManifest = Get-Content -LiteralPath (Join-Path $dxtData 'rr3-data-manifest.json') -Raw | ConvertFrom-Json
$etcManifest = Get-Content -LiteralPath (Join-Path $etcData 'rr3-data-manifest.json') -Raw | ConvertFrom-Json

# THE FOUR VOLUMES THE FRONT END READS COME FROM THE DXT PACK.
$fromDxt = @('core', 'gui-hd', 'gui-sd', 'tracks')
# THE THREE VOLUMES THE CARS READ COME FROM THE ETC PACK.
$fromEtc = @('vehicles-classic', 'vehicles-mid', 'vehicles-modern')

foreach ($name in $fromDxt) {
    $record = $dxtManifest.volumes.$name
    $src = Join-Path $dxtData $record.file
    $dst = Join-Path $outData $record.file
    Write-Host ("copy      : {0,-24} <- DXT ({1:N0} bytes)" -f $record.file, $record.romfs_bytes)
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $got = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
    if ($got -ne $record.sha256) {
        Write-Error ("{0}: sha256 {1}, the DXT build says {2}" -f $record.file, $got, $record.sha256)
        exit 1
    }
    Write-Host ("verified : {0,-24} sha256={1}" -f $record.file, $got.Substring(0, 16))
}

foreach ($name in $fromEtc) {
    $record = $etcManifest.volumes.$name
    $src = Join-Path $etcData $record.file
    $dst = Join-Path $outData $record.file
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        Write-Error "the ETC build is missing $($record.file)"
        exit 1
    }
    Write-Host ("copy      : {0,-24} <- ETC ({1:N0} bytes)" -f $record.file, $record.romfs_bytes)
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $got = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
    if ($got -ne $record.sha256) {
        Write-Error ("{0}: sha256 {1}, the ETC build says {2}" -f $record.file, $got, $record.sha256)
        exit 1
    }
    Write-Host ("verified : {0,-24} sha256={1}" -f $record.file, $got.Substring(0, 16))
    # The manifest has to describe what is actually on the card.
    $dxtManifest.volumes.$name.file = $record.file
    $dxtManifest.volumes.$name.files = $record.files
    $dxtManifest.volumes.$name.source_bytes = $record.source_bytes
    $dxtManifest.volumes.$name.romfs_bytes = $record.romfs_bytes
    $dxtManifest.volumes.$name.sha256 = $record.sha256
    $dxtManifest.volumes.$name.sentinel = $record.sentinel
}

$dxtManifest.source_archive = 'HYBRID: front end from the DXT pack, vehicle textures from the ETC pack'
$dxtManifest.source_sha256 = '{dxt=' + $dxtManifest.source_sha256 + ';etc=' + $etcManifest.source_sha256 + '}'
$manifestPath = Join-Path $outData 'rr3-data-manifest.json'
$dxtManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host ''
Write-Host ("manifest  : {0} (front end from DXT, vehicles from ETC)" -f $manifestPath)
foreach ($name in $fromDxt) {
    $record = $dxtManifest.volumes.$name
    $file = Join-Path $outData $record.file
    $got = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($got -ne $record.sha256) {
        Write-Error ("{0}: sha256 {1}, the manifest says {2}" -f $record.file, $got, $record.sha256)
        exit 1
    }
    Write-Host ("verified : {0,-24} sha256={1}" -f $record.file, $got.Substring(0, 16))
}
Write-Host ''
Write-Host 'The hybrid data directory is ready.  Put it on the card with the'
Write-Host 'deploy script and run a round: the front end has to come up AND the'
Write-Host 'vehicle textures have to be uploaded as ETC1.'
