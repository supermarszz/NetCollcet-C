param([switch]$Gui, [switch]$Benchmark)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
foreach ($program in @('core_tests', 'log_tests')) {
    & "./build/$program.exe"
    if ($LASTEXITCODE -ne 0) { throw "$program failed" }
}
$arguments = @('-B', 'ctests/integration.py')
if ($Gui) { $arguments += '--gui' }
python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Integration tests failed' }
python -B ctests/inspect_binary.py
if ($LASTEXITCODE -ne 0) { throw 'PE import validation failed' }
if ($Benchmark) {
    python -B ctests/benchmark.py --mixed
    if ($LASTEXITCODE -ne 0) { throw 'Mixed-protocol benchmark failed' }
}
