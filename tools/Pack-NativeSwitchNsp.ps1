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

    THE ROMFS IS IN THE NSP, AND IT HAS TO BE.
    The first version of this script passed "--noromfs", so the NSP carried
    only the executable, its meta and the control: 5,411,032 bytes, against
    53,756,401 for the NRO.  The difference was not packaging overhead --
    it was the whole RomFS (44,948,780 bytes over 35 files) missing,
    including the guest's own libraries:
        romfs/libRealRacing3.so      33,110,472
        romfs/libc++_shared.so         1,014,704
        romfs/libfmodex.so             1,274,928
        romfs/libNimble.so
        romfs/ui/*.otf, *.ttf          the catalogue faces
        romfs/original/.depot/text_en.txt, text_cn-Hans.txt
    Without it the port cannot start, and the reason is in the code rather
    than in the installer: main.cpp opens "romfs:/libc++_shared.so" with no
    card fallback, so the guest's C++ runtime has exactly one source.

    WHY THE ROMFS REACHES THE PROCESS AT ALL.  The Program NCA built by
    hacbrewpack carries TWO sections: section 0 is the ExeFS (the NSO and
    the NPDM) and section 1 is the RomFS, wrapped in IVFC and encrypted.
    libnx's romfsInit() -- romfsMountSelf() -- documents the two cases
    explicitly: "If the application is running as NRO, it mounts the
    embedded RomFS section inside the NRO.  If on the other hand it's an
    NSO, it behaves identically to romfsMountFromCurrentProcess."  An NSP
    launches an NSO, so the RomFS the process sees is the one in the
    Program NCA's second section -- which is exactly what "--romfsdir"
    builds.  The NRO and the NSP therefore carry the same tree, reached by
    two different mechanisms.

    WHAT IS AND IS NOT IN THE NSP: the executable, its RomFS, its meta and
    the control.  NOT the game's data (the seven volumes and the .depot
    tree, about 11.8 GiB) -- that has never been in the executable and is
    still copied onto the card separately.

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
    [string]$Keyset = '',
    [string]$RomfsDir = ''
)

$ErrorActionPreference = 'Stop'

if ($ProjectRoot -eq '') {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$native = Join-Path $ProjectRoot 'native-switch'
if ($Out -eq '') {
    $Out = Join-Path $native 'rr3-native-rehost.nsp'
}
# The tree that goes into the Program NCA's second section.  It is the same
# tree the NRO embeds: the guest's libraries and the catalogue faces.
if ($RomfsDir -eq '') {
    $RomfsDir = Join-Path $native 'romfs'
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

# The RomFS tree, and the one file in it that proves it is the right tree:
# the guest's own main binary.  Without it the port has no guest at all.
if (-not (Test-Path -LiteralPath $RomfsDir -PathType Container)) {
    throw ("the RomFS directory is missing: {0}" -f $RomfsDir)
}
$guestMain = Join-Path $RomfsDir 'libRealRacing3.so'
if (-not (Test-Path -LiteralPath $guestMain -PathType Leaf)) {
    throw ("the RomFS tree carries no guest main binary: {0}" -f $guestMain)
}
$romfsFiles = @(Get-ChildItem -LiteralPath $RomfsDir -Recurse -File)
$romfsBytes = [long](($romfsFiles | Measure-Object -Property Length -Sum).Sum)

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
    '--romfsdir', (Convert-ToWslPath $RomfsDir),
    '--controldir', (Convert-ToWslPath (Join-Path $cfw 'control')),
    '--nosignncasig2', '--nopatchnacplogo', '--nologo', '--keepncadir'
)
# THE PACKER'S OUTPUT NEVER REACHES THE LOG.
# hacbrewpack prints key-derived values in its error messages, so stdout and
# stderr both have to stay out of the console and out of the build log.  They
# go to temporary files instead of to $null, because $null is not enough:
# with $ErrorActionPreference = 'Stop', a native command that writes to
# stderr is turned into a TERMINATING error, and wsl.exe always writes a
# UTF-16 proxy warning to stderr on this machine ("wsl: 检测到 localhost
# 代理配置...").  That warning is not a failure -- the packer ran fine and
# printed "Program NCA RomFS Section: Yes" -- but it killed the script
# before the exit code could be read.  So: proxy variables are cleared for
# the call, stderr goes to a file outside the repository, and the exit code
# is the only thing that is kept.
$savedProxy = @{}
foreach ($name in @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY',
                     'http_proxy', 'https_proxy', 'all_proxy')) {
    $savedProxy[$name] = [Environment]::GetEnvironmentVariable($name)
    [Environment]::SetEnvironmentVariable($name, $null)
}
$stderrFile = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('rr3-nsp-pack-{0}.err' -f [guid]::NewGuid().ToString('N'))
$packerExit = -1
try {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & wsl @argv 1>$null 2>$stderrFile
        $packerExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
}
finally {
    foreach ($name in $savedProxy.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedProxy[$name])
    }
    if (Test-Path -LiteralPath $stderrFile) {
        Remove-Item -LiteralPath $stderrFile -Force -ErrorAction SilentlyContinue
    }
}
if ($packerExit -ne 0) { throw ('the CFW packer failed, exit ' + $packerExit) }

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
$sizes = @()
for ($index = 0; $index -lt $count; $index++) {
    $base = $table + ($index * 0x18)
    $stringOffset = [System.BitConverter]::ToUInt32($bytes, $base + 0x10)
    $end = $stringBase + [int]$stringOffset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
    $names += [System.Text.Encoding]::UTF8.GetString(
        $bytes, $stringBase + [int]$stringOffset, $end - ($stringBase + [int]$stringOffset))
    $sizes += [long][System.BitConverter]::ToUInt64($bytes, $base + 0x08)
}
$meta = @($names | Where-Object { $_ -like '*.cnmt.nca' })
if ($meta.Count -ne 1) {
    throw ("the NSP has no single content-meta NCA -- entries: {0}" -f ($names -join ', '))
}
if ($count -ne 3) {
    throw ("the NSP has {0} entries, expected Program + Control + Meta: {1}" -f $count, ($names -join ', '))
}

# THE SECOND CHECK: THE ROMFS HAS TO BE IN THE PACKAGE.
# The first version of this package was 5,411,032 bytes and it did not run,
# because the RomFS was not in it.  The ExeFS alone is under 9 MB, so the
# Program NCA has to be bigger than the whole RomFS tree for the tree to be
# inside it.  Reading the container's own entry sizes is the cheapest way to
# see that, and it needs no key: the PFS0 table is in the clear.
$largest = [long](($sizes | Measure-Object -Maximum).Maximum)
if ($largest -le $romfsBytes) {
    throw ("the largest NCA is {0:N0} bytes, which cannot hold the {1:N0} byte RomFS tree -- the RomFS is not in the package" -f $largest, $romfsBytes)
}

Copy-Item -LiteralPath $built.FullName -Destination $Out -Force

$info = Get-Item -LiteralPath $Out
Write-Host ("NSP      : {0}" -f $Out)
Write-Host ("size     : {0:N0} bytes" -f $info.Length)
Write-Host ("sha256   : {0}" -f (Get-FileHash -LiteralPath $Out -Algorithm SHA256).Hash)
Write-Host ("entries  : {0}" -f ($names -join ', '))
for ($index = 0; $index -lt $count; $index++) {
    Write-Host ("  NCA    : {0}  {1:N0} bytes" -f $names[$index], $sizes[$index])
}
Write-Host ("romfs    : {0} files, {1:N0} bytes (inside the Program NCA)" -f $romfsFiles.Count, $romfsBytes)
