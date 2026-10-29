param([switch]$SkipNativeBuild)
$ErrorActionPreference = 'Stop'
$bridgeRoot = Split-Path $PSScriptRoot -Parent
if (-not $SkipNativeBuild) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build-native.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
if (-not (Test-Path -LiteralPath (Join-Path $bridgeRoot 'native/out/ps5bridge.dll'))) {
    throw 'Missing native DLL. Run tools/build-native.ps1 first.'
}
$bridgeOutput = Join-Path $bridgeRoot 'dist/PS5InputBridge'
& dotnet test (Join-Path $bridgeRoot 'tests/Ps5Bridge.Tests/Ps5Bridge.Tests.csproj') -c Release --verbosity minimal
if ($LASTEXITCODE -ne 0) { throw 'Managed/native integration tests failed.' }
& dotnet run --project (Join-Path $bridgeRoot 'tests/Ps5Bridge.UiChecks/UiChecks.csproj') -c Release -p:TreatWarningsAsErrors=true
if ($LASTEXITCODE -ne 0) { throw 'Offline WPF checks failed.' }
& dotnet publish (Join-Path $bridgeRoot 'src/Ps5Bridge.App/Ps5Bridge.App.csproj') -c Release -r win-x64 --self-contained true -p:PublishSingleFile=false -p:TreatWarningsAsErrors=true -o $bridgeOutput
if ($LASTEXITCODE -ne 0) { throw 'Desktop publish failed.' }
$bridgeRuntimeConfig = Get-Content -LiteralPath (Join-Path $bridgeOutput 'Ps5InputBridge.runtimeconfig.json') -Raw | ConvertFrom-Json
$bridgePackages = $env:NUGET_PACKAGES
if (-not $bridgePackages) {
    $bridgeNugetLocation = & dotnet nuget locals global-packages --list
    $bridgePackages = ($bridgeNugetLocation -replace '^[^:]+:\s*', '').Trim()
}
$bridgeLicenses = Join-Path $bridgeRoot 'licenses'
foreach ($bridgeFramework in $bridgeRuntimeConfig.runtimeOptions.includedFrameworks) {
    $bridgePackageName = $bridgeFramework.name.ToLowerInvariant() + '.runtime.win-x64'
    $bridgePackagePath = Join-Path $bridgePackages "$bridgePackageName/$($bridgeFramework.version)"
    $bridgeLicenseSource = Join-Path $bridgePackagePath 'LICENSE.TXT'
    if (-not (Test-Path -LiteralPath $bridgeLicenseSource)) { $bridgeLicenseSource = Join-Path $bridgePackagePath 'LICENSE' }
    Copy-Item -LiteralPath $bridgeLicenseSource -Destination (Join-Path $bridgeLicenses "$bridgePackageName-LICENSE.txt") -Force
    $bridgeNotices = Join-Path $bridgePackagePath 'THIRD-PARTY-NOTICES.TXT'
    if (Test-Path -LiteralPath $bridgeNotices) {
        Copy-Item -LiteralPath $bridgeNotices -Destination (Join-Path $bridgeLicenses "$bridgePackageName-NOTICES.txt") -Force
    }
}
Copy-Item -LiteralPath (Join-Path $bridgeRoot 'README.md') -Destination $bridgeOutput -Force
Copy-Item -LiteralPath (Join-Path $bridgeRoot 'LICENSE') -Destination $bridgeOutput -Force
Copy-Item -LiteralPath (Join-Path $bridgeRoot 'THIRD-PARTY-NOTICES.md') -Destination $bridgeOutput -Force
Copy-Item -LiteralPath (Join-Path $bridgeRoot 'licenses') -Destination $bridgeOutput -Recurse -Force
foreach ($bridgeLauncher in @('launch-normal.cmd', 'launch-timing.cmd', 'launch-diagnostics.cmd')) {
    Copy-Item -LiteralPath (Join-Path $bridgeRoot "tools/latency-contention/$bridgeLauncher") -Destination $bridgeOutput -Force
}
Copy-Item -LiteralPath (Join-Path $bridgeRoot 'docs/validation/2026-10-29-latency-test-guide.md') -Destination (Join-Path $bridgeOutput 'INPUT-LATENCY-GUIDE.md') -Force
& python (Join-Path $PSScriptRoot 'package-source.py')
if ($LASTEXITCODE -ne 0) { throw 'Corresponding source package failed.' }
$bridgeArchive = Join-Path $bridgeRoot 'dist/PS5InputBridge-win-x64.zip'
$bridgeSourceArchive = Join-Path $bridgeRoot 'dist/PS5InputBridge-source.zip'
Compress-Archive -LiteralPath @($bridgeOutput, $bridgeSourceArchive) -DestinationPath $bridgeArchive -Force
Write-Output "Shareable package: $bridgeArchive"
Write-Output "Ready: $bridgeOutput/Ps5InputBridge.exe"
