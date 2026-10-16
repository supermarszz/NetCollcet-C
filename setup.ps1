$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$required = @('.tools/python/ziglang/zig.exe', '.tools/python/cmake/data/bin/cmake.exe', '.tools/python/bin/ninja.exe')
$missing = @($required | Where-Object { !(Test-Path $_) })
if ($missing.Count -gt 0) {
    python -m pip install --target .tools/python -r requirements-build.txt
    if ($LASTEXITCODE -ne 0) { throw 'Portable build tool installation failed' }
}
& ./bootstrap.ps1
