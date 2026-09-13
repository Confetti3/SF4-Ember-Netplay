param([string]$PackageDir, [string]$BuildDir = 'msvc-build/display')
$ErrorActionPreference = 'Stop'
if (!$PackageDir) { throw 'Specify -PackageDir for the candidate to validate.' }
& (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $PackageDir
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
if (!$ctest) { $ctest = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' } else { $ctest = $ctest.Source }
& $ctest --test-dir $BuildDir -C RelWithDebInfo --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native regression tests failed' }
