$ErrorActionPreference = 'Continue'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

$root = 'E:\AI\RealRacing3-switch'

Write-Output "===== BUILD ====="
& "$root\tools\Build-NativeSwitch.ps1"
Write-Output "BUILD EXIT=$LASTEXITCODE"
if ($LASTEXITCODE -ne 0) { exit 1 }

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
