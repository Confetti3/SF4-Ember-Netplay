$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('sf4-build-guard-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$source = Join-Path $fixture 'source'
$build = Join-Path $fixture 'build'
$stage = Join-Path $fixture 'stage'
New-Item -ItemType Directory -Path $source,$build,$stage | Out-Null
& git -C $source init -q
[IO.File]::WriteAllText((Join-Path $source 'feature.cxx'), 'current feature')
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$source")
[IO.File]::WriteAllText((Join-Path $stage 'Sidecar.dll'), 'fixture only - not executable')
$record = @{sourceRoot=$source;buildRoot=$build;stageRoot=$stage;sourceFingerprint=(Get-SourceFingerprint $source);binaries=@(@{path='Sidecar.dll';sha256=(Get-FileHash (Join-Path $stage 'Sidecar.dll')).Hash})}
$record | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $build 'build-provenance.json')
Assert-BuildReceipt $source $build $stage | Out-Null
Write-Output 'PASS matching source/cache/stage receipt'
function Expect-Rejection([string]$Expected) {
    try { Assert-BuildReceipt $source $build $stage | Out-Null; throw 'Unexpected acceptance' }
    catch { if ($_.Exception.Message -notlike $Expected) { throw }; Write-Output "PASS rejected: $Expected" }
}
[IO.File]::WriteAllText((Join-Path $source 'feature.cxx'), 'newer feature')
Expect-Rejection 'Source changed*'
[IO.File]::WriteAllText((Join-Path $source 'feature.cxx'), 'current feature')
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$fixture")
Expect-Rejection 'CMake source mismatch*'
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$source")
[IO.File]::WriteAllText((Join-Path $stage 'Sidecar.dll'), 'stale other build')
Expect-Rejection 'Staged binary changed*'
Write-Output "Fixtures retained at $fixture"

. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$standalone = Join-Path $fixture 'standalone'
New-Item -ItemType Directory -Path $standalone | Out-Null
@{sourceDirectory='.';buildDirectory='build/current';installDirectory='build/current/stage'} | ConvertTo-Json | Set-Content (Join-Path $standalone 'build-target.json')
if ((Get-EmberBuildTarget $standalone).buildDirectory -ne 'build/current') { throw 'Standalone target failed' }
@{sourceDirectory='standalone';buildDirectory='build/current';installDirectory='build/current/stage'} | ConvertTo-Json | Set-Content (Join-Path $fixture 'build-target.json')
Get-EmberBuildTarget $standalone | Out-Null
try { Get-EmberBuildTarget $source | Out-Null; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Wrong build target:*') { throw } }
@{sourceDirectory='standalone';buildDirectory='../outside';installDirectory='build/current/stage'} | ConvertTo-Json | Set-Content (Join-Path $fixture 'build-target.json')
try { Get-EmberBuildTarget $standalone | Out-Null; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Build and stage paths must be inside*') { throw } }
Write-Output 'PASS standalone target, workspace precedence, wrong-checkout and path-escape guards'
