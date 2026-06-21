# Build (if needed) and launch SYSCORE.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'build\syscore.exe'
if (-not (Test-Path $exe)) { & (Join-Path $root 'build.ps1') }
Start-Process $exe
Write-Host "SYSCORE launched. Right-click the panel for options, double-click to expand." -ForegroundColor Green
