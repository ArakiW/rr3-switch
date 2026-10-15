# Look at the ChaCha20 context and try to find the key/nonce
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# Look at the ChaCha20 constant context
Write-Output "=== ChaCha20 constant at 0x1B567A0 (context) ==="
$start = 0x1B56780
$end = [Math]::Min($so.Length - 1, 0x1B56840)
$ctx = $so[$start..$end]
$hex = ($ctx | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"

# Look at constant 2
Write-Output "`n=== ChaCha20 constant at 0x1B584C0 (context) ==="
$start = 0x1B58490
$end = [Math]::Min($so.Length - 1, 0x1B58520)
$ctx = $so[$start..$end]
$hex = ($ctx | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"

# Find function near the ChaCha20 constant
# Look for SIGSTK or .text section near this address
Write-Output "`n=== Map of SO ELF sections (rough) ==="
# ELF header is at the start
$elf_magic = $so[0..3]
Write-Output "  ELF magic: $([BitConverter]::ToString($elf_magic))"
# ELF64 header has e_shoff at offset 40 (u64)
$e_shoff = [System.BitConverter]::ToInt64($so, 40)
Write-Output "  e_shoff (section header offset): 0x$($e_shoff.ToString('X'))"
$e_shentsize = [System.BitConverter]::ToInt16($so, 58)
$e_shnum = [System.BitConverter]::ToInt16($so, 60)
Write-Output "  e_shnum: $e_shnum sections, entry size: $e_shentsize"
$e_shstrndx = [System.BitConverter]::ToInt16($so, 62)
Write-Output "  e_shstrndx: $e_shstrndx"

# Read section headers
$shstrtab_offset = $e_shoff + $e_shstrndx * $e_shentsize
$shstrtab_sh_offset = [System.BitConverter]::ToInt64($so, $shstrtab_offset + 24)
$shstrtab_sh_size = [System.BitConverter]::ToInt64($so, $shstrtab_offset + 32)

for ($i = 0; $i -lt $e_shnum; $i++) {
    $sh_off = $e_shoff + $i * $e_shentsize
    $sh_name = [System.BitConverter]::ToInt32($so, $sh_off)
    $sh_type = [System.BitConverter]::ToInt32($so, $sh_off + 4)
    $sh_addr = [System.BitConverter]::ToInt64($so, $sh_off + 16)
    $sh_offset = [System.BitConverter]::ToInt64($so, $sh_off + 24)
    $sh_size = [System.BitConverter]::ToInt64($so, $sh_off + 32)
    # Get name from shstrtab
    $name_start = $shstrtab_sh_offset + $sh_name
    $name_end = $name_start
    while ($name_end -lt $shstrtab_sh_offset + $shstrtab_sh_size -and $so[$name_end] -ne 0) { $name_end++ }
    $name = [System.Text.Encoding]::ASCII.GetString($so[$name_start..($name_end-1)])

    if ($sh_addr -gt 0 -and $sh_addr -lt 0x30000000) {
        Write-Output ("  [{0,2}] {1,-12}  addr=0x{2:X8}  off=0x{3:X8}  size=0x{4:X8}" -f $i, $name, $sh_addr, $sh_offset, $sh_size)
    }
}

# Try to decrypt with ChaCha20
Write-Output "`n=== ChaCha20 brute force attempt ==="
$ct = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot\Shaders\rr2\2dflat.vsh')

# Simple ChaCha20 implementation
function Quarter-Round([int[]]$state, [int]$a, [int]$b, [int]$c, [int]$d) {
    $state[$a] = ($state[$a] + $state[$b]) -band 0xFFFFFFFF
    $state[$d] = ($state[$d] -bxor $state[$a])
    $state[$d] = (($state[$d] -shl 16) -bor ($state[$d] -shr 16)) -band 0xFFFFFFFF
    $state[$c] = ($state[$c] + $state[$d]) -band 0xFFFFFFFF
    $state[$b] = ($state[$b] -bxor $state[$c])
    $state[$b] = (($state[$b] -shl 12) -bor ($state[$b] -shr 20)) -band 0xFFFFFFFF
    $state[$a] = ($state[$a] + $state[$b]) -band 0xFFFFFFFF
    $state[$d] = ($state[$d] -bxor $state[$a])
    $state[$d] = (($state[$d] -shl 8) -bor ($state[$d] -shr 24)) -band 0xFFFFFFFF
    $state[$c] = ($state[$c] + $state[$d]) -band 0xFFFFFFFF
    $state[$b] = ($state[$b] -bxor $state[$c])
    $state[$b] = (($state[$b] -shl 7) -bor ($state[$b] -shr 25)) -band 0xFFFFFFFF
}

function Get-ChaCha20Block([byte[]]$key, [byte[]]$nonce, [int]$counter) {
    # 16 x 32-bit state
    $state = New-Object int[] 16
    # Constants
    $state[0] = 0x61707865; $state[1] = 0x3320646e
    $state[2] = 0x79622d32; $state[3] = 0x6b206574
    # Key (8 x 32-bit)
    for ($i = 0; $i -lt 8; $i++) {
        $state[4 + $i] = [System.BitConverter]::ToInt32($key, $i * 4)
    }
    # Counter
    $state[12] = $counter
    # Nonce (12 bytes = 3 x 32-bit)
    for ($i = 0; $i -lt 3; $i++) {
        $state[13 + $i] = [System.BitConverter]::ToInt32($nonce, $i * 4)
    }

    # 20 rounds (10 column + 10 diagonal pairs)
    $working = $state.Clone()
    for ($i = 0; $i -lt 10; $i++) {
        # Column rounds
        Quarter-Round $working 0 4 8 12
        Quarter-Round $working 1 5 9 13
        Quarter-Round $working 2 6 10 14
        Quarter-Round $working 3 7 11 15
        # Diagonal rounds
        Quarter-Round $working 0 5 10 15
        Quarter-Round $working 1 6 11 12
        Quarter-Round $working 2 7 8 13
        Quarter-Round $working 3 4 9 14
    }
    # Add original state
    for ($i = 0; $i -lt 16; $i++) {
        $working[$i] = ($working[$i] + $state[$i]) -band 0xFFFFFFFF
    }
    # Output as bytes (little-endian)
    $out = New-Object byte[] 64
    for ($i = 0; $i -lt 16; $i++) {
        $bytes = [System.BitConverter]::GetBytes($working[$i])
        for ($j = 0; $j -lt 4; $j++) {
            $out[$i*4 + $j] = $bytes[$j]
        }
    }
    return $out
}

function Invoke-ChaCha20([byte[]]$data, [byte[]]$key, [byte[]]$nonce, [int]$counter = 0) {
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

# Brute force with various keys (ChaCha20 needs 32 bytes, nonce 12 bytes)
# First, derive a 32-byte key from various sources
function Get-KeyFromString([string]$s) {
    $kb = [System.Text.Encoding]::ASCII.GetBytes($s)
    if ($kb.Length -eq 32) { return $kb }
    $out = New-Object byte[] 32
    for ($i = 0; $i -lt 32; $i++) { $out[$i] = $kb[$i % $kb.Length] }
    return $out
}

function Get-KeyFromHex([string]$hex) {
    return [byte[]]($hex -replace '..', '0x$& ' -split ' ' | Where-Object { $_ -ne '' } | ForEach-Object { [byte]$_ })
}

# Try with MD5 of file as nonce (12 bytes truncated)
$md5hex = '75a7824385b76f23fb3b38d4d6f886e4'
$md5bytes = Get-KeyFromHex $md5hex
$nonce = New-Object byte[] 12
for ($i=0; $i -lt 12; $i++) { $nonce[$i] = $md5bytes[$i] }

$keys_to_try = @(
    ('rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr3' + 'rr' | Out-String).Trim() -as [string]
)
$keys = @('rr3rr3rr3rr3rr3rr3rr3rr3rr3rr3rr', 'RealRacing3RealRacing3RealRacing3Re', 'firemonkeysfiremonkeysfiremonk')
foreach ($k in $keys) {
    $key = Get-KeyFromString $k
    $dec = Invoke-ChaCha20 $ct $key $nonce 0
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq '#version 100') {
        Write-Output "  HIT key='$k' nonce=md5"
    }
}

# Brute force: 4-byte key repeated (32 bytes total)
Write-Output "`n=== ChaCha20 with key = 4-byte pattern repeated ==="
for ($a = 0; $a -lt 256; $a++) {
    $kb = New-Object byte[] 32
    for ($i = 0; $i -lt 32; $i++) { $kb[$i] = $a }
    $dec = Invoke-ChaCha20 $ct $kb $nonce 0
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq '#version 100') {
        Write-Output "  HIT key=0x$($a.ToString('X2'))-repeated"
    }
}

# Brute force: 8-byte key
Write-Output "`n=== ChaCha20 with 8-byte ASCII keys ==="
foreach ($s in @('rr3rr3rr', 'rr3shader', 'RR3RR3RR', 'Firemonke', 'csdkcsdk',
    'realracin', 'realracer', 'defaultke', '12345678', 'abcderrr',
    'rr3key00', 'rr3_key0', 'rr3encry', 'opengles0', 'opengles3',
    'shaders_c', 'shader00', 'shaders0', 'rr3s0', 'rr3s0d',
    'RealRac', 'emg-r3', 'rr3cm', 'reacers', 'ssrrcc0', 'rr3aes00')) {
    $key = Get-KeyFromString $s
    $dec = Invoke-ChaCha20 $ct $key $nonce 0
    if ($dec.Length -ge 13 -and [System.Text.Encoding]::ASCII.GetString($dec[0..12]) -eq '#version 100') {
        Write-Output "  HIT key='$s' nonce=md5"
    }
}