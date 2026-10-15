# Search for strings in the SO file related to shaders
$ErrorActionPreference = 'Stop'

# Use .NET to read the SO and search for strings
$so = [System.IO.File]::ReadAllBytes('E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so')
Write-Output "SO file size: $($so.Length) bytes"

# Find all printable strings of length >= 8
$strings = New-Object System.Collections.Generic.List[string]
$i = 0
while ($i -lt $so.Length) {
    if ($so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) {
        $start = $i
        $sb = New-Object System.Text.StringBuilder
        while ($i -lt $so.Length -and $so[$i] -ge 0x20 -and $so[$i] -lt 0x7f) {
            [void]$sb.Append([char]$so[$i])
            $i++
        }
        if ($sb.Length -ge 8) {
            $strings.Add("$start $($sb.ToString())")
        }
    } else {
        $i++
    }
}

Write-Output "Found $($strings.Count) strings of length >= 8"

# Look for shader-related strings
Write-Output "`n=== Shader-related strings ==="
$strings | Where-Object { $_ -match '(?i)shader|vsh|fsh|gl_|opengl|depot|asset|decrypt|cipher|aes|rc4|xor' } | Select-Object -First 50

Write-Output "`n=== File extension strings (.vsh, .fsh, .gui) ==="
$strings | Where-Object { $_ -match '\.(vsh|fsh|gui|xsh)\b' } | Select-Object -First 30

Write-Output "`n=== Strings with 'MT_GL_ES' ==="
$strings | Where-Object { $_ -match 'MT_GL' } | Select-Object -First 30

Write-Output "`n=== Strings with 'version' ==="
$strings | Where-Object { $_ -match 'version' } | Select-Object -First 30

Write-Output "`n=== Strings with '1B' or '0x1b' ==="
$strings | Where-Object { $_ -match '1B' } | Select-Object -First 30

Write-Output "`n=== Decryption/encryption related ==="
$strings | Where-Object { $_ -match '(?i)encrypt|decrypt|cipher|key|secret|obfusc|xor' } | Select-Object -First 30

Write-Output "`n=== Looking for 'csdk' (game SDK namespace) ==="
$strings | Where-Object { $_ -match 'csdk' } | Select-Object -First 30

Write-Output "`n=== 'mt3D' or 'mtShader' strings ==="
$strings | Where-Object { $_ -match 'mt3D|mtShader' } | Select-Object -First 30

Write-Output "`n=== 'rr3' literal strings ==="
$strings | Where-Object { $_ -match 'rr3' } | Select-Object -First 30

Write-Output "`n=== 'feats' / '.feats' ==="
$strings | Where-Object { $_ -match 'feats' } | Select-Object -First 30