param([switch]$SkipTests, [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$workspace = Split-Path $repo -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
& (Join-Path $PSScriptRoot 'test-build-provenance.ps1')
$build = Join-Path $repo $target.buildDirectory
$stage = Join-Path $repo $target.installDirectory
if (!$DiscordSdkArchive) { throw 'Set -DiscordSdkArchive or SF4E_DISCORD_SDK_ARCHIVE to the official archive matching cmake/discord-sdk-pin.json.' }
& (Join-Path $PSScriptRoot 'build-current-dependencies.ps1') -VisualStudioPath $tools.VisualStudioPath -VcpkgRoot $tools.VcpkgRoot
$dependencies = Join-Path $build 'dependencies'
$before = Get-SourceFingerprint $repo
$vs = $tools.VisualStudioPath
$environment = & cmd.exe /d /s /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x86 >nul && set"
foreach ($entry in $environment) { if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
& $cmake -S $repo -B $build -G Ninja '-U*_DIR' '-UZLIB_*' '-ULIB_DETOURS' '-UVALVEFILEVDF_INCLUDE_DIRS' '-USF4E_JSON_INCLUDE' '-UFIND_PACKAGE_MESSAGE_DETAILS_*' "-DCMAKE_MAKE_PROGRAM=$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSF4E_DEVELOPER_UI=OFF "-DCMAKE_TOOLCHAIN_FILE=$($tools.VcpkgRoot)/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x86-windows-wchar-filenames "-DVCPKG_INSTALLED_DIR=$dependencies" -DVCPKG_MANIFEST_INSTALL=OFF -DSF4E_BUILD_IROH_HELPER=ON -DSF4E_BUILD_DISCORD=ON "-DSF4E_DISCORD_SDK_ARCHIVE=$DiscordSdkArchive" "-DCMAKE_INSTALL_PREFIX=$stage"
if ($LASTEXITCODE) { throw 'Configure failed' }
Assert-CMakeSource $repo $build
if (Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') | Where-Object { $_ -match '^([^#/:]+):[^=]+=.*sf4-display-dxvk' }) {
    throw 'Stale dependency cache references a different checkout'
}
& $cmake --build $build --parallel 4
if ($LASTEXITCODE) { throw 'Build failed' }
if (!$SkipTests) {
    # Public-network and live Discord checks require a separate explicit run.
    & $ctest --test-dir $build --output-on-failure -E 'Iroh(Room|Game|Authorized|Recovery)|CustomRoom(FourTables|Spectators)|DiscordSmoke'
    if ($LASTEXITCODE) { throw 'Tests failed' }
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
[ordered]@{sourceRoot=$repo;buildRoot=$build;stageRoot=$stage;baseRevision=(& git -C $repo rev-parse HEAD);sourceFingerprint=$after;features=$target.features;testsPassed=(!$SkipTests);builtUtc=[DateTime]::UtcNow.ToString('o');binaries=@($binaries);dependencies=$dependencyReceipt} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $build 'build-provenance.json')
