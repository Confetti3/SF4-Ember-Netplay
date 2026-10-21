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
# Dashes, numeric suffixes and case distinguish ordinal from linguistic ordering.
# Thai collation can also treat a-b and ab as equal under Sort-Object -Unique.
$ordinalPaths = @('I.cxx','a-b.cxx','ab.cxx','color-1-cutout.png','color-10-cutout.png','feature.cxx','i-helper.cxx')
foreach ($relative in $ordinalPaths) {
    if ($relative -ne 'feature.cxx') { [IO.File]::WriteAllText((Join-Path $source $relative), $relative) }
}
$originalCulture = [cultureinfo]::CurrentCulture
try {
    [cultureinfo]::CurrentCulture = 'en-US'
    $fingerprint = Get-SourceFingerprint $source
    $record = @{sourceRoot=$source;buildRoot=$build;stageRoot=$stage;sourceFingerprint=$fingerprint;selfTest=$false;binaries=@(@{path='Sidecar.dll';sha256=(Get-FileHash (Join-Path $stage 'Sidecar.dll')).Hash})}
    $record | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $build 'build-provenance.json')
    foreach ($culture in @('en-US','th-TH','tr-TR','sv-SE','nl-BE')) {
        [cultureinfo]::CurrentCulture = $culture
        if ((Get-SourceFingerprint $source) -cne $fingerprint) { throw "Source fingerprint changed with culture: $culture" }
        Assert-BuildReceipt $source $build $stage | Out-Null
    }
    # Assert the serialization contract as well: pinning en-US alone is insufficient.
    $lines = foreach ($relative in $ordinalPaths) { "$relative $((Get-FileHash -LiteralPath (Join-Path $source $relative)).Hash)" }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $expected = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-','') }
    finally { $sha.Dispose() }
    if ($fingerprint -cne $expected) { throw 'Source fingerprint does not serialize every path in ordinal order' }
    Write-Output 'PASS ordinal source fingerprint and en-US build receipt across en-US/th-TH/tr-TR/sv-SE/nl-BE'
} finally { [cultureinfo]::CurrentCulture = $originalCulture }
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
[IO.File]::WriteAllText((Join-Path $stage 'Sidecar.dll'), 'fixture only - not executable')
# A self-test build is refused whether its receipt says so, omits the field, or the cache was seeded ON afterwards.
$goodReceipt = Get-Content (Join-Path $build 'build-provenance.json') -Raw
foreach ($selfTest in @($true, $null)) {
    $tampered = $goodReceipt | ConvertFrom-Json
    if ($null -eq $selfTest) { $tampered.PSObject.Properties.Remove('selfTest') } else { $tampered.selfTest = $selfTest }
    $tampered | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $build 'build-provenance.json')
    Expect-Rejection 'The build has the in-game self-test*'
}
Set-Content (Join-Path $build 'build-provenance.json') $goodReceipt
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$source`nSF4E_SELFTEST:BOOL=ON")
Expect-Rejection 'The build has the in-game self-test*'
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$source")
Assert-BuildReceipt $source $build $stage | Out-Null
Write-Output 'PASS self-test build refused by receipt, missing field and cache'

# A fake CTest whose first run fails HelperRust; a rerun fails it again only when $failAgain says so.
$tests = Join-Path $fixture 'tests'
New-Item -ItemType Directory -Path (Join-Path $tests 'Testing/Temporary') | Out-Null
$fakeCtest = {
    $rerun = $args -contains '--rerun-failed'
    $failing = if ($rerun) { $failAgain } else { 'HelperRust' }
    $cases = foreach ($name in $(if ($rerun) { 'HelperRust' } else { 'HelperRust', 'EmberRust', 'BridgeSdk' })) {
        if ($name -eq $failing) { "<testcase name=`"$name`" status=`"fail`"><failure/></testcase>" } else { "<testcase name=`"$name`" status=`"run`"/>" }
    }
    Set-Content -LiteralPath $args[[Array]::IndexOf($args, '--output-junit') + 1] -Value "<testsuite>$($cases -join '')</testsuite>"
    Set-Content -LiteralPath (Join-Path $tests 'Testing/Temporary/LastTestsFailed.log') -Value '84:HelperRust'
    $global:LASTEXITCODE = [int][bool]$failing
}
function Expect-TestFailure([string]$Expected, [scriptblock]$Run) {
    try { & $Run | Out-Null; throw 'Unexpected pass' }
    catch { if ($_.Exception.Message -notlike $Expected) { throw } }
}
$failAgain = $null
$passedOnRerun = @(Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 -RerunFailed 3>$null 6>$null)
if (($passedOnRerun -join ',') -ne 'HelperRust') { throw "Rerun reported '$($passedOnRerun -join ',')'" }
Expect-TestFailure 'Tests failed' { Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 6>$null }
$failAgain = 'HelperRust'
Expect-TestFailure 'Tests failed, and again*' { Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 -RerunFailed 6>$null }
$global:LASTEXITCODE = 0
Write-Output 'PASS a failed test passes on one rerun only with -RerunFailed, and a second failure stops the build'
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

$nightly = Join-Path $fixture 'nightly'
New-Item -ItemType Directory -Path $nightly | Out-Null
$designation = @{sourceDirectory='standalone';buildDirectory='build/current';installDirectory='build/current/stage';channels=@{nightly=@{sourceDirectory='nightly';buildDirectory='build/nightly';installDirectory='build/nightly/stage';features=@('fixture')}}}
$designation | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'build-target.json')
$target = Get-EmberBuildTarget $nightly
if ($target.channel -ne 'nightly' -or $target.sourceDirectory -ne 'nightly' -or $target.buildDirectory -ne 'build/nightly' -or $target.installDirectory -ne 'build/nightly/stage' -or $target.features[0] -ne 'fixture') { throw 'Channel target failed' }
Write-Output 'PASS channel target and properties'
if ((Get-EmberBuildTarget $standalone).channel -ne 'stable') { throw 'Stable target failed' }
Write-Output 'PASS top-level target remains stable'
try { Get-EmberBuildTarget $source | Out-Null; throw 'Unexpected acceptance' }
catch {
    if ($_.Exception.Message -notlike 'Wrong build target:*' -or !$_.Exception.Message.Contains($standalone) -or !$_.Exception.Message.Contains($nightly)) { throw }
}
Write-Output 'PASS unrelated checkout rejected with all designated sources'
$designation.channels.nightly.buildDirectory = '../outside'
$designation | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'build-target.json')
try { Get-EmberBuildTarget $nightly | Out-Null; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Build and stage paths must be inside*') { throw } }
Write-Output 'PASS channel path escape rejected'
$designation.channels.nightly.sourceDirectory = 'standalone'
$designation | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'build-target.json')
try { Get-EmberBuildTarget $standalone | Out-Null; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Ambiguous build target:*') { throw } }
Write-Output 'PASS duplicate checkout designation rejected'
