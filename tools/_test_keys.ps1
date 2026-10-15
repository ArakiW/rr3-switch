$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# .data.rel.ro is at 0x1E19AE0, size 0x1522A0
# Look for 32-byte chunks that could be a ChaCha20 key

$start = 0x1E19AE0
$end = 0x1E19AE0 + 0x1522A0

Write-Output "=== Looking for 32-byte patterns that look like keys in .data.rel.ro ==="
$count = 0
for ($i = $start; $i -lt $end - 32; $i += 4) {
    # Check if 32 bytes at this offset look like a key (e.g., has structure)
    $chunk = $so[$i..($i+31)]
    $nonZero = 0
    $ascii = 0
    foreach ($b in $chunk) {
        if ($b -ne 0) { $nonZero++ }
        if ($b -ge 0x20 -and $b -lt 0x7f) { $ascii++ }
    }
    if ($nonZero -eq 32 -and $ascii -ge 16) {
        $str = [System.Text.Encoding]::ASCII.GetString($chunk)
        if ($str -match '[A-Za-z]{4,}') {
            Write-Output ("  0x{0:X}: {1}" -f $i, $str)
            $count++
            if ($count -ge 30) { break }
        }
    }
}

# Now look in .data section (at 0x1F7E880, size 0x146A0)
Write-Output "`n=== Looking for keys in .data ==="
$start = 0x1F7E880
$end = 0x1F7E880 + 0x146A0
$count = 0
for ($i = $start; $i -lt $end - 32; $i += 4) {
    $chunk = $so[$i..($i+31)]
    $nonZero = 0
    $ascii = 0
    foreach ($b in $chunk) {
        if ($b -ne 0) { $nonZero++ }
        if ($b -ge 0x20 -and $b -lt 0x7f) { $ascii++ }
    }
    if ($nonZero -eq 32 -and $ascii -ge 16) {
        $str = [System.Text.Encoding]::ASCII.GetString($chunk)
        if ($str -match '[A-Za-z]{4,}') {
            Write-Output ("  0x{0:X}: {1}" -f $i, $str)
            $count++
            if ($count -ge 30) { break }
        }
    }
}

# Check for the specific pattern: 32 bytes that could be a ChaCha20 key
# SHA256 of "RealRacing3" or similar string
Write-Output "`n=== Try SHA256 of common strings ==="
$strings_to_try = @('RealRacing3', 'Real Racing 3', 'realracing3', 'rr3', 'firemonkeys',
    'Firemonkeys', 'csdk', 'shader', 'csdk shader', 'rr3shader',
    'FiremonkeysStudios', 'EA Real Racing', 'RealRacing',
    'csdk_depot', 'rr3_depot', 'rr3-csdk', 'rr3csdk',
    'depot shader', 'depot shader key', 'rr3_default_shader_key',
    'shader_default', 'opengl_es_3', 'mt_shader')

foreach ($s in $strings_to_try) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $hash = $sha.ComputeHash($bytes)
    $hex = ($hash | ForEach-Object { $_.ToString('X2') }) -join ''
    Write-Output "  SHA256('$s') = $hex"
}

# Try MD5
Write-Output "`n=== MD5 of common strings ==="
foreach ($s in $strings_to_try) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $hash = $md5.ComputeHash($bytes)
    $hex = ($hash | ForEach-Object { $_.ToString('X2') }) -join ''
    Write-Output "  MD5('$s') = $hex"
}