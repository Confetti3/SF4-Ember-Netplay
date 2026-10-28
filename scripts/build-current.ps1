param([switch]$SkipTests, [switch]$SelfTest, [switch]$CompilerCache, [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
# The scripts' own suites (test-build-provenance.ps1, test-package-files.ps1, test-nightly-publication.ps1 and
# test-nightly-paths.ps1) are CTest entries (cmake/sf4e_test_targets.cmake) that Invoke-DefaultTests requires.
$build = Join-Path $repo $target.buildDirectory
$stage = Join-Path $repo $target.installDirectory
# A run that stops before the end leaves no receipt, so packaging cannot take an
# earlier run's passing receipt for this one.
Remove-Item -LiteralPath (Join-Path $build 'build-provenance.json') -ErrorAction SilentlyContinue
if (!$DiscordSdkArchive) { throw 'Set -DiscordSdkArchive or SF4E_DISCORD_SDK_ARCHIVE to the official archive matching cmake/discord-sdk-pin.json.' }
& (Join-Path $PSScriptRoot 'build-current-dependencies.ps1') -VisualStudioPath $tools.VisualStudioPath -VcpkgRoot $tools.VcpkgRoot
$dependencies = Join-Path $build 'dependencies'
$before = Get-SourceFingerprint $repo
$vs = $tools.VisualStudioPath
Enter-EmberVcEnvironment $vs
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
# Nightly builds trust Ember's staging bridge. Every other channel says OFF, so
# a build directory once configured for Nightly never keeps it. The receipt
# records which the helper was built with.
$stagingBridge = if ($target.channel -eq 'nightly') { 'ON' } else { 'OFF' }
# The in-game self-tests (docs/development/IN_GAME_SELFTEST.md) are built only
# for -SelfTest, and every other configure says OFF for the same reason. Their
# receipt says selfTest, and Assert-BuildReceipt refuses it for a package.
$selfTestOption = if ($SelfTest) { 'ON' } else { 'OFF' }
# -CompilerCache compiles through ccache, which serves compiles whose inputs it
# has seen before, as after switching branches. It cannot cache /Zi, so the
# objects then carry their debug information (/Z7). Every other configure clears
# these options, so a build directory never keeps them; turning the switch on or
# off in one build directory recompiles everything once. The receipt records the
# ccache version a build used.
$ccacheVersion = $null
$compilerCacheOptions = if ($CompilerCache) {
    $ccache = (Get-Command ccache -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if (!$ccache) { throw '-CompilerCache needs ccache on PATH.' }
    $ccacheVersion = [regex]::Match((& $ccache --version | Select-Object -First 1), '\d+(\.\d+)+').Value
    if (!$ccacheVersion -or [version]$ccacheVersion -lt [version]'4.6') { throw "-CompilerCache needs ccache 4.6 or newer; $ccache is '$ccacheVersion'." }
    "-DCMAKE_C_COMPILER_LAUNCHER=$ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache", '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded'
} else { '-UCMAKE_C_COMPILER_LAUNCHER', '-UCMAKE_CXX_COMPILER_LAUNCHER', '-UCMAKE_MSVC_DEBUG_INFORMATION_FORMAT' }
& $cmake -S $repo -B $build -G Ninja '-U*_DIR' '-UZLIB_*' '-ULIB_DETOURS' '-USF4E_JSON_INCLUDE' '-UFIND_PACKAGE_MESSAGE_DETAILS_*' "-DCMAKE_MAKE_PROGRAM=$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSF4E_DEVELOPER_UI=OFF "-DCMAKE_TOOLCHAIN_FILE=$($tools.VcpkgRoot)/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x86-windows-wchar-filenames "-DVCPKG_INSTALLED_DIR=$dependencies" -DVCPKG_MANIFEST_INSTALL=OFF -DSF4E_BUILD_IROH_HELPER=ON "-DSF4E_STAGING_BRIDGE=$stagingBridge" "-DSF4E_SELFTEST=$selfTestOption" -DSF4E_BUILD_DISCORD=ON "-DSF4E_DISCORD_SDK_ARCHIVE=$DiscordSdkArchive" "-DCMAKE_INSTALL_PREFIX=$stage" @compilerCacheOptions
if ($LASTEXITCODE) { throw 'Configure failed' }
Assert-CMakeSource $repo $build
if (Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') | Where-Object { $_ -match '^([^#/:]+):[^=]+=.*sf4-display-dxvk' }) {
    throw 'Stale dependency cache references a different checkout'
}
& $cmake --build $build --parallel $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE) { throw 'Build failed' }
# Public-network and live Discord checks require a separate explicit run (BuildProvenance.ps1 names them).
$excludedTests = $EmberExcludedTests
# Nightly runs a failed test once more (Invoke-DefaultTests); Stable does not.
$passedOnRerun = @()
if (!$SkipTests) {
    $passedOnRerun = @(Invoke-DefaultTests $ctest $build $excludedTests $env:NUMBER_OF_PROCESSORS -RerunFailed:($target.channel -eq 'nightly'))
}
& $cmake --install $build *> (Join-Path $build 'stage.log')
if ($LASTEXITCODE) { throw 'Local staging failed' }
$after = Get-SourceFingerprint $repo
if ($before -ne $after) { throw 'Source changed during build; run again' }
$dependencyReceipt = Get-DependencyReceipt $repo $build
if ((Get-FileHash -LiteralPath (Join-Path $stage 'GGPO.dll')).Hash -ne $dependencyReceipt.artifacts[0].sha256) { throw 'Staged GGPO dependency mismatch' }
$binaries = foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe','.dll' }) {
    [pscustomobject]@{path=$file.FullName.Substring($stage.Length+1);sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
$components = @(
    [ordered]@{name='Windows native build and local staging';platform='windows';status='passed'},
    [ordered]@{name='Default CTest including helper Rust, Ember Rust, BridgeSdk and the publisher script suites (BuildProvenance, NightlyPublication, NightlyPaths, PackageFiles)';platform='windows';status=$(if ($SkipTests) {'unexecuted'} else {'passed'})},
    [ordered]@{name='Supplemental service, lint and Linux checks (scripts/test-full-quality.ps1)';platform='windows/linux';status='unexecuted'},
    [ordered]@{name='Public network matrix (scripts/test-current-network.ps1)';platform='windows';status='unexecuted'},
    [ordered]@{name='Live Discord and gameplay acceptance';platform='live';status='unexecuted'})
[ordered]@{sourceRoot=$repo;buildRoot=$build;stageRoot=$stage;branch=(& git -C $repo branch --show-current);baseRevision=(& git -C $repo rev-parse HEAD);sourceFingerprint=$after;features=$target.features;stagingBridge=($stagingBridge -eq 'ON');selfTest=($selfTestOption -eq 'ON');compilerCache=$ccacheVersion;testsPassed=(!$SkipTests);testsPassedOnRerun=$passedOnRerun;testsExcluded=$excludedTests;components=$components;builtUtc=[DateTime]::UtcNow.ToString('o');binaries=@($binaries);dependencies=$dependencyReceipt} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $build 'build-provenance.json')
