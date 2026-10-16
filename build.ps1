param([switch]$Debug)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$cmake = Join-Path $PSScriptRoot '.tools/python/cmake/data/bin/cmake.exe'
$ninja = Join-Path $PSScriptRoot '.tools/python/bin/ninja.exe'
$zig = Join-Path $PSScriptRoot '.tools/python/ziglang/zig.exe'
if (!(Test-Path $cmake) -or !(Test-Path $zig) -or !(Test-Path $ninja)) {
    throw 'Run setup.ps1 to install the pinned build tools and dependencies'
}
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot '.tools/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $PSScriptRoot '.tools/zig-local-cache'
$mode = if ($Debug) { 'Debug' } else { 'Release' }
& $cmake -S . -B build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$zig" '-DCMAKE_C_COMPILER_ARG1=cc' '-DCMAKE_C_FLAGS=-target x86_64-windows-gnu' "-DCMAKE_AR=$PSScriptRoot/scripts/zig-ar.cmd" "-DCMAKE_RANLIB=$PSScriptRoot/scripts/zig-ranlib.cmd" "-DCMAKE_BUILD_TYPE=$mode"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ./build/core_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Core tests failed' }
& ./build/log_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Logging tests failed' }
