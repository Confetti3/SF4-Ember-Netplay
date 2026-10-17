$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
. (Join-Path $PSScriptRoot 'PrivateBuild.ps1')
$fixture = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('ember-receipt-' + [Guid]::NewGuid().ToString('N'))))
function Reject([scriptblock]$Action, [string]$Name) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw "Accepted invalid $Name" }
}
try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    & git -C $fixture init -q
    Set-Content -LiteralPath (Join-Path $fixture '.gitignore') -Value 'build/' -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $fixture 'source.txt') -Value 'source' -Encoding Ascii
    & git -C $fixture add .
    & git -C $fixture -c user.name=Fixture -c user.email=fixture@example.invalid commit -qm fixture
    if ($LASTEXITCODE) { throw 'Fixture commit failed' }
    $build = Join-Path $fixture 'build'
    $stage = Join-Path $build 'stage'
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Value @("CMAKE_HOME_DIRECTORY:INTERNAL=$fixture",'SF4E_BUILD_DISCORD:BOOL=OFF')
    $binaries = foreach ($name in $script:PrivateRuntime) {
        $file = Join-Path $build $name
        Set-Content -LiteralPath $file -Value "tested $name" -Encoding Ascii
        Copy-Item -LiteralPath $file -Destination $stage
        [pscustomobject]@{path=$name;testedPath=$file;sha256=(Get-FileHash -LiteralPath $file).Hash}
    }
    $report = Join-Path $build 'private-tests.xml'
    $cases = ($script:PrivateTests | ForEach-Object { '<testcase name="' + $_ + '" status="run"/>' }) -join ''
    $valid = '<testsuite tests="' + $script:PrivateTests.Count + '" failures="0" skipped="0">' + $cases + '</testsuite>'
    Set-Content -LiteralPath $report -Value $valid
    $receipt = @{
        kind='private-test-build';discordEnabled=$false;testsPassed=$true
        sourceRoot=$fixture;buildRoot=$build;stageRoot=$stage
        baseRevision=(& git -C $fixture rev-parse HEAD);sourceFingerprint=(Get-SourceFingerprint $fixture)
        tests=(Read-EmberTestResults $report $script:PrivateTests)
        testReport=@{path=$report;sha256=(Get-FileHash -LiteralPath $report).Hash};binaries=$binaries
    }
    $receiptPath = Join-Path $build 'build-provenance.json'
    function SaveReceipt { $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath }
    function Check { Assert-PrivateBuildReceipt $fixture $build $stage | Out-Null }
    SaveReceipt; Check
    foreach ($bad in @(
        '<testsuite tests="0"/>',
        '<testsuite tests="1"/>',
        '<testsuite tests="1"><testcase name="unrelated" status="run"/></testsuite>',
        $valid.Replace('failures="0"','failures="1"'),
        $valid.Replace('skipped="0"','errors="1"'),
        $valid.Replace('skipped="0"','skipped="1"'),
        $valid.Replace('status="run"','status="notrun"'),
        $valid.Replace('name="PrivateBuildRetention"','name="PrivateBuildReceipt"')
    )) {
        Set-Content -LiteralPath $report -Value $bad
        Reject { Read-EmberTestResults $report $script:PrivateTests } 'JUnit gate'
    }
    Set-Content -LiteralPath $report -Value $valid
    Add-Content -LiteralPath $report -Value ' '
    Reject { Check } 'changed report'
    Set-Content -LiteralPath $report -Value $valid
    $receipt.binaries = @($binaries | Select-Object -Skip 1); SaveReceipt
    Reject { Check } 'missing runtime hash'
    $receipt.binaries = $binaries; SaveReceipt
    Add-Content -LiteralPath (Join-Path $stage 'Launcher.exe') -Value 'changed'
    Reject { Check } 'changed staged binary'
    Copy-Item -LiteralPath (Join-Path $build 'Launcher.exe') -Destination $stage -Force
    Add-Content -LiteralPath (Join-Path $build 'Launcher.exe') -Value 'changed'
    Reject { Check } 'changed tested binary'
    Copy-Item -LiteralPath (Join-Path $stage 'Launcher.exe') -Destination $build -Force
    Set-Content -LiteralPath (Join-Path $stage 'unknown.dll') -Value 'unknown'
    Reject { Check } 'unverified staged DLL'
    Remove-Item -LiteralPath (Join-Path $stage 'unknown.dll')
    Add-Content -LiteralPath (Join-Path $fixture 'source.txt') -Value 'changed'
    Reject { Check } 'changed source'
    Set-Content -LiteralPath (Join-Path $fixture 'source.txt') -Value 'source' -Encoding Ascii
    $receipt.baseRevision='old'; SaveReceipt
    Reject { Check } 'stale commit'
    Write-Output 'Private receipt: valid build accepted; 15 invalid gate/identity mutations rejected.'
} finally {
    if (!$fixture.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped temporary directory' }
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
