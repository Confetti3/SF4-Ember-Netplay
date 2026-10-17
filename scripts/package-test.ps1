# SDK-free private package, independent of the official build designation.
param([string]$Label = ('test-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
      [string]$BuildDir = '', [string]$OutDir = '', [string]$Python = $env:SF4E_PYTHON)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'PrivateBuild.ps1')
if ($Label -notmatch '^[a-zA-Z0-9._-]+$') { throw 'Invalid package label' }
$Python = Get-PrivatePython $Python
if (!$BuildDir) { $BuildDir = Join-Path $repo 'build/private-test' }
if (!$OutDir) { $OutDir = Join-Path $repo 'dist' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (!(Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) { throw 'Configure an x86 SDK-free CMake build with BUILD_TESTING=ON first; see BUILDING.md.' }
Assert-CMakeSource $repo $BuildDir
$cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')
foreach ($setting in @('SF4E_BUILD_DISCORD:BOOL=OFF','BUILD_TESTING:BOOL=ON')) {
    if ($cache -notcontains $setting) { throw "Required CMake setting: $setting" }
}
$cmakeLine = @($cache | Where-Object { $_ -like 'CMAKE_COMMAND:INTERNAL=*' })
if ($cmakeLine.Count -ne 1) { throw 'CMake executable missing from cache' }
$cmake = $cmakeLine[0].Split('=',2)[1]
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
# Use the caller's x86 developer environment, as used to configure this build.
$crt = Join-Path $env:VCToolsRedistDir 'x86/Microsoft.VC145.CRT'
if (!(Test-Path -LiteralPath $crt -PathType Container)) { throw 'Run from the Visual Studio 2026 x86 developer environment (VCToolsRedistDir is required).' }
$fingerprint = Get-SourceFingerprint $repo
$dependencies = Get-DependencyReceipt $repo $BuildDir
& $cmake --build $BuildDir --parallel 4
if ($LASTEXITCODE) { throw 'Build failed' }
# Capture the bytes BEFORE tests, then compare after tests and against install.
$runtime = foreach ($name in $script:PrivateRuntime) {
    $path = Join-Path $BuildDir $name
    [pscustomobject]@{path=$name;testedPath=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}
}
$report = Join-Path $BuildDir 'private-tests.xml'
if (Test-Path -LiteralPath $report) { Remove-Item -LiteralPath $report }
$pattern = '^(' + (($script:PrivateTests | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
$savedModulePath = $env:PSModulePath
try {
    # Windows PowerShell fixtures must initialize their own built-in modules
    # when this wrapper was launched from PowerShell 7.
    $env:PSModulePath = $null
    & $ctest --test-dir $BuildDir --no-tests=error -R $pattern --output-on-failure --output-junit $report
    if ($LASTEXITCODE) { throw 'Private package test gate failed' }
} finally { $env:PSModulePath = $savedModulePath }
$tests = Read-EmberTestResults $report $script:PrivateTests
if ((Get-SourceFingerprint $repo) -ne $fingerprint) { throw 'Source changed while building/testing' }
foreach ($binary in $runtime) {
    if ((Get-FileHash -LiteralPath $binary.testedPath).Hash -ne $binary.sha256) { throw 'Runtime changed during tests' }
}
$previousStage = ''
$receiptPath = Join-Path $BuildDir 'build-provenance.json'
if (Test-Path -LiteralPath $receiptPath) {
    $previousReceipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($previousReceipt.kind -eq 'private-test-build' -and $previousReceipt.buildRoot -eq $BuildDir) {
        $previousStage = $previousReceipt.stageRoot
    }
}
$stage = Join-Path $BuildDir ('private-stage-' + [Guid]::NewGuid().ToString('N'))
& $cmake --install $BuildDir --prefix $stage
if ($LASTEXITCODE) { throw 'Staging failed' }
foreach ($file in Get-ChildItem -LiteralPath $crt -File -Filter '*.dll') {
    Copy-Item -LiteralPath $file.FullName -Destination $stage
    $runtime += [pscustomobject]@{path=$file.Name;testedPath=$file.FullName;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
$receipt = @{
    kind='private-test-build';releaseValidated=$false;discordEnabled=$false;testsPassed=$true
    sourceRoot=$repo;buildRoot=$BuildDir;stageRoot=$stage
    baseRevision=(& git -C $repo rev-parse HEAD);sourceFingerprint=$fingerprint
    createdUtc=[DateTime]::UtcNow.ToString('o');features=@('SDK-free private test package')
    tests=$tests;testReport=@{path=$report;sha256=(Get-FileHash -LiteralPath $report).Hash}
    dependencies=$dependencies;binaries=@($runtime)
}
$receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $BuildDir 'build-provenance.json') -Encoding UTF8
$null = Assert-PrivateBuildReceipt $repo $BuildDir $stage
$scratch = Join-Path (Join-Path (Split-Path $OutDir -Parent) 'build') ('private-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
try {
    & (Join-Path $PSScriptRoot 'package-team.ps1') -PrivateTest -BuildDir $BuildDir -InstallDir $stage -OutDir $scratch -VersionLabel $Label -Python $Python
    $PackageZipPath = Join-Path $scratch ("sf4-ember-netplay-$Label.zip")
    & $Python -X utf8 (Join-Path $PSScriptRoot 'package-test.py') --dist $OutDir --incoming $PackageZipPath --history (Join-Path (Split-Path $scratch -Parent) 'private-packages')
    if ($LASTEXITCODE) { throw "Publication failed; verified candidate retained at $scratch" }
    # Only this newly created scratch directory is removed; no player output.
    if (([IO.Path]::GetFullPath($scratch)) -ne $scratch -or (Split-Path $scratch -Leaf) -notlike 'private-package-*') { throw 'Invalid scratch path' }
    Remove-Item -LiteralPath $scratch -Recurse -Force
} catch {
    Write-Warning "Package evidence retained at $scratch"
    throw
}
if ($previousStage -and (Test-Path -LiteralPath $previousStage)) {
    $resolved = [IO.Path]::GetFullPath($previousStage)
    if ((Split-Path $resolved -Parent) -eq $BuildDir -and (Split-Path $resolved -Leaf) -match '^private-stage-[0-9a-f]{32}$') {
        try {
            & $Python -c 'import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); from private_build_retention import assert_plain,file_index; p=assert_plain(Path(sys.argv[2])); file_index(p)' $PSScriptRoot $resolved
            if ($LASTEXITCODE) { throw 'Previous stage contains a reparse path' }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        } catch { Write-Warning "Published; previous stage cleanup needs attention: $resolved" }
    }
}
Write-Host "Play: $(Join-Path $OutDir 'PLAY.cmd')"
Write-Host "Share: $(Join-Path $OutDir 'Share with friend.zip')"
