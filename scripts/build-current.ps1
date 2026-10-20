param([switch]$SkipTests, [switch]$SelfTest, [switch]$CompilerCache, [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
& (Join-Path $PSScriptRoot 'test-build-provenance.ps1')
# The Nightly publisher's tests mock git, gh, ssh, scp and WSL, so they are safe to run here.
# The Bash checks need Git for Windows' bash.exe; a build without one fails rather than skipping them.
& (Join-Path $PSScriptRoot 'test-nightly-publication.ps1')
& (Join-Path $PSScriptRoot 'test-nightly-paths.ps1') -BashPath (Get-EmberGitBash)
$build = Join-Path $repo $target.buildDirectory
$stage = Join-Path $repo $target.installDirectory
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
# Public-network and live Discord checks require a separate explicit run. The
# receipt names them, so testsPassed never implies they ran (ledger A-015).
$excludedTests = 'Iroh(Room|Game|Authorized|Recovery)|CustomRoom(FourTables|Spectators)|DiscordSmoke'
if (!$SkipTests) {
    $junit = Join-Path $build 'ctest-junit.xml'
    Remove-Item -LiteralPath $junit -ErrorAction SilentlyContinue
    & $ctest --test-dir $build --output-on-failure --parallel $env:NUMBER_OF_PROCESSORS --no-tests=error -E $excludedTests --output-junit $junit
    if ($LASTEXITCODE) { throw 'Tests failed' }
    # The receipt names these components, so the run itself must show them.
    $ran = @(([xml](Get-Content -Raw -LiteralPath $junit)).testsuite.testcase | Where-Object { $_.status -eq 'run' -and !$_.failure } | ForEach-Object { $_.name })
    $missing = @('HelperRust','EmberRust','BridgeSdk') | Where-Object { $_ -notin $ran }
    if ($missing) { throw "Default CTest did not run: $($missing -join ', ')" }
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
    [ordered]@{name='Default CTest including helper Rust, Ember Rust and BridgeSdk';platform='windows';status=$(if ($SkipTests) {'unexecuted'} else {'passed'})},
    [ordered]@{name='Supplemental service, lint and Linux checks (scripts/test-full-quality.ps1)';platform='windows/linux';status='unexecuted'},
    [ordered]@{name='Public network matrix (scripts/test-current-network.ps1)';platform='windows';status='unexecuted'},
    [ordered]@{name='Live Discord and gameplay acceptance';platform='live';status='unexecuted'})
[ordered]@{sourceRoot=$repo;buildRoot=$build;stageRoot=$stage;branch=(& git -C $repo branch --show-current);baseRevision=(& git -C $repo rev-parse HEAD);sourceFingerprint=$after;features=$target.features;stagingBridge=($stagingBridge -eq 'ON');selfTest=($selfTestOption -eq 'ON');compilerCache=$ccacheVersion;testsPassed=(!$SkipTests);testsExcluded=$excludedTests;components=$components;builtUtc=[DateTime]::UtcNow.ToString('o');binaries=@($binaries);dependencies=$dependencyReceipt} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $build 'build-provenance.json')
