# Try zlib decompression and other algorithms
$ErrorActionPreference = 'Stop'

$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')

Write-Output "Loaded $($ct.Length) bytes"

# Try various zlib interpretations
Write-Output "`n=== Zlib interpretations ==="

# Raw zlib (wbits=15)
try {
    $ms = New-Object System.IO.MemoryStream (,$ct)
    $zs = New-Object System.IO.Compression.ZLibStream $ms, ([System.IO.Compression.CompressionMode]::Decompress)
    $sr = New-Object System.IO.StreamReader $zs
    $result = $sr.ReadToEnd()
    Write-Output "  raw zlib: $($result.Length) bytes, preview: $([BitConverter]::ToString([System.Text.Encoding]::ASCII.GetBytes($result)[0..15]))"
    Write-Output "  text: $($result.Substring(0, [Math]::Min(50, $result.Length)))"
} catch {
    Write-Output "  raw zlib: FAILED"
}

# gzip (wbits=31)
try {
    $ms = New-Object System.IO.MemoryStream (,$ct)
    $zs = New-Object System.IO.Compression.GZipStream $ms, ([System.IO.Compression.CompressionMode]::Decompress)
    $sr = New-Object System.IO.StreamReader $zs
    $result = $sr.ReadToEnd()
    Write-Output "  gzip: $($result.Length) bytes"
} catch {
    Write-Output "  gzip: FAILED"
}

# deflate (wbits=-15)
try {
    $ms = New-Object System.IO.MemoryStream (,$ct)
    $zs = New-Object System.IO.Compression.DeflateStream $ms, ([System.IO.Compression.CompressionMode]::Decompress)
    $sr = New-Object System.IO.StreamReader $zs
    $result = $sr.ReadToEnd()
    Write-Output "  deflate: $($result.Length) bytes"
} catch {
    Write-Output "  deflate: FAILED"
}

# Skip first N bytes then zlib
foreach ($skip in @(0, 1, 2, 4, 6, 8)) {
    try {
        $ms = New-Object System.IO.MemoryStream (,$ct[$skip..($ct.Length-1)])
        $zs = New-Object System.IO.Compression.ZLibStream $ms, ([System.IO.Compression.CompressionMode]::Decompress)
        $sr = New-Object System.IO.StreamReader $zs
        $result = $sr.ReadToEnd()
        Write-Output "  zlib skip $skip`: $($result.Length) bytes, preview: $([BitConverter]::ToString([System.Text.Encoding]::ASCII.GetBytes($result)[0..15]))"
        if ($result.Length -gt 0) {
            Write-Output "    text: $($result.Substring(0, [Math]::Min(50, $result.Length)))"
        }
    } catch {
        Write-Output "  zlib skip $skip`: FAILED"
    }
}

# Check header bytes for various compression signatures
Write-Output "`n=== First 16 bytes of various encodings ==="
Write-Output "  Hex: $([BitConverter]::ToString($ct[0..15]))"

# Maybe it's LZMA?
Write-Output "`n=== LZMA test ==="
# PowerShell doesn't have built-in LZMA, but maybe SevenZip?

# Check entropy to see if it looks like random
Write-Output "`n=== Entropy analysis ==="
$freq = New-Object int[] 256
foreach ($b in $ct) { $freq[$b]++ }
$entropy = 0.0
foreach ($f in $freq) {
    if ($f -gt 0) {
        $p = $f / $ct.Length
        $entropy += $p * [Math]::Log($p, 2)
    }
}
Write-Output "  Shannon entropy: $(-1 * $entropy) bits/byte (max=8 for random)"

# Print byte frequency distribution
Write-Output "`n=== Byte value histogram (top 16) ==="
$idx = 0..255 | Sort-Object @{ Expression={ $freq[$_] }; Descending=$true }
for ($i=0; $i -lt 16; $i++) {
    Write-Output ("  {0:X2}: {1}" -f $idx[$i], $freq[$idx[$i]])
}

# Look for repeating patterns
Write-Output "`n=== Repeating byte patterns ==="
for ($period = 1; $period -le 16; $period++) {
    $matches = 0
    for ($i = $period; $i -lt [Math]::Min(64, $ct.Length); $i++) {
        if ($ct[$i] -eq $ct[$i % $period]) { $matches++ }
    }
    if ($matches -gt 30) {
        Write-Output "  period $period`: $matches matches"
    }
}

# Try XORing the file with itself shifted by N to find repeating keystream
Write-Output "`n=== Auto-correlation ==="
for ($shift = 1; $shift -le 32; $shift++) {
    $matches = 0
    $n = [Math]::Min(64, $ct.Length - $shift)
    for ($i=0; $i -lt $n; $i++) {
        if ($ct[$i] -eq $ct[$i + $shift]) { $matches++ }
    }
    if ($matches -gt 30) {
        Write-Output "  shift $shift`: $matches / $n matches (>= $(($n * 0.7)) is suspicious)"
    }
}