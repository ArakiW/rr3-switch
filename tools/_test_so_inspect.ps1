# Inspect MT_GL context in SO
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# MT_GL at 0x47D8D3
$offset = 0x47D8D3
Write-Output "=== MT_GL string at 0x$($offset.ToString('X')) ==="
Write-Output "Context (-16 to +64):"
$start = [Math]::Max(0, $offset - 16)
$end = [Math]::Min($so.Length - 1, $offset + 64)
$ctx = $so[$start..$end]
Write-Output "  Hex: $([BitConverter]::ToString($ctx))"
$text = [System.Text.Encoding]::ASCII.GetString($ctx)
Write-Output "  Text: '$text'"

# Get more context (find string boundaries)
Write-Output "`n=== Full string at MT_GL location ==="
$i = $offset
while ($i -gt 0 -and $so[$i-1] -ge 0x20 -and $so[$i-1] -lt 0x7f) { $i-- }
$j = $offset
while ($j -lt $so.Length -and $so[$j] -ge 0x20 -and $so[$j] -lt 0x7f) { $j++ }
$fullstr = [System.Text.Encoding]::ASCII.GetString($so[$i..($j-1)])
Write-Output "  string: '$fullstr'"
Write-Output "  string offset: 0x$($i.ToString('X'))"
Write-Output "  string length: $($j - $i)"

# Look for nearby strings
Write-Output "`n=== Surrounding context (256 bytes) ==="
$start = [Math]::Max(0, $offset - 64)
$end = [Math]::Min($so.Length - 1, $offset + 256)
$surrounding = $so[$start..$end]
$hex = ($surrounding | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"

# Try parsing as printable
$sb = New-Object System.Text.StringBuilder
for ($i = $start; $i -le $end; $i++) {
    if ($so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) { [void]$sb.Append([char]$so[$i]) }
    else { [void]$sb.Append('.') }
}
Write-Output "  text: $($sb.ToString())"

# Look for the .vsh reference sites - first one at 0x398CDB
Write-Output "`n=== .vsh sites ==="
$vshPattern = [System.Text.Encoding]::ASCII.GetBytes('.vsh')
$pos = 0
$count = 0
while ($pos -lt $so.Length - 4) {
    $idx = [System.Array]::IndexOf($so, $vshPattern[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $vshPattern.Length; $j++) {
        if ($so[$idx + $j] -ne $vshPattern[$j]) { $match = $false; break }
    }
    if ($match) {
        # Get more context
        $i = $idx
        while ($i -gt 0 -and $so[$i-1] -ge 0x20 -and $so[$i-1] -lt 0x7f) { $i-- }
        $j = $idx
        while ($j -lt $so.Length -and $so[$j] -ge 0x20 -and $so[$j] -lt 0x7f) { $j++ }
        $str = [System.Text.Encoding]::ASCII.GetString($so[$i..($j-1)])
        Write-Output ("  @ 0x{0:X}  string: '{1}'" -f $idx, $str)
        $count++
        if ($count -ge 8) { break }
        $pos = $idx + 1
    } else {
        $pos = $idx + 1
    }
}

# Look for #version string references
Write-Output "`n=== '#version' references ==="
$verPattern = [System.Text.Encoding]::ASCII.GetBytes('#version')
$pos = 0
$count = 0
while ($pos -lt $so.Length - 8) {
    $idx = [System.Array]::IndexOf($so, $verPattern[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $verPattern.Length; $j++) {
        if ($so[$idx + $j] -ne $verPattern[$j]) { $match = $false; break }
    }
    if ($match) {
        $i = [Math]::Max(0, $idx - 16)
        $j = [Math]::Min($so.Length - 1, $idx + 32)
        $str = [System.Text.Encoding]::ASCII.GetString($so[$i..$j])
        Write-Output ("  @ 0x{0:X}  context: '{1}'" -f $idx, $str)
        $count++
        if ($count -ge 5) { break }
        $pos = $idx + 1
    } else {
        $pos = $idx + 1
    }
}

# Look for "version 100" string (already compiled into a GLSL header?)
Write-Output "`n=== 'version 100' literal ==="
$ver100 = [System.Text.Encoding]::ASCII.GetBytes('version 100')
$pos = 0
$count = 0
while ($pos -lt $so.Length - 11) {
    $idx = [System.Array]::IndexOf($so, $ver100[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $ver100.Length; $j++) {
        if ($so[$idx + $j] -ne $ver100[$j]) { $match = $false; break }
    }
    if ($match) {
        $i = [Math]::Max(0, $idx - 32)
        $j = [Math]::Min($so.Length - 1, $idx + 64)
        $str = [System.Text.Encoding]::ASCII.GetString($so[$i..$j])
        Write-Output ("  @ 0x{0:X}  context: '{1}'" -f $idx, $str)
        $count++
        if ($count -ge 5) { break }
        $pos = $idx + 1
    } else {
        $pos = $idx + 1
    }
}

# Search for AES tables (used by AES-128) in the SO .rodata
Write-Output "`n=== AES S-box first 16 bytes (0x63,0x7c,0x77,0x7b,...) ==="
$aesSbox = [byte[]]@(0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76)
$pos = 0
$count = 0
while ($pos -lt $so.Length - 16) {
    $idx = [System.Array]::IndexOf($so, $aesSbox[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $aesSbox.Length; $j++) {
        if ($so[$idx + $j] -ne $aesSbox[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  AES S-box found at 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 3) { break }
        $pos = $idx + 1
    } else {
        $pos = $idx + 1
    }
}
Write-Output "  AES S-box occurrences: $count"