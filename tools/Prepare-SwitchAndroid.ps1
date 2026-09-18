[CmdletBinding()]
param(
    [ValidateSet('DXT', 'ETC', 'ATC')]
    [string]$TextureFormat = 'DXT',

    [Parameter(Mandatory)]
    [string]$Destination,

    [switch]$Extract
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$archive = Get-ChildItem $projectRoot -Filter "$TextureFormat`_*.zip" | Select-Object -First 1

if (-not $archive) {
    throw "No $TextureFormat data archive found in $projectRoot"
}

$destinationFull = [IO.Path]::GetFullPath($Destination)
$destinationRoot = [IO.Path]::GetPathRoot($destinationFull)
if ($destinationFull -eq $destinationRoot) {
    throw 'Destination must be a dedicated staging directory, not a drive root.'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive.FullName)
try {
    $uncompressedBytes = ($zip.Entries | Measure-Object Length -Sum).Sum
    $entryCount = $zip.Entries.Count
    if (-not $zip.GetEntry('files/.depot/asset_packs.cfg')) {
        throw 'The archive does not contain files/.depot/asset_packs.cfg.'
    }

    $driveName = (Split-Path -Qualifier $destinationFull).TrimEnd(':')
    $drive = Get-PSDrive -Name $driveName -ErrorAction Stop
    $requiredBytes = [int64]($uncompressedBytes * 1.15)
    if ($drive.Free -lt $requiredBytes) {
        throw "Insufficient free space. Need at least $([math]::Round($requiredBytes / 1GB, 1)) GiB."
    }

    Write-Host "Archive: $($archive.Name)"
    Write-Host "Entries: $entryCount"
    Write-Host "Uncompressed: $([math]::Round($uncompressedBytes / 1GB, 3)) GiB"
    Write-Host "Destination: $destinationFull"

    if (-not $Extract) {
        Write-Host 'Validation only. Re-run with -Extract to create the staging directory.' -ForegroundColor Yellow
        return
    }

    if (Test-Path $destinationFull) {
        $existing = Get-ChildItem $destinationFull -Force -ErrorAction Stop | Select-Object -First 1
        if ($existing) {
            throw 'Destination already exists and is not empty. Choose a new staging directory.'
        }
    }
    else {
        New-Item -ItemType Directory -Path $destinationFull | Out-Null
    }

    [IO.Compression.ZipFileExtensions]::ExtractToDirectory($zip, $destinationFull)
}
finally {
    $zip.Dispose()
}

$depotPath = Join-Path $destinationFull 'files\.depot'
if (-not (Test-Path $depotPath -PathType Container)) {
    throw 'Extraction finished but the depot directory is missing.'
}

$fileCount = (Get-ChildItem $destinationFull -File -Recurse | Measure-Object).Count
Write-Host "Prepared $fileCount files in $destinationFull" -ForegroundColor Green

