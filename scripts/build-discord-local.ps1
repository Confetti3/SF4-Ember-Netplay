param([switch]$RustChecks)
$ErrorActionPreference = 'Stop'
$vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
Enter-EmberVcEnvironment $vsRoot
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if ($RustChecks) {
    $env:CARGO_TARGET_DIR = Join-Path (Get-Location) 'build/discord/rust-target'
    Push-Location rust/sf4-net
    & cargo +1.98.0 fmt --all --check
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & cargo +1.98.0 test --locked --offline --lib
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & cargo +1.98.0 clippy --locked --offline --all-targets -- -D warnings
    exit $LASTEXITCODE
}
& $cmake -S . -B build/discord -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=C:/Users/Kate/Desktop/sf4/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x86-windows-wchar-filenames -DVCPKG_INSTALLED_DIR=C:/Users/Kate/Desktop/sf4/sf4-display-dxvk/msvc-build/ember-clean/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF -DSF4E_BUILD_IROH_HELPER=ON -DSF4E_BUILD_DISCORD=ON -DSF4E_DISCORD_APPLICATION_ID=1546980049692135514 -DSF4E_DISCORD_SDK_ARCHIVE=C:/Users/Kate/Downloads/DiscordSocialSdk-1.10.19337.zip -DCMAKE_INSTALL_PREFIX=C:/Users/Kate/Desktop/sf4/sf4-discord/msvc-out/discord
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build build/discord --parallel 4
exit $LASTEXITCODE
