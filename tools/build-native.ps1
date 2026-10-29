param([switch]$SetupOnly, [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$bridgeRoot = Split-Path $PSScriptRoot -Parent
& python (Join-Path $bridgeRoot 'native/scripts/bootstrap.py')
if ($LASTEXITCODE -ne 0) { throw 'Native dependency setup failed.' }
if ($SetupOnly) { exit 0 }
$bridgeDeps = Join-Path $bridgeRoot 'native/.deps'
$bridgeToolchain = Join-Path $bridgeDeps 'llvm-mingw-20261006-ucrt-x86_64/bin'
$bridgePython = Join-Path $bridgeDeps 'python/Scripts'
$env:PATH = "$bridgeToolchain;$bridgePython;$(Join-Path $bridgeDeps 'protoc/bin');$env:PATH"
$bridgeCmake = Join-Path $bridgePython 'cmake.exe'
$bridgeBuild = Join-Path $bridgeRoot 'native/build'
& $bridgeCmake -S (Join-Path $bridgeRoot 'native') -B $bridgeBuild -G Ninja "-DCMAKE_C_COMPILER=$bridgeToolchain/x86_64-w64-mingw32-clang.exe" "-DCMAKE_CXX_COMPILER=$bridgeToolchain/x86_64-w64-mingw32-clang++.exe" '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw 'Native CMake configure failed.' }
& $bridgeCmake --build $bridgeBuild --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
if (-not $SkipTests) {
    foreach ($bridgeTest in @('ps5bridge-tests.exe', 'ps5bridge-feedback-tests.exe', 'ps5bridge-feedback-wakeup-tests.exe', 'ps5bridge-feedback-diagnostics-tests.exe', 'ps5bridge-input-diagnostics-tests.exe', 'ps5bridge-crypto-tests.exe', 'ps5bridge-history-tests.exe', 'ps5bridge-transport-tests.exe', 'ps5bridge-input-tests.exe', 'ps5bridge-hid-tests.exe', 'ps5bridge-snapshot-tests.exe')) {
        & (Join-Path $bridgeBuild $bridgeTest)
        if ($LASTEXITCODE -ne 0) { throw "Native test failed: $bridgeTest" }
    }
    & python -m unittest discover -s (Join-Path $bridgeRoot 'native/tests') -p 'test_*.py' -v
    if ($LASTEXITCODE -ne 0) { throw 'Native ABI/lifetime/bootstrap tests failed.' }
}
$bridgeOut = Join-Path $bridgeRoot 'native/out'
New-Item -ItemType Directory -Force -Path $bridgeOut | Out-Null
Copy-Item -LiteralPath (Join-Path $bridgeBuild 'ps5bridge.dll') -Destination $bridgeOut -Force
Write-Output "Native DLL: $bridgeOut/ps5bridge.dll"
