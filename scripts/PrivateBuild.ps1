# Shared by the builder and direct assembler; neither can bypass the same gate.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'TestResults.ps1')
$script:PrivateRuntime = @('Launcher.exe','Sidecar.dll','Updater.exe','sf4-net.exe','GGPO.dll','spdlog.dll','fmt.dll','zlib1.dll')
$script:PrivateTests = @('PrivateBuildRetention','PrivateBuildReceipt','PrivateTestUpdateGuard',
    'PackageInstaller','UpdaterRun','UpdaterDownloadValidation','UpgradeRecoveryPowerShell',
    'InputDelay','AutoDelayCheck','SessionController','SettingsStore','Localization',
    'GgpoDelayRollbackZeroZero','GgpoDelayRollbackZeroOne','GgpoDelayRollbackFiveZero','GgpoDelayRollbackCorruptRestore')
function Get-PrivatePython([string]$Python = $env:SF4E_PYTHON) {
    if (!$Python) { $Python = 'python' }
    $command = Get-Command $Python -ErrorAction Stop
    $resolved = $command.Source
    & $resolved -c 'import sys; sys.exit(0 if sys.version_info >= (3,12) else 1)'
    if ($LASTEXITCODE) { throw 'Private packaging requires Python 3.12 or newer' }
    return $resolved
}
function Assert-PrivateBuildReceipt([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot) {
    $receipt = Assert-BuildReceipt $SourceRoot $BuildRoot $StageRoot
    if ($receipt.kind -ne 'private-test-build' -or $receipt.discordEnabled -ne $false -or !$receipt.testsPassed) { throw 'Expected a passing SDK-free private receipt' }
    if ((Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt')) -notcontains 'SF4E_BUILD_DISCORD:BOOL=OFF') { throw 'Private packaging requires SF4E_BUILD_DISCORD=OFF' }
    if ($receipt.baseRevision -ne (& git -C $SourceRoot rev-parse HEAD)) { throw 'Receipt source commit changed' }
    $report = Join-Path $BuildRoot 'private-tests.xml'
    if ($receipt.testReport.path -ne $report -or (Get-FileHash -LiteralPath $report).Hash -ne $receipt.testReport.sha256) { throw 'Test report identity changed' }
    $tests = Read-EmberTestResults $report $script:PrivateTests
    if ($receipt.tests.total -ne $tests.total -or !$receipt.tests.passed) { throw 'Test receipt does not match report' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($binary in $receipt.binaries) {
        if (!$binary.path -or [IO.Path]::GetFileName($binary.path) -ne $binary.path -or !$seen.Add($binary.path) -or $binary.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid binary receipt' }
        if ($binary.path -in $script:PrivateRuntime -and $binary.testedPath -ne (Join-Path $BuildRoot $binary.path)) { throw 'Runtime was not tested in this build' }
        if ((Get-FileHash -LiteralPath $binary.testedPath).Hash -ne $binary.sha256) { throw "Tested binary changed: $($binary.path)" }
        if ((Get-FileHash -LiteralPath (Join-Path $StageRoot $binary.path)).Hash -ne $binary.sha256) { throw "Staged binary changed: $($binary.path)" }
    }
    foreach ($name in $script:PrivateRuntime) { if (!$seen.Contains($name)) { throw "Missing tested binary: $name" } }
    foreach ($file in Get-ChildItem -LiteralPath $StageRoot -File | Where-Object { $_.Extension -in @('.exe','.dll') }) {
        if (!$seen.Contains($file.Name)) { throw "Unverified staged runtime: $($file.Name)" }
    }
    return $receipt
}
