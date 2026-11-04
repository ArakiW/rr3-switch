# Retry `git push` until the host can reach GitHub again.
#
# WHY: 2026-11-04, four pushes succeeded and then the host lost GitHub
# entirely -- both `git push` and a plain HTTPS HEAD to github.com failed
# with the connection closed mid-handshake.  That is the network, not the
# repository, and the commit is already made, so the only thing left is to
# wait and try again.
#
# The proxy is NOT set: with HTTP_PROXY/HTTPS_PROXY exported, git's schannel
# backend fails the handshake outright.  A direct connection works when the
# host's route to GitHub is up.
#
# Usage: powershell -File .build\retry-push.ps1 -MaxAttempts 40 -DelaySeconds 30

param(
    [int]$MaxAttempts = 40,
    [int]$DelaySeconds = 30,
    [string]$Remote = 'origin',
    [string]$Branch = 'master'
)

$ErrorActionPreference = 'Continue'
$root = 'E:\AI\RealRacing3-switch'

for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
    Write-Host ("attempt {0}/{1}  at {2}" -f $attempt, $MaxAttempts, (Get-Date -Format 'HH:mm:ss'))
    $output = & git -C $root -c http.proxy= -c https.proxy= push $Remote $Branch 2>&1
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host ("   {0}" -f $_) }
    if ($code -eq 0) {
        Write-Host 'PUSHED'
        exit 0
    }
    Write-Host ("   failed (exit {0}); waiting {1}s" -f $code, $DelaySeconds)
    Start-Sleep -Seconds $DelaySeconds
}

Write-Host 'GAVE UP: the commit is still local'
exit 1
