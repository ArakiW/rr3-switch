# Search for function names in the SO
$ErrorActionPreference = 'Stop'

$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')

# Get ELF section info
$e_shoff = [System.BitConverter]::ToInt64($so, 40)
$e_shentsize = [System.BitConverter]::ToInt16($so, 58)
$e_shnum = [System.BitConverter]::ToInt16($so, 60)
$e_shstrndx = [System.BitConverter]::ToInt16($so, 62)
$shstrtab_offset = $e_shoff + $e_shstrndx * $e_shentsize
$shstrtab_sh_offset = [System.BitConverter]::ToInt64($so, $shstrtab_offset + 24)
$shstrtab_sh_size = [System.BitConverter]::ToInt64($so, $shstrtab_offset + 32)

# Find .dynstr section (dynamic strings)
$dynstr_off = 0; $dynstr_size = 0
$symtab_off = 0; $symtab_size = 0; $symtab_entsize = 0; $symtab_link = 0
$strtab_off = 0; $strtab_size = 0
for ($i = 0; $i -lt $e_shnum; $i++) {
    $sh_off = $e_shoff + $i * $e_shentsize
    $sh_name = [System.BitConverter]::ToInt32($so, $sh_off)
    $name_start = $shstrtab_sh_offset + $sh_name
    $name_end = $name_start
    while ($name_end -lt $shstrtab_sh_offset + $shstrtab_sh_size -and $so[$name_end] -ne 0) { $name_end++ }
    $name = [System.Text.Encoding]::ASCII.GetString($so[$name_start..($name_end-1)])
    $sh_offset_val = [System.BitConverter]::ToInt64($so, $sh_off + 24)
    $sh_size_val = [System.BitConverter]::ToInt64($so, $sh_off + 32)
    $sh_link = [System.BitConverter]::ToInt32($so, $sh_off + 40)
    $sh_entsize = [System.BitConverter]::ToInt64($so, $sh_off + 56)

    switch -wildcard ($name) {
        '.dynstr' { $dynstr_off = $sh_offset_val; $dynstr_size = $sh_size_val }
        '.dynsym' { $symtab_off = $sh_offset_val; $symtab_size = $sh_size_val; $symtab_entsize = $sh_entsize }
        '.strtab' { $strtab_off = $sh_offset_val; $strtab_size = $sh_size_val }
        '.symtab' { $symtab_off = $sh_offset_val; $symtab_size = $sh_size_val; $symtab_entsize = $sh_entsize }
    }
}

Write-Output "dynstr off=$dynstr_off size=$dynstr_size"
Write-Output "dynsym off=$symtab_off size=$symtab_size entsize=$symtab_entsize"

# Parse dynamic symbol table
function Get-String($off) {
    $start = $off
    while ($start -lt $so.Length -and $so[$start] -ne 0) { $start++ }
    if ($start -eq $off) { return "" }
    return [System.Text.Encoding]::ASCII.GetString($so[$off..($start-1)])
}

Write-Output "`n=== Symbols matching chacha/salsa/crypt/cipher/shader ==="
$num_syms = [int]($symtab_size / $symtab_entsize)
$hits = 0
for ($i = 0; $i -lt $num_syms; $i++) {
    $sym_off = $symtab_off + $i * $symtab_entsize
    $st_name = [System.BitConverter]::ToInt32($so, $sym_off)
    $st_value = [System.BitConverter]::ToInt64($so, $sym_off + 8)
    $st_size = [System.BitConverter]::ToInt64($so, $sym_off + 16)
    $name = Get-String ($dynstr_off + $st_name)
    if ($name -match '(?i)chacha|salsa|crypt|cipher|shader|shadunif|shadercache') {
        Write-Output ("  {0,-40}  addr=0x{1:X8}  size={2}" -f $name, $st_value, $st_size)
        $hits++
        if ($hits -ge 50) { break }
    }
}

Write-Output "`n=== Looking for ALL symbols with 'sha' in name ==="
$num_syms = [int]($symtab_size / $symtab_entsize)
$hits = 0
for ($i = 0; $i -lt $num_syms; $i++) {
    $sym_off = $symtab_off + $i * $symtab_entsize
    $st_name = [System.BitConverter]::ToInt32($so, $sym_off)
    $st_value = [System.BitConverter]::ToInt64($so, $sym_off + 8)
    $st_size = [System.BitConverter]::ToInt64($so, $sym_off + 16)
    $name = Get-String ($dynstr_off + $st_name)
    if ($name -match '(?i)sha[dc]|encrypt|decrypt') {
        Write-Output ("  {0,-40}  addr=0x{1:X8}" -f $name, $st_value)
        $hits++
        if ($hits -ge 30) { break }
    }
}

# Look at .text section symbols - functions
Write-Output "`n=== Functions near 2dflat.vsh string reference ==="
# 2dflat.vsh at 0x3D6E82 - need to find cross-reference
# Look for .text section symbols that include 0x3D6E82 area
$num_syms = [int]($symtab_size / $symtab_entsize)
for ($i = 0; $i -lt $num_syms; $i++) {
    $sym_off = $symtab_off + $i * $symtab_entsize
    $st_name = [System.BitConverter]::ToInt32($so, $sym_off)
    $st_value = [System.BitConverter]::ToInt64($so, $sym_off + 8)
    $st_size = [System.BitConverter]::ToInt64($so, $sym_off + 16)
    $name = Get-String ($dynstr_off + $st_name)
    if ($st_size -gt 0 -and $st_size -lt 1000 -and $st_value -gt 0x3D6000 -and $st_value -lt 0x3D7000) {
        Write-Output ("  {0,-40}  addr=0x{1:X8}  size={2}" -f $name, $st_value, $st_size)
    }
}