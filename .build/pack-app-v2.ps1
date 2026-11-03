# Build the APPLICATION-ONLY update package: one file, ~45 MiB.
#
# WHY IT EXISTS
# -------------
# The full package is ~10 GiB because of the game's data volumes, and a
# card that is tight on space cannot take it.  But the APPLICATION -- the
# NRO, the port's romfs, the nine control scripts and the console
# configuration -- is 45 MiB, and that is the part that has to be replaced
# when the port changes.
#
# So there is a package that is one file and fits on anything, and it is
# this one.  Unpack it over the card root: it overwrites the app and the
# scripts and leaves the data volumes alone.
#
# rr3-gfx.conf IS INCLUDED, and it goes to switch\ (not to
# switch\rr3-native-rehost\): the port reads it there, and deleting it
# switches off the four knobs -- ccdisp / ccloginok / ccsocialok / objid
# -- that decide whether the guest gets past its cloud-sync spin and
# reaches the race at all.

param(
    [string]$Stamp = '20261102'
)

$ErrorActionPreference = 'Stop'

$projectRoot = 'E:\AI\RealRacing3-switch'
$cardApp = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
# THE APP COMES FROM THE BUILD OUTPUT, not from the card: the card's copy can
# be locked by a running emulator, and the build output is the thing that was
# just tested anyway.
$buildRoot = Join-Path $projectRoot 'native-switch'
$outDir = Join-Path $projectRoot ("dist\rr3-switch-app-{0}" -f $Stamp)
$outZip = Join-Path $projectRoot ("dist\rr3-switch-app-{0}.zip" -f $Stamp)

if (-not (Test-Path -LiteralPath $cardApp -PathType Container)) {
    throw "card app dir missing: $cardApp"
}

New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'switch\rr3-native-rehost') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'switch') | Out-Null

foreach ($name in @('rr3-native-rehost.nro', 'rr3.romfs')) {
    $src = Join-Path $buildRoot $name
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
        throw "build output missing: $src"
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $outDir "switch\rr3-native-rehost\$name") -Force
}

$scriptSource = Join-Path $projectRoot 'native-switch\card'
$scriptNames = @('rr3-touch.txt', 'rr3-drive.txt', 'rr3-pad.txt', 'rr3-action.txt',
    'rr3-screen.txt', 'rr3-settings.txt', 'rr3-ui.txt', 'rr3-open.txt',
    'rr3-gamepad.txt')
foreach ($name in $scriptNames) {
    Copy-Item -LiteralPath (Join-Path $scriptSource $name) `
        -Destination (Join-Path $outDir "switch\rr3-native-rehost\$name") -Force
}

Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\rr3-gfx.conf') `
    -Destination (Join-Path $outDir 'switch\rr3-gfx.conf') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\README-hardware-test.md') `
    -Destination (Join-Path $outDir 'README-hardware-test.md') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\Copy-To-SwitchCard.ps1') `
    -Destination (Join-Path $outDir 'Copy-To-SwitchCard.ps1') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\Join-And-Extract.ps1') `
    -Destination (Join-Path $outDir 'Join-And-Extract.ps1') -Force

if (Test-Path -LiteralPath $outZip) { Remove-Item -LiteralPath $outZip -Force }
Write-Host 'zipping ...'
& tar.exe -a -cf $outZip -C $outDir .
if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)" }

# ---- verify -------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($outZip)
try {
    $uncompressed = 0L
    $members = @($zip.Entries | ForEach-Object { $_.FullName })
    foreach ($e in $zip.Entries) { $uncompressed += $e.Length }
}
finally { $zip.Dispose() }

$pkgBytes = (Get-ChildItem -LiteralPath $outDir -Recurse -File -Force | Measure-Object Length -Sum).Sum
if ($uncompressed -ne $pkgBytes) {
    throw ("the archive holds {0} bytes but the folder holds {1}" -f $uncompressed, $pkgBytes)
}
foreach ($required in @('switch/rr3-gfx.conf', 'switch/rr3-native-rehost/rr3-native-rehost.nro',
    'switch/rr3-native-rehost/rr3.romfs', 'switch/rr3-native-rehost/rr3-gamepad.txt')) {
    # bsdtar writes the members with a leading "./", so the comparison is
    # made on the normalised name.
    $hit = @($members | Where-Object {
        ($_ -replace '^\./', '') -eq $required
    })
    if ($hit.Count -eq 0) { throw "the archive is missing $required" }
}
Write-Host ("members   : {0}" -f (($members | Select-Object -First 6) -join ', '))
Write-Host ("app zip  : {0}" -f $outZip)
Write-Host ("           {0} entries, {1:N1} MiB compressed, {2:N1} MiB uncompressed" -f `
    $members.Count, ((Get-Item $outZip).Length / 1MB), ($uncompressed / 1MB))
