# Zip the release package and cut it into parts that can actually be
# downloaded.
#
# WHY PARTS: the package is ~12 GiB of files.  A chat attachment of that
# size is not a thing anybody can click, so the archive is cut into 2 GiB
# pieces; tools\Join-And-Extract.ps1 puts them back together.
#
# The zip itself is produced by bsdtar (tar.exe), which is in Windows 10+.

param(
    [string]$Stamp = '20261103',
    [string]$Package,
    [string]$OutZip,
    [string]$PartDir,
    [long]$PartSize = 2GB
)

$ErrorActionPreference = 'Stop'

# The stamp is the single source of truth for the paths.  The old defaults
# were hard-wired to 20261102, which is how a repack could quietly write
# next to the previous day's deliverables.
if (-not $PSBoundParameters.ContainsKey('Package')) {
    $Package = "E:\AI\RealRacing3-switch\dist\rr3-switch-$Stamp"
}
if (-not $PSBoundParameters.ContainsKey('OutZip')) {
    $OutZip = "E:\AI\RealRacing3-switch\dist\rr3-switch-$Stamp.zip"
}
if (-not $PSBoundParameters.ContainsKey('PartDir')) {
    $PartDir = "E:\AI\RealRacing3-switch\dist\parts-$Stamp"
}

$pkg = $Package
$outZip = $OutZip
$partSize = $PartSize
$partDir = $PartDir

if (-not (Test-Path -LiteralPath $pkg -PathType Container)) {
    throw "package missing: $pkg"
}

# ---- 0. refuse to archive a package that contains a copy of itself --------
#
# A nested copy of the whole package was found inside the package directory
# once, and it went into the archive: the archive grew past the size the
# package needs, which is how it was noticed.  The archive is therefore
# checked BEFORE it is built, not after.
$pkgName = Split-Path $pkg -Leaf
$selfCopies = @(Get-ChildItem -LiteralPath $pkg -Directory -Recurse -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -eq $pkgName })
if ($selfCopies.Count -gt 0) {
    foreach ($c in $selfCopies) {
        Write-Host ("SELF COPY : {0}" -f $c.FullName)
    }
    throw "the package contains a copy of itself; the archive would contain it twice"
}
$pkgFiles = @(Get-ChildItem -LiteralPath $pkg -Recurse -File -Force).Count
$pkgDirs = @(Get-ChildItem -LiteralPath $pkg -Recurse -Directory -Force).Count
Write-Host ("package  : {0} files, {1} directories, no self copy" -f $pkgFiles, $pkgDirs)

# ---- 1. zip ------------------------------------------------------------------
#
# The archive used to be reused whenever it existed.  A stale archive was
# shipped that way once: the package's README and console config had changed,
# the archive had not, and the archive ended up 1480 bytes off the package
# (the size check below caught it, but only after a whole repack cycle).
#
# So the archive is fingerprinted against the package it was built from, and
# an archive whose fingerprint does not match is REBUILT rather than reused.
# The fingerprint is the package's file list with sizes and timestamps; it
# costs one directory walk, not a re-hash of 11 GiB.
function Get-PackageFingerprint {
    param([string]$Root)
    $lines = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force | ForEach-Object {
        $rel = $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
        '{0}|{1}|{2}' -f $rel, $_.Length, $_.LastWriteTimeUtc.Ticks
    } | Sort-Object)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
        (($sha.ComputeHash($bytes)) | ForEach-Object { $_.ToString('x2') }) -join ''
    }
    finally { $sha.Dispose() }
}

$fpFile = "$outZip.fingerprint"
$fp = Get-PackageFingerprint -Root $pkg
$reuse = $false
if (Test-Path -LiteralPath $outZip) {
    $stored = $null
    if (Test-Path -LiteralPath $fpFile) { $stored = (Get-Content -LiteralPath $fpFile -Raw).Trim() }
    if ($stored -eq $fp) {
        $reuse = $true
        Write-Host ("existing zip: {0:N2} GiB (fingerprint matches -- reusing)" -f ((Get-Item $outZip).Length / 1GB))
    }
    else {
        Write-Host ("existing zip: {0:N2} GiB is STALE (package changed) -- rebuilding" -f ((Get-Item $outZip).Length / 1GB))
        Remove-Item -LiteralPath $outZip -Force
    }
}
if (-not $reuse) {
    Write-Host 'zipping ...'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & tar.exe -a -cf $outZip -C (Split-Path $pkg -Parent) (Split-Path $pkg -Leaf)
    if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)" }
    $sw.Stop()
    Write-Host ("  done in {0:N0}s: {1:N2} GiB" -f $sw.Elapsed.TotalSeconds, ((Get-Item $outZip).Length / 1GB))
    # The fingerprint is written only AFTER the archive was built, so an
    # interrupted run leaves no fingerprint and the next run rebuilds.
    [System.IO.File]::WriteAllText($fpFile, $fp)
    Write-Host ("  fingerprint {0}" -f $fp.Substring(0, 16))
}

# ---- 2. verify the archive ---------------------------------------------------
Write-Host 'listing the archive ...'
$entries = @(& tar.exe -tf $outZip)
if ($LASTEXITCODE -ne 0) { throw 'the archive does not read back' }
Write-Host ("  {0} entries vs {1} files + {2} directories in the package" -f $entries.Count, $pkgFiles, $pkgDirs)
if ($entries.Count -lt $pkgFiles) {
    throw ("archive holds {0} entries but the package has {1} files" -f $entries.Count, $pkgFiles)
}

# The UNCOMPRESSED total has to equal the package's total.  A copy of the
# package inside the package (which happened once) shows up here as twice
# the size, and this is the check that catches it.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($outZip)
try {
    $uncompressed = 0L
    $memberCount = 0
    foreach ($e in $zip.Entries) {
        $uncompressed += $e.Length
        $memberCount++
    }
}
finally { $zip.Dispose() }
$pkgBytes = (Get-ChildItem -LiteralPath $pkg -Recurse -File -Force | Measure-Object Length -Sum).Sum
Write-Host ("  archive members {0}, uncompressed {1:N2} GiB" -f $memberCount, ($uncompressed / 1GB))
Write-Host ("  package files   {0}, bytes      {1:N2} GiB" -f $pkgFiles, ($pkgBytes / 1GB))
if ($uncompressed -ne $pkgBytes) {
    throw ("the archive holds {0} bytes but the package holds {1} -- something extra went in" -f $uncompressed, $pkgBytes)
}
Write-Host '  sizes match.'

# ---- 3. cut it into parts ----------------------------------------------------
New-Item -ItemType Directory -Force -Path $partDir | Out-Null
# Any part left over from an earlier run would be concatenated into the new
# archive, so the old ones go first.
Get-ChildItem -LiteralPath $partDir -File -Filter '*.zip.???*' -ErrorAction SilentlyContinue |
    ForEach-Object {
        Write-Host ("  removing stale part: {0}" -f $_.Name)
        Remove-Item -LiteralPath $_.FullName -Force
    }

$in = [System.IO.File]::OpenRead($outZip)
try {
    $index = 1
    $total = 0
    while ($true) {
        # Read a byte first: creating the part file and then finding the
        # archive exhausted left an empty trailing part behind.
        $probe = New-Object byte[] 1
        $probeRead = $in.Read($probe, 0, 1)
        if ($probeRead -le 0) { break }
        $in.Seek(-1, [System.IO.SeekOrigin]::Current) | Out-Null

        $partPath = Join-Path $partDir ('{0}.{1:d3}' -f (Split-Path $outZip -Leaf), $index)
        $out = [System.IO.File]::Create($partPath)
        try {
            $written = 0L
            while ($written -lt $partSize) {
                # 2 GiB does not fit in an Int32, so the chunk size is computed
                # in Int64 and only the CHUNK is cast down.
                $want = [int][Math]::Min([long]4194304, [long]($partSize - $written))
                $buf = New-Object byte[] $want
                $read = $in.Read($buf, 0, $want)
                if ($read -le 0) { break }
                $out.Write($buf, 0, $read)
                $written += $read
            }
        }
        finally { $out.Dispose() }
        if ($written -eq 0) { break }
        $total += $written
        Write-Host ("  part {0}: {1:N2} GiB" -f (Split-Path $partPath -Leaf), ($written / 1GB))
        $index++
    }
}
finally { $in.Dispose() }

$zipLen = (Get-Item $outZip).Length
if ($total -ne $zipLen) {
    throw ("parts total {0} but the zip is {1}" -f $total, $zipLen)
}
Write-Host ("parts total {0:N2} GiB == zip {1:N2} GiB" -f ($total / 1GB), ($zipLen / 1GB))
Write-Host ''
Get-ChildItem -LiteralPath $partDir -File | Sort-Object Name |
    ForEach-Object { Write-Host ("  {0,-52} {1,14:N0} bytes" -f $_.Name, $_.Length) }
