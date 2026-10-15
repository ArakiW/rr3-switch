# Look for hardcoded keys near ChaCha20 constants
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# Look around the ChaCha20 constants
Write-Output "=== Bytes around ChaCha20 @ 0x1B567A0 (256 bytes) ==="
$start = 0x1B567A0 - 64
$end = 0x1B567A0 + 64
$ctx = $so[$start..$end]
$hex = ($ctx | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"

# Print as text where printable
$sb = New-Object System.Text.StringBuilder
for ($i = $start; $i -le $end; $i++) {
    if ($so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) { [void]$sb.Append([char]$so[$i]) }
    else { [void]$sb.Append('.') }
}
Write-Output "  text: $($sb.ToString())"

Write-Output "`n=== Bytes around ChaCha20 @ 0x1B584C0 (256 bytes) ==="
$start = 0x1B584C0 - 64
$end = 0x1B584C0 + 64
$ctx = $so[$start..$end]
$hex = ($ctx | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"
$sb = New-Object System.Text.StringBuilder
for ($i = $start; $i -le $end; $i++) {
    if ($so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) { [void]$sb.Append([char]$so[$i]) }
    else { [void]$sb.Append('.') }
}
Write-Output "  text: $($sb.ToString())"

# Find all places where 'csdk' appears with .vsh files (the call site)
Write-Output "`n=== Looking at where shader paths are referenced (call site context) ==="
# 2dflat.vsh at 0x3D6E82
$start = 0x3D6E50
$end = 0x3D6EE0
$ctx = $so[$start..$end]
$hex = ($ctx | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output "  hex: $hex"

# Print as text
$sb = New-Object System.Text.StringBuilder
for ($i = $start; $i -le $end; $i++) {
    if ($so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) { [void]$sb.Append([char]$so[$i]) }
    elseif ($so[$i] -eq 0x00) { [void]$sb.Append('|0|') }
    else { [void]$sb.Append('.') }
}
Write-Output "  text: $($sb.ToString())"

# Look at the code that USES the ChaCha20 constant
# This is in the .text section, so we should look for function prologues near 0x1B567xx
# But we need to know the .text section boundaries first
$e_shoff = [System.BitConverter]::ToInt64($so, 40)
$e_shentsize = [System.BitConverter]::ToInt16($so, 58)
$e_shnum = [System.BitConverter]::ToInt16($so, 60)
$e_shstrndx = [System.BitConverter]::ToInt16($so, 62)
$shstrtab_offset = $e_shoff + $e_shstrndx * $e_shentsize
$shstrtab_sh_offset = [System.BitConverter]::ToInt64($so, $shstrtab_offset + 24)
$shstrtab_sh_size = [System.BitConverter]::ToInt64($so, $shstrtab_offset + 32)

Write-Output "`n=== ELF sections containing ChaCha20 constant 0x1B567A0 ==="
for ($i = 0; $i -lt $e_shnum; $i++) {
    $sh_off = $e_shoff + $i * $e_shentsize
    $sh_addr = [System.BitConverter]::ToInt64($so, $sh_off + 16)
    $sh_size = [System.BitConverter]::ToInt64($so, $sh_off + 32)
    if ($sh_addr -le 0x1B567A0 -and ($sh_addr + $sh_size) -gt 0x1B567A0) {
        $sh_name = [System.BitConverter]::ToInt32($so, $sh_off)
        $name_start = $shstrtab_sh_offset + $sh_name
        $name_end = $name_start
        while ($name_end -lt $shstrtab_sh_offset + $shstrtab_sh_size -and $so[$name_end] -ne 0) { $name_end++ }
        $name = [System.Text.Encoding]::ASCII.GetString($so[$name_start..($name_end-1)])
        Write-Output ("  [{0,2}] {1,-12}  addr=0x{2:X8}  size=0x{3:X8}" -f $i, $name, $sh_addr, $sh_size)
    }
}