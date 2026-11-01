<#
.SYNOPSIS
    Switch the port between the emulator configuration and the console one.

.DESCRIPTION
    sdmc:/switch/rr3-gfx.conf is read ONCE per process
    (gfx_backend.cpp: resolveBackend/readConf) and it carries THREE kinds of
    settings.  The old version of this script only knew about the first one:

      1. WHICH BACKEND.  An absent file, an absent backend= key, or any
         value other than null/0/on selects MESA (nouveau/DRM, NVN on
         Maxwell) -- the path real hardware needs.  Only a literal
         "backend=null" selects the stub backend that answers every GL call
         from a table and never draws.

      2. HOW THE GUEST BEHAVES.  ccdisp / ccloginok / ccsocialok / objid.
         These decide whether the guest gets past its cloud-sync spin and
         reaches the race at all; with them off the guest loops on
         "Sub Sync Queued: 10765" and the run dies at frame 4.

      3. WHERE THE PORT MAY TOUCH THE HARDWARE.  nofb / nogo / mkdiridem.

    So DELETING the file is not "the hardware mode": it is Mesa PLUS four
    guest-behaviour switches turned off, which is a boot that never reaches
    the race.  This script therefore INSTALLS the right file for the target
    instead of removing it.

.PARAMETER Mode
    hardware  - install tools\rr3-gfx-hardware.conf (the console file).
    eden      - install tools\rr3-gfx.conf (the emulator file).
    status    - report what is live, without changing anything.

.PARAMETER CardRoot
    The card root -- the folder that CONTAINS `switch\`.  Defaults to Eden's
    own SD card.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\Use-Real-Hardware.ps1 -Mode status
    powershell -ExecutionPolicy Bypass -File tools\Use-Real-Hardware.ps1 -Mode hardware
#>
[CmdletBinding()]
param(
    [ValidateSet('hardware', 'eden', 'status')]
    [string]$Mode = 'status',
    [string]$CardRoot = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

if ($CardRoot -eq '') {
    $CardRoot = Join-Path 'E:\Eden' 'user\sdmc'
}
$confDir = Join-Path $CardRoot 'switch'
$conf = Join-Path $confDir 'rr3-gfx.conf'

function Get-ConfSettings {
    $result = @{
        present = $false
        backend = '(unset)'
        null    = $false
        keys    = @{}
        bytes   = 0
    }
    if (-not (Test-Path -LiteralPath $conf -PathType Leaf)) {
        return $result
    }
    $result.present = $true
    $result.bytes = (Get-Item $conf).Length
    foreach ($line in (Get-Content -LiteralPath $conf)) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        if ($t -match '^([A-Za-z0-9_-]+)\s*=\s*(.*)$') {
            $result.keys[$Matches[1].ToLower()] = $Matches[2].Trim()
        }
        elseif ($t -notmatch '=') {
            # the original one-word spelling: a bare line IS the backend name
            $result.backend = $t
        }
    }
    if ($result.keys.ContainsKey('backend')) {
        $result.backend = $result.keys['backend']
    }
    $result.null = ($result.backend -in @('null', '0', 'on'))
    return $result
}

function Show-Status {
    $s = Get-ConfSettings
    Write-Host ("conf      : {0}" -f $(if ($s.present) { $conf } else { "$conf (absent)" }))
    Write-Host ("bytes     : {0}" -f $s.bytes)
    Write-Host ("backend   : {0}" -f $s.backend)
    Write-Host ("effective : {0}" -f $(if ($s.null) { 'NULL (never draws)' } else { 'mesa (nouveau/DRM)' }))
    foreach ($key in @('ccdisp', 'ccloginok', 'ccsocialok', 'objid', 'nofb', 'nogo', 'mkdiridem', 'ccmode')) {
        $value = if ($s.keys.ContainsKey($key)) { $s.keys[$key] } else { '(unset)' }
        Write-Host ("  {0,-11}: {1}" -f $key, $value)
    }
    if (-not $s.present -or -not $s.keys.ContainsKey('ccdisp')) {
        Write-Warning ("ccdisp is unset: the managed (JNI) methods are NOT dispatched, so the guest" )
        Write-Warning "cannot get past its cloud-sync spin and will never reach the race."
        Write-Warning "Install the whole file instead of deleting it:"
        Write-Warning "  powershell -ExecutionPolicy Bypass -File tools\Use-Real-Hardware.ps1 -Mode hardware"
    }
    if ($s.null) {
        Write-Warning "the NULL backend is active: nothing is ever drawn. Never on a console."
    }
}

switch ($Mode) {
    'status' { Show-Status }
    'hardware' {
        # There is no separate console configuration: the file the measured
        # rounds ran IS the console configuration.  The two hardware-specific
        # variations that were hypothesised (nogo=1, mkdiridem=1) were
        # MEASURED and REJECTED -- q106 ran with them and the guest never
        # reached "CC: AssetManager::StartDownloadService()" (q105: 301
        # 'download' log lines, q106: 0), the scene framebuffer stayed 0x0
        # and nothing was drawn.  So the shipped file is the measured one.
        $source = Join-Path $projectRoot 'tools\rr3-gfx.conf'
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "missing: $source" }
        New-Item -ItemType Directory -Force -Path $confDir | Out-Null
        Copy-Item -LiteralPath $source -Destination $conf -Force
        Write-Host ("installed : {0} ({1} bytes) -- the measured configuration" -f $conf, (Get-Item $conf).Length)
        Show-Status
        Write-Host "restart the port: the backend is resolved once per process."
        Write-Host "note     : nogo=0 means the socket stack comes up. On a console with"
        Write-Host "           Wi-Fi, turn the Wi-Fi OFF first: EA's service is gone, and a"
        Write-Host "           connection that hangs is worse than one that fails fast."
    }
    'eden' {
        $source = Join-Path $projectRoot 'tools\rr3-gfx.conf'
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "missing: $source" }
        New-Item -ItemType Directory -Force -Path $confDir | Out-Null
        Copy-Item -LiteralPath $source -Destination $conf -Force
        Write-Host ("installed : {0} ({1} bytes) -- the measured configuration" -f $conf, (Get-Item $conf).Length)
        Show-Status
        Write-Host "restart the port: the backend is resolved once per process."
    }
}
