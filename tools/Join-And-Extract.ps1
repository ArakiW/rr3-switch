<#
.SYNOPSIS
    Join the split archive parts of an RR3 release package and install it.

.DESCRIPTION
    The release package is ~12 GiB of files, which is too big for one
    download, so it is shipped as a .zip cut into parts:

        rr3-fulltest-YYYYMMDD.zip.001
        rr3-fulltest-YYYYMMDD.zip.002
        ...

    Put all the parts in ONE folder, put this script in that same folder,
    and run it.  It joins the parts, unpacks the package, and -- if you give
    it -Card -- installs it onto the SD card by calling the package's own
    Copy-To-SwitchCard.ps1.

    If you already have the whole .zip (not parts), this script unpacks it
    as well.

.PARAMETER PartsDir
    The folder holding the .zip.001 ... parts (and this script).
    Defaults to the folder this script is in.

.PARAMETER OutDir
    Where to unpack.  Defaults to <PartsDir>\rr3-package.
    Use a folder on a drive that has ~13 GiB free.

.PARAMETER Card
    Optional.  The SD card root (the folder that CONTAINS switch\).
    When given, the unpacked package is installed onto it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Join-And-Extract.ps1 -Card F:\
#>
[CmdletBinding()]
param(
    [string]$PartsDir = '',
    [string]$OutDir = '',
    [string]$Card = ''
)

$ErrorActionPreference = 'Stop'

if ($PartsDir -eq '') {
    $PartsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if ($OutDir -eq '') {
    $OutDir = Join-Path $PartsDir 'rr3-package'
}

Write-Host ("parts dir : {0}" -f $PartsDir)

# ---- 1. find the archive -----------------------------------------------------
$parts = @(Get-ChildItem -LiteralPath $PartsDir -File -Filter '*.zip.???*' -ErrorAction SilentlyContinue |
    Sort-Object Name)
$whole = @(Get-ChildItem -LiteralPath $PartsDir -File -Filter '*.zip' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notmatch '\.\d{3}$' })

$archive = $null
if ($parts.Count -gt 0) {
    Write-Host ("parts     : {0}" -f (($parts | ForEach-Object { $_.Name }) -join ', '))
    $joined = Join-Path $PartsDir ($parts[0].Name -replace '\.\d{3}$', '')
    Write-Host ("joining   : {0} ({1:N1} GiB)" -f (Split-Path $joined -Leaf),
        (($parts | Measure-Object Length -Sum).Sum / 1GB))
    $out = [System.IO.File]::Create($joined)
    try {
        foreach ($part in $parts) {
            $in = [System.IO.File]::OpenRead($part.FullName)
            try { $in.CopyTo($out, 4MB) }
            finally { $in.Dispose() }
            Write-Host ("  + {0}" -f $part.Name)
        }
    }
    finally { $out.Dispose() }
    $archive = $joined
}
elseif ($whole.Count -gt 0) {
    $archive = $whole[0].FullName
    Write-Host ("archive   : {0} (already whole)" -f (Split-Path $archive -Leaf))
}
else {
    Write-Error "no *.zip.001 parts and no *.zip found in $PartsDir"
    exit 1
}

# ---- 2. unpack ---------------------------------------------------------------
if (Test-Path -LiteralPath $OutDir -PathType Container) {
    Write-Host ("unpack    : {0} already exists -- reusing it" -f $OutDir)
}
else {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Write-Host ("unpacking : {0}" -f $archive)
    # tar.exe (bsdtar) ships with Windows 10+ and reads zip.
    & tar.exe -xf $archive -C $OutDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "unpack failed (tar exit $LASTEXITCODE)"
        exit 2
    }
}

$app = Join-Path $OutDir 'switch\rr3-native-rehost'
if (-not (Test-Path -LiteralPath $app -PathType Container)) {
    # The archive carries the package directory itself, so the tree lands one
    # level deeper: <OutDir>\<package name>\switch\rr3-native-rehost.
    $roots = @(Get-ChildItem -LiteralPath $OutDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'switch\rr3-native-rehost') })
    if ($roots.Count -eq 1) {
        $OutDir = $roots[0].FullName
        $app = Join-Path $OutDir 'switch\rr3-native-rehost'
        Write-Host ("package   : {0}" -f $OutDir)
    }
}
if (-not (Test-Path -LiteralPath $app -PathType Container)) {
    Write-Error "the archive did not contain switch\rr3-native-rehost -- wrong archive?"
    exit 2
}
$nro = Join-Path $app 'rr3-native-rehost.nro'
$files = @(Get-ChildItem -LiteralPath (Join-Path $OutDir 'switch') -Recurse -File -Force).Count
$bytes = (Get-ChildItem -LiteralPath (Join-Path $OutDir 'switch') -Recurse -File -Force |
    Measure-Object -Property Length -Sum).Sum
Write-Host ("verified  : {0} files, {1:N2} GiB unpacked" -f $files, ($bytes / 1GB))
Write-Host ("            NRO sha256 = {0}" -f (Get-FileHash $nro -Algorithm SHA256).Hash.ToLower())

# ---- 3. install onto the card ------------------------------------------------
if ($Card -eq '') {
    Write-Host ""
    Write-Host "Unpacked only. To install onto the console, run:"
    Write-Host ("  powershell -ExecutionPolicy Bypass -File `"{0}\Copy-To-SwitchCard.ps1`" -Card F:\" -f $OutDir)
    exit 0
}

$copy = Join-Path $OutDir 'Copy-To-SwitchCard.ps1'
if (-not (Test-Path -LiteralPath $copy -PathType Leaf)) {
    Write-Error "Copy-To-SwitchCard.ps1 is missing from the unpacked package"
    exit 2
}
Write-Host ""
Write-Host ("installing onto {0}" -f $Card)
& powershell.exe -ExecutionPolicy Bypass -File $copy -Card $Card -PackageRoot $OutDir
exit $LASTEXITCODE
