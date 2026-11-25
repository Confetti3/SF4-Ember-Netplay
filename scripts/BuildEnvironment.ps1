$ErrorActionPreference = 'Stop'

function Get-EmberBuildTarget([string]$SourceRoot) {
    $SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
    $workspace = Split-Path $SourceRoot -Parent
    $targetPath = Join-Path $workspace 'build-target.json'
    if (!(Test-Path -LiteralPath $targetPath)) {
        $workspace = $SourceRoot
        $targetPath = Join-Path $SourceRoot 'build-target.json'
    }
    $target = Get-Content -LiteralPath $targetPath -Raw | ConvertFrom-Json
    $entries = @([pscustomobject]@{channel='stable';target=$target})
    foreach ($property in $target.channels.PSObject.Properties) {
        $entries += [pscustomobject]@{channel=$property.Name;target=$property.Value}
    }
    $designated = @()
    $matching = @()
    foreach ($entry in $entries) {
        $expected = [IO.Path]::GetFullPath((Join-Path $workspace $entry.target.sourceDirectory)).TrimEnd('\','/')
        $designated += $expected
        if ($SourceRoot.TrimEnd('\','/') -eq $expected) { $matching += $entry }
    }
    if ($matching.Count -eq 0) {
        throw "Wrong build target: $SourceRoot. Designated source: $($designated -join ', ') ($targetPath)"
    }
    if ($matching.Count -ne 1) { throw "Ambiguous build target: $SourceRoot ($targetPath)" }
    # emberService is the origin of Ember's own service (Linked accounts, public rooms, points, friends) that a
    # channel's builds and packages use; build-current.ps1 builds with it and packaging requires it. Stable stays on
    # production (an empty origin), so only a channel entry may name one.
    if ($target.PSObject.Properties['emberService']) {
        throw "The top-level (stable) entry of $targetPath must not name an emberService: Stable uses production."
    }
    $target = $matching[0].target
    $service = if ($matching[0].channel -cne 'stable' -and $target.PSObject.Properties['emberService']) { [string]$target.emberService } else { '' }
    if ($service -and $service -cnotmatch '^https://[a-z0-9.-]+(:[0-9]{1,5})?$') {
        throw "channels.$($matching[0].channel).emberService in $targetPath must be an https origin, not '$service'."
    }
    $target | Add-Member -NotePropertyName channel -NotePropertyValue $matching[0].channel -Force
    $target | Add-Member -NotePropertyName emberService -NotePropertyValue $service -Force
    foreach ($relative in @($target.buildDirectory, $target.installDirectory)) {
        $resolved = [IO.Path]::GetFullPath((Join-Path $SourceRoot $relative))
        if (!$resolved.StartsWith($SourceRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Build and stage paths must be inside the designated source: $resolved"
        }
    }
    return $target
}

# Publication and packaging for one channel must never run from the other channel's checkout.
function Assert-EmberChannel([object]$Target, [string]$Expected, [string]$Purpose) {
    if ($Target.channel -cne $Expected) {
        throw "$Purpose requires the $Expected channel, but this checkout is designated $($Target.channel) in build-target.json."
    }
}

# The channel is not enough for Stable: a checkout whose parent folder has no build-target.json
# falls back to its own file, which names only the top-level (stable) entry, so a Nightly checkout
# set up that way reads as stable. Stable work also requires the release branch.
function Assert-EmberStableBranch([string]$Branch, [string]$Purpose) {
    if ("$Branch".Trim() -cne 'release') {
        throw "$Purpose requires the release branch, but this checkout is on '$("$Branch".Trim())'."
    }
}

function Get-EmberToolPaths([string]$SourceRoot, [string]$VisualStudioPath, [string]$VcpkgRoot) {
    if (!$VisualStudioPath) { $VisualStudioPath = $env:SF4E_VISUAL_STUDIO_PATH }
    if (!$VisualStudioPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (!$VisualStudioPath -or !(Test-Path -LiteralPath (Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvarsall.bat'))) {
        throw 'Install Visual Studio 2026 C++ Build Tools or set -VisualStudioPath.'
    }
    if (!$VcpkgRoot) { $VcpkgRoot = $env:VCPKG_ROOT }
    if (!$VcpkgRoot) { $VcpkgRoot = Join-Path (Split-Path $SourceRoot -Parent) 'vcpkg' }
    if (!(Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
        throw 'Bootstrap vcpkg and set -VcpkgRoot or VCPKG_ROOT.'
    }
    [pscustomobject]@{VisualStudioPath=[IO.Path]::GetFullPath($VisualStudioPath);VcpkgRoot=[IO.Path]::GetFullPath($VcpkgRoot)}
}

# The cross build's tools on a Linux build PC (docs/release/LAPTOP_BUILD.md): clang-cl and lld-link from SF4E_LLVM_BIN
# against the Microsoft CRT and Windows SDK in the xwin splat SF4E_XWIN, vcpkg from VCPKG_ROOT, and the program that
# runs a Windows test program (CMAKE_CROSSCOMPILING_EMULATOR) from SF4E_TEST_EMULATOR. CMake, CTest and Ninja are
# the ones on PATH. Throws, naming what is missing, before anything is built.
function Get-EmberCrossTools([string]$VcpkgRoot) {
    if (!$IsLinux) { throw 'The cross build runs on Linux; on Windows build-current.ps1 uses Visual Studio.' }
    foreach ($name in 'SF4E_XWIN', 'SF4E_LLVM_BIN', 'SF4E_TEST_EMULATOR') {
        if (![Environment]::GetEnvironmentVariable($name)) { throw "The cross build needs $name (docs/release/LAPTOP_BUILD.md)." }
    }
    if (!$VcpkgRoot) { $VcpkgRoot = $env:VCPKG_ROOT }
    if (!$VcpkgRoot -or !(Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg') -PathType Leaf)) { throw 'Bootstrap vcpkg and set -VcpkgRoot or VCPKG_ROOT.' }
    $xwin = [IO.Path]::GetFullPath($env:SF4E_XWIN)
    $llvm = [IO.Path]::GetFullPath($env:SF4E_LLVM_BIN)
    if (!(Test-Path -LiteralPath (Join-Path $xwin 'crt/include/crtversion.h') -PathType Leaf)) { throw "SF4E_XWIN is not an xwin splat: $xwin" }
    foreach ($tool in 'clang-cl', 'lld-link', 'llvm-lib', 'llvm-rc', 'llvm-mt', 'llvm-ml') {
        if (!(Test-Path -LiteralPath (Join-Path $llvm $tool) -PathType Leaf)) { throw "SF4E_LLVM_BIN has no ${tool}: $llvm" }
    }
    $paths = [ordered]@{}
    foreach ($tool in 'cmake', 'ctest', 'ninja', 'cargo', 'python3', 'bash') {
        $found = Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (!$found) { throw "The cross build needs $tool on PATH." }
        $paths[$tool] = $found.Source
    }
    $emulator = Get-Command $env:SF4E_TEST_EMULATOR -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$emulator) { throw "SF4E_TEST_EMULATOR is not a program: $env:SF4E_TEST_EMULATOR" }
    $crt = Get-Content -LiteralPath (Join-Path $xwin 'crt/include/crtversion.h') | ForEach-Object {
        if ($_ -match '^#define _VC_CRT_(MAJOR|MINOR|BUILD)_VERSION (\d+)') { $Matches[2] } }
    $sdk = @(Get-ChildItem -LiteralPath (Join-Path $xwin 'sdk/include') -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^\d+\.\d+\.\d+\.\d+$' | Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty Name)
    $splat = (Get-Content -LiteralPath (Join-Path $xwin '.xwin-version') -ErrorAction SilentlyContinue | Select-Object -First 1)
    [pscustomobject]@{
        VcpkgRoot=[IO.Path]::GetFullPath($VcpkgRoot); Xwin=$xwin; LlvmBin=$llvm; Emulator=$emulator.Source
        Cmake=$paths.cmake; Ctest=$paths.ctest; Ninja=$paths.ninja; Bash=$paths.bash; Python=$paths.python3
        Toolchain=[ordered]@{
            compiler="$(& (Join-Path $llvm 'clang-cl') --version | Select-Object -First 1)".Trim()
            linker='lld-link'
            crt=($crt -join '.')
            sdk=$(if ($sdk) { $sdk } else { $null })
            splat=$(if ($splat) { "xwin $splat".Trim() } else { $null })
        }
    }
}

# The environment of a cross build (Get-EmberCrossTools), for this process and what it starts: the Rust helper's
# x64 build (its linker, C compiler, rc.exe and ml64.exe through cmake/cross/rust-wrappers.sh in <build>/cross-bin)
# and its tests, which run through the test emulator; Windows test programs find vcpkg's DLLs through WINEPATH.
# The xwin splat gets the import-library case links the sources need (cmake/cross/case-links.py).
function Enter-EmberCrossEnvironment([object]$Tools, [string]$SourceRoot, [string]$BuildRoot, [string]$DependencyBin) {
    & $Tools.Python -I (Join-Path $SourceRoot 'cmake/cross/case-links.py') $SourceRoot $Tools.Xwin
    if ($LASTEXITCODE) { throw 'Import-library case links failed' }
    $wrappers = Join-Path $BuildRoot 'cross-bin'
    & $Tools.Bash (Join-Path $SourceRoot 'cmake/cross/rust-wrappers.sh') $wrappers
    if ($LASTEXITCODE) { throw 'The Rust helper''s tool wrappers failed' }
    $env:PATH = "$wrappers$([IO.Path]::PathSeparator)$($Tools.LlvmBin)$([IO.Path]::PathSeparator)$env:PATH"
    $env:CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER = Join-Path $wrappers 'lld-link-x64'
    $env:CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUNNER = $Tools.Emulator
    $env:CC_x86_64_pc_windows_msvc = Join-Path $wrappers 'clang-cl-x64'
    $env:CXX_x86_64_pc_windows_msvc = Join-Path $wrappers 'clang-cl-x64'
    $env:AR_x86_64_pc_windows_msvc = Join-Path $Tools.LlvmBin 'llvm-lib'
    $env:WINEPATH = "Z:$DependencyBin"
    if (!$env:SF4E_GIT_BASH) { $env:SF4E_GIT_BASH = $Tools.Bash }
    # Under Wine the helper's network tests run slower than on Windows; a test binary's default of one thread per
    # processor starves the timed ones (checkpoint export, latency tables), which pass alone. RUST_TEST_THREADS overrides.
    if (!$env:RUST_TEST_THREADS) { $env:RUST_TEST_THREADS = '6' }
}

# Runs vcvarsall once per process. vcvarsall prepends to PATH, INCLUDE and LIB each time it runs, so a second
# entry would double them; when this process already holds the environment of the same installation and target
# architecture (VSINSTALLDIR and VSCMD_ARG_TGT_ARCH, which vcvarsall sets), it returns at once.
function Enter-EmberVcEnvironment([string]$VisualStudioPath, [string]$Architecture = 'x86') {
    $entered = $env:VSINSTALLDIR
    if ($entered -and $env:VSCMD_ARG_TGT_ARCH -ceq $Architecture -and
        [IO.Path]::GetFullPath($entered).TrimEnd('\','/') -ieq [IO.Path]::GetFullPath($VisualStudioPath).TrimEnd('\','/')) { return }
    $environment = & cmd.exe /d /s /c "`"$VisualStudioPath\VC\Auxiliary\Build\vcvarsall.bat`" $Architecture >nul && set"
    if ($LASTEXITCODE) { throw 'Compiler environment failed' }
    foreach ($entry in $environment) { if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
}

# Git for Windows' bash.exe, never a WSL launcher that may be first on PATH. git.exe sits in
# <root>\cmd or <root>\mingw64\bin, so look for <directory>\bin\bash.exe up the path from it.
# SF4E_GIT_BASH overrides the search. Throws when no bash.exe is found.
function Get-EmberGitBash([string]$GitExecutable = '', [string]$Override = $env:SF4E_GIT_BASH) {
    if ($Override) {
        if (!(Test-Path -LiteralPath $Override -PathType Leaf)) { throw "SF4E_GIT_BASH is not a file: $Override" }
        return $Override
    }
    if (!$GitExecutable) {
        $command = Get-Command git -ErrorAction SilentlyContinue
        if (!$command) { throw 'git was not found; set SF4E_GIT_BASH to the path of Git for Windows'' bash.exe.' }
        $GitExecutable = $command.Source
    }
    $directory = Split-Path $GitExecutable -Parent
    for ($level = 0; $directory -and $level -lt 4; $level++) {
        $candidate = Join-Path $directory 'bin\bash.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        $directory = Split-Path $directory -Parent
    }
    throw "No bash.exe found for $GitExecutable; install Git for Windows or set SF4E_GIT_BASH."
}
