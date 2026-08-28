param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build64 = Join-Path $root "build-x64"
$build32 = Join-Path $root "build-win32"

cmake -S $root -B $build64 -A x64
cmake --build $build64 --config $Configuration
cmake -S $root -B $build32 -A Win32
cmake --build $build32 --config $Configuration

$output = Join-Path $build64 $Configuration
Copy-Item (Join-Path $build32 "$Configuration\OverlayInputHook32.dll") $output -Force
Copy-Item (Join-Path $build32 "$Configuration\GameOverlayInjector32.exe") $output -Force
Write-Host "Pacote pronto em $output"
