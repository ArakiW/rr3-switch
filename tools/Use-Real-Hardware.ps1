<#
.SYNOPSIS
    Switch the deployed port between the Eden/null backend and real hardware.

.DESCRIPTION
    gfx_backend.cpp picks its backend once per process, from this file:

        sdmc:/switch/rr3-gfx.conf

    Absent file, absent `backend=` key, or any value other than null/0/on means
    MESA -- which is the path real Switch hardware needs, because the NRO
    ships devkitPro's Mesa (nouveau/DRM) and the console is NVIDIA Maxwell
    driven through NVN.

    So the null backend is an EMULATOR opt-in, and only that. Leaving
    `backend=null` on a real console is the single easiest way to make the
    game come up on a black screen forever while the real path sits unused
    in the binary: the null backend answers every GL call from a stub table,
    so nothing is ever submitted.

    This script moves between the two modes and prints which one is live.
    It is reversible and only ever touches that one conf file.

.PARAMETER Mode
    hardware  - remove the override, so Mesa is selected. Use on a console.
    eden      - write backend=null. Use under Eden, where there is no DRM
                device and eglInitialize spins forever on the Mesa path.
    status    - report the current setting without changing anything.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\Use-Real-Hardware.ps1 -Mode status
    powershell -ExecutionPolicy Bypass -File tools\Use-Real-Hardware.ps1 -Mode hardware
#>
[CmdletBinding()]
param(
    [ValidateSet('hardware', 'eden', 'status')]
    [string]$Mode = 'status',
    [string]$EdenRoot = 'E:\Eden'
)

$ErrorActionPreference = 'Stop'

$confDir = Join-Path $EdenRoot 'user\sdmc\switch'
$conf = Join-Path $confDir 'rr3-gfx.conf'

function Get-BackendSetting {
    if (-not (Test-Path -LiteralPath $conf -PathType Leaf)) {
        return @{ present = $false; backend = $null; raw = $null }
    }
    $raw = (Get-Content -LiteralPath $conf -Raw)
    $line = ($raw -split "`n" |
        Where-Object { $_ -notmatch '^\s*#' -and $_ -notmatch '^\s*$' } |
        Select-Object -First 1)
    $backend = $null
    if ($line -match '^\s*backend\s*=\s*(\S+)\s*$') {
        $backend = $Matches[1]
    }
    elseif ($line) {
        # the original one-word spelling: a bare line IS the backend name
        $backend = $line.Trim()
    }
    return @{ present = $true; backend = $backend; raw = $raw }
}

$state = Get-BackendSetting
$effective = if (-not $state.present) { 'mesa' }
             elseif ($state.backend -in @('null', '0', 'on')) { 'null' }
             else { 'mesa' }

switch ($Mode) {
    'status' {
        Write-Host ("conf    : {0}" -f $(if ($state.present) { $conf } else { "$conf (absent)" }))
        Write-Host ("backend : {0}" -f $(if ($state.present -and $state.backend) { $state.backend } else { '(unset)' }))
        Write-Host ("effective: {0}" -f $effective)
        if ($effective -eq 'null') {
            Write-Warning "null backend is active: this is correct under Eden, and wrong on a console."
        }
        else {
            Write-Host "Mesa (nouveau/DRM) is active: this is correct on a console."
        }
    }
    'hardware' {
        if (Test-Path -LiteralPath $conf -PathType Leaf) {
            Remove-Item -LiteralPath $conf -Force
            Write-Host "removed  : $conf"
        }
        else {
            Write-Host "already absent: $conf"
        }
        Write-Host "effective: mesa (nouveau/DRM) -- the real Switch path."
        Write-Host "note     : re-deploy the NRO if it was built with a different conf default;"
        Write-Host "           the backend is resolved once per process, so restart the port."
    }
    'eden' {
        if (-not (Test-Path -LiteralPath $confDir -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $confDir | Out-Null
        }
        # No trailing newline: the original parser stripped only leading
        # whitespace, and a newline made every ordinary way of writing this
        # file silently fall back to Mesa. gfx_backend.cpp now strips both
        # ends, but keeping the file byte-minimal costs nothing.
        Set-Content -LiteralPath $conf -Value 'backend=null' -Encoding ascii -NoNewline
        Write-Host "wrote    : $conf"
        Write-Host "effective: null -- required under Eden (no DRM device; Mesa's"
        Write-Host "           eglInitialize spins forever there)."
    }
}
