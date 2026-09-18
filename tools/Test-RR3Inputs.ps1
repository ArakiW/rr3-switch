[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$expectedPackage = 'com.ea.games.r3_row'
$aapt2Candidates = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\build-tools" -Filter aapt2.exe -Recurse -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending
$aapt2 = $aapt2Candidates | Select-Object -First 1 -ExpandProperty FullName
$apk = Get-ChildItem $projectRoot -Filter *.apk | Select-Object -First 1
$archives = Get-ChildItem $projectRoot -Filter *.zip | Sort-Object Name

if (-not $apk) {
    throw "No APK found in $projectRoot"
}
if ($archives.Count -eq 0) {
    throw "No data ZIP found in $projectRoot"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

$results = foreach ($archive in $archives) {
    $zip = [IO.Compression.ZipFile]::OpenRead($archive.FullName)
    try {
        $hasDepot = $null -ne $zip.GetEntry('files/.depot/asset_packs.cfg')
        $textureGroups = @(
            $zip.Entries |
                Where-Object FullName -Match '\.(dxt|etc|atc)\.dds\.z$' |
                ForEach-Object { if ($_.FullName -match '\.(dxt|etc|atc)\.dds\.z$') { $Matches[1].ToUpperInvariant() } } |
                Group-Object |
                Sort-Object Count -Descending
        )
        $dominantTexture = if ($textureGroups.Count) { $textureGroups[0].Name } else { '' }
        $textureSummary = $textureGroups | ForEach-Object { "$($_.Name):$($_.Count)" }
        [pscustomobject]@{
            Archive = $archive.Name
            CompressedGiB = [math]::Round($archive.Length / 1GB, 3)
            Entries = $zip.Entries.Count
            UncompressedGiB = [math]::Round((($zip.Entries | Measure-Object Length -Sum).Sum / 1GB), 3)
            HasDepot = $hasDepot
            DominantTexture = $dominantTexture
            TextureCounts = ($textureSummary -join ',')
        }
    }
    finally {
        $zip.Dispose()
    }
}

Write-Host "APK: $($apk.Name)"
Write-Host "APK size: $([math]::Round($apk.Length / 1MB, 1)) MiB"
if ($aapt2) {
    $badging = & $aapt2 dump badging $apk.FullName
    $packageLine = $badging | Select-String '^package:' | Select-Object -First 1
    $nativeLine = $badging | Select-String '^native-code:' | Select-Object -First 1
    Write-Host $packageLine
    Write-Host $nativeLine
    if ($packageLine -notmatch "name='$([regex]::Escape($expectedPackage))'") {
        throw "Unexpected package name. Expected $expectedPackage"
    }
    if ($nativeLine -notmatch "'arm64-v8a'") {
        throw 'APK does not contain the required arm64-v8a ABI.'
    }
}
else {
    Write-Warning 'aapt2 was not found; APK metadata was not validated.'
}

$results | Format-Table -AutoSize

if (-not ($results | Where-Object { $_.Archive -like 'DXT_*' -and $_.DominantTexture -eq 'DXT' -and $_.HasDepot })) {
    throw 'A valid DXT depot archive was not found.'
}

Write-Host 'Input preflight passed.' -ForegroundColor Green
