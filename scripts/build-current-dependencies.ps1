param([string]$VisualStudioPath = '', [string]$VcpkgRoot = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$dependencyRoot = Join-Path (Join-Path $repo $target.buildDirectory) 'dependencies'
if ($IsLinux) {
    # The cross build (docs/release/LAPTOP_BUILD.md): the repository's own triplet, so the packages and the
    # dependency receipt keep the desktop's name and options, with the x86 cross toolchain chainloaded into every
    # port build, release packages only. Detours's own port builds with nmake, on Windows only:
    # cmake/cross/vcpkg-ports has it built with CMake. A port's compiles go through the build's compiler cache
    # (SF4E_COMPILER_LAUNCHER, which build-current.ps1 sets for -CompilerCache); where that cache is changes no
    # package, so vcpkg does not hash it.
    $tools = Get-EmberCrossTools $VcpkgRoot
    $triplets = Join-Path (Join-Path $repo $target.buildDirectory) 'cross-triplets'
    New-Item -ItemType Directory -Path $triplets -Force | Out-Null
    $toolchain = Join-Path $repo 'cmake/cross/clang-cl-x86.cmake'
    $text = (Get-Content -LiteralPath (Join-Path $repo 'vcpkg-overlays/triplets/x86-windows-wchar-filenames.cmake') -Raw).Replace("`r`n", "`n")
    $text += "`n# build-current.ps1 on Linux: clang-cl (cmake/cross/clang-cl-x86.cmake); release packages only.`n" +
        "set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE `"$toolchain`")`nset(VCPKG_LOAD_VCVARS_ENV OFF)`nset(VCPKG_BUILD_TYPE release)`n" +
        "set(VCPKG_ENV_PASSTHROUGH SF4E_XWIN SF4E_LLVM_BIN)`n" +
        "set(VCPKG_ENV_PASSTHROUGH_UNTRACKED SF4E_COMPILER_LAUNCHER SCCACHE_DIR SCCACHE_SERVER_PORT SCCACHE_BASEDIRS)`n"
    [IO.File]::WriteAllText((Join-Path $triplets 'x86-windows-wchar-filenames.cmake'), $text)
    $env:VCPKG_ROOT = $tools.VcpkgRoot
    & (Join-Path $tools.VcpkgRoot 'vcpkg') install --triplet=x86-windows-wchar-filenames --host-triplet=x64-linux `
        "--overlay-triplets=$triplets" "--overlay-ports=$(Join-Path $repo 'cmake/cross/vcpkg-ports')" `
        "--x-manifest-root=$repo" "--x-install-root=$dependencyRoot"
    if ($LASTEXITCODE) { throw 'Designated dependency build failed' }
    Write-Output "Verified dependency installation: $dependencyRoot"
    return
}
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
Enter-EmberVcEnvironment $tools.VisualStudioPath
# vcpkg computes an ABI from the manifest, triplet, port patches, and toolchain.
# Never use another checkout's installed tree when a local fork changes.
$env:VCPKG_ROOT = $tools.VcpkgRoot
& (Join-Path $tools.VcpkgRoot 'vcpkg.exe') install --triplet=x86-windows-wchar-filenames --host-triplet=x86-windows-wchar-filenames "--x-manifest-root=$repo" "--x-install-root=$dependencyRoot"
if ($LASTEXITCODE) { throw 'Designated dependency build failed' }
Write-Output "Verified dependency installation: $dependencyRoot"
