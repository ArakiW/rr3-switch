# Find all GLSL shader-related strings
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# Look for all strings containing GLSL keywords
$patterns = @(
    'precision ', 'precision highp', 'precision mediump', 'precision lowp',
    '#version', '#version 100', '#version 300', '#version 150', '#version 200',
    'gl_Position', 'gl_FragColor', 'gl_FragCoord', 'gl_PointSize',
    'attribute ', 'uniform ', 'varying ', 'inout ',
    'void main', 'void main(', 'mat4', 'vec4', 'vec3', 'vec2',
    'texture2D(', 'textureCube(', 'discard;', 'gl_FragData',
    'precision highp float', 'precision mediump float',
    'attribute vec', 'varying vec', 'varying highp'
)
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
        Write-Output ("  {0,-30}  x{1,3}" -f $p, $hits.Count)
    }
}

Write-Output "`n=== Look for AES constants / RC4 markers ==="
# AES round constants
$aesRCON = [byte[]]@(0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)
$pos = 0
$count = 0
while ($pos -lt $so.Length - 10) {
    $idx = [System.Array]::IndexOf($so, $aesRCON[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $aesRCON.Length; $j++) {
        if ($so[$idx + $j] -ne $aesRCON[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  AES RCON @ 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 3) { break }
        $pos = $idx + 1
    } else { $pos = $idx + 1 }
}

# SHA256 round constants (H)
$shaH = [byte[]]@(0x6a, 0x09, 0xe6, 0x67, 0xbb, 0x67, 0xae, 0x85, 0x3c, 0x6e, 0xf3, 0x72)
$pos = 0
$count = 0
while ($pos -lt $so.Length - 12) {
    $idx = [System.Array]::IndexOf($so, $shaH[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $shaH.Length; $j++) {
        if ($so[$idx + $j] -ne $shaH[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  SHA256 H[0] @ 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 2) { break }
        $pos = $idx + 1
    } else { $pos = $idx + 1 }
}

# MD5 init values
$md5 = [byte[]]@(0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10)
$pos = 0
$count = 0
while ($pos -lt $so.Length - 16) {
    $idx = [System.Array]::IndexOf($so, $md5[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $md5.Length; $j++) {
        if ($so[$idx + $j] -ne $md5[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  MD5 init @ 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 3) { break }
        $pos = $idx + 1
    } else { $pos = $idx + 1 }
}

# CRC32 table first 4 bytes (0x00000000 reversed)
$crc = [byte[]]@(0x00, 0x00, 0x00, 0x00, 0x96, 0x30, 0x07, 0x77)
$pos = 0
$count = 0
while ($pos -lt $so.Length - 8) {
    $idx = [System.Array]::IndexOf($so, $crc[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $crc.Length; $j++) {
        if ($so[$idx + $j] -ne $crc[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  CRC32 table @ 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 2) { break }
        $pos = $idx + 1
    } else { $pos = $idx + 1 }
}

# ChaCha constant "expand 32-byte k"
$chacha = [byte[]]@(0x65, 0x78, 0x70, 0x61, 0x6e, 0x64, 0x20, 0x33, 0x32, 0x2d, 0x62, 0x79, 0x74, 0x65, 0x20, 0x6b)
$pos = 0
$count = 0
while ($pos -lt $so.Length - 16) {
    $idx = [System.Array]::IndexOf($so, $chacha[0], $pos)
    if ($idx -lt 0) { break }
    $match = $true
    for ($j=1; $j -lt $chacha.Length; $j++) {
        if ($so[$idx + $j] -ne $chacha[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Output ("  ChaCha constant @ 0x{0:X}" -f $idx)
        $count++
        if ($count -ge 2) { break }
        $pos = $idx + 1
    } else { $pos = $idx + 1 }
}

Write-Output "`n=== Look for AES-NI / SHA / crypto symbols ==="
foreach ($p in @('AES_encrypt', 'AES_decrypt', 'AES_set_encrypt_key', 'AES_set_decrypt_key',
    'SHA256_Init', 'SHA256_Update', 'SHA256_Final', 'MD5_Init', 'MD5_Update',
    'ChaCha20', 'chacha20_block', 'salsa20', 'arc4random', 'RC4_set_key',
    'EVP_EncryptInit', 'EVP_DecryptInit', 'CRYPTO_', 'gcry_')) {
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
        Write-Output ("  {0,-20}  x{1,3}" -f $p, $hits.Count)
    }
}