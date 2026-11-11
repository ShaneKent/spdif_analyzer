param([string]$SdkDir = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$argsList = @('-S', $root, '-B', "$root\build-win", '-G', 'Visual Studio 16 2019', '-A', 'x64')
if ($SdkDir) { $argsList += "-DSPDIF_SDK_DIR=$SdkDir" }
& cmake @argsList
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build "$root\build-win" --config Release --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& ctest --test-dir "$root\build-win" -C Release --output-on-failure
exit $LASTEXITCODE
