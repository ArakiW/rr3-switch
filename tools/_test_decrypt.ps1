# PowerShell decryption test for RR3 .vsh files
$ErrorActionPreference = 'Stop'

$paths = @{
    '2dflat'              = 'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh'
    'particle'            = 'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\particle.vsh'
    'cubemap_blur'        = 'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\cubemap_blur.vsh'
    'full_screen_effects' = 'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\full_screen_effects.vsh'
    'basic'               = 'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\basic.vsh'
    'flat_colour'         = 'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\flat_colour.vsh'
}

$data = @{}
foreach ($k in $paths.Keys) { $data[$k] = [System.IO.File]::ReadAllBytes($paths[$k]) }

# Verify magic bytes 2-5 = 31 42 3A 44 for all .vsh files
Write-Output "=== Header sanity ==="
foreach ($k in $data.Keys) {
    $b = $data[$k]
    $magic = [BitConverter]::ToString($b[2..5])
    Write-Output ("  {0,-22}  len={1,4}  bytes[2:6]={2}  bytes[0:2]={3}" -f $k, $b.Length, $magic, ([BitConverter]::ToString($b[0..1])))
}

# Cross-XOR between files to confirm bytes 2-5 are plaintext magic
Write-Output "`n=== Cross-XOR between .vsh files (bytes 0-15) ==="
$keys = @('2dflat','particle','cubemap_blur','full_screen_effects')
for ($i=0; $i -lt $keys.Count; $i++) {
    for ($j=$i+1; $j -lt $keys.Count; $j++) {
        $a = $data[$keys[$i]]; $b = $data[$keys[$j]]
        $n = [Math]::Min($a.Length, $b.Length)
        $xor = New-Object int[] 16
for ($x=0; $x -lt 16; $x++) { $xor[$x] = ([int]$a[$x]) -bxor ([int]$b[$x]) }
        $hex = ($xor | ForEach-Object { $_.ToString('X2') }) -join '-'
        Write-Output ("  {0,-14} xor {1,-22} = {2}" -f $keys[$i], $keys[$j], $hex)
    }
}

# Derive keystream for 2dflat assuming plaintext starts at offset 6 with `#version 100` + newline + a 14th char
$ct = $data['2dflat']
$pt = [byte[]]@(35, 118, 101, 114, 115, 105, 111, 110, 32, 49, 48, 48, 10)  # '#version 100\n'
$keystream = New-Object int[] $pt.Length
for ($i=0; $i -lt $pt.Length; $i++) { $keystream[$i] = ([int]$ct[6+$i]) -bxor ([int]$pt[$i]) }
Write-Output "`n=== Keystream for 2dflat.vsh (offset 6+), assuming plaintext = `#version 100`+LF ==="
Write-Output ("  ks[6:18] = " + (($keystream | ForEach-Object { $_.ToString('X2') }) -join ' '))

# Try RC4 with various keys
function Invoke-RC4([byte[]]$data, [byte[]]$key, [int]$drop = 0) {
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
        $out[$n] = ([int]$data[$n]) -bxor ([int]$S[($S[$i] + $S[$j]) -band 0xFF])
    }
    return ,$out
}

function Test-RC4 {
    param([byte[]]$Cipher, [string]$Key, [string]$PlainStart, [int]$Drop = 0, [string]$Name = "")
    $kb = [System.Text.Encoding]::ASCII.GetBytes($Key)
    $dec = Invoke-RC4 $Cipher $kb $Drop
    $prefix = [System.Text.Encoding]::ASCII.GetString($dec[0..([Math]::Min($PlainStart.Length-1, $dec.Length-1))])
    $match = $prefix.StartsWith($PlainStart)
    return [PSCustomObject]@{
        Name   = $Name
        Key    = $Key
        Drop   = $Drop
        Match  = $match
        Prefix = $prefix
        Hex    = [BitConverter]::ToString($dec[0..15])
    }
}

Write-Output "`n=== RC4 brute force: 2dflat.vsh plaintext should start with `#version 100` ==="
$candidates = @('RR3','rr3','Firemint','Firemonkeys','Cloudcell','depot','shader','EA','glu','csdk',
    'RealRacing','realracing','vsh','fsh','gui','MT_GL_ES_3','MT_GL_ES_2','1B:D','vsh1','vsh0','shad',
    '12345678','abcdefgh','EAGL','rr3_')
foreach ($key in $candidates) {
    foreach ($drop in @(0, 256, 512, 1024, 1536, 2048, 3072)) {
        $r = Test-RC4 $ct $key '#version 100' $drop "RC4 key=$key drop=$drop"
        if ($r.Match) {
            Write-Output ("  HIT  {0,-32}  prefix={1}" -f ("key=$key drop=$drop"), $r.Prefix.Substring(0, [Math]::Min(40, $r.Prefix.Length)))
        }
    }
}

# Try RC4 with key starting from offset 6 of cipher (use first 6 bytes as part of key)
Write-Output "`n=== RC4 with header-mixed keys (using first N bytes of cipher as key) ==="
foreach ($n in @(4, 6, 8)) {
    $kb = $ct[0..($n-1)]
    foreach ($drop in @(0, 256, 512)) {
        $r = Test-RC4 $ct[6..($ct.Length-1)] ([BitConverter]::ToString($kb).Replace('-','')) '#version 100' $drop "key=${n}-byte header"
        if ($r.Match) {
            Write-Output ("  HIT  n={0} drop={1}" -f $n, $drop)
        }
    }
}

# Try RC4 with key from bytes 0-1 of cipher (file-specific nonce)
Write-Output "`n=== RC4 with key = bytes 0-1 of cipher (per-file nonce) ==="
foreach ($b0 in 0..15) {
    foreach ($b1 in 0..15) {
        $key = [byte[]]@(([int]$ct[0]) -bxor $b0, ([int]$ct[1]) -bxor $b1)
        $dec = Invoke-RC4 $ct[6..($ct.Length-1)] $key 0
        if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq '#version 100') {
            Write-Output ("  HIT  b0={0:X2} b1={1:X2} prefix={2}" -f $b0, $b1, ([System.Text.Encoding]::ASCII.GetString($dec[0..30])))
        }
    }
}

# Constant XOR with each seed at offset 6
Write-Output "`n=== Constant XOR at offset 6 (each seed) ==="
for ($seed = 0; $seed -lt 256; $seed++) {
    $dec = New-Object byte[] ($ct.Length - 6)
for ($i=6; $i -lt $ct.Length; $i++) { $dec[$i-6] = (([int]$ct[$i]) -bxor $seed) -as [byte] }
    $text = [System.Text.Encoding]::ASCII.GetString($dec[0..12])
    if ($text.StartsWith('#version 100')) {
        Write-Output ("  HIT  seed=0x{0:X2}" -f $seed)
    }
}

# Try XOR with a per-position counter
Write-Output "`n=== XOR with linear keystream: ks[i] = (a + i*b) mod 256 (offset 6+) ==="
for ($a = 0; $a -lt 256; $a += 1) {
    for ($b = 1; $b -lt 256; $b += 1) {
        $dec = New-Object byte[] ($ct.Length - 6)
        for ($i=0; $i -lt $dec.Length; $i++) {
            $ks = ($a + $i * $b) -band 0xFF
            $dec[$i] = ([int]$ct[6+$i]) -bxor $ks
        }
        if ([System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq '#version 100') {
            Write-Output ("  HIT  a=0x{0:X2} b=0x{1:X2}  preview={2}" -f $a, $b, ([System.Text.Encoding]::ASCII.GetString($dec[0..30])))
        }
    }
}