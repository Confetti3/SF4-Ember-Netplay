param([switch]$SkipTests, [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE, [switch]$WithoutDiscord)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$validationTools = Get-EmberValidationTools $repo
& (Join-Path $PSScriptRoot 'check-build-prerequisites.ps1') -ResolvedTools $validationTools -VisualStudioPath $VisualStudioPath -VcpkgRoot $VcpkgRoot -DiscordSdkArchive $DiscordSdkArchive -WithoutDiscord:$WithoutDiscord
if ($LASTEXITCODE) { throw 'Build prerequisites are incomplete; see the checks above.' }
$target = Get-EmberBuildTarget $repo -WithoutDiscord:$WithoutDiscord
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'TestResults.ps1')
& (Join-Path $PSScriptRoot 'test-build-provenance.ps1')
# The Nightly publisher's tests mock git, gh, ssh, scp and WSL, so they are safe to run here.
# The Bash checks need Git for Windows' bash.exe; a build without one fails rather than skipping them.
& (Join-Path $PSScriptRoot 'test-nightly-publication.ps1')
& (Join-Path $PSScriptRoot 'test-nightly-paths.ps1') -BashPath (Get-EmberGitBash)
$build = Join-Path $repo $target.buildDirectory
$stage = Join-Path $repo $target.installDirectory
if (!$WithoutDiscord -and !$DiscordSdkArchive) { throw 'Set -DiscordSdkArchive or SF4E_DISCORD_SDK_ARCHIVE, or use -WithoutDiscord for a development build.' }
$discordOption = if ($WithoutDiscord) { 'OFF' } else { 'ON' }
& (Join-Path $PSScriptRoot 'build-current-dependencies.ps1') -VisualStudioPath $tools.VisualStudioPath -VcpkgRoot $tools.VcpkgRoot -WithoutDiscord:$WithoutDiscord
$dependencies = Join-Path $build 'dependencies'
$before = Get-SourceFingerprint $repo
$vs = $tools.VisualStudioPath
Enter-EmberVcEnvironment $vs
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
& $cmake -S $repo -B $build -G Ninja '-U*_DIR' '-UZLIB_*' '-ULIB_DETOURS' '-USF4E_JSON_INCLUDE' '-UFIND_PACKAGE_MESSAGE_DETAILS_*' "-DCMAKE_MAKE_PROGRAM=$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DBUILD_TESTING=ON -DSF4E_COMPONENT_TESTS=ON "-DSF4E_CARGO_EXECUTABLE=$($validationTools.Cargo)" "-DSF4E_NODE_EXECUTABLE=$($validationTools.Node)" "-DPython3_EXECUTABLE=$($validationTools.Python)" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSF4E_DEVELOPER_UI=OFF "-DCMAKE_TOOLCHAIN_FILE=$($tools.VcpkgRoot)/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x86-windows-wchar-filenames "-DVCPKG_INSTALLED_DIR=$dependencies" -DVCPKG_MANIFEST_INSTALL=OFF -DSF4E_BUILD_IROH_HELPER=ON "-DSF4E_BUILD_DISCORD=$discordOption" "-DSF4E_DISCORD_SDK_ARCHIVE=$DiscordSdkArchive" "-DCMAKE_INSTALL_PREFIX=$stage"
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
$testResults = $null
if (!$SkipTests) {
    $junit = Join-Path $build 'ctest-junit.xml'
    Remove-Item -LiteralPath $junit -ErrorAction SilentlyContinue
    & $ctest --test-dir $build --output-on-failure --parallel $env:NUMBER_OF_PROCESSORS --no-tests=error -E $excludedTests --output-junit $junit
    if ($LASTEXITCODE) { throw 'Tests failed' }
    $testResults = Read-EmberTestResults $junit @('HelperRust','EmberRust','ember-shortRust','ember-roomsRust','BridgeSdk','BuildValidation')
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
    [ordered]@{name='Default CTest including helper Rust, all services and BridgeSdk';platform='windows';status=$(if ($SkipTests) {'unexecuted'} else {'passed'})},
    [ordered]@{name='Supplemental service, lint and Linux checks (scripts/test-full-quality.ps1)';platform='windows/linux';status='unexecuted'},
    [ordered]@{name='Public network matrix (scripts/test-current-network.ps1)';platform='windows';status='unexecuted'},
    [ordered]@{name='Live Discord and gameplay acceptance';platform='live';status='unexecuted'})
[ordered]@{sourceRoot=$repo;buildRoot=$build;stageRoot=$stage;branch=(& git -C $repo branch --show-current);baseRevision=(& git -C $repo rev-parse HEAD);sourceFingerprint=$after;features=@($target.features | Where-Object { !$WithoutDiscord -or $_ -ne 'Discord' });discordEnabled=(!$WithoutDiscord);testsPassed=($null -ne $testResults -and $testResults.passed);testResults=$testResults;testsExcluded=$excludedTests;components=$components;builtUtc=[DateTime]::UtcNow.ToString('o');binaries=@($binaries);dependencies=$dependencyReceipt} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $build 'build-provenance.json')
