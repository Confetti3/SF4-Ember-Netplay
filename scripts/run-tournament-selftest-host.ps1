# The host side of the two-PC tournament self-test
# (docs/design/TOURNAMENT_SELFTEST.md), run on the other PC by its scheduled
# task, from a job scripts/run-tournament-selftest.ps1 wrote. It runs the host
# game through run-selftest.ps1, then prints the game log's self-test lines.
# Its own PID goes to -PidFile first, so the runner can end the game under it,
# and the line `tourney-host done` is the last thing it prints, which the
# runner waits for.
param(
    [Parameter(Mandatory)] [string]$PackageDir,
    # The exchange folder the runner reaches over ssh, the bridge's origin there.
    [Parameter(Mandatory)] [string]$Exchange,
    [Parameter(Mandatory)] [string]$Bridge,
    [Parameter(Mandatory)] [string]$PidFile,
    # The bridge's connection to link to, and the sign-in site to connect an
    # account on instead (the Challonge mode); see run-selftest.ps1.
    [string]$Connection = "mock-local",
    [string]$Site = "",
    [int]$TimeoutMinutes = 20
)

$ErrorActionPreference = "Stop"
Set-Content -LiteralPath $PidFile -Value $PID
$began = Get-Date
try {
    & (Join-Path $PSScriptRoot "run-selftest.ps1") -PackageDir $PackageDir -Test tournament-host -Exchange $Exchange -Bridge $Bridge -Connection $Connection -Site $Site -TimeoutMinutes $TimeoutMinutes
    "run-selftest tournament-host exit $LASTEXITCODE"
}
catch { "FAIL run-selftest tournament-host: $_" }
finally {
    $log = Join-Path $env:APPDATA "sf4e\logs\sf4e.log"
    if (Test-Path $log) {
        '--- sf4e.log lines from this run:'
        Get-Content $log | Where-Object { $_ -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' -and [datetime]$Matches[1] -ge $began.AddSeconds(-5) } |
            Where-Object { $_ -match 'SelfTest|Tournament|error|fail|crash|exception' } | Select-Object -Last 80
    }
    'tourney-host done'
}
