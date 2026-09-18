[CmdletBinding()]
param(
    [string]$AdbPath,
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$packageName = 'com.ea.games.r3_row'
$apk = Get-ChildItem $projectRoot -Filter *.apk | Select-Object -First 1
if (-not $apk) {
    throw "No APK found in $projectRoot"
}

if (-not $AdbPath) {
    $command = Get-Command adb -ErrorAction SilentlyContinue
    if ($command) {
        $AdbPath = $command.Source
    }
    else {
        $AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
    }
}
if (-not (Test-Path $AdbPath -PathType Leaf)) {
    throw "adb not found at $AdbPath"
}

$deviceLines = & $AdbPath devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
if (@($deviceLines).Count -ne 1) {
    throw "Expected exactly one authorized Android device; found $(@($deviceLines).Count)."
}

$abi = (& $AdbPath shell getprop ro.product.cpu.abi).Trim()
$sdk = [int]((& $AdbPath shell getprop ro.build.version.sdk).Trim())
$model = (& $AdbPath shell getprop ro.product.model).Trim()
$fingerprint = (& $AdbPath shell getprop ro.build.fingerprint).Trim()
Write-Host "Device: $model"
Write-Host "ABI: $abi"
Write-Host "Android API: $sdk"
Write-Host "Build: $fingerprint"

if ($sdk -lt 26) {
    throw 'The APK requires Android API 26 or newer.'
}
if ($abi -notmatch 'arm64') {
    throw "Expected an arm64 device, got $abi"
}

& $AdbPath install -r $apk.FullName
if ($LASTEXITCODE -ne 0) {
    throw 'APK installation failed.'
}

if ($Launch) {
    & $AdbPath shell am force-stop $packageName
    & $AdbPath shell monkey -p $packageName -c android.intent.category.LAUNCHER 1
    if ($LASTEXITCODE -ne 0) {
        throw 'APK installed, but launch failed.'
    }
}

Write-Host 'APK deployment completed. Runtime behavior is not yet verified.' -ForegroundColor Green

