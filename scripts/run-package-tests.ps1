param([string]$PackageDir, [string]$BuildDir = '', [string]$VisualStudioPath = '', [string]$VcpkgRoot = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$repo = Split-Path $PSScriptRoot -Parent
if (!$BuildDir) { $BuildDir = Join-Path $repo (Get-EmberBuildTarget $repo).buildDirectory }
if (!$PackageDir) { throw 'Specify -PackageDir for the candidate to validate.' }
& (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $PackageDir
# The tools of the build PC, and the tests build-current.ps1 runs: the public-network and live Discord checks are separate runs.
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
$ctest = Join-Path $tools.VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $ctest --test-dir $BuildDir -C RelWithDebInfo --output-on-failure -E $EmberExcludedTests
if ($LASTEXITCODE -ne 0) { throw 'Native regression tests failed' }
