$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path $projectRoot -Parent
$outputDir = Join-Path $projectRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'MSVC C compiler is required for these host tests.' }
$toolchain = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$kitRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (!$kitRoot) { $kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$kitVersion = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory |
    Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending |
    Select-Object -First 1 -ExpandProperty Name
$previous = @{ INCLUDE = $env:INCLUDE; LIB = $env:LIB; Path = $env:Path }
try {
    $env:INCLUDE = (@((Join-Path $toolchain 'include'),
        (Join-Path $kitRoot "Include\$kitVersion\ucrt"),
        (Join-Path $kitRoot "Include\$kitVersion\shared"),
        (Join-Path $kitRoot "Include\$kitVersion\um")) -join ';')
    $env:LIB = (@((Join-Path $toolchain 'lib\x64'),
        (Join-Path $kitRoot "Lib\$kitVersion\ucrt\x64"),
        (Join-Path $kitRoot "Lib\$kitVersion\um\x64")) -join ';')
    $compilerBin = Join-Path $toolchain 'bin\Hostx64\x64'
    $env:Path = $compilerBin + ';' + $env:Path
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    foreach ($test in @('touch_rotation_test', 'audio_capture_test', 'qspi_window_test')) {
        $program = Join-Path $outputDir ($test + '.exe')
        $arguments = @('/nologo', '/std:c11', '/experimental:c11atomics',
            ('/I' + (Join-Path $PSScriptRoot 'stubs')),
            ('/I' + (Join-Path $projectRoot 'components\sugarat_board')),
            ('/I' + (Join-Path $repoRoot 'sugarat\main')),
            (Join-Path $PSScriptRoot ($test + '.c')),
            ('/Fo' + (Join-Path $outputDir ($test + '.obj'))), ('/Fe' + $program))
        & (Join-Path $compilerBin 'cl.exe') @arguments
        if ($LASTEXITCODE -ne 0) { throw "$test compilation failed." }
        & $program
        if ($LASTEXITCODE -ne 0) { throw "$test failed." }
    }
} finally {
    $env:INCLUDE = $previous.INCLUDE
    $env:LIB = $previous.LIB
    $env:Path = $previous.Path
}
