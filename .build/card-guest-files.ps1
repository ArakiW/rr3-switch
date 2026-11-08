# Move the guest's files off the card, so a build can be tested against a
# card that has NOTHING of EA's on it.
#
# WHY: the clean loader's whole promise is that the NRO carries no EA
# material and the user supplies the rest.  A test with the files still on
# the card does not prove that, because the port would find them either way.
# The interesting case is the card as it comes out of the box: the NRO, the
# control scripts and nothing else.
#
# Usage:
#   powershell -File .build\card-guest-files.ps1 -Off    (move them aside)
#   powershell -File .build\card-guest-files.ps1 -Back   (put them back)
param(
    [switch]$Off,
    [switch]$Back
)

$ErrorActionPreference = 'Stop'
$app = 'E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost'
$stash = 'E:\AI\RealRacing3-switch\.build\card-guest-files-stash'

$items = @('libRealRacing3.so', 'libNimble.so', 'libfmodex.so', 'ui', 'original')

if ($Off) {
    New-Item -ItemType Directory -Force -Path $stash | Out-Null
    foreach ($name in $items) {
        $src = Join-Path $app $name
        if (Test-Path -LiteralPath $src) {
            $dst = Join-Path $stash $name
            if (Test-Path -LiteralPath $dst) {
                Write-Host ("  already stashed: {0}" -f $name)
                continue
            }
            Move-Item -LiteralPath $src -Destination $dst
            Write-Host ("  stashed: {0}" -f $name)
        }
    }
    Write-Host 'the card now carries the port only: no EA material at all.'
    exit 0
}

if ($Back) {
    foreach ($name in $items) {
        $src = Join-Path $stash $name
        if (Test-Path -LiteralPath $src) {
            $dst = Join-Path $app $name
            Move-Item -LiteralPath $src -Destination $dst
            Write-Host ("  restored: {0}" -f $name)
        }
    }
    Write-Host 'the card carries the guest files again.'
    exit 0
}

Write-Host 'usage: card-guest-files.ps1 -Off | -Back'
exit 1
