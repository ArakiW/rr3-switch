# Simple RC4 test
$ErrorActionPreference = 'Stop'

function Invoke-RC4 {
    param([byte[]]$data, [byte[]]$key, [int]$drop = 0)
    $S = New-Object int[] 256
    for ($i=0; $i -lt 256; $i++) { $S[$i] = $i }
    $j = 0
    for ($i=0; $i -lt 256; $i++) {
        $j = ($j + $S[$i] + $key[$i % $key.Length]) -band 0xFF
        $tmp = $S[$i]; $S[$i] = $S[$j]; $S[$j] = $tmp
    }
    $i = 0; $j = 0
    for ($x=0; $x -lt $drop; $x++) {
        $i = ($i + 1) -band 0xFF
        $j = ($j + $S[$i]) -band 0xFF
        $tmp = $S[$i]; $S[$i] = $S[$j]; $S[$j] = $tmp
    }
    $out = New-Object byte[] $data.Length
    for ($n=0; $n -lt $data.Length; $n++) {
        $i = ($i + 1) -band 0xFF
        $j = ($j + $S[$i]) -band 0xFF
        $tmp = $S[$i]; $S[$i] = $S[$j]; $S[$j] = $tmp
        $kk = [int]$S[($S[$i] + $S[$j]) -band 0xFF]
        $dd = [int]$data[$n]
        $out[$n] = ($dd -bxor $kk) -as [byte]
    }
    return ,$out
}

$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')
Write-Output "Loaded $($ct.Length) bytes"
Write-Output "First 16 bytes: $([BitConverter]::ToString($ct[0..15]))"
Write-Output "Bytes 2-5: $([BitConverter]::ToString($ct[2..5]))"

# Try RC4 with key='RR3' on entire file
$key = [System.Text.Encoding]::ASCII.GetBytes('RR3')
$dec = Invoke-RC4 $ct $key 0
Write-Output "`nRC4 key=RR3 drop=0 first 30 bytes: $([System.Text.Encoding]::ASCII.GetString($dec[0..29]))"

$dec = Invoke-RC4 $ct $key 1024
Write-Output "RC4 key=RR3 drop=1024 first 30 bytes: $([System.Text.Encoding]::ASCII.GetString($dec[0..29]))"

# Try RC4 starting at offset 6 (after magic) with key='RR3'
$ct6 = $ct[6..($ct.Length-1)]
$dec6 = Invoke-RC4 $ct6 $key 0
Write-Output "`nRC4 [offset 6] key=RR3 first 30 bytes: $([System.Text.Encoding]::ASCII.GetString($dec6[0..29]))"

# Try constant XOR at offset 6
Write-Output "`nConstant XOR at offset 6 (all seeds):"
for ($seed = 0; $seed -lt 256; $seed++) {
    $dec_x = New-Object byte[] ($ct.Length - 6)
    for ($i=6; $i -lt $ct.Length; $i++) { $dec_x[$i-6] = ([int]$ct[$i] -bxor $seed) -as [byte] }
    if ([System.Text.Encoding]::ASCII.GetString($dec_x[0..12]) -eq '#version 100') {
        Write-Output "  HIT seed=0x$($seed.ToString('X2'))"
    }
}

# Try per-file nonce: bytes 0-1 are nonce, run RC4 on offset 6+
Write-Output "`nRC4 with per-file nonce bytes 0-1, applied to body offset 6+:"
$key = New-Object byte[] 2
for ($k0 = 0; $k0 -lt 256; $k0++) {
    for ($k1 = 0; $k1 -lt 256; $k1++) {
        $key[0] = $k0 -as [byte]; $key[1] = $k1 -as [byte]
        $dec_b = Invoke-RC4 $ct6 $key 0
        if ([System.Text.Encoding]::ASCII.GetString($dec_b[0..12]) -eq '#version 100') {
            Write-Output "  HIT nonce=$($k0.ToString('X2'))$($k1.ToString('X2'))"
        }
    }
}