$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$version = (Get-Content -LiteralPath VERSION -Raw).Trim()
if ($version -notmatch '^V[0-9]+\.[0-9]+$') { throw 'Invalid VERSION' }
$binaryVersion = (& ./build/netcollect-cli.exe --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $binaryVersion -ne "NetCollcet-C $version") {
    throw 'Executable version does not match VERSION; run build.ps1 before packaging'
}
python -B ctests/make_example.py
if ($LASTEXITCODE -ne 0) { throw 'Example workbook creation failed' }
$destination = Join-Path $PSScriptRoot 'dist/NetCollect-C'
New-Item -ItemType Directory -Force $destination, "$destination/licenses" | Out-Null
New-Item -ItemType Directory -Force "$destination/docs" | Out-Null
Copy-Item -LiteralPath docs/BUILDING.md, docs/RELEASE_V0.1.md -Destination "$destination/docs" -Force
Copy-Item -LiteralPath build/netcollect.exe, build/netcollect-cli.exe, README_C.md, C版采集模板.xlsx -Destination $destination -Force
Copy-Item -LiteralPath README.md, VERSION, CHANGELOG.md, THIRD_PARTY_NOTICES.md -Destination $destination -Force
if (Test-Path TEST_REPORT_C.md) { Copy-Item -LiteralPath TEST_REPORT_C.md -Destination $destination -Force }
Copy-Item -LiteralPath third_party/libssh2/COPYING -Destination "$destination/licenses/libssh2.txt" -Force
Copy-Item -LiteralPath third_party/libexpat/expat/COPYING -Destination "$destination/licenses/expat.txt" -Force
Copy-Item -LiteralPath third_party/miniz/LICENSE -Destination "$destination/licenses/miniz.txt" -Force
Copy-Item -LiteralPath third_party/libtelnet/COPYING -Destination "$destination/licenses/libtelnet.txt" -Force
# Package only release assets; never include logs left by local use or testing.
$releaseFiles = @('netcollect.exe', 'netcollect-cli.exe', 'README.md', 'README_C.md', 'VERSION', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md', 'C版采集模板.xlsx', 'licenses') | ForEach-Object { Join-Path $destination $_ }
$releaseFiles += "$destination/docs"
if (Test-Path "$destination/TEST_REPORT_C.md") { $releaseFiles += "$destination/TEST_REPORT_C.md" }
Compress-Archive -LiteralPath $releaseFiles -DestinationPath dist/NetCollect-C-Windows-x64.zip -Force
$versionedArchive = "NetCollcet-C-$version-Windows-x64.zip"
Copy-Item -LiteralPath dist/NetCollect-C-Windows-x64.zip -Destination "dist/$versionedArchive" -Force
$hash = (Get-FileHash -LiteralPath "dist/$versionedArchive" -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "dist/$versionedArchive.sha256" -Value "$hash  $versionedArchive" -Encoding ascii
Get-ChildItem -LiteralPath $destination | Select-Object Name, Length
