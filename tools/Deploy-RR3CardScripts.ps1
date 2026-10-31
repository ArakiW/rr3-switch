<#
.SYNOPSIS
    Deploy the guest's card-side CONTROL scripts onto an emulator's SD card.

.DESCRIPTION
    The port reads four control files from the card at
    <card>\switch\rr3-native-rehost\:

        rr3-touch.txt   the touch path: "at <frame> <x> <y> down|up"
        rr3-drive.txt   the closed-loop steering: enable/probe/base/sign
        rr3-pad.txt     the gamepad timeline: "[at <frame>] lx=.. rt=.. b=.."
        rr3-action.txt  the guest's own named commands (currently none)
        rr3-screen.txt  the screen size the port reports to the guest
        rr3-settings.txt the launcher-side settings, translated into the
                        guest's own encrypted device profile (eds/*.plist)

    They are EXPERIMENT INPUTS, not environment: a run is only reproducible
    if the inputs are on record with the result.  They live in the repository
    under native-switch\card\ and this script puts them on the card, which is
    the only supported way to write there (the host's own write policy refuses
    a direct write outside the workspace).

    The rules this script follows:

      1. EDEN MUST NOT BE RUNNING.  rr3-touch.txt is APPEND-ONLY for the port:
         the port reads the file's first 64 bytes as a signature and consumes
         complete lines from the offset it has reached, so APPENDING is safe
         while a run is live but REWRITING is not -- a changed signature makes
         the port replay every event it has already delivered.  Between runs
         the file may be replaced freely.  The gate is still here, because the
         moment the file is rewritten mid-run is the moment the run's input
         silently changes.
      2. PREFLIGHT every source file exists and is non-empty.
      3. COPY to a temporary name in the SAME directory, verify it there, then
         move it over the target.
      4. VERIFY length and sha256 of source and destination; any mismatch is a
         non-zero exit code.

    Nothing is written unless -Apply was passed.  The default run is a dry
    run: it prints the plan and changes nothing.

    EXIT CODES
      0  success (dry run completed, or apply finished and verified)
      1  preflight failed (a source file is missing or empty)
      2  Eden is running (nothing was written)
      3  a post-copy verification mismatch (source vs destination)
      4  an apply step failed after the plan was approved

.PARAMETER EmulatorRoot
    Eden's installation directory.  Default: E:\Eden.

.PARAMETER Apply
    Write the files.  Without it the script only prints the plan.

.PARAMETER ScreenView
    Override the size the port reports to the guest, e.g. "2048x1152" or
    "1280x720".  The three size lines of rr3-screen.txt (platform, ui and
    view) are rewritten to this one size.  The card file in the repository
    is the HD one, which is what the port ships; a measurement round that
    needs the OLD layout uses this switch instead of editing the file by
    hand, so the layout under test is always a parameter and never an
    uncommitted edit.  (The touch script's pixel coordinates were measured
    on the 1280x720 layout, so a round that has to drive into a race needs
    that one; the picture the port ships needs the 2048x1152 one.)

.PARAMETER TouchMode    "legacy" (the default) deploys the repository's rr3-touch.txt, whose
    hops are pixel coordinates measured on the 1280x720 layout and anchored
    to frame numbers.  "glove" deploys a COMMENTS-ONLY touch file instead:
    the file is still there, so the port's touch control owns the touch
    path, but it injects nothing until the host appends a line.  That is
    what tools\follow_glove.py does -- it watches the guest's own red glove
    cursor and appends a click where the guest wants to be pressed, which
    is immune to both the view size and the timing.

    The legacy script is timing-fragile: the hops are anchored to frame
    numbers, so when the guest loads slower than the round that measured
    them, every hop lands on the wrong widget and the guest never reaches
    the race.  Measured 2026-10-29: q12 spent its whole round in the front
    end for exactly this reason, while q10 and q11 with the same script
    did reach the race.

.PARAMETER SettingsFile
    Deploy THIS file as the card's rr3-settings.txt, instead of the
    repository's.  The settings file is the launcher-side one: the port
    translates it into the guest's own encrypted device profile
    (eds/*.plist), the way the PS Vita port translates its config.txt into
    eds/Vita*.plist.  The repository's copy ships NO settings at all -- a
    quality switch costs the guest a full shader recompile, so the default
    has to be "nothing happens" -- and this switch is how a round asks for
    something else without editing the repository.

.PARAMETER UiFile
    Deploy THIS file as the card's rr3-ui.txt, instead of the repository's.
    The ui file is the SEMANTIC UI request list: one control id per line
    ("BTN_STATS"), and the port presses exactly that control, at the
    rectangle the guest's own screen tree gives.  The repository's copy ships
    NO requests at all, because a request that names the wrong control
    presses the wrong thing -- and this switch is how a round asks for a
    named control without editing the repository.

.PARAMETER OpenFile
    Deploy THIS file as the card's rr3-open.txt, instead of the repository's.
    The open file is the SCREEN request list: one registered screen name per
    line ("CarPurchaseScreen"), and the port opens that screen through the
    guest's own OpenScreen().  The repository's copy ships NO requests at
    all, because opening the wrong screen takes the guest somewhere the
    round did not ask for.

.EXAMPLE
    .\Deploy-RR3CardScripts.ps1 -Apply
.EXAMPLE
    .\Deploy-RR3CardScripts.ps1 -ScreenView 1280x720 -Apply
.EXAMPLE
    .\Deploy-RR3CardScripts.ps1 -TouchMode glove -Apply
#>
[CmdletBinding()]
param(
    [string]$EmulatorRoot = 'E:\Eden',
    [switch]$Apply,
    [string]$ScreenView = '',
    [ValidateSet('legacy', 'glove')]
    [string]$TouchMode = 'legacy',
    [string]$SettingsFile = '',
    [string]$UiFile = '',
    [string]$OpenFile = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $projectRoot 'native-switch\card'
$appDir = Join-Path (Join-Path (Join-Path $EmulatorRoot 'user\sdmc') 'switch') 'rr3-native-rehost'

$scripts = @('rr3-touch.txt', 'rr3-drive.txt', 'rr3-pad.txt', 'rr3-action.txt',
    'rr3-screen.txt', 'rr3-settings.txt', 'rr3-ui.txt', 'rr3-open.txt')

# --- the screen override ------------------------------------------------------
#
# When -ScreenView is given, rr3-screen.txt is deployed with its three size
# lines rewritten.  The rewrite happens on a COPY inside the workspace, so
# the repository's file is never touched and the hash in the plan is the hash
# of what is actually written.
$screenOverrideFile = $null
if ($ScreenView -ne '') {
    if ($ScreenView -notmatch '^\d+x\d+$') {
        Write-Host ("refused  : -ScreenView '{0}' is not WIDTHxHEIGHT" -f $ScreenView)
        exit 1
    }
    $original = Join-Path $sourceRoot 'rr3-screen.txt'
    if (-not (Test-Path -LiteralPath $original -PathType Leaf)) {
        Write-Host ("preflight: MISSING rr3-screen.txt in {0}" -f $sourceRoot)
        exit 1
    }
    $scratchDir = Join-Path $projectRoot '.build'
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
    $screenOverrideFile = Join-Path $scratchDir ('rr3-screen-override-{0}.txt' -f ($ScreenView -replace '[^0-9x]', ''))
    $rewritten = foreach ($line in (Get-Content -LiteralPath $original)) {
        if ($line -match '^\s*(platform|ui|view)=') {
            "{0}={1}" -f $Matches[1], $ScreenView
        } else {
            $line
        }
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($screenOverrideFile, [string[]]$rewritten, $utf8NoBom)
    Write-Host ("screen  : -ScreenView {0} (the repository's rr3-screen.txt is NOT modified)" -f $ScreenView)
}

# --- the touch-mode override --------------------------------------------------
#
# "glove" deploys a COMMENTS-ONLY touch file.  The file is still on the card,
# so the port's touch control owns the touch path, but it injects nothing
# until the host appends a line -- which is what tools\follow_glove.py does.
$touchOverrideFile = $null
if ($TouchMode -eq 'glove') {
    $scratchDir = Join-Path $projectRoot '.build'
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
    $touchOverrideFile = Join-Path $scratchDir 'rr3-touch-glove.txt'
    $gloveText = @(
        '# The touch path is driven by the GUEST''S OWN RED GLOVE CURSOR.'
        '#'
        '# This file is deliberately EMPTY of events: the port injects one line'
        '# per line that is appended, and nothing is appended here.  The host'
        '# runs tools\follow_glove.py, which watches the port''s own framebuffer'
        '# captures, finds the glove -- the sprite the guest draws on the control'
        '# it wants pressed -- and appends a click there.'
        '#'
        '# Why not the hard-coded hops of the legacy script: those are pixel'
        '# coordinates measured on the 1280x720 layout AND anchored to frame'
        '# numbers, so they break when the view size changes and when the guest'
        '# loads slower than the round that measured them (2026-10-29: q12 spent'
        '# its whole round in the front end for exactly that reason).'
        '#'
        '# The format the port parses: [at <frame>] <x> <y> <action> [pointerId]'
    )
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($touchOverrideFile, [string[]]$gloveText, $utf8NoBom)
    Write-Host ("touch   : -TouchMode glove (a comments-only file; the host appends the clicks)")
}

Write-Host ("card     : {0}" -f $appDir)

# --- the port's own configuration ---------------------------------------------
#
# rr3-gfx.conf is the PORT's configuration (which backend, whether the
# socket stack comes up, how the Cloudcell calls are answered).  It lives one
# level above the app directory, and until now it existed ONLY on the card,
# which meant a round could not be reproduced from the repository.  It is
# kept here, and installed from here.
#
# nogo=1 is the important one: with the socket stack up, the guest's main
# thread parks in libnx send() waiting for a Cloudcell response that never
# comes, and the run never gets past the cloud sync -- measured 2026-10-29:
# the guest log stopped growing at 1.5 MB with not one frame rendered.  With
# no socket stack every outbound connection fails at once and the guest takes
# its own offline path.
#
# The port reads this file into a 4096-byte buffer and drops the rest in
# silence (conf_trunc=1), so keep it short.
$gfxConfSource = Join-Path $projectRoot 'tools\rr3-gfx.conf'
if (Test-Path -LiteralPath $gfxConfSource -PathType Leaf) {
    $gfxConfTarget = Join-Path (Split-Path -Parent $appDir) 'rr3-gfx.conf'
    $gfxBytes = (Get-Item -LiteralPath $gfxConfSource).Length
    if ($gfxBytes -gt 4096) {
        Write-Host ("refused  : tools\rr3-gfx.conf is {0} bytes; the port reads 4096 and drops the rest" -f $gfxBytes)
        exit 1
    }
    Copy-Item -LiteralPath $gfxConfSource -Destination $gfxConfTarget -Force
    Write-Host ("gfxconf : {0} bytes -> {1}" -f $gfxBytes, $gfxConfTarget)
}

# --- 1. preflight ------------------------------------------------------------
$plan = foreach ($name in $scripts) {
    $source = Join-Path $sourceRoot $name
    if ($name -eq 'rr3-screen.txt' -and $null -ne $screenOverrideFile) {
        $source = $screenOverrideFile
    }
    if ($name -eq 'rr3-touch.txt' -and $null -ne $touchOverrideFile) {
        $source = $touchOverrideFile
    }
    if ($name -eq 'rr3-settings.txt' -and $SettingsFile -ne '') {
        $source = $SettingsFile
    }
    if ($name -eq 'rr3-ui.txt' -and $UiFile -ne '') {
        $source = $UiFile
    }
    if ($name -eq 'rr3-open.txt' -and $OpenFile -ne '') {
        $source = $OpenFile
    }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        Write-Host ("preflight: MISSING {0} in {1}" -f $name, $sourceRoot)
        exit 1
    }
    $info = Get-Item -LiteralPath $source
    if ($info.Length -eq 0) {
        Write-Host ("preflight: EMPTY {0}" -f $name)
        exit 1
    }
    [pscustomobject]@{
        Name   = $name
        Source = $source
        Target = Join-Path $appDir $name
        Bytes  = $info.Length
        Sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    }
}
foreach ($entry in $plan) {
    $state = 'absent'
    if (Test-Path -LiteralPath $entry.Target -PathType Leaf) {
        $state = 'present'
    }
    Write-Host ("plan     : {0,-16} {1,7} bytes  sha256={2}  target={3}" -f `
        $entry.Name, $entry.Bytes, $entry.Sha256.Substring(0, 16), $state)
}
if (-not $Apply) {
    Write-Host "dry run  : nothing written (pass -Apply to deploy)"
    exit 0
}

# --- 2. the emulator must not be running -------------------------------------
$running = @(Get-Process -Name 'eden' -ErrorAction SilentlyContinue)
if ($running.Count -ne 0) {
    Write-Host ("refused  : {0} Eden process(es) are running; the control files must not be rewritten mid-run" -f $running.Count)
    exit 2
}

# --- 3. copy -----------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $appDir | Out-Null
foreach ($entry in $plan) {
    $temporary = "$($entry.Target).deploy-tmp"
    try {
        Copy-Item -LiteralPath $entry.Source -Destination $temporary -Force
        $copied = Get-FileHash -LiteralPath $temporary -Algorithm SHA256
        if ($copied.Hash -ne $entry.Sha256) {
            Write-Host ("FAILED   : {0} did not survive the copy to {1}" -f $entry.Name, $temporary)
            exit 4
        }
        Move-Item -LiteralPath $temporary -Destination $entry.Target -Force
    }
    catch {
        Write-Host ("FAILED   : {0}: {1}" -f $entry.Name, $_.Exception.Message)
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
        exit 4
    }
}

# --- 4. verify ---------------------------------------------------------------
$bad = 0
foreach ($entry in $plan) {
    $written = Get-FileHash -LiteralPath $entry.Target -Algorithm SHA256
    $size = (Get-Item -LiteralPath $entry.Target).Length
    $ok = ($written.Hash -eq $entry.Sha256) -and ($size -eq $entry.Bytes)
    if (-not $ok) {
        $bad++
    }
    Write-Host ("verify   : {0,-16} {1,7} bytes  sha256={2}  {3}" -f `
        $entry.Name, $size, $written.Hash.Substring(0, 16), $(if ($ok) { 'OK' } else { 'MISMATCH' }))
}
if ($bad -ne 0) {
    Write-Host ("FAILED   : {0} file(s) do not match the source" -f $bad)
    exit 3
}
Write-Host ("deployed : {0} control scripts, {0}/{0} sha256 match" -f $plan.Count)
exit 0
