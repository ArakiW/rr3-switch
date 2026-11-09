# Enumerate the assets the GUEST ITSELF reported as unresolvable, and diff two rounds.
#
# WHY this exists: the port's own "asset-invariant"/"assetlist-census" lines only prove that
# assets listed in the asset lists were served with the right digest. They say nothing about
# assets the guest opened DIRECTLY (LoadReadOnlyMappedFile). The guest's own log lines
# ("LoadReadOnlyMappedFile failed getting full path: X") are the only lines in the whole
# archive that actually PROVE a file was not found. Every earlier magenta-car hypothesis was
# tested against the wrong lines, so this instrument reads those lines in full.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File extract-failed-assets.ps1 -Log <guest-view.log> [-Compare <other log>]
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [string]$Compare = ''
)

$ErrorActionPreference = 'Stop'

function Get-FailedAssets([string]$path) {
    $rx = [regex]'failed getting full path: (.+?)\s*$'
    $set = New-Object 'System.Collections.Generic.HashSet[string]'
    $reader = New-Object System.IO.StreamReader($path)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            $m = $rx.Match($line)
            if ($m.Success) { [void]$set.Add($m.Groups[1].Value.Trim()) }
        }
    } finally { $reader.Close() }
    return $set
}

if (-not (Test-Path -LiteralPath $Log)) { throw "log not found: $Log" }
$here = Get-FailedAssets $Log
Write-Output ("ROUND {0}" -f (Split-Path -Leaf (Split-Path -Parent $Log)))
Write-Output ("failed-assets-unique: {0}" -f $here.Count)
foreach ($p in ($here | Sort-Object)) { Write-Output ("  FAIL {0}" -f $p) }

$carish = $here | Where-Object { $_ -match '(?i)car|livery|vehicle' }
Write-Output ("failed-assets-car-ish: {0}" -f $carish.Count)
foreach ($p in ($carish | Sort-Object)) { Write-Output ("  CAR  {0}" -f $p) }

if ($Compare -ne '') {
    if (-not (Test-Path -LiteralPath $Compare)) { throw "compare log not found: $Compare" }
    $there = Get-FailedAssets $Compare
    Write-Output ("COMPARE {0} failed-assets-unique: {1}" -f (Split-Path -Leaf $Compare), $there.Count)
    $onlyHere = @($here | Where-Object { -not $there.Contains($_) } | Sort-Object)
    $onlyThere = @($there | Where-Object { -not $here.Contains($_) } | Sort-Object)
    Write-Output ("ONLY-IN-FIRST: {0}" -f $onlyHere.Count)
    foreach ($p in $onlyHere) { Write-Output ("  ONLY-A {0}" -f $p) }
    Write-Output ("ONLY-IN-SECOND: {0}" -f $onlyThere.Count)
    foreach ($p in $onlyThere) { Write-Output ("  ONLY-B {0}" -f $p) }
}
