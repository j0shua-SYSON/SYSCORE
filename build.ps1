# ============================================================================
#  SYSCORE build script (MinGW-w64 g++)
#  Usage:  powershell -ExecutionPolicy Bypass -File .\build.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# locate g++
$gpp = 'C:\mingw64\bin\g++.exe'
if (-not (Test-Path $gpp)) {
    $c = Get-Command g++ -ErrorAction SilentlyContinue
    if ($c) { $gpp = $c.Source } else { throw "g++ not found (looked in C:\mingw64\bin and PATH)" }
}
Write-Host "compiler : $gpp" -ForegroundColor DarkGray

$srcDir = Join-Path $root 'src'
$src = Get-ChildItem (Join-Path $srcDir '*.cpp') | ForEach-Object FullName
$buildDir = Join-Path $root 'build'
New-Item -ItemType Directory -Force $buildDir | Out-Null
$out = Join-Path $buildDir 'syscore.exe'

$flags = @(
    '-std=c++17','-O2','-municode','-mwindows',
    '-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00',
    '-static','-static-libgcc','-static-libstdc++'
)
# libraries (order matters: after sources)
$libs = @(
    '-lgdiplus','-lgdi32','-luser32','-lpdh','-lpowrprof',
    '-ldxgi','-lpsapi','-lole32','-loleaut32','-ladvapi32','-lshell32','-lwinhttp'
)

Write-Host "building  : $out" -ForegroundColor Cyan
& $gpp @flags @src '-o' $out @libs
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

$sz = '{0:N0} KB' -f ((Get-Item $out).Length / 1KB)
Write-Host "OK        : $out  ($sz)" -ForegroundColor Green
