# Put the ETC-built data volumes on the card, one file at a time.
#
# WHY ONE AT A TIME: the card has about 675 MB free and the volumes are 12 GB
# in total, so the new file cannot exist next to the old one.  Each volume is
# therefore deleted and then replaced, and the peak usage stays where it was.
# The old volumes are not backed up: they are reproducible from the DXT pack
# with tools/prepare_rr3_external_data.py, and keeping a second copy of 12 GB
# on a card that cannot hold it is not an option.
#
# WHY ETC: the guest asks for a texture by SPELLING and walks the list
# pvr -> etc -> dxt -> atc, taking the first one that exists.  The card's loose
# .depot has the pvr/etc spellings for the 82 cars it carries, and the
# volumes were built from the DXT pack, which carries ONLY the dxt spelling.
# A car the card does not have therefore fell through to dxt, and Mesa's S3TC
# decode zeroes the green channel of that texture -- which is the magenta
# (docs/OPEN-ISSUES.md 9.18 and 9.19).  The ETC pack carries the etc spelling
# for the whole roster, and the etc path is already proven to render correctly
# (the 82 cars on the card).
#
# EVERY FILE IS VERIFIED: the sha256 of each volume is compared with the
# manifest that the rebuild wrote.
param(
    [string]$Package = 'E:\AI\RealRacing3-switch\dist\rr3-native-rehost-complete-etc',
    [string]$Card = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
)

$ErrorActionPreference = 'Stop'

$running = Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue
if ($running) {
    Write-Error ("the emulator is running (pid {0}); the card is in use" -f ($running.Id -join ', '))
    exit 1
}

$dataSource = Join-Path $Package 'switch\rr3-native-rehost\data'
$dataTarget = Join-Path $Card 'data'
if (-not (Test-Path -LiteralPath $dataSource -PathType Container)) {
    Write-Error "the rebuilt package has no data directory: $dataSource"
    exit 1
}
if (-not (Test-Path -LiteralPath $dataTarget -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $dataTarget | Out-Null
}

$manifestPath = Join-Path $dataSource 'rr3-data-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

$volumes = @()
foreach ($name in $manifest.volumes.PSObject.Properties.Name) {
    $record = $manifest.volumes.$name
    $volumes += [pscustomobject]@{
        Name = $name
        File = $record.file
        Sha256 = $record.sha256
        Bytes = $record.romfs_bytes
    }
}
# The manifest itself, plus the program entry, are copied after the images.
$volumes += [pscustomobject]@{
    Name = 'manifest'
    File = 'rr3-data-manifest.json'
    Sha256 = ''
    Bytes = 0
}

foreach ($volume in $volumes) {
    $src = Join-Path $dataSource $volume.File
    $dst = Join-Path $dataTarget $volume.File
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        Write-Error "the rebuilt package is missing $($volume.File)"
        exit 1
    }
    $srcBytes = (Get-Item -LiteralPath $src).Length
    if ($volume.Bytes -gt 0 -and $srcBytes -ne $volume.Bytes) {
        Write-Error ("{0}: the rebuilt file is {1} bytes, the manifest says {2}" -f $volume.File, $srcBytes, $volume.Bytes)
        exit 1
    }
    if (Test-Path -LiteralPath $dst -PathType Leaf) {
        Write-Host ("replace  : {0,-28} {1,14:N0} bytes (old removed first: the card has no room for both)" -f $volume.File, $srcBytes)
        Remove-Item -LiteralPath $dst -Force
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $dstBytes = (Get-Item -LiteralPath $dst).Length
    if ($dstBytes -ne $srcBytes) {
        Write-Error ("{0}: copied {1} bytes, expected {2}" -f $volume.File, $dstBytes, $srcBytes)
        exit 1
    }
    if ($volume.Sha256 -ne '') {
        $got = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
        if ($got -ne $volume.Sha256) {
            Write-Error ("{0}: sha256 {1}, the manifest says {2}" -f $volume.File, $got, $volume.Sha256)
            exit 1
        }
        Write-Host ("verified : {0,-28} {1,14:N0} bytes  sha256={2}" -f $volume.File, $dstBytes, $got.Substring(0, 16))
    }
    else {
        Write-Host ("copied   : {0,-28} {1,14:N0} bytes" -f $volume.File, $dstBytes)
    }
}

Write-Host ''
Write-Host 'The card now carries the ETC volumes.  The guest will ask for the etc'
Write-Host 'spelling of every texture and find it, so no texture falls through to'
Write-Host 'the DXT path that Mesa decodes wrongly.'
