$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $projectRoot 'build\ui-preview'
$images = Join-Path $PSScriptRoot 'renders\device-states'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'MSVC C compiler is required for the native preview.' }
$toolchain = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$kitRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (!$kitRoot) { $kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$kitVersion = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory |
    Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending |
    Select-Object -First 1 -ExpandProperty Name
$cmake = 'D:\esp_idf\v6.1\tools\cmake\4.0.3\bin\cmake.exe'
$ninja = 'D:\esp_idf\v6.1\tools\ninja\1.12.1\ninja.exe'
$previous = @{ INCLUDE = $env:INCLUDE; LIB = $env:LIB; Path = $env:Path }
try {
    $env:INCLUDE = (@((Join-Path $toolchain 'include'),
        (Join-Path $kitRoot "Include\$kitVersion\ucrt"),
        (Join-Path $kitRoot "Include\$kitVersion\shared"),
        (Join-Path $kitRoot "Include\$kitVersion\um")) -join ';')
    $env:LIB = (@((Join-Path $toolchain 'lib\x64'),
        (Join-Path $kitRoot "Lib\$kitVersion\ucrt\x64"),
        (Join-Path $kitRoot "Lib\$kitVersion\um\x64")) -join ';')
    $sdkBin = Join-Path $kitRoot "bin\$kitVersion\x64"
    $env:Path = (Join-Path $toolchain 'bin\Hostx64\x64') + ';' + $sdkBin + ';' + $env:Path
    New-Item -ItemType Directory -Force -Path $output, $images | Out-Null
    & $cmake -S $PSScriptRoot -B $output -G Ninja ('-DCMAKE_MAKE_PROGRAM=' + $ninja) `
        ('-DCMAKE_RC_COMPILER=' + (Join-Path $sdkBin 'rc.exe')) `
        ('-DCMAKE_MT=' + (Join-Path $sdkBin 'mt.exe')) -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'Preview configuration failed.' }
    & $cmake --build $output --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'Preview compilation failed.' }
    & (Join-Path $output 'amoled_ui_preview.exe') $output
    if ($LASTEXITCODE -ne 0) { throw 'Preview rendering failed.' }
    & python (Join-Path $PSScriptRoot 'package-renders.py') $output $images
    if ($LASTEXITCODE -ne 0) { throw 'Preview packaging failed.' }
} finally {
    $env:INCLUDE = $previous.INCLUDE
    $env:LIB = $previous.LIB
    $env:Path = $previous.Path
}
