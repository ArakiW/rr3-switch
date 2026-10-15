$ErrorActionPreference = 'Stop'

$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')
$ct_gui = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\gui\processed\LoadingScreen_Init.gui')

# File's MD5
$md5 = [System.Security.Cryptography.MD5]::Create()
$file_hash = $md5.ComputeHash($ct)
$file_hash_hex = ($file_hash | ForEach-Object { $_.ToString('X2') }) -join ''

Write-Output "File MD5: $file_hash_hex"

# Implement ChaCha20
function Quarter-Round {
    param([uint32[]]$state, [int]$a, [int]$b, [int]$c, [int]$d)
    $state[$a] = ($state[$a] + $state[$b]) -band 0xFFFFFFFF
    $state[$d] = ($state[$d] -bxor $state[$a]) -band 0xFFFFFFFF
    $state[$d] = (($state[$d] -shl 16) -bor ($state[$d] -shr 16)) -band 0xFFFFFFFF
    $state[$c] = ($state[$c] + $state[$d]) -band 0xFFFFFFFF
    $state[$b] = ($state[$b] -bxor $state[$c]) -band 0xFFFFFFFF
    $state[$b] = (($state[$b] -shl 12) -bor ($state[$b] -shr 20)) -band 0xFFFFFFFF
    $state[$a] = ($state[$a] + $state[$b]) -band 0xFFFFFFFF
    $state[$d] = ($state[$d] -bxor $state[$a]) -band 0xFFFFFFFF
    $state[$d] = (($state[$d] -shl 8) -bor ($state[$d] -shr 24)) -band 0xFFFFFFFF
    $state[$c] = ($state[$c] + $state[$d]) -band 0xFFFFFFFF
    $state[$b] = ($state[$b] -bxor $state[$c]) -band 0xFFFFFFFF
    $state[$b] = (($state[$b] -shl 7) -bor ($state[$b] -shr 25)) -band 0xFFFFFFFF
}

function Get-ChaCha20Block {
    param([byte[]]$key, [byte[]]$nonce, [uint32]$counter)
    $state = New-Object uint32[] 16
    $state[0] = 0x61707865
    $state[1] = 0x3320646e
    $state[2] = 0x79622d32
    $state[3] = 0x6b206574
    for ($i = 0; $i -lt 8; $i++) {
        $state[4 + $i] = [System.BitConverter]::ToUInt32($key, $i * 4)
    }
    $state[12] = $counter
    for ($i = 0; $i -lt 3; $i++) {
        $state[13 + $i] = [System.BitConverter]::ToUInt32($nonce, $i * 4)
    }
    $working = $state.Clone()
    for ($i = 0; $i -lt 10; $i++) {
        Quarter-Round $working 0 4 8 12
        Quarter-Round $working 1 5 9 13
        Quarter-Round $working 2 6 10 14
        Quarter-Round $working 3 7 11 15
        Quarter-Round $working 0 5 10 15
        Quarter-Round $working 1 6 11 12
        Quarter-Round $working 2 7 8 13
        Quarter-Round $working 3 4 9 14
    }
    for ($i = 0; $i -lt 16; $i++) {
        $working[$i] = ($working[$i] + $state[$i]) -band 0xFFFFFFFF
    }
    $out = New-Object byte[] 64
    for ($i = 0; $i -lt 16; $i++) {
        $bytes = [System.BitConverter]::GetBytes($working[$i])
        for ($j = 0; $j -lt 4; $j++) { $out[$i*4 + $j] = $bytes[$j] }
    }
    return $out
}

function Invoke-ChaCha20 {
    param([byte[]]$data, [byte[]]$key, [byte[]]$nonce, [uint32]$counter = 0)
    $out = New-Object byte[] $data.Length
    $pos = 0
    $c = $counter
    while ($pos -lt $data.Length) {
        $block = Get-ChaCha20Block $key $nonce $c
        $end = [Math]::Min(64, $data.Length - $pos)
        for ($i = 0; $i -lt $end; $i++) {
            $out[$pos + $i] = ([int]$data[$pos + $i] -bxor [int]$block[$i]) -as [byte]
        }
        $pos += 64
        $c += 1
    }
    return ,$out
}

# Try ChaCha20 with SHA256 of various strings as key, MD5 first 12 bytes as nonce
$sha = [System.Security.Cryptography.SHA256]::Create()

$candidates = @('RealRacing3', 'Real Racing 3', 'realracing3', 'rr3', 'firemonkeys',
    'Firemonkeys', 'csdk', 'shader', 'csdk shader', 'rr3shader',
    'FiremonkeysStudios', 'EA Real Racing', 'RealRacing',
    'csdk_depot', 'rr3_depot', 'rr3-csdk', 'rr3csdk',
    'depot shader', 'depot shader key', 'rr3_default_shader_key',
    'shader_default', 'opengl_es_3', 'mt_shader',
    'rr3 shader', 'rr3 vsh', 'rr3 gui', 'rr3 enc', 'rr3 file',
    'csdk key', 'rr3 key', 'rr3 csdk key', 'csdk_vsh_key',
    'csdk_shader_key', 'csdk_gui_key', 'rr3 cipher',
    'rr3 encrypt', 'rr3 decrypt', 'rr3 stream',
    'rr3 stream cipher', 'rr3 cipher key',
    'rr3 crypt', 'rr3 crypto', 'rr3 open', 'rr3 file key',
    'depot key', 'csdk default key',
    'rr3 default', 'csdk default', 'rr3 depotshader',
    'rr3-depot-shader', 'rr3-depot-gui',
    'csdkdepot', 'csdk-depot-shader', 'csdk depot',
    'firemonkeys studios pty ltd', 'EA Firemonkeys',
    'Firemonkeys Pty Ltd', 'realracing3firemonkeys')

$expected = '#version 100'
Write-Output "`n=== ChaCha20 brute force (SHA256 key, MD5 nonce first 12 bytes) ==="
foreach ($s in $candidates) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $key = $sha.ComputeHash($bytes)
    # Try nonce = first 12 bytes of MD5
    $nonce1 = New-Object byte[] 12
    for ($i = 0; $i -lt 12; $i++) { $nonce1[$i] = $file_hash[$i] }
    $dec = Invoke-ChaCha20 $ct $key $nonce1 0
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$s' nonce=md5-first-12"
    }
    # Try nonce = all zeros
    $nonce2 = New-Object byte[] 12
    $dec = Invoke-ChaCha20 $ct $key $nonce2 0
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$s' nonce=zero"
    }
    # Try nonce = last 12 bytes of MD5
    $nonce3 = New-Object byte[] 12
    for ($i = 0; $i -lt 12; $i++) { $nonce3[$i] = $file_hash[4 + $i] }
    $dec = Invoke-ChaCha20 $ct $key $nonce3 0
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$s' nonce=md5-last-12"
    }
}

# Try MD5 hashes as key too
Write-Output "`n=== ChaCha20 brute force (MD5 key, various nonces) ==="
$md5 = [System.Security.Cryptography.MD5]::Create()
foreach ($s in $candidates) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $key = $md5.ComputeHash($bytes)
    # Pad to 32 bytes
    $key32 = New-Object byte[] 32
    for ($i = 0; $i -lt 32; $i++) { $key32[$i] = $key[$i % 16] }
    $nonce1 = New-Object byte[] 12
    for ($i = 0; $i -lt 12; $i++) { $nonce1[$i] = $file_hash[$i] }
    $dec = Invoke-ChaCha20 $ct $key32 $nonce1 0
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key=MD5('$s') nonce=md5"
    }
    $nonce2 = New-Object byte[] 12
    $dec = Invoke-ChaCha20 $ct $key32 $nonce2 0
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key=MD5('$s') nonce=zero"
    }
}

# Maybe the cipher is XOR with SHA256 of (file path or key string)
Write-Output "`n=== XOR with SHA256 of various keys ==="
foreach ($s in $candidates) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $key = $sha.ComputeHash($bytes)
    $dec = New-Object byte[] $ct.Length
    for ($i = 0; $i -lt $ct.Length; $i++) {
        $dec[$i] = ([int]$ct[$i] -bxor [int]$key[$i % 32]) -as [byte]
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT SHA256('$s') XOR"
    }
}

# Try with MD5 as 16-byte XOR key (cycling)
foreach ($s in $candidates) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($s)
    $key = $md5.ComputeHash($bytes)
    $dec = New-Object byte[] $ct.Length
    for ($i = 0; $i -lt $ct.Length; $i++) {
        $dec[$i] = ([int]$ct[$i] -bxor [int]$key[$i % 16]) -as [byte]
    }
    if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT MD5('$s') XOR"
    }
}