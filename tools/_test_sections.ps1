$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')
$e_shoff = [System.BitConverter]::ToInt64($so, 40)
$e_shentsize = [System.BitConverter]::ToInt16($so, 58)
$e_shnum = [System.BitConverter]::ToInt16($so, 60)
$e_shstrndx = [System.BitConverter]::ToInt16($so, 62)
$shstrtab_off = $e_shoff + $e_shstrndx * $e_shentsize
$strtab_off = [System.BitConverter]::ToInt64($so, $shstrtab_off + 24)
$strtab_size = [System.BitConverter]::ToInt64($so, $shstrtab_off + 32)
$sb = New-Object System.Text.StringBuilder
$i = 0
while ($i -lt $e_shnum) {
    $sh_off = $e_shoff + $i * $e_shentsize
    $sh_name_idx = [System.BitConverter]::ToInt32($so, $sh_off)
    $nstart = $strtab_off + $sh_name_idx
    $nend = $nstart
    while ($nend -lt $strtab_off + $strtab_size -and $so[$nend] -ne 0) { $nend++ }
    $name = [System.Text.Encoding]::ASCII.GetString($so[$nstart..($nend-1)])
    $sh_size_val = [System.BitConverter]::ToInt64($so, $sh_off + 32)
    $sh_off_val = [System.BitConverter]::ToInt64($so, $sh_off + 24)
    $sh_addr = [System.BitConverter]::ToInt64($so, $sh_off + 16)
    [void]$sb.AppendLine(("[{0,2}] {1,-15} addr=0x{2:X8} off=0x{3:X8} size=0x{4:X8}" -f $i, $name, $sh_addr, $sh_off_val, $sh_size_val))
    $i++
}
Write-Output $sb.ToString()