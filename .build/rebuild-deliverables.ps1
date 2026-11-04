# Rebuild the release deliverables with the CURRENT build output.
#
# WHY: the package directory carried the NRO of an OLDER build (the one
# that crashed the console), because the package is refreshed by a separate
# step from the one that builds.  That is the same class of bug as the stale
# archive, one level up: a deliverable that does not contain the thing it
# claims to contain.
#
# So this runs the canonical packager (which copies the build output and
# recomputes the manifest), then the archive chain, and then it PROVES that
# the NRO in the package, in the app zip and on the card are the same file.
#
# Usage: powershell -File .build\rebuild-deliverables.ps1 -Stamp 20261103

param(
    [string]$Stamp = '20261103'
)

$ErrorActionPreference = 'Stop'

$root = 'E:\AI\RealRacing3-switch'
$cardNro = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-native-rehost.nro'
$buildNro = Join-Path $root 'native-switch\rr3-native-rehost.nro'
$pkgNro = Join-Path $root ("dist\rr3-switch-{0}\switch\rr3-native-rehost\rr3-native-rehost.nro" -f $Stamp)
$pkgApp = Join-Path $root ("dist\rr3-switch-{0}\switch\rr3-native-rehost" -f $Stamp)
$appZip = Join-Path $root ("dist\rr3-switch-app-{0}.zip" -f $Stamp)

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ''
    Write-Host ("=== {0} ===" -f $Name)
    & $Body
}

Step 'make-package' {
    & (Join-Path $root '.build\make-package-v2.ps1') -Stamp $Stamp
}
Step 'repack' {
    & (Join-Path $root '.build\run-repack.ps1') -Stamp $Stamp
}
Step 'prove' {
    $build = (Get-FileHash -LiteralPath $buildNro -Algorithm SHA256).Hash
    $pkg = (Get-FileHash -LiteralPath $pkgNro -Algorithm SHA256).Hash
    $card = (Get-FileHash -LiteralPath $cardNro -Algorithm SHA256).Hash
    Write-Host ("build : {0}" -f $build)
    Write-Host ("package: {0}" -f $pkg)
    Write-Host ("card  : {0}" -f $card)
    if ($build -ne $pkg) { throw 'the package does NOT contain the build output' }
    if ($build -ne $card) { throw 'the card does NOT contain the build output' }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($appZip)
    try {
        $entry = $zip.Entries | Where-Object {
            $_.FullName -replace '^\./', '' -eq 'switch/rr3-native-rehost/rr3-native-rehost.nro'
        }
        if ($null -eq $entry) { throw 'the app zip has no NRO' }
        $stream = $entry.Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { $app = ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '' }
            finally { $sha.Dispose() }
        }
        finally { $stream.Dispose() }
    }
    finally { $zip.Dispose() }
    Write-Host ("app zip: {0}" -f $app)
    if ($build.ToLowerInvariant() -ne $app) { throw 'the app zip does NOT contain the build output' }

    # THE SHIPPING INPUTS ARE CHECKED, NOT ASSUMED.
    #
    # MEASURED 2026-11-04: the packager copied the card's copy of the control
    # scripts OVER the repository's, because the live card files were written
    # after them -- the opposite of what the packager's own comment said.  The
    # card carried the EXPERIMENT pad script (the weave), so the release tree
    # would have shipped a script that holds the accelerator and, before the
    # input-path fix, ignores the player's controller entirely.
    #
    # ⇒ The packager's order is fixed, and these two files are checked here so
    # that a future order change cannot undo it silently.
    $pkgGamepad = Join-Path $pkgApp 'rr3-gamepad.txt'
    $pkgPad = Join-Path $pkgApp 'rr3-pad.txt'
    $repoGamepad = Join-Path $root 'native-switch\card\rr3-gamepad.txt'
    $repoPad = Join-Path $root 'native-switch\card\rr3-pad.txt'
    foreach ($pair in @(@($pkgGamepad, $repoGamepad), @($pkgPad, $repoPad))) {
        $packageFile = $pair[0]
        $repoFile = $pair[1]
        if (-not (Test-Path -LiteralPath $packageFile -PathType Leaf)) {
            throw ("the package is missing {0}" -f (Split-Path -Leaf $packageFile))
        }
        $inPackage = (Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash
        $inRepo = (Get-FileHash -LiteralPath $repoFile -Algorithm SHA256).Hash
        if ($inPackage -ne $inRepo) {
            $leaf = Split-Path -Leaf $packageFile
            throw ("the package's $leaf is NOT the repository's copy -- the packager's order is wrong again")
        }
        Write-Host ("{0,-18} package == repository  {1}" -f (Split-Path -Leaf $packageFile), $inPackage.Substring(0, 16))
    }
    $autopilot = (Select-String -LiteralPath $pkgGamepad -Pattern '^\s*autopilot\s*=\s*(\d+)' |
        Select-Object -First 1).Matches.Groups[1].Value
    if ($autopilot -ne '0') {
        throw ("the package's rr3-gamepad.txt says autopilot = {0}; the shipping default is 0 (the player owns the car)" -f $autopilot)
    }
    $padEvents = @(Select-String -LiteralPath $pkgPad -Pattern '^\s*\[').Count
    if ($padEvents -ne 0) {
        throw ("the package's rr3-pad.txt has {0} event line(s); the console's file must have none, or it takes the throttle away from the player" -f $padEvents)
    }
    Write-Host 'the shipping inputs are the repository''s, the player owns the car, and no script holds the throttle.'
    Write-Host ''
    Write-Host 'THE BUILD OUTPUT IS IN THE PACKAGE, IN THE APP ZIP AND ON THE CARD.'
}
