# Try RC4 with skip on bytes 2-5
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

function Invoke-RC4-Skip {
    # RC4 but skip bytes 2-5 of input (apply keystream but skip writing those)
    param([byte[]]$data, [byte[]]$key, [int]$skipStart, [int]$skipLen, [int]$drop = 0)
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
    $skipEnd = $skipStart + $skipLen - 1
    for ($n=0; $n -lt $data.Length; $n++) {
        $i = ($i + 1) -band 0xFF
        $j = ($j + $S[$i]) -band 0xFF
        $tmp = $S[$i]; $S[$i] = $S[$j]; $S[$j] = $tmp
        if ($n -ge $skipStart -and $n -le $skipEnd) {
            $out[$n] = $data[$n]  # plaintext
        } else {
            $kk = [int]$S[($S[$i] + $S[$j]) -band 0xFF]
            $dd = [int]$data[$n]
            $out[$n] = ($dd -bxor $kk) -as [byte]
        }
    }
    return ,$out
}

$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')
$expected = '#version 100'

# Test that RC4 with key RR3 produces keystream starting with E2 at offset 0
$key = [System.Text.Encoding]::ASCII.GetBytes('RR3')
$dec = Invoke-RC4 $ct $key 0
Write-Output "RC4 key=RR3 drop=0 first 13 bytes (raw, all bytes decrypted): $([System.Text.Encoding]::ASCII.GetString($dec[0..12]))"
Write-Output "  hex: $([BitConverter]::ToString($dec[0..15]))"

# Try skip version: keep bytes 2-5 as plaintext
$dec_skip = Invoke-RC4-Skip $ct $key 2 4 0
Write-Output "`nRC4-SKIP key=RR3 skip[2:5] first 13: $([System.Text.Encoding]::ASCII.GetString($dec_skip[0..12]))"
Write-Output "  hex: $([BitConverter]::ToString($dec_skip[0..15]))"

# Try a comprehensive set of keys with skip 2:5
$candidates = @(
    'RR3','rr3','Firemint','Firemonkeys','Cloudcell','depot','shader','EA','glu','csdk',
    'RealRacing','realracing','vsh','fsh','gui','MT_GL_ES_3','MT_GL_ES_2','1B:D',
    'vsh1','shad','EAGL','rr3_','rr3-depot','rr3shaders','game','default',
    'OpenGL','GLES','mt','key','secret','Android','FB','FM',
    '12345678','abcdefgh','00000000','01234567','2dflat',
    'firemonkeys','FM','csdk_depot','opengl','rr3shader','rr3-mobile',
    'rr3_android','rr3shader','rr3_shader','shaders_csdk',
    'csdk-shader','rr3shadersv2','rr3shaders_v2'
)

Write-Output "`n=== RC4-SKIP [2:6] brute force ==="
foreach ($keyStr in $candidates) {
    foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072, 4096)) {
        $kb = [System.Text.Encoding]::ASCII.GetBytes($keyStr)
        $dec = Invoke-RC4-Skip $ct $kb 2 4 $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT key='$keyStr' drop=$drop"
            Write-Output "    first 30: $([System.Text.Encoding]::ASCII.GetString($dec[0..29]))"
        }
    }
}

# Try also keeping only bytes 0-1 encrypted, body starting at 2
Write-Output "`n=== RC4 on body [2:] with key, then prepend first 2 bytes plaintext ==="
# Doesn't make sense - skip this

# 4-byte brute force
Write-Output "`n=== RC4-SKIP brute force with 1-byte keys (drop=0..6144) ==="
for ($a = 0; $a -lt 256; $a++) {
    $kb = New-Object byte[] 1
    $kb[0] = $a
    foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072, 4096, 5120, 6144)) {
        $dec = Invoke-RC4-Skip $ct $kb 2 4 $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT key=$($a.ToString('X2')) drop=$drop"
        }
    }
}

# 2-byte brute force (limited to printable)
Write-Output "`n=== RC4-SKIP brute force with 2-byte printable keys ==="
for ($a = 0x20; $a -lt 0x7f; $a++) {
    for ($b = 0x20; $b -lt 0x7f; $b++) {
        $kb = New-Object byte[] 2
        $kb[0] = $a; $kb[1] = $b
        foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072)) {
            $dec = Invoke-RC4-Skip $ct $kb 2 4 $drop
            if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
                Write-Output "  HIT key=$($a.ToString('X2'))$($b.ToString('X2')) drop=$drop"
            }
        }
    }
}

Write-Output "`n=== Full file brute force: assume plaintext starts at byte 0 ==="
# This is the original interpretation: the magic is encrypted too (but coincidentally
# encrypts to itself for these files).
foreach ($keyStr in $candidates) {
    foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072, 4096)) {
        $kb = [System.Text.Encoding]::ASCII.GetBytes($keyStr)
        $dec = Invoke-RC4 $ct $kb $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT key='$keyStr' drop=$drop"
        }
    }
}