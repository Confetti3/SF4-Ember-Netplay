# Compiles Ember's game-neutral code as SFV Ember builds (x64, C++20, its
# warning flags), from cmake/ember-x64-probe. SF4 Ember's own build is not
# touched; this proves the library's sources are ready for the other game.
#
# The build tree, the x64 packages and vcpkg's build trees, downloads and
# binary cache all stay on the D: drive; the script stops before building
# when C: is under 120 GB.

param(
	[string]$BuildDir = 'D:\sf4-build\ember-x64-probe',
	[string]$VcpkgCache = 'D:\sf4-build\vcpkg-cache',
	[string]$VcpkgRoot = '',
	[string]$VisualStudioPath = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')

$freeGb = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($freeGb -lt 120) { throw "C: has $freeGb GB free; stopping before the build (floor 120 GB)" }

$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
Enter-EmberVcEnvironment $tools.VisualStudioPath 'x64'
$cmakeDir = Join-Path $tools.VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'
$probe = Join-Path $repo 'cmake\ember-x64-probe'
$triplet = 'x64-windows-static-md-wchar-filenames'
$installed = Join-Path $BuildDir 'vcpkg_installed'

# vcpkg keeps its build trees, packages and downloads beside itself and its
# binary cache under the user profile unless told otherwise: both on C:.
foreach ($folder in 'archives', 'buildtrees', 'packages', 'downloads') {
	New-Item -ItemType Directory -Force (Join-Path $VcpkgCache $folder) | Out-Null
}
$env:VCPKG_ROOT = $tools.VcpkgRoot
& (Join-Path $tools.VcpkgRoot 'vcpkg.exe') install "--triplet=$triplet" "--host-triplet=$triplet" `
	"--x-manifest-root=$probe" "--x-install-root=$installed" `
	"--x-buildtrees-root=$VcpkgCache\buildtrees" "--x-packages-root=$VcpkgCache\packages" `
	"--downloads-root=$VcpkgCache\downloads" "--binarysource=clear;files,$VcpkgCache\archives,readwrite"
if ($LASTEXITCODE) { throw "vcpkg install failed ($LASTEXITCODE)" }

& $cmake -S $probe -B $BuildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=RelWithDebInfo `
	"-DCMAKE_TOOLCHAIN_FILE=$($tools.VcpkgRoot)\scripts\buildsystems\vcpkg.cmake" `
	"-DVCPKG_TARGET_TRIPLET=$triplet" -DVCPKG_MANIFEST_INSTALL=OFF "-DVCPKG_INSTALLED_DIR=$installed"
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
# Keep going past a failed file, so one run lists everything that fails.
& $cmake --build $BuildDir --target ember_neutral_x64 -- -k 0
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
