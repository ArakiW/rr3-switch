[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$StagePath,

    [string]$AdbPath,

    [switch]$AllowRootAdb
)

$ErrorActionPreference = 'Stop'
$packageName = 'com.ea.games.r3_row'
$stageFull = [IO.Path]::GetFullPath($StagePath)
$filesPath = Join-Path $stageFull 'files'
if (-not (Test-Path (Join-Path $filesPath '.depot') -PathType Container)) {
    throw 'StagePath must contain files/.depot.'
}
if (-not $AllowRootAdb) {
    throw 'Private app data deployment is disabled by default. Pass -AllowRootAdb only on a device you control whose adbd supports root.'
}

if (-not $AdbPath) {
    $command = Get-Command adb -ErrorAction SilentlyContinue
    $AdbPath = if ($command) { $command.Source } else { "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" }
}
if (-not (Test-Path $AdbPath -PathType Leaf)) {
    throw "adb not found at $AdbPath"
}

& $AdbPath root
if ($LASTEXITCODE -ne 0) {
    throw 'This Android build does not allow adb root. No data was copied.'
}
& $AdbPath wait-for-device
$identity = (& $AdbPath shell id).Trim()
if ($identity -notmatch 'uid=0\(root\)') {
    throw "adbd is not root: $identity"
}

$packageInfo = & $AdbPath shell cmd package list packages -U $packageName
if ($packageInfo -notmatch 'uid:(\d+)') {
    throw 'The app is not installed or its uid could not be determined.'
}
$uid = $Matches[1]
$dataRoot = "/data/user/0/$packageName"

& $AdbPath shell am force-stop $packageName
& $AdbPath shell mkdir -p "$dataRoot/files"
if ($LASTEXITCODE -ne 0) {
    throw 'Could not create the app files directory.'
}

Write-Host "Copying staged data to $dataRoot/files ..."
& $AdbPath push "$filesPath\." "$dataRoot/files/"
if ($LASTEXITCODE -ne 0) {
    throw 'Data copy failed.'
}

& $AdbPath shell chown -R "$uid`:$uid" "$dataRoot/files"
& $AdbPath shell restorecon -RF "$dataRoot/files"
Write-Host 'Data deployment completed. Launch and collect runtime proof next.' -ForegroundColor Green

