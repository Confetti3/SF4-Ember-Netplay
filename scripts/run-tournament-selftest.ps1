# Runs the two-PC tournament self-test (docs/design/TOURNAMENT_SELFTEST.md):
# a local test bridge, the host game on the other PC and the guest game here,
# each run by run-selftest.ps1 from a build with SF4E_SELFTEST=ON. The bridge
# side (sdk/typescript/tools/selftest-bridge.ts) links both games, creates
# the match and checks its signed result. Prints every side's PASS and FAIL
# lines; exit code 0 when all passed, 1 when one failed.
#
# The other PC is reached with ssh (key sign-in, default shell PowerShell) and
# starts its game through a scheduled task that runs in the signed-in user's
# session ("Ember Interactive"): a process started over ssh has no desktop.
# The game has to be closed and Steam signed in on both PCs. Needs Node.js 22.18
# or later here, and a free -Port and -Port + 1 on this PC and -Port on the other.
param(
    # This PC's package, the guest's.
    [Parameter(Mandatory)] [string]$PackageDir,
    # ember-bridge.exe built from the same checkout.
    [Parameter(Mandatory)] [string]$BridgeExe,
    # The other PC: its ssh name and the checkout there, with this branch and
    # its staged build in build\current\stage.
    [string]$RemoteHost = "director",
    [string]$RemoteCheckout = "D:\sf4\prs\tourney-selftest",
    # The scheduled task, and the folder its jobs and logs live in.
    [string]$RemoteTask = "Ember Interactive",
    [string]$RemoteJobs = "D:\sf4\interactive",
    [int]$Port = 18787,
    [int]$TimeoutMinutes = 30
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

# A script run on the other PC, passed whole so nothing in it needs quoting
# for the command line.
function Invoke-Remote([string]$Script) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script))
    # ssh's own notices on stderr start with "**".
    $output = @(ssh $RemoteHost "powershell -NoProfile -NonInteractive -EncodedCommand $encoded" 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^\*\*' })
    if ($LASTEXITCODE) { throw "ssh $RemoteHost failed ($LASTEXITCODE): $($output -join ' ')" }
    $output
}

function Test-PortFree([int]$Number) { -not (Get-NetTCPConnection -LocalPort $Number -State Listen -ErrorAction SilentlyContinue) }

if (Get-Process SSFIV -ErrorAction SilentlyContinue) { throw "Close Ultra Street Fighter IV first." }
if (-not (Test-Path $BridgeExe)) { throw "ember-bridge.exe not found at $BridgeExe" }
if (-not (Test-PortFree $Port) -or -not (Test-PortFree ($Port + 1))) { throw "Port $Port or $($Port + 1) is in use on this PC." }
$remoteExchange = "$RemoteJobs\tourney-exchange"
$remoteState = Invoke-Remote @"
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) { 'port-busy' }
if (Get-Process SSFIV -ErrorAction SilentlyContinue) { 'game-running' }
if (-not (Test-Path '$RemoteCheckout\build\current\stage\Launcher.exe')) { 'no-package' }
"@
if ($remoteState) { throw "$RemoteHost is not ready: $($remoteState -join ', ')" }

$work = Join-Path $env:TEMP ("ember-tourney-" + [guid]::NewGuid().ToString("N"))
$bridgeWork = Join-Path $work "bridge"
$guestExchange = Join-Path $work "guest"
New-Item -ItemType Directory -Path $guestExchange -Force | Out-Null
$coordinatorLog = Join-Path $work "bridge.log"
$guestLog = Join-Path $work "guest.log"
$hostLog = Join-Path $work "host.log"
$origin = "http://127.0.0.1:$Port"
$coordinator = $null; $guest = $null; $hostStarted = $false

# The lines a side printed, labelled with the side.
function Get-Steps([string]$Side, [string]$Log) {
    if (-not (Test-Path $Log)) { return @("FAIL $($Side): no output") }
    @(Get-Content $Log | Where-Object { $_ -match '^(PASS|FAIL|RESULT) ' } | ForEach-Object { "$Side $_" })
}

try {
    # The bridge side first: its bridge, result receiver and tunnel are up
    # when it prints READY.
    $node = (Get-Command node).Source
    $coordinator = Start-Process -FilePath $node -PassThru -NoNewWindow -RedirectStandardOutput $coordinatorLog -RedirectStandardError "$coordinatorLog.err" -WorkingDirectory "$repo\sdk\typescript" -ArgumentList @(
        "tools/selftest-bridge.ts", "--bridge", "`"$BridgeExe`"", "--work", "`"$bridgeWork`"",
        "--host", "`"ssh:$($RemoteHost):$remoteExchange`"", "--guest", "`"$guestExchange`"",
        "--port", $Port, "--minutes", $TimeoutMinutes)
    $ready = (Get-Date).AddMinutes(2)
    while (-not ((Test-Path $coordinatorLog) -and (Select-String -Path $coordinatorLog -Pattern '^READY$' -Quiet))) {
        if ($coordinator.HasExited -or (Get-Date) -gt $ready) { throw "The bridge side did not start: $(Get-Content $coordinatorLog, "$coordinatorLog.err" -ErrorAction SilentlyContinue)" }
        Start-Sleep -Seconds 1
    }
    Get-Steps "bridge" $coordinatorLog | Write-Host

    # The host, through the other PC's task: a job of its own, its own log,
    # started in the same call that sees the task free.
    $hostJob = @"
`$ErrorActionPreference = 'Stop'
`$began = Get-Date
& '$RemoteCheckout\scripts\run-selftest.ps1' -PackageDir '$RemoteCheckout\build\current\stage' -Test tournament-host -Exchange '$remoteExchange' -Bridge '$origin' -Connection mock-local
`$code = `$LASTEXITCODE
"run-selftest tournament-host exit `$code"
`$log = Join-Path `$env:APPDATA 'sf4e\logs\sf4e.log'
if (Test-Path `$log) {
    '--- sf4e.log lines from this run:'
    Get-Content `$log | Where-Object { `$_ -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' -and [datetime]`$Matches[1] -ge `$began.AddSeconds(-5) } |
        Where-Object { `$_ -match 'SelfTest|Tournament|error|fail|crash|exception' } | Select-Object -Last 80
}
exit `$code
"@
    $started = Invoke-Remote @"
Remove-Item '$remoteExchange' -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path '$remoteExchange' -Force | Out-Null
Set-Content -LiteralPath '$RemoteJobs\tourney-host.ps1' -Value @'
$hostJob
'@
Set-Content -LiteralPath '$RemoteJobs\tourney-host.cmd' -Value '@"%ProgramFiles%\PowerShell\7\pwsh.exe" -NoProfile -ExecutionPolicy Bypass -File $RemoteJobs\tourney-host.ps1 > $RemoteJobs\tourney-host.log 2>&1'
Remove-Item '$RemoteJobs\tourney-host.log' -ErrorAction SilentlyContinue
if ((schtasks /query /tn '$RemoteTask' /fo csv /nh) -notmatch '"Ready"') { 'busy'; return }
Copy-Item '$RemoteJobs\tourney-host.cmd' '$RemoteJobs\job.cmd' -Force
schtasks /run /tn '$RemoteTask' | Out-Null
'started'
"@
    if ($started -notcontains "started") { throw "The task '$RemoteTask' on $RemoteHost is busy; nothing was started there." }
    $hostStarted = $true
    Write-Host "host started on $RemoteHost"

    # The guest, here.
    $pwsh = (Get-Process -Id $PID).Path
    $guest = Start-Process -FilePath $pwsh -PassThru -NoNewWindow -RedirectStandardOutput $guestLog -RedirectStandardError "$guestLog.err" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSScriptRoot\run-selftest.ps1`"", "-PackageDir", "`"$PackageDir`"",
        "-Test", "tournament-guest", "-Exchange", "`"$guestExchange`"", "-Bridge", $origin, "-Connection", "mock-local",
        "-TimeoutMinutes", $TimeoutMinutes)
    Write-Host "guest started here"

    # Both games close by themselves; the task is Ready again once the host's has.
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes + 2)
    $hostDone = $false
    while (-not ($guest.HasExited -and $hostDone)) {
        if ((Get-Date) -gt $deadline) { Write-Host "FAIL timeout: the games were still running after $TimeoutMinutes minutes"; break }
        Start-Sleep -Seconds 10
        if (-not $hostDone) { $hostDone = (Invoke-Remote "schtasks /query /tn '$RemoteTask' /fo csv /nh") -match '"Ready"' }
    }
}
finally {
    # Nothing more comes from the games: the bridge side checks the match,
    # stops its bridge, receiver and tunnel and deletes its folder.
    if ($coordinator) {
        New-Item -ItemType File -Path (Join-Path $bridgeWork "stop") -Force | Out-Null
        if (-not $coordinator.WaitForExit(90000)) {
            # Its children by PID: the bridge and the tunnel it started.
            Get-CimInstance Win32_Process -Filter "ParentProcessId=$($coordinator.Id)" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
            Stop-Process -Id $coordinator.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($guest -and -not $guest.HasExited) { Stop-Process -Id $guest.Id -Force -ErrorAction SilentlyContinue }
    if ($hostStarted) {
        try { Invoke-Remote "Get-Content -LiteralPath '$RemoteJobs\tourney-host.log' -ErrorAction SilentlyContinue; Remove-Item '$remoteExchange' -Recurse -Force -ErrorAction SilentlyContinue" | Set-Content $hostLog }
        catch { Write-Host "The host's log could not be read: $_" }
    }
}

$steps = @(Get-Steps "host" $hostLog) + @(Get-Steps "guest" $guestLog) + @(Get-Steps "bridge" $coordinatorLog)
$steps | Write-Host
$passed = ($steps -match '^\w+ RESULT pass$').Count -eq 3 -and -not ($steps -match '^\w+ (FAIL|RESULT fail)')
# The logs stay for a failed run.
if ($passed) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
else { Write-Host "Logs: $work (the host's sf4e.log lines are in host.log)" }
Write-Host $(if ($passed) { "RESULT pass" } else { "RESULT fail" })
exit $(if ($passed) { 0 } else { 1 })
