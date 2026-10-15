# Try AES decryption with common keys
$ErrorActionPreference = 'Stop'

$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')
Write-Output "Ciphertext: $($ct.Length) bytes"
Write-Output "  hex: $([BitConverter]::ToString($ct[0..15]))"

$expected = '#version 100'

# Try AES-ECB decrypt with various keys (skipping first 16 bytes since AES is 16-byte block)
Write-Output "`n=== AES-ECB decrypt (skip first 16 bytes) ==="
$aes = [System.Security.Cryptography.Aes]::Create()
$aes.Mode = [System.Security.Cryptography.CipherMode]::ECB
$aes.Padding = [System.Security.Cryptography.PaddingMode]::None

$keys = @(
    'RR3','rr3','Firemint','Firemonkeys','Cloudcell','depot','shader','EA','glu','csdk',
    'RealRacing','realracing','vsh','fsh','gui','MT_GL_ES_3','MT_GL_ES_2','1B:D',
    'csdk_depot','rr3shader','opengl','Android','GLES','Firemonkeys Studios',
    'FiremonkeysStudios','FiremonkeysPty','rr3shaderkey'
)

foreach ($k in $keys) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($k)
    # Pad to 16 bytes with PKCS7
    $padded = New-Object byte[] 16
    for ($i=0; $i -lt [Math]::Min(16, $kb.Length); $i++) { $padded[$i] = $kb[$i] }
    for ($i=$kb.Length; $i -lt 16; $i++) { $padded[$i] = [byte](16 - $kb.Length) }
    $aes.Key = $padded

    try {
        $decryptor = $aes.CreateDecryptor()
        # AES needs 16-byte aligned data
        $body = $ct[16..($ct.Length-1)]
        $dec = $decryptor.TransformFinalBlock($body, 0, $body.Length)
        $plain = [System.Text.Encoding]::ASCII.GetString($dec[0..12])
        if ($plain.StartsWith($expected)) {
            Write-Output "  HIT key='$k' first 13: '$plain'"
        }
    } catch {
        # Skip
    }
}

# Try AES-CTR (manual since .NET doesn't support natively)
Write-Output "`n=== AES-CTR decrypt (counter starts at 0) ==="
$aes2 = [System.Security.Cryptography.Aes]::Create()
$aes2.Mode = [System.Security.Cryptography.CipherMode]::ECB
$aes2.Padding = [System.Security.Cryptography.PaddingMode]::None

foreach ($k in $keys) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($k)
    $padded = New-Object byte[] 16
    for ($i=0; $i -lt [Math]::Min(16, $kb.Length); $i++) { $padded[$i] = $kb[$i] }
    for ($i=$kb.Length; $i -lt 16; $i++) { $padded[$i] = 0 }
    $aes2.Key = $padded

    $decryptor = $aes2.CreateDecryptor()
    $dec = New-Object byte[] $ct.Length
    $counter = New-Object byte[] 16
    $blockSize = 16
    $offset = 0
    while ($offset -lt $ct.Length) {
        $keystream = $decryptor.TransformFinalBlock($counter, 0, 16)
        $end = [Math]::Min($blockSize, $ct.Length - $offset)
        for ($i=0; $i -lt $end; $i++) {
            $dec[$offset + $i] = ([int]$ct[$offset + $i]) -bxor ([int]$keystream[$i]) -as [byte]
        }
        # Increment counter
        for ($i=15; $i -ge 0; $i--) {
            $counter[$i] = ($counter[$i] + 1) -as [byte]
            if ($counter[$i] -ne 0) { break }
        }
        $offset += $blockSize
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$k'"
    }
}

# Try AES-CTR with IV = first 16 bytes of file
Write-Output "`n=== AES-CTR with IV = first 16 bytes (then encrypt from offset 16) ==="
$aes3 = [System.Security.Cryptography.Aes]::Create()
$aes3.Mode = [System.Security.Cryptography.CipherMode]::ECB
$aes3.Padding = [System.Security.Cryptography.PaddingMode]::None

foreach ($k in $keys) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($k)
    $padded = New-Object byte[] 16
    for ($i=0; $i -lt [Math]::Min(16, $kb.Length); $i++) { $padded[$i] = $kb[$i] }
    for ($i=$kb.Length; $i -lt 16; $i++) { $padded[$i] = 0 }
    $aes3.Key = $padded

    $decryptor = $aes3.CreateDecryptor()
    $dec = New-Object byte[] $ct.Length
    $counter = $ct[0..15]  # IV = first 16 bytes
    $offset = 16
    while ($offset -lt $ct.Length) {
        $keystream = $decryptor.TransformFinalBlock($counter, 0, 16)
        $end = [Math]::Min(16, $ct.Length - $offset)
        for ($i=0; $i -lt $end; $i++) {
            $dec[$offset + $i] = ([int]$ct[$offset + $i]) -bxor ([int]$keystream[$i]) -as [byte]
        }
        for ($i=15; $i -ge 0; $i--) {
            $counter[$i] = ($counter[$i] + 1) -as [byte]
            if ($counter[$i] -ne 0) { break }
        }
        $offset += 16
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$k'"
    }
}

# Try XOR with a fixed table that's common in iOS/Android games
Write-Output "`n=== Try XOR with position-keyed table ==="
foreach ($k in $keys) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($k)
    # Method 1: XOR with key cycle (repeating)
    $dec = New-Object byte[] $ct.Length
    for ($i=0; $i -lt $ct.Length; $i++) {
        $dec[$i] = ([int]$ct[$i] -bxor ([int]$kb[$i % $kb.Length])) -as [byte]
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT cycle key='$k'"
    }
    # Method 2: XOR with key sum
    $keySum = 0
    foreach ($b in $kb) { $keySum = ($keySum + $b) -band 0xFF }
    $dec2 = New-Object byte[] $ct.Length
    for ($i=0; $i -lt $ct.Length; $i++) {
        $dec2[$i] = ([int]$ct[$i] -bxor $keySum) -as [byte]
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec2[0..12]) -eq $expected) {
        Write-Output "  HIT sum key='$k' (sum=$keySum)"
    }
}

# Maybe the file IS actually the GLSL source with light obfuscation
# Try various decode bases (base64, etc.)
Write-Output "`n=== Base64 test ==="
try {
    $decoded = [System.Convert]::FromBase64String([System.Text.Encoding]::ASCII.GetString($ct))
    Write-Output "  base64: $($decoded.Length) bytes"
} catch {
    Write-Output "  base64: invalid"
}

# Maybe it's a custom encoding: each 2 bytes maps to 1 ASCII char
Write-Output "`n=== 2-byte-per-char test ==="
$dec = New-Object System.Text.StringBuilder
for ($i=0; $i -lt [Math]::Min(40, $ct.Length); $i += 2) {
    $val = ([int]$ct[$i] -shl 8) -bor [int]$ct[$i+1]
    if ($val -lt 256 -and $val -ge 32) {
        [void]$dec.Append([char]$val)
    } else {
        [void]$dec.Append('?')
    }
}
Write-Output "  2byte/char: $($dec.ToString())"

Write-Output "`n=== Reverse byte order test ==="
$rev = $ct[($ct.Length-1)..0]
Write-Output "  Reversed first 16: $([BitConverter]::ToString($rev[0..15]))"

# Maybe it's a fixed offset cipher: ciphertext[i] = plaintext[i] + offset (mod 256)
Write-Output "`n=== Caesar cipher test (additive) ==="
for ($offset = 0; $offset -lt 256; $offset++) {
    $dec = New-Object byte[] $ct.Length
    for ($i=0; $i -lt $ct.Length; $i++) {
        $dec[$i] = (([int]$ct[$i] - $offset) -band 0xFF) -as [byte]
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT offset=$offset"
    }
}