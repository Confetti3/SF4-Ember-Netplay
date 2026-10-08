# Focused label and quoting checks: never execute the publisher or run builds.
param([string]$BashPath = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
$outDirectory = Join-Path ([IO.Path]::GetTempPath()) ("nightly labels [literal] ' " + [Guid]::NewGuid().ToString('N'))
$baseLabel = '1.2.0-nightly20261008'
$existingTags = @()
$fixtures = @()
try {
    New-Item -ItemType Directory -Path $outDirectory | Out-Null
    if ((Get-NightlyLabel $baseLabel $existingTags $outDirectory) -cne $baseLabel) { throw 'Unused base label was not selected' }
    $existingTags = @("v$baseLabel", "v$baseLabel.2")
    if ((Get-NightlyLabel $baseLabel $existingTags $outDirectory) -cne "$baseLabel.3") { throw 'Remote tag collisions were not skipped' }
    $existingTags = @()
    foreach ($extension in @('', '.zip', '.zip.sha256', '-setup.exe', '-setup.exe.sha256')) {
        $fixture = Join-Path $outDirectory "sf4-ember-netplay-$baseLabel$extension"
        $fixtures += $fixture
        if (!$extension) {
            New-Item -ItemType Directory -Path $fixture | Out-Null
        } else {
            Set-Content -LiteralPath $fixture -Value 'preserve existing output'
        }
        if ((Get-NightlyLabel $baseLabel $existingTags $outDirectory) -cne "$baseLabel.2") { throw "Local output collision was not skipped: $extension" }
        if (!(Test-Path -LiteralPath $fixture)) { throw 'Chooser removed an existing output' }
        if ($extension -and (Get-Content -LiteralPath $fixture -Raw).Trim() -cne 'preserve existing output') {
            throw 'Chooser changed an existing output'
        }
        Remove-Item -LiteralPath $fixture
    }
    # Alternate remote and partial local outputs, including a repeated run.
    $existingTags = @("v$baseLabel.2", "v$baseLabel.4", "v$baseLabel.7")
    foreach ($ending in @('', '.3.zip', '.5-setup.exe.sha256')) {
        $fixture = Join-Path $outDirectory "sf4-ember-netplay-$baseLabel$ending"
        $fixtures += $fixture
        Set-Content -LiteralPath $fixture -Value 'preserve existing output'
    }
    $firstLabel = Get-NightlyLabel $baseLabel $existingTags $outDirectory
    if ($firstLabel -cne "$baseLabel.6") { throw 'Mixed collisions did not select the first free suffix' }
    $fixture = Join-Path $outDirectory "sf4-ember-netplay-$firstLabel"
    $fixtures += $fixture
    New-Item -ItemType Directory -Path $fixture | Out-Null
    if ((Get-NightlyLabel $baseLabel $existingTags $outDirectory) -cne "$baseLabel.8") { throw 'Repeated run reused an occupied label' }
} finally {
    # Delete only the fixtures created above; no real dist outputs are touched.
    foreach ($fixture in $fixtures) {
        if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture }
    }
    if (Test-Path -LiteralPath $outDirectory) { Remove-Item -LiteralPath $outDirectory }
}
Write-Output 'PASS Nightly labels skip remote tags and all local outputs, preserve outputs'

$cases = @(
    @('C:\Users\Kate\Desktop\sf4\sf4-nightly\dist\nightly-room-hosts\1.2.0-nightly20261008\room-host-src.tgz',
      '/mnt/c/Users/Kate/Desktop/sf4/sf4-nightly/dist/nightly-room-hosts/1.2.0-nightly20261008/room-host-src.tgz'),
    @('D:\build dirs\room-host-src.tgz', '/mnt/d/build dirs/room-host-src.tgz'),
    @('G:/mixed\paths/build-wsl.sh', '/mnt/g/mixed/paths/build-wsl.sh'),
    @('c:\', '/mnt/c/'),
    @('Z:\build\..\dist\.\hosts', '/mnt/z/dist/hosts'),
    @('C:\Kate''s files\$HOME; & (literal) `tick\hosts', '/mnt/c/Kate''s files/$HOME; & (literal) `tick/hosts'),
    @('C:\日本語\hosts', '/mnt/c/日本語/hosts')
)
foreach ($case in $cases) {
    $actual = ConvertTo-NightlyWslPath $case[0]
    if ($actual -cne $case[1]) { throw "Wrong WSL path: $actual; expected $($case[1])" }
}
foreach ($unsupported in @('', 'dist\hosts', 'C:dist\hosts', '\dist\hosts', '\\server\share\hosts', '\\?\C:\hosts', '/mnt/c/hosts')) {
    try { ConvertTo-NightlyWslPath $unsupported | Out-Null; throw 'Unexpected path acceptance' }
    catch { if ($_.Exception.Message -notlike 'Expected an absolute drive path for WSL Ubuntu:*') { throw } }
}
Write-Output 'PASS drive conversion, separators, spaces, shell metacharacters, Unicode and unsupported-path rejection'

# Exercise the actual WSL call with a recorder instead of starting Ubuntu.
$linuxScriptFile = ConvertTo-NightlyWslPath 'C:\Kate''s files\$HOME; & (literal) `tick\build-wsl.sh'
$sourceRevision = '0123456789abcdef0123456789abcdef01234567'
$linuxArchive = ConvertTo-NightlyWslPath 'C:\Kate''s files\$HOME; & (literal) `tick\room-host-src.tgz'
$linuxArtifacts = ConvertTo-NightlyWslPath 'C:\Kate''s files\$HOME; & (literal) `tick\output dirs'
function Invoke-NightlyCommand([string]$Command, [string[]]$Arguments, [string]$Failure) {
    $expected = @('-d', 'Ubuntu', '-u', 'kate', '--exec', 'bash', '--',
        $linuxScriptFile, $sourceRevision, $linuxArchive, $linuxArtifacts)
    if ($Command -ne 'wsl' -or $Arguments.Count -ne $expected.Count) { throw 'Unexpected WSL invocation' }
    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($Arguments[$index] -cne $expected[$index]) { throw "WSL argv mismatch at $index" }
    }
}
Invoke-NightlyLinuxBuild 'C:\Kate''s files\$HOME; & (literal) `tick\build-wsl.sh' $sourceRevision `
    'C:\Kate''s files\$HOME; & (literal) `tick\room-host-src.tgz' 'C:\Kate''s files\$HOME; & (literal) `tick\output dirs'
Write-Output 'PASS WSL invocation uses --exec and separate unchanged argv'

if (!$BashPath) {
    Write-Output 'SKIP Bash syntax/argv check (supply -BashPath to a local bash.exe)'
    return
}
$fixture = Join-Path ([IO.Path]::GetTempPath()) ("nightly paths ' " + [Guid]::NewGuid().ToString('N') + '.sh')
try {
    foreach ($name in @('build-nightly-room-hosts.sh', 'stage-nightly-room-hosts.sh', 'test-nightly-staging.sh')) {
        & $BashPath -n -- (Join-Path $PSScriptRoot $name)
        if ($LASTEXITCODE -ne 0) { throw "$name has invalid Bash syntax" }
    }
    # This probe only prints argv; it never executes the build script.
    [IO.File]::WriteAllText($fixture, 'printf ''%s\n'' "$@"' + "`n", [Text.UTF8Encoding]::new($false))
    $expected = @($sourceRevision, $linuxArchive, $linuxArtifacts)
    $actual = @(& $BashPath -- $fixture @expected)
    if ($LASTEXITCODE -ne 0 -or $actual.Count -ne $expected.Count) { throw 'Bash argv probe failed' }
    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($actual[$index] -cne $expected[$index]) { throw "Bash changed argv at $index" }
    }
    Write-Output 'PASS dedicated Bash syntax and native argv round trip with spaces and shell metacharacters'
} finally {
    Remove-Item -LiteralPath $fixture -ErrorAction SilentlyContinue
}
& $BashPath -- (Join-Path $PSScriptRoot 'test-nightly-staging.sh')
if ($LASTEXITCODE -ne 0) { throw 'Atomic staging regression tests failed' }
