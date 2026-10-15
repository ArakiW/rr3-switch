# Investigate the #version 100 literal context
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

Write-Output "=== Full context around #version 100 at 0x3C13AC ==="
$start = [Math]::Max(0, 0x3C1300)
$end = [Math]::Min($so.Length - 1, 0x3C1500)
$ctx = $so[$start..$end]
$hex = ($ctx | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"

# Print as text with non-printable as .
$sb = New-Object System.Text.StringBuilder
for ($i = $start; $i -le $end; $i++) {
    if ($so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) { [void]$sb.Append([char]$so[$i]) }
    elseif ($so[$i] -eq 0x0a) { [void]$sb.Append('\n') }
    elseif ($so[$i] -eq 0x00) { [void]$sb.Append('|0|') }
    else { [void]$sb.Append('.') }
}
Write-Output "  text: $($sb.ToString())"

# Try to find full string boundaries
$idx = 0x3C13AC
$i = $idx
while ($i -gt 0 -and ($so[$i-1] -ge 0x20 -and $so[$i-1] -lt 0x7f)) { $i-- }
$j = $idx
while ($j -lt $so.Length -and ($so[$j] -ge 0x20 -and $so[$j] -lt 0x7f)) { $j++ }
Write-Output "`n  String at 0x$($idx.ToString('X')): '$([System.Text.Encoding]::ASCII.GetString($so[$i..($j-1)]))'"
Write-Output "  Start: 0x$($i.ToString('X'))"

# Look around for nearby null-terminated strings (likely string table)
Write-Output "`n=== Strings around #version 100 ==="
$idx = 0x3C1300
while ($idx -lt 0x3C1700) {
    # Find start of next string (skip null bytes)
    while ($idx -lt $so.Length -and $so[$idx] -eq 0x00) { $idx++ }
    if ($idx -ge 0x3C1700) { break }
    $start = $idx
    while ($idx -lt $so.Length -and $so[$idx] -ne 0x00) { $idx++ }
    if ($idx - $start -ge 3) {
        $str = [System.Text.Encoding]::ASCII.GetString($so[$start..($idx-1)])
        Write-Output ("  0x{0:X}  ({1,3} bytes)  '{2}'" -f $start, ($idx - $start), $str)
    }
}

# Search for "default.vert" or similar shader template names
Write-Output "`n=== Default/template shader names ==="
$patterns = @('default.vert', 'default.frag', 'common.glsl', 'common.inc', 'shader.glsl',
    'shader_header', 'shaderFooter', 'shaderHeader', 'header.glsl', 'glsl_prefix',
    'shader_src', 'shader_code', 'shader_prefix')
foreach ($p in $patterns) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($p)
    $hits = New-Object System.Collections.Generic.List[int]
    $pos = 0
    while ($pos -lt $so.Length - $bytes.Length) {
        $idx = [System.Array]::IndexOf($so, $bytes[0], $pos)
        if ($idx -lt 0) { break }
        $match = $true
        for ($j=1; $j -lt $bytes.Length; $j++) {
            if ($so[$idx + $j] -ne $bytes[$j]) { $match = $false; break }
        }
        if ($match) { $hits.Add($idx) }
        $pos = $idx + 1
    }
    if ($hits.Count -gt 0) {
        Write-Output ("  {0,-20}  x{1}  first @ 0x{2:X}" -f $p, $hits.Count, $hits[0])
    }
}

# Look for "ES3" or "100" alone
Write-Output "`n=== Other shader-related strings ==="
foreach ($p in @('shaderHeader', 'shaderPrefix', 'shader_header', 'shader_src',
    'OpenGL ES 3.0', 'GLES3', 'ES3', '#define precision', 'precision highp',
    '#version', 'precision mediump')) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($p)
    $hits = New-Object System.Collections.Generic.List[int]
    $pos = 0
    while ($pos -lt $so.Length - $bytes.Length) {
        $idx = [System.Array]::IndexOf($so, $bytes[0], $pos)
        if ($idx -lt 0) { break }
        $match = $true
        for ($j=1; $j -lt $bytes.Length; $j++) {
            if ($so[$idx + $j] -ne $bytes[$j]) { $match = $false; break }
        }
        if ($match) { $hits.Add($idx) }
        $pos = $idx + 1
    }
    if ($hits.Count -gt 0) {
        Write-Output ("  {0,-22}  x{1,3}" -f $p, $hits.Count)
    }
}

# Look for "r3_update" or "mt3D" path
Write-Output "`n=== Source paths ==="
foreach ($p in @('mt3D\\OpenGL', 'mtShader', 'mtShaderManager', 'OpenGL\\mt')) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($p)
    $hits = New-Object System.Collections.Generic.List[int]
    $pos = 0
    while ($pos -lt $so.Length - $bytes.Length) {
        $idx = [System.Array]::IndexOf($so, $bytes[0], $pos)
        if ($idx -lt 0) { break }
        $match = $true
        for ($j=1; $j -lt $bytes.Length; $j++) {
            if ($so[$idx + $j] -ne $bytes[$j]) { $match = $false; break }
        }
        if ($match) { $hits.Add($idx) }
        $pos = $idx + 1
    }
    if ($hits.Count -gt 0) {
        Write-Output ("  {0,-22}  x{1,3}" -f $p, $hits.Count)
    }
}