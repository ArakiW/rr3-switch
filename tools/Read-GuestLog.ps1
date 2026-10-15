<#
.SYNOPSIS
    Summarise a guest-view.log, whether it came from Eden or from a real card.

.DESCRIPTION
    Every metric here was chosen because it decided something. A metric that has
    never changed a decision does not belong in this file, and a count of 0 is
    printed as 0 -- it means "none seen in THIS log", not "never happens".

    The four that carry the most weight:

      asset-singleton verdict=  PRESENT / NULL-FIRST-WORD / NULL-SINGLETON.
          The asset manager's first word. It read 0 for an entire session and
          that 0 is what raised the fatal data-load dialog. PRESENT means the
          asset pipeline came up.

      render-call-pre / -post  pre is emitted on the FIRST frame-loop draw call
          only, and post is the literal next statement. pre>0 with post=0 means
          that call never returned.

      mmap-file-ok / -fail     the file-backed mapping probe. "fail" lines name
          the reason (zero-length, prot-exec, table-full, read, lseek, memalign)
          -- read those before theorising about a mapping failure.

      stat-layout o30_is_size  whether the guest's +0x30 stat offset actually
          holds the real file size. 0 on every line is a stat ABI mismatch.

.PARAMETER Path
    Path to rr3-native-rehost.guest-view.log. Accepts a bare file, a directory
    containing one, or an .build\eden-runs\<label> archive directory.

.PARAMETER Top
    How many rows in each phase histogram. Default 15.

.EXAMPLE
    .\tools\Read-GuestLog.ps1 -Path .build\eden-runs\20261004-094723-nettoffline
    .\tools\Read-GuestLog.ps1 -Path F:\switch\rr3-native-rehost\rr3-native-rehost.guest-view.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [int]$Top = 15
)

$ErrorActionPreference = 'Stop'

function Resolve-GuestLog([string]$given) {
    # NOT named $input: that is a PowerShell automatic variable, and using it as
    # a parameter name shadows the pipeline enumerator and silently returns
    # nothing here.
    if (Test-Path -LiteralPath $given -PathType Container) {
        $found = Join-Path $given 'rr3-native-rehost.guest-view.log'
        if (Test-Path -LiteralPath $found -PathType Leaf) { return $found }
        throw "No rr3-native-rehost.guest-view.log under $given"
    }
    if (Test-Path -LiteralPath $given -PathType Leaf) { return $given }
    throw "Not found: $given"
}

$log = Resolve-GuestLog $Path
$text = [IO.File]::ReadAllText($log)

function Count-Of([string]$needle) { return ([regex]::Matches($text, [regex]::Escape($needle))).Count }
function Histogram([string]$pattern) {
    return [regex]::Matches($text, $pattern) | ForEach-Object { $_.Groups[1].Value } |
        Group-Object | Sort-Object Count -Descending
}

Write-Host ''
Write-Host "log      : $log"
Write-Host ("size     : {0} bytes, {1} lines" -f (Get-Item -LiteralPath $log).Length,
            ([regex]::Matches($text, "`n")).Count)
Write-Host ''

# --- the four load-bearing metrics ------------------------------------------
Write-Host '== load-bearing ==' -ForegroundColor Cyan
# Anchored on the line's own shape. A bare `verdict=(\S+)` matches unrelated
# lines earlier in the log and reports their value as the asset verdict.
$singleton = [regex]::Match($text, 'asset-singleton:.*?first_word=(\S+)\s+verdict=(\S+)')
if ($singleton.Success) {
    $colour = if ($singleton.Groups[2].Value -eq 'PRESENT') { 'Green' } else { 'Red' }
    Write-Host ("  asset-singleton   : {0}  first_word={1}" -f `
        $singleton.Groups[2].Value, $singleton.Groups[1].Value) -ForegroundColor $colour
} else {
    Write-Host '  asset-singleton   : (no line -- the guest never got that far)' -ForegroundColor DarkYellow
}
Write-Host ("  render-call pre/post : {0} / {1}   (pre>0, post=0 => that call never returned)" -f `
    (Count-Of 'render-call-pre'), (Count-Of 'render-call-post'))
Write-Host ("  mmap ok / fail       : {0} / {1}" -f (Count-Of 'mmap-file-ok'), (Count-Of 'mmap-file-fail'))
Write-Host ("  stat o30_is_size=1/0 : {0} / {1}   (0 on every line => stat ABI mismatch)" -f `
    (Count-Of 'o30_is_size=1'), (Count-Of 'o30_is_size=0'))
Write-Host ''

# --- mmap failure reasons ----------------------------------------------------
$reasons = Histogram 'phase=mmap-file-fail.*?reason=(\S+)'
if ($reasons) {
    Write-Host '== mmap failure reasons ==' -ForegroundColor Cyan
    $reasons | Select-Object -First 10 | ForEach-Object { Write-Host ("  {0,6}  {1}" -f $_.Count, $_.Name) }
    Write-Host ''
}

# --- boot self-test ----------------------------------------------------------
$selfTest = [regex]::Match($text, 'Bionic misc selftest=(\S+) expected=(\S+)')
if ($selfTest.Success) {
    Write-Host ("  selftest  : {0}  expected={1}" -f $selfTest.Groups[1].Value, $selfTest.Groups[2].Value)
}

# --- what the guest is doing -------------------------------------------------
Write-Host ''
Write-Host '== guest activity ==' -ForegroundColor Cyan
foreach ($probe in @(
    @('shaders compiled',  'glShaderSource'),
    @('gfx calls',         'phase=gfx-proc'),
    @('fatal dialog',      'gate-show-message'),
    @('exitApp',           'gate-static-void'),
    @('jni static objects','jni-static-obj'),
    @('census samples',    'phase=census-ord'))) {
    Write-Host ("  {0,-18}: {1}" -f $probe[0], (Count-Of $probe[1]))
}
Write-Host ''

# --- routing failures: the ones that are OUR problem -------------------------
$routeFail = Select-String -LiteralPath $log -Pattern 'opendir-fail|fopen-fail' |
    ForEach-Object { ($_.Line -split ' ')[-1] } | Sort-Object -Unique
if ($routeFail) {
    Write-Host '== routing failures (ours to fix) ==' -ForegroundColor Cyan
    $routeFail | Select-Object -First $Top | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
}

# --- phase histogram ---------------------------------------------------------
Write-Host ("== top $Top phases ==") -ForegroundColor Cyan
Histogram 'phase=([A-Za-z0-9_:.-]+)' | Select-Object -First $Top |
    ForEach-Object { Write-Host ("  {0,6}  {1}" -f $_.Count, $_.Name) }
Write-Host ''

Write-Host 'Remember: a 0 here means "not in this log". Compare across runs, do not read it as "never happens".' -ForegroundColor DarkGray
Write-Host ''
