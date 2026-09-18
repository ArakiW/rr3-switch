[CmdletBinding()]
param(
    [string]$AdbPath,
    [int]$LogSeconds = 20
)

$ErrorActionPreference = 'Stop'
$packageName = 'com.ea.games.r3_row'
$projectRoot = Split-Path -Parent $PSScriptRoot
$artifactRoot = Join-Path $projectRoot 'artifacts'
if (-not $AdbPath) {
    $command = Get-Command adb -ErrorAction SilentlyContinue
    $AdbPath = if ($command) { $command.Source } else { "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" }
}
if (-not (Test-Path $AdbPath -PathType Leaf)) {
    throw "adb not found at $AdbPath"
}
if ($LogSeconds -lt 5 -or $LogSeconds -gt 60) {
    throw 'LogSeconds must be between 5 and 60.'
}

New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$reportPath = Join-Path $artifactRoot "rr3-switch-android-$stamp.txt"

$report = [Collections.Generic.List[string]]::new()
$report.Add("Captured: $(Get-Date -Format o)")
$report.Add('=== DEVICE ===')
$report.Add((& $AdbPath shell getprop ro.product.model).Trim())
$report.Add((& $AdbPath shell getprop ro.build.fingerprint).Trim())
$report.Add("ABI: $((& $AdbPath shell getprop ro.product.cpu.abi).Trim())")
$report.Add("SDK: $((& $AdbPath shell getprop ro.build.version.sdk).Trim())")
$report.Add('=== EGL/GLES ===')
$report.AddRange([string[]](& $AdbPath shell dumpsys SurfaceFlinger | Select-String -Pattern 'GLES|EGL|GL_' | ForEach-Object Line))
$report.Add('=== PACKAGE ===')
$report.AddRange([string[]](& $AdbPath shell dumpsys package $packageName | Select-String -Pattern 'versionName|versionCode|primaryCpuAbi|dataDir|userId' | ForEach-Object Line))

& $AdbPath logcat -c
& $AdbPath shell am force-stop $packageName
& $AdbPath shell monkey -p $packageName -c android.intent.category.LAUNCHER 1 | Out-Null
Start-Sleep -Seconds 3
$pid = (& $AdbPath shell pidof $packageName).Trim()
$report.Add('=== PROCESS ===')
$report.Add("PID: $pid")

if ($pid) {
    $logJob = Start-Job -ScriptBlock {
        param($Adb, $ProcessId)
        & $Adb logcat --pid=$ProcessId -v threadtime
    } -ArgumentList $AdbPath, $pid
    Start-Sleep -Seconds $LogSeconds
    Stop-Job $logJob
    $report.Add('=== LOGCAT ===')
    $report.AddRange([string[]](Receive-Job $logJob))
    Remove-Job $logJob
}
else {
    $report.Add('Process exited before log capture; collecting crash buffer.')
    $report.AddRange([string[]](& $AdbPath logcat -b crash -d -v threadtime))
}

$report | Set-Content -Path $reportPath -Encoding utf8
Write-Host "Runtime report: $reportPath" -ForegroundColor Green

