# More comprehensive brute force
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
$ct6 = $ct[6..($ct.Length-1)]
$expected = '#version 100'
$expLen = $expected.Length

# More candidate keys
$candidates = @(
    'RR3','rr3','Firemint','Firemonkeys','Cloudcell','depot','.depot',
    'shader','EA','glu','csdk','RealRacing','realracing','vsh','fsh','gui',
    'MT_GL_ES_3','MT_GL_ES_2','1B:D','vsh1','vsh0','shad','EAGL','rr3_',
    'rr3-depot','rr3shaders','shader1','default','game','global',
    'Real Racing','Real Racing 3','EA Games','Firemonkeys Pty',
    'Firemonkeys Studios','RealRacing3','realracing3','RR3 Shader',
    'rr3shader','opengl','OpenGL','OpenGL ES','GLES','GLES2','GLES3',
    'mt','mt1','mt2','key','secret','hidden','assets','Android',
    'fb','fm','FM','csdk_depot','csdk-shader','shaders',
    '1234567890','abcdefghij','01234567','00000000','11111111',
    '2dflat','particle','cubemap','basic','flat','alpha','debug'
)

Write-Output "=== Brute force RC4 keys on 2dflat.vsh (full file) ==="
foreach ($key in $candidates) {
    foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072, 4096)) {
        $kb = [System.Text.Encoding]::ASCII.GetBytes($key)
        $dec = Invoke-RC4 $ct $kb $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT key='$key' drop=$drop"
        }
    }
}

Write-Output "`n=== Brute force RC4 keys on 2dflat.vsh (offset 6+) ==="
foreach ($key in $candidates) {
    foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072, 4096)) {
        $kb = [System.Text.Encoding]::ASCII.GetBytes($key)
        $dec = Invoke-RC4 $ct6 $kb $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT key='$key' drop=$drop"
        }
    }
}

# 4-byte hex brute force: try all 4-byte keys of form 0xAABBBBBB
Write-Output "`n=== RC4 brute force: 4-byte key (a=0x00, b=0x00..0xff, c=0x00, d=0x00) ==="
for ($b = 0; $b -lt 256; $b++) {
    $kb = New-Object byte[] 4
    $kb[0] = 0; $kb[1] = $b; $kb[2] = 0; $kb[3] = 0
    $dec = Invoke-RC4 $ct $kb 0
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key=00-$($b.ToString('X2'))-00-00"
    }
}

# XOR with derived keystream against multiple known-plaintext patterns
# Check if there's any cyclic pattern of period 1, 2, 3, 4, 5 in keystream
Write-Output "`n=== Keystream analysis: assume plaintext=`#version 100` at offset 6 ==="
$pt = [byte[]]@(35, 118, 101, 114, 115, 105, 111, 110, 32, 49, 48, 48)
$ks = New-Object int[] 12
for ($i=0; $i -lt 12; $i++) { $ks[$i] = ([int]$ct[6+$i]) -bxor ([int]$pt[$i]) }
Write-Output "  Keystream: $([string]::Join(' ', ($ks | ForEach-Object { $_.ToString('X2') })))"
Write-Output "  Sorted:    $([string]::Join(' ', (($ks.Clone() | Sort-Object) | ForEach-Object { $_.ToString('X2') })))"

# Check if keystream bytes are linearly increasing
Write-Output "`n  Differences:"
for ($i=0; $i -lt 11; $i++) {
    $diff = ($ks[$i+1] - $ks[$i]) -band 0xFF
    Write-Output "    ks[$i]-ks[$($i+1)] = $($diff.ToString('X2'))"
}

# Now try RC4 with the magic key 1B:D
$key_magic = [byte[]]@(0x31, 0x42, 0x3A, 0x44)
Write-Output "`n=== RC4 with key=1B:D (raw magic bytes) ==="
foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072)) {
    $dec = Invoke-RC4 $ct6 $key_magic $drop
    if ($dec.Length -ge 13) {
        $s = [System.Text.Encoding]::ASCII.GetString($dec[0..12])
        Write-Output "  drop=$drop first 13: '$s'"
    }
}

# Try with key = bytes from each file's header
Write-Output "`n=== Try RC4 with key = cipher[0..1] (treating 2 bytes as key) ==="
$allkeys = @()
foreach ($b0 in 0..255) {
    foreach ($b1 in 0..255) {
        $kb = New-Object byte[] 2
        $kb[0] = $b0; $kb[1] = $b1
        $dec = Invoke-RC4 $ct6 $kb 0
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT nonce=$($b0.ToString('X2'))$($b1.ToString('X2'))"
        }
    }
}