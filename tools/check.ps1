# Local release check for the renderer on Windows: builds the C core, every production shader (SPIR-V, MSL)
# and the native tests with the vendored SDL_shadercross, runs the tests (the device and window smoke tests
# need a GPU and a display, which this machine has) and compiles the public headers on their own.
#
#   powershell -ExecutionPolicy Bypass -File tools/check.ps1 [-BuildDir <dir>] [-SkipGpuTests]
#
# Needs CMake, Ninja and Visual Studio 2022 (C++ workload). A cold build compiles DXC and takes a while;
# later runs are incremental. Submodules must be initialised (git submodule update --init --recursive).

param(
    [string]$BuildDir = (Join-Path $env:LOCALAPPDATA "btbh-renderer-check"),
    [switch]$SkipGpuTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

function Invoke-Msvc([string]$command) {
    cmd /c "`"$vcvars`" >nul && $command"
    if ($LASTEXITCODE -ne 0) { throw "failed ($LASTEXITCODE): $command" }
}

Write-Host "== configure and build ($BuildDir)"
Invoke-Msvc ("cmake -S `"$root`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release " +
    "-DR3D_ASSIMP_VENDORED=ON -DBTBH_BUILD_GPU_SPIKE=ON -DBTBH_BUILD_GPU_NATIVE_TESTS=ON " +
    "-DBTBH_BUILD_GPU_PACKAGE_RUNTIME=ON")
Invoke-Msvc "cmake --build `"$BuildDir`" --parallel"

Write-Host "== shader artifacts"
$out = Join-Path $BuildDir "generated\btbh_gpu\shaders"
$names = Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern '"(production_[a-z0-9_]+\.(vert|frag|comp))\|' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$missing = @()
foreach ($name in $names) {
    foreach ($extension in "spv", "msl") {
        if (-not (Test-Path (Join-Path $out "$name.$extension"))) { $missing += "$name.$extension" }
    }
}
if ($missing) { throw "missing shader artifacts: $($missing -join ', ')" }
Write-Host "$($names.Count) shaders: SPIR-V and MSL present"

Write-Host "== native tests"
$exclude = if ($SkipGpuTests) { "-E `"btbh_gpu_device_smoke|btbh_gpu_window_smoke`"" } else { "" }
Invoke-Msvc "ctest --test-dir `"$BuildDir`" --output-on-failure $exclude"

Write-Host "== public headers on their own (C11, C++17, /W4 /WX)"
$headerDir = Join-Path $BuildDir "header-check"
New-Item -ItemType Directory -Force $headerDir | Out-Null
$source = "#include <btbh_gpu/btbh_gpu.h>`n#include <btbh_gpu/btbh_gpu_resources.h>`nint main(void){return 0;}`n"
Set-Content -Path (Join-Path $headerDir "check.c") -Value $source -NoNewline
Set-Content -Path (Join-Path $headerDir "check.cpp") -Value $source -NoNewline
$include = Join-Path $root "include"
Invoke-Msvc "cd /d `"$headerDir`" && cl /nologo /W4 /WX /std:c11 /I`"$include`" /c check.c && cl /nologo /W4 /WX /std:c++17 /EHsc /I`"$include`" /c check.cpp"

Write-Host "== all checks passed"
