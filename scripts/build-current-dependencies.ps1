param([string]$VisualStudioPath = '', [string]$VcpkgRoot = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$workspace = Split-Path $repo -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
$dependencyRoot = Join-Path (Join-Path $repo $target.buildDirectory) 'dependencies'
$vs = $tools.VisualStudioPath
$environment = & cmd.exe /d /s /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul && set"
if ($LASTEXITCODE) { throw 'Dependency compiler environment failed' }
foreach ($entry in $environment) { if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
# vcpkg computes an ABI from the manifest, triplet, port patches, and toolchain.
# Never use another checkout's installed tree when a local fork changes.
$env:VCPKG_ROOT = $tools.VcpkgRoot
& (Join-Path $tools.VcpkgRoot 'vcpkg.exe') install --triplet=x86-windows-wchar-filenames --host-triplet=x86-windows-wchar-filenames "--x-manifest-root=$repo" "--x-install-root=$dependencyRoot"
if ($LASTEXITCODE) { throw 'Designated dependency build failed' }
Write-Output "Verified dependency installation: $dependencyRoot"
