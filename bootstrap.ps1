param([switch]$Build)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force third_party, .tools | Out-Null
$dependencies = Get-Content -LiteralPath scripts/dependencies.json -Raw | ConvertFrom-Json
foreach ($dependency in $dependencies) {
    $destination = Join-Path 'third_party' $dependency.destination
    if (Test-Path (Join-Path $destination $dependency.marker)) { continue }
    $archive = Join-Path '.tools' $dependency.archive
    $valid = (Test-Path $archive) -and ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -eq $dependency.sha256)
    if (!$valid) { Invoke-WebRequest $dependency.url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $dependency.sha256) {
        throw "SHA256 mismatch: $($dependency.name). Extraction was not attempted."
    }
    if ($dependency.format -eq 'zip') {
        Expand-Archive -LiteralPath $archive -DestinationPath third_party -Force
    } else {
        tar -xf $archive -C third_party
        if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $($dependency.name)" }
    }
    Move-Item -LiteralPath (Join-Path 'third_party' $dependency.extracted) -Destination $destination
    if (!(Test-Path (Join-Path $destination $dependency.marker))) { throw "Missing dependency files: $($dependency.name)" }
}
if ($Build) { & "$PSScriptRoot/build.ps1" }
