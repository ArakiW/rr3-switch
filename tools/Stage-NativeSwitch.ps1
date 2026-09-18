[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$nro = Join-Path $projectRoot 'native-switch\rr3-native-rehost.nro'
$romfsImage = Join-Path $projectRoot 'native-switch\rr3.romfs'
$gameLibrary = Join-Path $projectRoot 'reverse\native\arm64\libRealRacing3.so'
$destination = Join-Path $projectRoot 'dist\rr3-native-rehost-ui-0.18.3'

if (-not (Test-Path $nro -PathType Leaf)) {
    throw 'NRO is missing. Run Build-NativeSwitch.ps1 first.'
}
if (-not (Test-Path $gameLibrary -PathType Leaf)) {
    throw 'Extracted libRealRacing3.so is missing.'
}
if (-not (Test-Path $romfsImage -PathType Leaf)) {
    throw 'Built RomFS image is missing.'
}

$nroBytes = [IO.File]::ReadAllBytes($nro)
if ($nroBytes.Length -lt 0x80 -or [Text.Encoding]::ASCII.GetString($nroBytes, 0x10, 4) -ne 'NRO0') {
    throw 'Built file is not a valid NRO.'
}
$nroImageSize = [BitConverter]::ToUInt32($nroBytes, 0x18)
if ($nroImageSize + 0x38 -gt $nroBytes.Length -or
    [Text.Encoding]::ASCII.GetString($nroBytes, $nroImageSize, 4) -ne 'ASET') {
    throw 'NRO asset header is missing.'
}
$embeddedRomfsSize = [BitConverter]::ToUInt64($nroBytes, $nroImageSize + 0x30)
if ($embeddedRomfsSize -lt (Get-Item $gameLibrary).Length) {
    throw "Embedded RomFS is too small: $embeddedRomfsSize"
}
$embeddedRomfsOffset = [BitConverter]::ToUInt64($nroBytes, $nroImageSize + 0x28)
$stream = [IO.File]::OpenRead($nro)
try {
    $stream.Position = $nroImageSize + $embeddedRomfsOffset
    $sha = [Security.Cryptography.SHA256]::Create()
    $embeddedRomfsHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
}
finally {
    $stream.Dispose()
}
$rawRomfsHash = (Get-FileHash $romfsImage -Algorithm SHA256).Hash
if ($embeddedRomfsHash -ne $rawRomfsHash) {
    throw 'Embedded RomFS hash does not match the built RomFS image.'
}

New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -LiteralPath $nro -Destination (Join-Path $destination 'rr3-native-rehost.nro') -Force

$readme = @(
    'RR3 Native Rehost - Formal Loader UI 0.18.3'
    ''
    'Replace the previous NRO with rr3-native-rehost.nro.'
    'The game core, Nimble, FMOD, and libc++ are embedded; no separate SO is required.'
    'Recommended path: sdmc:/switch/rr3-native-rehost/rr3-native-rehost.nro'
    'Launch rr3-native-rehost.nro from hbmenu.'
    ''
    'This build provides a four-page native loader dashboard, one-pass ModuleCode loading,'
    'Mesa EGL/GLES providers, global symbol scope, RELRO/TLS audit, recovery guard,'
    'optional report export, lifecycle ordering, and rollback gates.'
    'No guest constructors are executed.'
    'It does not yet start the game core.'
    ''
    'Controls:'
    '  L/R, D-pad, or A: change page'
    '  Y: release console and run a read-only default-window Mesa EGL/GLES probe'
    '  X: explicitly export loader-report.txt'
    '  B: clear a recovery marker after inspection'
    '  PLUS: exit'
    ''
    'Host reference:'
    '  core images=4 mapped=35999744 bytes permissions=11'
    '  needed embedded/virtual/missing=4/20/0'
    '  reloc R/I/H/X/W/M=145752/3026/731/8899/37/0'
    '  patch/verify=158445/158445 invalid=0'
    '  unique host/cross/missing=519/247/0'
    '  lifecycle constructors/finalizers/targets=184/7/191'
    '  resolver=531 table=594136e24e970742'
    '  plan=999a029e95729174'
    ''
    'Constructors are never run automatically in this release.'
)
$readme | Set-Content -Path (Join-Path $destination 'README.txt') -Encoding utf8

$hashLines = Get-ChildItem $destination -File |
    Where-Object Name -ne 'checksums.sha256' |
    Sort-Object Name |
    ForEach-Object {
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    }
$hashLines | Set-Content -Path (Join-Path $destination 'checksums.sha256') -Encoding ascii

Write-Host "Staged: $destination" -ForegroundColor Green
