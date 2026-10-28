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
# -SkipFingerprint drops only the fresh fingerprint of the tree: a changed tree passes, every other check still rejects.
[IO.File]::WriteAllText((Join-Path $source 'feature.cxx'), 'newer feature')
Assert-BuildReceipt $source $build $stage -SkipFingerprint | Out-Null
[IO.File]::WriteAllText((Join-Path $stage 'Sidecar.dll'), 'stale other build')
try { Assert-BuildReceipt $source $build $stage -SkipFingerprint | Out-Null; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Staged binary changed*') { throw } }
[IO.File]::WriteAllText((Join-Path $stage 'Sidecar.dll'), 'fixture only - not executable')
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$fixture")
try { Assert-BuildReceipt $source $build $stage -SkipFingerprint | Out-Null; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'CMake source mismatch*') { throw } }
[IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), "CMAKE_HOME_DIRECTORY:INTERNAL=$source")
[IO.File]::WriteAllText((Join-Path $source 'feature.cxx'), 'current feature')
Write-Output 'PASS the receipt check without the fresh fingerprint still refuses a changed binary and a foreign CMake source'

# A fake CTest whose first run fails HelperRust; a rerun fails it again only when $failAgain says so, and a
# first run leaves out the test $omit names. Its tests are the ones a receipt with testsPassed must show as run
# (the $missing list of Invoke-DefaultTests): the helper and Ember Rust suites, the SDK and the publisher's suites.
$tests = Join-Path $fixture 'tests'
New-Item -ItemType Directory -Path (Join-Path $tests 'Testing/Temporary') | Out-Null
$fakeCtest = {
    $rerun = $args -contains '--rerun-failed'
    $failing = if ($rerun) { $failAgain } elseif ($omit) { $null } else { 'HelperRust' }
    $cases = foreach ($name in $(if ($rerun) { 'HelperRust' } else { 'HelperRust', 'EmberRust', 'BridgeSdk', 'BuildProvenance', 'NightlyPublication', 'NightlyPaths', 'PackageFiles' })) {
        if ($name -eq $omit) { continue }
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
$omit = $null
$passedOnRerun = @(Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 -RerunFailed 3>$null 6>$null)
if (($passedOnRerun -join ',') -ne 'HelperRust') { throw "Rerun reported '$($passedOnRerun -join ',')'" }
Expect-TestFailure 'Tests failed' { Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 6>$null }
$failAgain = 'HelperRust'
Expect-TestFailure 'Tests failed, and again*' { Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 -RerunFailed 6>$null }
$global:LASTEXITCODE = 0
Write-Output 'PASS a failed test passes on one rerun only with -RerunFailed, and a second failure stops the build'
# A run that leaves out any one of the tests the components name is refused, whichever one it is.
$failAgain = $null
foreach ($name in 'HelperRust', 'EmberRust', 'BridgeSdk', 'BuildProvenance', 'NightlyPublication', 'NightlyPaths', 'PackageFiles') {
    $omit = $name
    Expect-TestFailure "Default CTest did not run: $name" { Invoke-DefaultTests $fakeCtest $tests 'Excluded' 2 6>$null }
}
$omit = $null
$global:LASTEXITCODE = 0
Write-Output 'PASS a CTest run without one of the suites the receipt components name is refused'
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

# Stable publication and packaging refuse the Nightly checkout; a receipt must match its channel's staging bridge.
$designation.channels.nightly.buildDirectory = 'build/nightly'
$designation.channels.nightly.sourceDirectory = 'nightly'
$designation | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $fixture 'build-target.json')
Assert-EmberChannel (Get-EmberBuildTarget $standalone) 'stable' 'Stable publication'
try { Assert-EmberChannel (Get-EmberBuildTarget $nightly) 'stable' 'Stable publication'; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Stable publication requires the stable channel*nightly*') { throw } }
try { Assert-EmberChannel (Get-EmberBuildTarget $standalone) 'nightly' 'Nightly publication'; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike 'Nightly publication requires the nightly channel*stable*') { throw } }
Write-Output 'PASS the channel guard accepts its own checkout and refuses the other channel'
function Expect-ChannelRejection([object]$Receipt, [string]$Channel) {
    try { Assert-ReceiptChannel $Receipt $Channel; throw 'Unexpected acceptance' }
    catch { if ($_.Exception.Message -notlike "The build receipt's staging bridge setting does not match*") { throw } }
}
Assert-ReceiptChannel ([pscustomobject]@{stagingBridge=$false;branch='release'}) 'stable'
Assert-ReceiptChannel ([pscustomobject]@{stagingBridge=$true;branch='nightly'}) 'nightly'
Expect-ChannelRejection ([pscustomobject]@{stagingBridge=$true}) 'stable'
Expect-ChannelRejection ([pscustomobject]@{stagingBridge=$false}) 'nightly'
Expect-ChannelRejection ([pscustomobject]@{sourceRoot='predates the field'}) 'stable'
Expect-ChannelRejection ([pscustomobject]@{stagingBridge='true'}) 'nightly'
Write-Output 'PASS a receipt must record the staging bridge setting of its own channel; a missing field is refused'

# A Nightly checkout whose parent folder has no build-target.json falls back to its own file,
# which names only the top-level entry, so it reads as stable. The receipt's branch still refuses it.
$fallbackParent = Join-Path $fixture 'fallback-parent'
$fallback = Join-Path $fallbackParent 'sf4-nightly'
New-Item -ItemType Directory -Path $fallback | Out-Null
@{sourceDirectory='.';buildDirectory='build/current';installDirectory='build/current/stage'} | ConvertTo-Json | Set-Content (Join-Path $fallback 'build-target.json')
& git -C $fallback init -q -b nightly
if ($LASTEXITCODE) { throw 'git init failed' }
$fallbackTarget = Get-EmberBuildTarget $fallback
if ($fallbackTarget.channel -ne 'stable') { throw 'The fallback designation no longer reads as stable; revisit this case' }
Assert-EmberChannel $fallbackTarget 'stable' 'Stable publication'
$fallbackBranch = & git -C $fallback branch --show-current
$fallbackReceipt = [pscustomobject]@{stagingBridge=$false;branch=$fallbackBranch}
try { Assert-ReceiptChannel $fallbackReceipt 'stable'; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike "The build receipt is from branch 'nightly'; Stable builds come only from the release branch.") { throw } }
try { Assert-EmberStableBranch $fallbackBranch 'Stable release packaging'; throw 'Unexpected acceptance' }
catch { if ($_.Exception.Message -notlike "Stable release packaging requires the release branch, but this checkout is on 'nightly'.") { throw } }
foreach ($branch in @($null, '', 'Release', 'release-candidate')) {
    try { Assert-ReceiptChannel ([pscustomobject]@{stagingBridge=$false;branch=$branch}) 'stable'; throw 'Unexpected acceptance' }
    catch { if ($_.Exception.Message -notlike "The build receipt is from branch*") { throw } }
    try { Assert-EmberStableBranch $branch 'Stable publication'; throw 'Unexpected acceptance' }
    catch { if ($_.Exception.Message -notlike 'Stable publication requires the release branch*') { throw } }
}
Assert-EmberStableBranch 'release' 'Stable publication'
Write-Output 'PASS a fallback-designated checkout off the release branch reads as stable but its receipt and branch are refused for Stable'

# Entering the compiler environment is once per process: a process that already holds the same installation and
# target architecture returns at once (the path below has no vcvarsall.bat, so running it would throw), while another
# architecture or installation still runs it.
$vcSaved = @{VSINSTALLDIR=$env:VSINSTALLDIR;VSCMD_ARG_TGT_ARCH=$env:VSCMD_ARG_TGT_ARCH}
try {
    $vcRoot = Join-Path $fixture 'vs'
    $env:VSINSTALLDIR = "$vcRoot\"
    $env:VSCMD_ARG_TGT_ARCH = 'x86'
    Enter-EmberVcEnvironment $vcRoot
    Enter-EmberVcEnvironment ($vcRoot.ToUpperInvariant() + '\') 'x86'
    foreach ($case in @(@($vcRoot, 'x64'), @((Join-Path $fixture 'other-vs'), 'x86'))) {
        try { Enter-EmberVcEnvironment $case[0] $case[1] 2>$null; throw 'Unexpected skip' }
        catch { if ($_.Exception.Message -ne 'Compiler environment failed') { throw } }
    }
    $env:VSINSTALLDIR = $null
    try { Enter-EmberVcEnvironment $vcRoot 2>$null; throw 'Unexpected skip' }
    catch { if ($_.Exception.Message -ne 'Compiler environment failed') { throw } }
} finally {
    $env:VSINSTALLDIR = $vcSaved.VSINSTALLDIR
    $env:VSCMD_ARG_TGT_ARCH = $vcSaved.VSCMD_ARG_TGT_ARCH
}
Write-Output 'PASS the compiler environment is entered once per installation and architecture'
# The receipt names what the default test run leaves out; build-current.ps1 and run-package-tests.ps1 share this string.
if ($EmberExcludedTests -cne 'Iroh(Room|Game|Authorized|Recovery)|CustomRoom(FourTables|Spectators)|DiscordSmoke') { throw 'The default test exclusions changed; the build receipt and docs name them' }
Write-Output 'PASS the default test exclusions are the ones the receipt names'
