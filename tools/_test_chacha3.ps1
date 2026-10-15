# Search for potential 32-byte keys in the SO
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# Look for hardcoded keys: typical patterns are repeated bytes, MD5 hashes, all-zero, etc.
# A 32-byte key in the SO .rodata could be:
# - All zeros
# - Sequence of ASCII characters (32 bytes long)
# - SHA256 of some string
# - Pattern of bytes

# First, look for all-zero 32-byte sequences (could be a key placeholder)
Write-Output "=== All-zero 32-byte sequences ==="
$count = 0
for ($i = 0; $i -lt $so.Length - 32; $i++) {
    $allZero = $true
    for ($j = 0; $j -lt 32; $j++) {
        if ($so[$i + $j] -ne 0) { $allZero = $false; break }
    }
    if ($allZero) {
        Write-Output ("  0x{0:X}" -f $i)
        $count++
        if ($count -ge 5) { break }
    }
}

# Look for repeated 16-byte ASCII patterns
Write-Output "`n=== Look for 16-byte ASCII patterns (potential key halves) ==="
$patterns = @('RealRacing3RealRa', 'firemonkeysstudios', 'FiremonkeysStudios!', 'csdk-depot-csdk-dep',
    'EARealRacing3EAReal', 'EA-RR3-EA-RR3-EA-RR3-EA', 'opengl-es3opengl-es3', 'mtShaderManager-mtSha',
    'rr3rr3rr3rr3rr3rr3rr3rr3rr3rr3rr', 'RR3RR3RR3RR3RR3RR3RR3RR3RR3RR3RR',
    'r3_update_ar3_updat', 'r3_update_a_r3_upd', 'Firemonkeys Studios')

foreach ($p in $patterns) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($p)
    if ($bytes.Length -lt 16) { continue }
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
        Write-Output ("  {0,-30}  x{1}  first @ 0x{2:X}" -f $p.Substring(0, [Math]::Min(20, $p.Length)), $hits.Count, $hits[0])
    }
}

# Try a simpler theory: maybe the key is 16 bytes of zeros plus 16 bytes of file MD5
# Or the key is the asset list "rr3-shader-defaults"
Write-Output "`n=== Quick check: try ChaCha20 with MD5 of file as nonce, various 32-byte keys ==="

# Now let me look at the loading shader features code more carefully
# The function 012caea0 has a bit-reversal table - maybe it's used to obfuscate the byte order

# Look for the exact "bit 3 reversal" table in the SO
Write-Output "`n=== Bit-3 reversal table (bit flip on bit 3) ==="
$table = New-Object byte[] 256
for ($i = 0; $i -lt 256; $i++) {
    $table[$i] = ($i -bxor 0x08) -as [byte]
}

# Search for this 256-byte table in the SO
$pos = 0
$count = 0
while ($pos -lt $so.Length - 256) {
    $idx = [System.Array]::IndexOf($so, $table[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt 256; $j++) {
        if ($so[$idx + $j] -ne $table[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  Bit-3 reversal table @ 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 3) { break }
        $pos = $idx + 1
    } else { $pos = $idx + 1 }
}
Write-Output "  Occurrences: $count"

# Maybe the algorithm is simpler: XOR with a table that flips bits
# Try XOR with bit-3 reversal on 2dflat.vsh
Write-Output "`n=== XOR with bit-3 reversal (each byte b -> b XOR 0x08) ==="
$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')
$dec = New-Object byte[] $ct.Length
for ($i = 0; $i -lt $ct.Length; $i++) {
    $dec[$i] = ([int]$ct[$i] -bxor 0x08) -as [byte]
}
Write-Output "  First 30: $([System.Text.Encoding]::ASCII.GetString($dec[0..29]))"
Write-Output "  Hex first 16: $([BitConverter]::ToString($dec[0..15]))"

# Try XOR with a different single bit
Write-Output "`n=== XOR with each single bit (0x01-0x80) ==="
$expected = '#version 100'
for ($bit = 1; $bit -lt 256; $bit *= 2) {
    $dec = New-Object byte[] $ct.Length
    for ($i = 0; $i -lt $ct.Length; $i++) {
        $dec[$i] = ([int]$ct[$i] -bxor $bit) -as [byte]
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT bit=0x$($bit.ToString('X2'))"
    }
}

# Try bit reversal within bytes (high/low nibble swap)
$dec = New-Object byte[] $ct.Length
for ($i = 0; $i -lt $ct.Length; $i++) {
    $b = [int]$ct[$i]
    $dec[$i] = ((($b -band 0x0F) -shl 4) -bor (($b -shr 4) -band 0x0F)) -as [byte]
}
Write-Output "`n=== Nibble swap ==="
Write-Output "  First 30: $([System.Text.Encoding]::ASCII.GetString($dec[0..29]))"

# Try bits reversed within each byte
$dec = New-Object byte[] $ct.Length
for ($i = 0; $i -lt $ct.Length; $i++) {
    $b = [int]$ct[$i]
    $rev = 0
    for ($j = 0; $j -lt 8; $j++) {
        $rev = ($rev -shl 1) -bor ($b -band 1)
        $b = $b -shr 1
    }
    $dec[$i] = $rev -as [byte]
}
Write-Output "`n=== Full bit reversal ==="
Write-Output "  First 30: $([System.Text.Encoding]::ASCII.GetString($dec[0..29]))"