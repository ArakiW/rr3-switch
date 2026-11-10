<#
.SYNOPSIS
    Package the port as an installable NSP.

.DESCRIPTION
    WHY THIS SCRIPT AND NOT A PLAIN PFS0.  A PFS0 with "exefs/main" and
    "exefs/main.npdm" is an EXEFS IMAGE, not an NSP: it carries no content
    metadata, and every installer refuses it with "no content meta found"
    (the Chinese installers say 未找到内容元数据).  An NSP that installs
    carries THREE NCAs -- Program, Control and Meta -- and the Meta NCA holds
    the content meta whose digest the installer checks.

    THE TWO STAGES, AND WHY BOTH.

    Stage 1 -- the official Nintendo SDK (the one installed on this machine)
    produces the signed pieces:
        elf2nso                      -> exefs/main
        MakeMeta.exe                  -> exefs/main.npdm
        AuthoringTool.exe creatensp   -> the development NSP
    The control (control.nacp + icon_AmericanEnglish.dat) is then extracted
    back out of that development NSP, because the control is what carries the
    title's NAME and ICON, and the SDK generates it from the .nmeta.

    Stage 2 -- the user's own CFW packer (hacBrewPack with the local
    CNMT-digest fix) turns those pieces into the unsigned homebrew NSP:
        hacbrewpack --keyset <the user's own keyset> --keygeneration 1 ...
    The digest fix matters: upstream hashes only part of the content meta, so
    the digest in the Meta NCA does not match the meta the installer
    recomputes, and the installer reports that the content metadata is
    missing.  The fork hashes the CNMT BODY (header + extended header + the
    content records), so the last 32 bytes are SHA256 of the first 160.

    THE STDOUT/STDERNO DISCARD IS NOT A STYLE CHOICE.  hacbrewpack prints
    key-derived values in its error messages, so both streams are sent to
    the void and only the exit code is kept.

    WHAT IS AND IS NOT IN THE NSP: the executable, its meta and the
    control.  NOT the game's data (the seven volumes and the .depot tree,
    about 11.8 GiB) -- that has never been in the executable and is still
    copied onto the card separately.

.PARAMETER Out
    Where to write the NSP.  Defaults to native-switch\rr3-native-rehost.nsp.

.PARAMETER ProjectRoot
    The repository root.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\Pack-NativeSwitchNsp.ps1
#>
param(
    [string]$Out = '',
    [string]$ProjectRoot = '',
    [string]$SdkRoot = '',
    [string]$Packer = '',
    [string]$Keyset = ''
)

$ErrorActionPreference = 'Stop'

if ($ProjectRoot -eq '') {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$native = Join-Path $ProjectRoot 'native-switch'
if ($Out -eq '') {
    $Out = Join-Path $native 'rr3-native-rehost.nsp'
}

# --- the pieces, and where they live ------------------------------------------
if ($SdkRoot -eq '') {
    $SdkRoot = 'E:\AI\Nintendo SDK 18.3(1).1\installed-sdk-18.3.1\NintendoSDK'
}
if ($Packer -eq '') {
    $Packer = 'E:\AI\AC-SWITCH\analysis\nsp_sdk_20261012\implementer\cfw_packer_cnmt_digest_fork\source\hacbrewpack'
}
if ($Keyset -eq '') {
    $Keyset = 'E:\AI\FH1_SWITCH\tools\Eden-Windows-v0.2.1-pgo\keys\prod.keys'
}

foreach ($required in @(
    (Join-Path $SdkRoot 'Tools\CommandLineTools\AuthoringTool\AuthoringTool.exe'),
    (Join-Path $SdkRoot 'Tools\CommandLineTools\MakeMeta\MakeMeta.exe'),
    (Join-Path $SdkRoot 'Resources\SpecFiles\Desc\NX-NXFP2-a64\Application.autogen.desc'),
    (Join-Path $SdkRoot 'Resources\SpecFiles\NintendoSDK_Application.bmp'),
    $Packer, $Keyset, (Join-Path $native 'rr3-native-rehost.elf'),
    (Join-Path $native 'rr3.nmeta'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw ("required input is missing: {0}" -f $required)
    }
}

# The tools that make an NSO and a meta.  The local devkitPro has them.
$toolBin = Join-Path $ProjectRoot '.tools\devkitPro\msys64\opt\devkitpro\tools\bin'
$elf2nso = Join-Path $toolBin 'elf2nso.exe'
if (-not (Test-Path -LiteralPath $elf2nso -PathType Leaf)) {
    throw ("elf2nso is missing: {0}" -f $elf2nso)
}

# --- the stage ----------------------------------------------------------------
$stage = Join-Path $native 'nsp-sdk-stage'
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
foreach ($dir in @('meta', 'code', 'out', 'control', 'cfw')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $dir) | Out-Null
}

# The .nmeta's IconPath is a MARKER here, and it is resolved to the SDK's own
# sample icon in the staged copy only.  The repository's copy stays
# machine-independent.
$metaSource = Join-Path $native 'rr3.nmeta'
$metaStaged = Join-Path $stage 'meta\rr3.nmeta'
$metaText = [System.IO.File]::ReadAllText($metaSource)
$metaText = $metaText.Replace(
    '@SDK_SAMPLE_ICON@',
    (Join-Path $SdkRoot 'Resources\SpecFiles\NintendoSDK_Application.bmp'))
[System.IO.File]::WriteAllText($metaStaged, $metaText, (New-Object System.Text.UTF8Encoding($false)))

$env:NINTENDO_SDK_ROOT = $SdkRoot
$authoringTool = Join-Path $SdkRoot 'Tools\CommandLineTools\AuthoringTool\AuthoringTool.exe'
$makeMeta = Join-Path $SdkRoot 'Tools\CommandLineTools\MakeMeta\MakeMeta.exe'
$desc = Join-Path $SdkRoot 'Resources\SpecFiles\Desc\NX-NXFP2-a64\Application.autogen.desc'

# --- stage 1: the official SDK -------------------------------------------------
Write-Host '=== stage 1: the official SDK ==='

& $authoringTool validatemeta --platform NX --meta $metaStaged --type Application
if ($LASTEXITCODE -ne 0) { throw 'validatemeta failed.' }

& $elf2nso (Join-Path $native 'rr3-native-rehost.elf') (Join-Path $stage 'code\main')
if ($LASTEXITCODE -ne 0) { throw 'elf2nso failed.' }

& $makeMeta --platform NX --meta $metaStaged --desc $desc -o (Join-Path $stage 'code\main.npdm')
if ($LASTEXITCODE -ne 0) { throw 'MakeMeta failed.' }

$devNsp = Join-Path $stage 'out\rr3-dev.nsp'
& $authoringTool creatensp --platform NX -o $devNsp --meta $metaStaged --type Application `
    --desc $desc --program (Join-Path $stage 'code') --ignore-nss-nrs-option
if ($LASTEXITCODE -ne 0) { throw 'creatensp failed.' }

& $authoringTool checkintegrity --platform NX $devNsp
if ($LASTEXITCODE -ne 0) { throw 'the development NSP does not pass checkintegrity.' }

# The control comes back out of the development NSP: the SDK generates it from
# the .nmeta, and it carries the title's name and icon.
& $authoringTool extract -o (Join-Path $stage 'control') `
    --targetregex 'control\.nacp|icon_AmericanEnglish\.dat' $devNsp | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'extracting the control failed.' }

$controlDir = Join-Path $stage 'control'
$controlNacp = Get-ChildItem -LiteralPath $controlDir -Recurse -Filter 'control.nacp' |
    Select-Object -First 1
$controlIcon = Get-ChildItem -LiteralPath $controlDir -Recurse -Filter 'icon_AmericanEnglish.dat' |
    Select-Object -First 1
if (-not $controlNacp -or -not $controlIcon) {
    throw 'the control was not extracted.'
}

# --- stage 2: the CFW packer ---------------------------------------------------
Write-Host '=== stage 2: the CFW packer ==='

$cfw = Join-Path $stage 'cfw'
# 'temp' and 'nca' are NOT created: the packer deletes them, and the proven
# contract requires both to be absent before it runs.
foreach ($dir in @('exefs', 'control', 'nsp', 'backup')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $cfw $dir) | Out-Null
}
Copy-Item -LiteralPath (Join-Path $stage 'code\main') -Destination (Join-Path $cfw 'exefs\main')
Copy-Item -LiteralPath (Join-Path $stage 'code\main.npdm') -Destination (Join-Path $cfw 'exefs\main.npdm')
Copy-Item -LiteralPath $controlNacp.FullName -Destination (Join-Path $cfw 'control\control.nacp')
Copy-Item -LiteralPath $controlIcon.FullName -Destination (Join-Path $cfw 'control\icon_AmericanEnglish.dat')

# WSL paths: /mnt/<drive>/<rest>.  The path does NOT have to exist -- the
# packer's own temp and nca directories are supposed to be absent.
function Convert-ToWslPath([string]$windowsPath) {
    $full = [System.IO.Path]::GetFullPath($windowsPath)
    return '/mnt/' + $full.Substring(0, 1).ToLower() + '/' + $full.Substring(3).Replace('\', '/')
}

$argv = @(
    '-d', 'Ubuntu',
    '--cd', (Convert-ToWslPath $cfw),
    '--exec', (Convert-ToWslPath $Packer),
    '--keyset', (Convert-ToWslPath $Keyset),
    '--keygeneration', '1',
    '--tempdir', (Convert-ToWslPath (Join-Path $cfw 'temp')),
    '--ncadir', (Convert-ToWslPath (Join-Path $cfw 'nca')),
    '--nspdir', (Convert-ToWslPath (Join-Path $cfw 'nsp')),
    '--backupdir', (Convert-ToWslPath (Join-Path $cfw 'backup')),
    '--exefsdir', (Convert-ToWslPath (Join-Path $cfw 'exefs')),
    '--controldir', (Convert-ToWslPath (Join-Path $cfw 'control')),
    '--nosignncasig2', '--nopatchnacplogo', '--noromfs', '--nologo', '--keepncadir'
)
& wsl @argv 1>$null 2>$null
if ($LASTEXITCODE -ne 0) { throw ('the CFW packer failed, exit ' + $LASTEXITCODE) }

$built = Get-ChildItem -LiteralPath (Join-Path $cfw 'nsp') -Filter '*.nsp' |
    Select-Object -First 1
if (-not $built) { throw 'the CFW packer produced no NSP.' }

# --- the check that matters ----------------------------------------------------
# Read the container back.  An NSP that an installer will accept has exactly
# three NCAs in it -- Program, Control and Meta -- and the Meta one is the
# content metadata whose absence is the "no content meta found" failure.
$bytes = [System.IO.File]::ReadAllBytes($built.FullName)
if ($bytes.Length -lt 0x40 -or
    [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'PFS0') {
    throw 'the CFW packer did not produce a PFS0 container.'
}
$count = [System.BitConverter]::ToUInt32($bytes, 4)
$table = 0x10
$stringBase = $table + ($count * 0x18)
$names = @()
for ($index = 0; $index -lt $count; $index++) {
    $base = $table + ($index * 0x18)
    $stringOffset = [System.BitConverter]::ToUInt32($bytes, $base + 0x10)
    $end = $stringBase + [int]$stringOffset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
    $names += [System.Text.Encoding]::UTF8.GetString(
        $bytes, $stringBase + [int]$stringOffset, $end - ($stringBase + [int]$stringOffset))
}
$meta = @($names | Where-Object { $_ -like '*.cnmt.nca' })
if ($meta.Count -ne 1) {
    throw ("the NSP has no single content-meta NCA -- entries: {0}" -f ($names -join ', '))
}
if ($count -ne 3) {
    throw ("the NSP has {0} entries, expected Program + Control + Meta: {1}" -f $count, ($names -join ', '))
}

Copy-Item -LiteralPath $built.FullName -Destination $Out -Force

$info = Get-Item -LiteralPath $Out
Write-Host ("NSP      : {0}" -f $Out)
Write-Host ("size     : {0:N0} bytes" -f $info.Length)
Write-Host ("sha256   : {0}" -f (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash)
Write-Host ("entries  : {0}" -f ($names -join ', '))
