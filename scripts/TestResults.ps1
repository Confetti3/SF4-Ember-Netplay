$ErrorActionPreference = 'Stop'
function Read-EmberTestResults([string]$Path, [string[]]$RequiredNames = @()) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Test report missing: $Path" }
    [xml]$report = Get-Content -LiteralPath $Path -Raw
    $suite = $report.SelectSingleNode('/testsuite')
    if (!$suite) { throw 'Expected a CTest JUnit testsuite.' }
    $cases = @($suite.SelectNodes('testcase'))
    $declared = 0
    if (![int]::TryParse($suite.GetAttribute('tests'), [ref]$declared) -or
        $declared -le 0 -or $declared -ne $cases.Count) { throw 'Empty or inconsistent test report.' }
    foreach ($attribute in @('failures','errors','skipped','disabled')) {
        $value = $suite.GetAttribute($attribute)
        $count = 0
        if ($value -and (![int]::TryParse($value,[ref]$count) -or $count -ne 0)) {
            throw "Test report has $attribute : $value"
        }
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($case in $cases) {
        if ($case.GetAttribute('status') -ne 'run' -or $case.SelectNodes('failure|error|skipped').Count) {
            throw "Test did not pass: $($case.GetAttribute('name'))"
        }
        $name = $case.GetAttribute('name')
        if (!$name -or !$names.Add($name)) { throw 'Missing or duplicate test name.' }
    }
    foreach ($name in $RequiredNames) { if (!$names.Contains($name)) { throw "Required test did not run: $name" } }
    [pscustomobject]@{passed=$true;total=$cases.Count;failed=0;skipped=0;names=@($names | Sort-Object)}
}
