# Faster SO string search using IndexOf
$ErrorActionPreference = 'Stop'

# Read the SO - this might be slow on Windows due to AV scanning
Write-Output "Reading SO..."
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')
$sw.Stop()
Write-Output "  Loaded in $($sw.ElapsedMilliseconds)ms, size: $($so.Length) bytes"

# Use Boyer-Moore-like search via .NET's IndexOf on byte arrays
function Find-Pattern([byte[]]$haystack, [byte[]]$needle) {
    $results = New-Object System.Collections.Generic.List[int]
    $pos = 0
    while ($pos -lt $haystack.Length - $needle.Length) {
        $idx = [System.Array]::IndexOf($haystack, $needle[0], $pos)
        if ($idx -lt 0) { break }
        # Check the rest of the pattern
        $match = $true
        for ($j=1; $j -lt $needle.Length -and ($idx + $j) -lt $haystack.Length; $j++) {
            if ($haystack[$idx + $j] -ne $needle[$j]) {
                $match = $false
                break
            }
        }
        if ($match) { $results.Add($idx) }
        $pos = $idx + 1
    }
    return $results
}

$patterns = @{
    'mtShader' = [System.Text.Encoding]::ASCII.GetBytes('mtShader')
    'mt3D' = [System.Text.Encoding]::ASCII.GetBytes('mt3D')
    'Shaders' = [System.Text.Encoding]::ASCII.GetBytes('Shaders')
    'firemonkeys_lower' = [System.Text.Encoding]::ASCII.GetBytes('firemonkeys')
    'firemonkeys_cap' = [System.Text.Encoding]::ASCII.GetBytes('Firemonkeys')
    '.vsh' = [System.Text.Encoding]::ASCII.GetBytes('.vsh')
    '.fsh' = [System.Text.Encoding]::ASCII.GetBytes('.fsh')
    'MT_GL' = [System.Text.Encoding]::ASCII.GetBytes('MT_GL')
    'csdk' = [System.Text.Encoding]::ASCII.GetBytes('csdk')
    'rr3' = [System.Text.Encoding]::ASCII.GetBytes('rr3')
    '1B:D' = [System.Text.Encoding]::ASCII.GetBytes('1B:D')
    'feats' = [System.Text.Encoding]::ASCII.GetBytes('feats')
    'aes' = [System.Text.Encoding]::ASCII.GetBytes('aes')
    'decrypt' = [System.Text.Encoding]::ASCII.GetBytes('decrypt')
    'cipher' = [System.Text.Encoding]::ASCII.GetBytes('cipher')
    'obfusc' = [System.Text.Encoding]::ASCII.GetBytes('obfusc')
    'binary' = [System.Text.Encoding]::ASCII.GetBytes('binary')
    'RC4' = [System.Text.Encoding]::ASCII.GetBytes('RC4')
    'r3_update' = [System.Text.Encoding]::ASCII.GetBytes('r3_update')
}

foreach ($name in $patterns.Keys) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $hits = Find-Pattern $so $patterns[$name]
    $sw.Stop()
    if ($hits.Count -gt 0) {
        Write-Output ("  {0,-12}  x{1,3}  first 5: {2}  ({3}ms)" -f $name, $hits.Count, ([string]::Join(',', ($hits | Select-Object -First 5 | ForEach-Object { '0x{0:X}' -f $_ }))), $sw.ElapsedMilliseconds)
    } else {
        Write-Output ("  {0,-12}  x{0,3}  not found" -f $name, 0)
    }
}