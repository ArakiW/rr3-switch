# One round: build, gate, deploy the card inputs, run Eden, archive.
#
# WHY A SCRIPT: the steps have to run in this order and none of them may be
# piped or redirected (the gate's stderr is the gate's output, and a pipe
# would swallow it).  Running them by hand is how a round ends up measuring a
# stale NRO.
#
# The measurement this round is for: the splash title.
#
#   * the guest asks for "Player ID: CCDI123456789 / CCRR123456789" at size
#     140, which measures 2421 px on a 1280 px screen, and the title is
#     clipped at the right edge (measured: ink x 257..1279, 1398 px hidden);
#   * the port's renderToTexture() did NOT scale the glyphs by the guest's
#     scale factor, so any text drawn through that path came out 1/f too big;
#   * that path had no log line at all, so the new "glyph-rtt" line is what
#     tells us whether the guest uses it.

param(
    [int]$Seconds = 300,
    [string]$Tag = 'q133-rtt-scale'
)

$ErrorActionPreference = 'Stop'

$root = 'E:\AI\RealRacing3-switch'
$emulatorRoot = 'E:\Eden-rr3'

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ''
    Write-Host ("=== {0} ===" -f $Name)
    $t = [System.Diagnostics.Stopwatch]::StartNew()
    & $Body
    $t.Stop()
    Write-Host ("--- {0} done in {1:N0}s" -f $Name, $t.Elapsed.TotalSeconds)
}

# ---- 0. Eden must not be running ---------------------------------------------
$running = @(Get-Process -Name eden, eden-cli, eden-rr3, eden-rr3-cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw ("Eden is running: {0}" -f (($running | ForEach-Object { $_.ProcessName }) -join ', '))
}

Step 'build' {
    & (Join-Path $root 'tools\Build-NativeSwitch.ps1')
}
Step 'gate' {
    & (Join-Path $root '.build\gate.ps1')
}
Step 'deploy-romfs' {
    & (Join-Path $root '.build\deploy-card-romfs.ps1')
}
Step 'deploy-conf' {
    # The emulator's console configuration lives in tools\rr3-gfx-hardware.conf
    # when there is one.  There is not, today: the shipped file IS the
    # emulator's (nogo=0, mkdiridem=0), and the card already carries it.
    # A missing source is therefore NOT a failure -- it just means there is
    # nothing to deploy.
    $source = 'E:\AI\RealRacing3-switch\tools\rr3-gfx-hardware.conf'
    if (Test-Path -LiteralPath $source) {
        & (Join-Path $root '.build\deploy-card-conf.ps1')
    }
    else {
        Write-Host 'no console-only conf to deploy (the shipped one is the emulator build)'
    }
}
Step 'eden' {
    & (Join-Path $root 'tools\Test-OnEden.ps1') -EmulatorRoot $emulatorRoot `
        -EmulatorExe eden-rr3.exe -EmulatorName eden-rr3 -Seconds $Seconds -Tag $Tag
}

Write-Host ''
Write-Host 'ROUND COMPLETE'
