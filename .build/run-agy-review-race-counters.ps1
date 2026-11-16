$ErrorActionPreference = 'Continue'
$agy = 'C:\Users\L\AppData\Local\agy\bin\agy.exe'
$repo = 'E:\AI\RealRacing3-switch'
$spec = Join-Path $repo '.build\agy-review-race-counters.txt'

if (-not (Test-Path $agy)) { Write-Output "MISSING agy: $agy"; exit 2 }
if (-not (Test-Path $spec)) { Write-Output "MISSING spec: $spec"; exit 2 }

Set-Location $repo

$prompt = 'Read ' + $spec + ' and do exactly what it says.  READ ONLY: do not edit any file, do not build, do not run Eden, do not commit, do not push.  Report findings only.'

$args = @(
  '--model', 'gemini-3.8-flash-high',
  '--effort', 'high',
  '--mode', 'plan',
  '--dangerously-skip-permissions',
  '--print-timeout', '900s',
  '--print',
  $prompt
)

Write-Output ("START {0} model=gemini-3.8-flash-high effort=high mode=plan" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
& $agy @args
Write-Output ("EXIT {0}" -f $LASTEXITCODE)
Write-Output ("END {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
