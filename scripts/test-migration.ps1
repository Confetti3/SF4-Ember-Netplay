param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ Build Tools first.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'No Visual Studio installation with x86/x64 C++ tools was found.' }
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64'

Push-Location $repoRoot
try {
    cmake -S . -B build/core -G Ninja -DSF4E_CORE_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'Core CMake configuration failed.' }
    cmake --build build/core
    if ($LASTEXITCODE -ne 0) { throw 'Core C++ build failed.' }
    ctest --test-dir build/core --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
    Push-Location rust/sf4-net
    try {
        # Working directory selects the checked-in Rust toolchain.
        cargo fmt --check
        if ($LASTEXITCODE -ne 0) { throw 'Rust formatting check failed.' }
        cargo clippy --locked --all-targets -- -D warnings
        if ($LASTEXITCODE -ne 0) { throw 'Rust lint/build check failed.' }
        cargo test --locked
        if ($LASTEXITCODE -ne 0) { throw 'Rust tests failed.' }
    } finally { Pop-Location }
} finally { Pop-Location }
