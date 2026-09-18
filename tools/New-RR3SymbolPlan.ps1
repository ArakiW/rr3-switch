[CmdletBinding()]
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputPath) {
    $OutputPath = Join-Path $projectRoot 'analysis\rr3-symbol-binding-plan.json'
}

$androidBin = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.3.13750724\toolchains\llvm\prebuilt\windows-x86_64\bin"
$androidNm = Join-Path $androidBin 'llvm-nm.exe'
$dkpRoot = Join-Path $projectRoot '.tools\devkitPro\msys64\opt\devkitpro'
$targetNm = Join-Path $dkpRoot 'devkitA64\bin\aarch64-none-elf-nm.exe'
$gameLibrary = Join-Path $projectRoot 'reverse\native\arm64\libRealRacing3.so'

foreach ($required in @($androidNm, $targetNm, $gameLibrary)) {
    if (-not (Test-Path $required -PathType Leaf)) {
        throw "Missing required file: $required"
    }
}

$undefined = [ordered]@{}
& $androidNm --dynamic --undefined-only $gameLibrary | ForEach-Object {
    if ($_ -match '^\s+([Uw])\s+(.+)$') {
        $name = $Matches[2] -replace '@.*$', ''
        $isWeak = $Matches[1] -eq 'w'
        if (-not $undefined.Contains($name) -or -not $isWeak) {
            $undefined[$name] = $isWeak
        }
    }
}

$libraries = @(
    (Join-Path $dkpRoot 'devkitA64\aarch64-none-elf\lib\libc.a'),
    (Join-Path $dkpRoot 'devkitA64\aarch64-none-elf\lib\libm.a'),
    (Join-Path $dkpRoot 'devkitA64\aarch64-none-elf\lib\libstdc++.a'),
    (Join-Path $dkpRoot 'devkitA64\aarch64-none-elf\lib\libsupc++.a'),
    (Join-Path $dkpRoot 'libnx\lib\libnx.a')
)
$available = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($library in $libraries) {
    & $targetNm -g --defined-only $library 2>$null | ForEach-Object {
        if ($_ -match '^\s*[0-9A-Fa-f]+\s+[A-Za-z]\s+(.+)$') {
            [void]$available.Add($Matches[1])
        }
    }
}

function Get-Category([string]$Name) {
    if ($Name -match '^FMOD_|^_ZN4FMOD') { return 'fmod' }
    if ($Name -match '^egl') { return 'egl' }
    if ($Name -match '^gl[A-Z]') { return 'gles' }
    if ($Name -match '^Ar[A-Z]') { return 'arcore' }
    if ($Name -match '^pthread') { return 'pthread' }
    if ($Name -match '^__android_') { return 'android' }
    if ($Name -match '^_Z|^__cxa|^__gxx|^_Unwind') { return 'cxx' }
    return 'other'
}

$bionicAbiNames = @(
    '__errno', '__memcpy_chk', '__memmove_chk', '__memset_chk', '__stack_chk_fail',
    '__strcat_chk', '__strcpy_chk', '__strncpy_chk', '__vsnprintf_chk', '__vsprintf_chk',
    '_ctype_', 'longjmp', 'setjmp'
)

$records = foreach ($name in ($undefined.Keys | Sort-Object)) {
    $weak = [bool]$undefined[$name]
    $category = Get-Category $name
    $nameMatch = $available.Contains($name)
    $binding = if ($weak -and -not $nameMatch) {
        'optional-weak-stub'
    }
    elseif ($nameMatch -and $category -eq 'cxx') {
        'cxx-abi-review'
    }
    elseif ($nameMatch -and $name -in $bionicAbiNames) {
        'bionic-abi-shim'
    }
    elseif ($nameMatch) {
        'direct-name-match-candidate'
    }
    elseif ($category -in @('gles', 'egl')) {
        'graphics-shim'
    }
    elseif ($category -in @('fmod', 'arcore')) {
        'middleware-or-disable'
    }
    elseif ($category -eq 'pthread') {
        'pthread-shim'
    }
    elseif ($category -eq 'android') {
        'android-log-shim'
    }
    elseif ($category -eq 'cxx') {
        'libcxx-bionic-runtime'
    }
    else {
        'platform-shim-review'
    }

    [pscustomobject]@{
        name = $name
        weak = $weak
        category = $category
        target_name_match = $nameMatch
        binding = $binding
    }
}

$bindingCounts = [ordered]@{}
$categoryCounts = [ordered]@{}
foreach ($record in $records) {
    $bindingCounts[$record.binding] = 1 + [int]($bindingCounts[$record.binding])
    $categoryCounts[$record.category] = 1 + [int]($categoryCounts[$record.category])
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString('o')
    input = $gameLibrary
    undefined_unique = $records.Count
    weak_unique = @($records | Where-Object weak).Count
    target_defined_names = $available.Count
    target_name_matches = @($records | Where-Object target_name_match).Count
    binding_counts = $bindingCounts
    category_counts = $categoryCounts
    caveats = @(
        'A matching symbol name is not proof of Bionic/newlib or libc++/libstdc++ ABI compatibility.',
        'No external relocation is patched by this report.',
        'Socket APIs require target service initialization before use.'
    )
    symbols = $records
}

$parent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $parent | Out-Null
$report | ConvertTo-Json -Depth 6 | Set-Content -Path $OutputPath -Encoding utf8
Write-Host "Symbol plan: $OutputPath" -ForegroundColor Green

