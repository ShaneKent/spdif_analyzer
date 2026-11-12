param([string]$SdkDir = "", [string]$BuildDir = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$BuildDir) { $BuildDir = "$root\build-win" }
elseif (![System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
$argsList = @('-S', $root, '-B', $BuildDir, '-G', 'Visual Studio 16 2019', '-A', 'x64')
if ($SdkDir) { $argsList += "-DSPDIF_SDK_DIR=$SdkDir" }
& cmake @argsList
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
