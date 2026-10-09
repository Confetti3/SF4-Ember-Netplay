param([Parameter(Mandatory=$true)][string]$Updater)
$ErrorActionPreference = 'Stop'
$originalTemp = $env:TEMP
$originalTmp = $env:TMP
$temporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryBase ('ember-private-update-' + [Guid]::NewGuid().ToString('N'))
$resolvedFixture = [System.IO.Path]::GetFullPath($fixture)
if (-not $resolvedFixture.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test fixture escaped temporary directory' }
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    $target = Join-Path $fixture 'package'
    $stage = Join-Path $fixture 'stage'
    New-Item -ItemType Directory -Path $target, $stage | Out-Null
    Set-Content -LiteralPath (Join-Path $target 'PRIVATE_TEST_BUILD.txt') -Value 'Private build fixture' -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $target 'preserved.txt') -Value 'Keep this file' -Encoding Ascii
    $before = (Get-FileHash -LiteralPath (Join-Path $target 'preserved.txt')).Hash
    $env:TEMP = $fixture
    $env:TMP = $fixture
    & $Updater -InstallDir $target -StagingDir $stage
    if ($LASTEXITCODE -ne 1) { throw 'Private test update was not refused' }
    $log = Get-Content -LiteralPath (Join-Path $fixture 'sf4-netplay-update.log') -Raw
    if ($log -notmatch 'public updates are disabled for this private test package') { throw 'Updater refused for an unexpected reason' }
    if ((Get-FileHash -LiteralPath (Join-Path $target 'preserved.txt')).Hash -ne $before) { throw 'Target file changed' }
    if (@(Get-ChildItem -LiteralPath $target).Count -ne 2) { throw 'Update mutated the private package' }
    Write-Output 'Private test update guard passed; package files preserved.'
} finally {
    $env:TEMP = $originalTemp
    $env:TMP = $originalTmp
    if ([System.IO.Path]::GetFullPath($fixture) -ne $resolvedFixture) { throw 'Fixture path changed before cleanup' }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
