# Try path-based keys and other theories
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
$expected = '#version 100'

# Path-based keys
$pathKeys = @(
    'rr2', 'rr2/2dflat', '/rr2/2dflat', '.depot/Shaders/rr2/2dflat',
    '2dflat', '2dflat.vsh', '.vsh', 'rr2/', '/rr2/',
    'Shaders/rr2/2dflat', 'Shaders', 'csdk', 'FM', 'EA',
    'RealRacing3/rr2/2dflat', 'rr3', 'rr3/rr2/2dflat',
    'rr3rr22dflat', 'rr3-rr2-2dflat'
)
foreach ($k in $pathKeys) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($k)
    foreach ($drop in @(0, 256, 512, 1024, 2048)) {
        $dec = Invoke-RC4 $ct $kb $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT path-key='$k' drop=$drop"
        }
    }
}

# Maybe the cipher is just XOR with a 256-byte S-box seeded by the file's MD5
Write-Output "`n=== XOR with permuted S-box (S[i] = (i + 1) ^ key) for various keys ==="
foreach ($key in @('rr3', 'Firemonkeys', 'RealRacing3', '1B:D', 'RR3', 'csdk')) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($key)
    # Compute a simple hash of the key as a 256-byte table
    $S = New-Object int[] 256
    $keySum = 0
    foreach ($b in $kb) { $keySum = ($keySum + $b) -band 0xFF }
    for ($i=0; $i -lt 256; $i++) { $S[$i] = ($i + $keySum) -band 0xFF }

    # Apply XOR
    $dec = New-Object byte[] $ct.Length
    for ($i=0; $i -lt $ct.Length; $i++) {
        $dec[$i] = ([int]$ct[$i] -bxor $S[$i -band 0xFF]) -as [byte]
    }
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$key' sum=$keySum"
    }
}

# Maybe the cipher is XOR with rotation
Write-Output "`n=== XOR with rotating S-box (S[i] = i XOR (key[i%klen])) ==="
foreach ($key in @('rr3', 'Firemonkeys', '1B:D', 'RR3', 'rr2', 'csdk', 'EA', 'RealRacing')) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($key)
    $dec = New-Object byte[] $ct.Length
    for ($i=0; $i -lt $ct.Length; $i++) {
        $dec[$i] = ([int]$ct[$i] -bxor ([int]$kb[$i % $kb.Length])) -as [byte]
    }
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT key='$key' (repeating XOR)"
    }
}

# Check if the keystream has RC4 fingerprint
Write-Output "`n=== RC4 fingerprint test on keystream ==="
# A property of RC4: the second byte of keystream tends to be 0 for some keys.
# More importantly, RC4 has biased initial bytes (Mantin's analysis).

# Let me check: what if the keystream is a rotated RC4 output?
# I.e., what if the cipher starts somewhere in the middle of RC4 output?

# Try RC4 with various keys AND skip first N bytes
Write-Output "`n=== RC4 with KEY + skip offset 0..100, all common keys ==="
$keys = @('RR3','rr3','Firemint','Firemonkeys','Cloudcell','depot','shader','EA','glu','csdk',
    'RealRacing','realracing','vsh','fsh','gui','MT_GL_ES_3','1B:D','vsh1','shad','rr2',
    'csdk_depot','rr3shader','opengl','Android','GLES')
foreach ($k in $keys) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($k)
    foreach ($drop in 0..200) {
        $dec = Invoke-RC4 $ct $kb $drop
        if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
            Write-Output "  HIT key='$k' drop=$drop"
        }
    }
}

# Maybe a different cipher: XOR with (i*i mod 256) or similar
Write-Output "`n=== XOR with keystream generated from polynomial ==="
foreach ($poly in @(
    @{ Name='i mod 256'; F={param($i) $i -band 0xFF} },
    @{ Name='(i*i) mod 256'; F={param($i) ($i * $i) -band 0xFF} },
    @{ Name='(i+5) mod 256'; F={param($i) ($i + 5) -band 0xFF} },
    @{ Name='(i*5) mod 256'; F={param($i) ($i * 5) -band 0xFF} },
    @{ Name='(i*7) mod 256'; F={param($i) ($i * 7) -band 0xFF} },
    @{ Name='(i*13) mod 256'; F={param($i) ($i * 13) -band 0xFF} }
)) {
    $dec = New-Object byte[] $ct.Length
    for ($i=0; $i -lt $ct.Length; $i++) {
        $ks = & $poly.F $i
        $dec[$i] = ([int]$ct[$i] -bxor $ks) -as [byte]
    }
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq $expected) {
        Write-Output "  HIT poly=$($poly.Name)"
    }
}