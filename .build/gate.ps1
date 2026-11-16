# The gate must not run a harness that cannot even PARSE.
#
# WHY THIS EXISTS.  2026-11-16: a change to the round census's print block
# put an apostrophe inside a single-quoted PowerShell string.  The file
# parsed with errors, and the failure surfaced TWELVE MINUTES LATER, in the
# middle of an Eden round, after the round had already deployed the build:
# "Unexpected token 's' in expression or statement".  The build, the tests
# and the symbols were all fine -- the gate never looked at the script that
# the round runs.  **=> A GATE THAT DOES NOT READ THE HARNESS IS NOT A
# GATE ON THE HARNESS.**  Parsing is cheap (milliseconds) and the failure it
# catches costs a whole round.
$ErrorActionPreference = 'Continue'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$root = 'E:\AI\RealRacing3-switch'

Write-Output "===== BUILD ====="
& "$root\tools\Build-NativeSwitch.ps1"
Write-Output "BUILD EXIT=$LASTEXITCODE"
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Output "===== POWERSHELL PARSE ====="
$parseFail = 0
$parsed = 0
foreach ($script in @(
        "$root\tools\Test-OnEden.ps1",
        "$root\tools\Build-NativeSwitch.ps1",
        "$root\tools\Test-RR3Inputs.ps1",
        "$root\.build\run-pinned-round.ps1",
        "$root\.build\guest-state.ps1",
        "$root\.build\deploy-card-romfs.ps1",
        "$root\.build\deploy-nro.ps1",
        "$root\.build\pack-app-dated.ps1")) {
    if (-not (Test-Path -LiteralPath $script)) { continue }
    $errs = $null
    $toks = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $script, [ref]$toks, [ref]$errs) | Out-Null
    $parsed++
    if ($errs.Count -gt 0) {
        $parseFail++
        Write-Output ("PARSE FAIL {0}" -f (Split-Path -Leaf $script))
        $errs | Select-Object -First 5 | ForEach-Object {
            Write-Output ("    " + $_.ToString())
        }
    }
}
Write-Output ("PS PARSE: {0} parsed, {1} failed" -f $parsed, $parseFail)
if ($parseFail -ne 0) { exit 1 }

Write-Output "===== INPUTS ====="
& "$root\tools\Test-RR3Inputs.ps1"
Write-Output "INPUTS EXIT=$LASTEXITCODE"

Write-Output "===== TESTS ====="
$fail = 0
$n = 0
Get-ChildItem "$root\native-switch\tests" -Filter 'test_*.py' | Sort-Object Name | ForEach-Object {
    $n++
    $out = python $_.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        $fail++
        Write-Output ("FAIL {0}" -f $_.Name)
        $out | Select-Object -Last 12 | ForEach-Object { Write-Output ("    " + $_) }
    }
}
Write-Output ("TESTS: {0} run, {1} failed" -f $n, $fail)

Write-Output "===== CC SYMBOLS ====="
python "$root\tools\check-cc-symbols.py"
Write-Output "SYMBOLS EXIT=$LASTEXITCODE"

if ($fail -ne 0) { exit 1 }
Write-Output "GATE OK"
