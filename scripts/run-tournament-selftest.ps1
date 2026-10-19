# Runs the two-PC tournament self-test (docs/design/TOURNAMENT_SELFTEST.md):
# a local test bridge, the host game on the other PC and the guest game here,
# each run by run-selftest.ps1 from a build made with
# scripts\build-current.ps1 -SelfTest. The bridge side
# (sdk/typescript/tools/selftest-bridge.ts) links both games, creates the
# match and checks its signed result. Prints every side's PASS and FAIL
# lines; exit code 0 when all passed, 1 when one failed.
#
# The other PC is reached with ssh (key sign-in, default shell PowerShell) and
# starts its game through a scheduled task that runs in the signed-in user's
# session: a process started over ssh has no desktop. The task's contract is
# in the design document. The game has to be closed and Steam signed in on both
# PCs. Needs Node.js 22.18 or later here, and a free -Port and -Port + 1 on
# this PC and -Port on the other.
#
# -Site challonge plays a real Challonge tournament instead of the bridge's
# mock provider (docs/design/TOURNAMENT_SELFTEST.md, "Challonge mode"): each
# game connects its Challonge account in the browser of its PC, where a person
# signs in and approves (five minutes each), and the ember-brackets connector
# makes the match from the tournament's one open set and reports its score.
# -ChallongeConfig is the local run configuration, kept outside the
# repository, and -ConnectorExe is ember-brackets.exe built from the same
# checkout as the bridge.
param(
    # This PC's package, the guest's.
    [Parameter(Mandatory)] [string]$PackageDir,
    # ember-bridge.exe built from the same checkout.
    [Parameter(Mandatory)] [string]$BridgeExe,
    # The other PC: its ssh name and the checkout there, with this branch and
    # its self-test build staged in build\current\stage.
    [Parameter(Mandatory)] [string]$RemoteHost,
    [Parameter(Mandatory)] [string]$RemoteCheckout,
    # The scheduled task, and the folder it runs job.cmd from; the runner's
    # own job, log and exchange files go there too.
    [string]$RemoteTask = "Ember Interactive",
    [Parameter(Mandatory)] [string]$RemoteJobs,
    [int]$Port = 18787,
    [int]$TimeoutMinutes = 30,
    [ValidateSet("mock", "challonge")] [string]$Site = "mock",
    [string]$ChallongeConfig = "",
    [string]$ConnectorExe = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot "SelfTestProcesses.ps1")

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

# The lines a side printed, labelled with the side.
function Get-Steps([string]$Side, [string]$Log) {
    if (-not (Test-Path $Log)) { return @("FAIL $($Side): no output") }
    @(Get-Content $Log | Where-Object { $_ -match '^(PASS|FAIL|RESULT) ' } | ForEach-Object { "$Side $_" })
}

if (Get-Process SSFIV -ErrorAction SilentlyContinue) { throw "Close Ultra Street Fighter IV first." }
if (-not (Test-Path $BridgeExe)) { throw "ember-bridge.exe not found at $BridgeExe" }
# The bridge's connection the games link on, and the arguments that change for
# a sign-in site: the bridge side's, the host job's and the guest's.
$connection = "mock-local"
$siteBridgeArguments = @()
$siteGameArguments = @()
if ($Site -eq "challonge") {
    if (-not $ChallongeConfig -or -not (Test-Path $ChallongeConfig -PathType Leaf)) { throw "-ChallongeConfig needs the local run configuration file." }
    if (-not $ConnectorExe -or -not (Test-Path $ConnectorExe -PathType Leaf)) { throw "-ConnectorExe needs ember-brackets.exe." }
    $connection = "brackets"
    $siteBridgeArguments = @("--site", "challonge", "--challonge", "`"$((Resolve-Path $ChallongeConfig).Path)`"", "--connector", "`"$((Resolve-Path $ConnectorExe).Path)`"", "--connection", $connection)
    $siteGameArguments = @("-Site", "challonge")
}
elseif ($ChallongeConfig -or $ConnectorExe) { throw "-ChallongeConfig and -ConnectorExe are for -Site challonge." }
if (-not (Test-PortFree $Port) -or -not (Test-PortFree ($Port + 1))) { throw "Port $Port or $($Port + 1) is in use on this PC." }
# The remote paths go into single-quoted strings and a batch line.
foreach ($path in $RemoteCheckout, $RemoteJobs) { if ($path -match "['`"%]") { throw "The remote path $path has a quote or percent sign in it." } }
$remoteExchange = "$RemoteJobs\tourney-exchange"
$remoteLog = "$RemoteJobs\tourney-host.log"
$remotePid = "$RemoteJobs\tourney-host.pid"
$remoteJob = "$RemoteJobs\tourney-host.cmd"
$remoteState = Invoke-Remote @"
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) { 'port-busy' }
if (Get-Process SSFIV -ErrorAction SilentlyContinue) { 'game-running' }
if (-not (Test-Path '$RemoteCheckout\build\current\stage\Launcher.exe')) { 'no-package' }
if (-not (Test-Path '$RemoteCheckout\scripts\run-tournament-selftest-host.ps1')) { 'no-host-script' }
if (-not (Get-Command pwsh -ErrorAction SilentlyContinue)) { 'no-pwsh' }
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
$coordinator = $null; $guest = $null; $hostStarted = $false; $hostDone = $false; $timedOut = $false; $runnerError = $null

try {
    # The bridge side first: its bridge, result receiver and tunnel are up
    # when it prints READY. Its own deadline is later than the games' (and the
    # runner's), so it never tears the bridge down under a game still inside
    # its timeout.
    $node = (Get-Command node).Source
    $coordinator = Start-Process -FilePath $node -PassThru -NoNewWindow -RedirectStandardOutput $coordinatorLog -RedirectStandardError "$coordinatorLog.err" -WorkingDirectory "$repo\sdk\typescript" -ArgumentList (@(
        "tools/selftest-bridge.ts", "--bridge", "`"$BridgeExe`"", "--work", "`"$bridgeWork`"",
        "--host", "`"ssh:$($RemoteHost):$remoteExchange`"", "--guest", "`"$guestExchange`"",
        "--port", $Port, "--minutes", ($TimeoutMinutes + 5)) + $siteBridgeArguments)
    $ready = (Get-Date).AddMinutes(2)
    while (-not ((Test-Path $coordinatorLog) -and (Select-String -Path $coordinatorLog -Pattern '^READY$' -Quiet))) {
        if ($coordinator.HasExited -or (Get-Date) -gt $ready) { throw "The bridge side did not start: $(Get-Content $coordinatorLog, "$coordinatorLog.err" -ErrorAction SilentlyContinue)" }
        Start-Sleep -Seconds 1
    }
    Write-Host "bridge side ready"

    # The host, through the other PC's task: a job of its own, its own log,
    # started in the same call that sees the task free. The host script is
    # this checkout's, there; the line it ends the log with says it is done.
    $started = Invoke-Remote @"
if ((schtasks /query /tn '$RemoteTask' /fo csv /nh) -notmatch '"Ready"') { 'busy'; return }
Remove-Item '$remoteExchange' -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path '$remoteExchange' -Force | Out-Null
Remove-Item '$remoteLog','$remotePid' -ErrorAction SilentlyContinue
`$pwsh = (Get-Command pwsh).Source
Set-Content -LiteralPath '$remoteJob' -Encoding ASCII -Value ('@"{0}" -NoProfile -ExecutionPolicy Bypass -File "$RemoteCheckout\scripts\run-tournament-selftest-host.ps1" -PackageDir "$RemoteCheckout\build\current\stage" -Exchange "$remoteExchange" -Bridge "$origin" -PidFile "$remotePid" -Connection "$connection" $($siteGameArguments -join ' ') -TimeoutMinutes $TimeoutMinutes > "$remoteLog" 2>&1' -f `$pwsh)
Copy-Item '$remoteJob' '$RemoteJobs\job.cmd' -Force
schtasks /run /tn '$RemoteTask' | Out-Null
'started'
"@
    if ($started -notcontains "started") { throw "The task '$RemoteTask' on $RemoteHost is busy; nothing was started there." }
    $hostStarted = $true
    Write-Host "host started on $RemoteHost"

    # The guest, here.
    $pwsh = (Get-Process -Id $PID).Path
    $guest = Start-Process -FilePath $pwsh -PassThru -NoNewWindow -RedirectStandardOutput $guestLog -RedirectStandardError "$guestLog.err" -ArgumentList (@(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSScriptRoot\run-selftest.ps1`"", "-PackageDir", "`"$PackageDir`"",
        "-Test", "tournament-guest", "-Exchange", "`"$guestExchange`"", "-Bridge", $origin, "-Connection", $connection,
        "-TimeoutMinutes", $TimeoutMinutes) + $siteGameArguments)
    Write-Host "guest started here"
    if ($Site -eq "challonge") { Write-Host "A browser opens on each PC: sign in to Challonge as that PC's player and approve, within five minutes." }

    # Both games close by themselves, and the host's job says when it is done:
    # its log ends with a line, or its task is Ready again after its PID file
    # was written (a job that died). A failed ssh call is tried again.
    $hostStatus = @"
if ((Test-Path '$remoteLog') -and (Select-String -LiteralPath '$remoteLog' -Pattern '^tourney-host done`$' -Quiet)) { 'done' }
elseif ((Test-Path '$remotePid') -and ((schtasks /query /tn '$RemoteTask' /fo csv /nh) -match '"Ready"')) { 'done' }
"@
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes + 2)
    $failedPolls = 0
    while (-not ($guest.HasExited -and $hostDone)) {
        if ((Get-Date) -gt $deadline) { $timedOut = $true; break }
        Start-Sleep -Seconds 10
        if ($hostDone) { continue }
        try { $hostDone = (Invoke-Remote $hostStatus) -contains "done"; $failedPolls = 0 }
        catch {
            if (++$failedPolls -ge 6) { throw "The host could not be asked six times in a row: $_" }
            Write-Host "The host could not be asked ($failedPolls): $_"
        }
    }
}
catch { $runnerError = $_.Exception.Message }
finally {
    # A game still running is ended, not its wrapper: the wrapper then puts
    # back the settings, saves and Ember folder it changed. A wrapper that has
    # not finished after two minutes is ended too, and says what it left.
    if ($guest -and -not $guest.HasExited) {
        Stop-Descendants $guest.Id
        if (-not $guest.WaitForExit(120000)) {
            Write-Host "The guest's script did not finish; settings.json.selftest and ui-preferences.json.selftest in %APPDATA%\sf4e may need putting back."
            Stop-Process -Id $guest.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($hostStarted -and -not $hostDone) {
        try {
            Invoke-Remote @"
. '$RemoteCheckout\scripts\SelfTestProcesses.ps1'
if (Test-Path '$remotePid') { Stop-Descendants ([int](Get-Content '$remotePid')) }
foreach (`$try in 1..24) {
    if ((Test-Path '$remoteLog') -and (Select-String -LiteralPath '$remoteLog' -Pattern '^tourney-host done`$' -Quiet)) { break }
    Start-Sleep -Seconds 5
}
"@ | Out-Null
        }
        catch { Write-Host "The host's game could not be ended: $_" }
    }
    # Nothing more comes from the games: the bridge side checks the match,
    # stops its bridge, receiver and tunnel and deletes its folder.
    if ($coordinator) {
        New-Item -ItemType File -Path (Join-Path $bridgeWork "stop") -Force | Out-Null
        if (-not $coordinator.WaitForExit(90000)) {
            # Its children: the bridge and the tunnel it started.
            Stop-Descendants $coordinator.Id
            Stop-Process -Id $coordinator.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($hostStarted) {
        try { Invoke-Remote "Get-Content -LiteralPath '$remoteLog' -ErrorAction SilentlyContinue; Remove-Item '$remoteExchange' -Recurse -Force -ErrorAction SilentlyContinue; Remove-Item '$remoteLog','$remotePid','$remoteJob' -ErrorAction SilentlyContinue" | Set-Content $hostLog }
        catch { Write-Host "The host's log could not be read: $_" }
    }
}

$steps = @(Get-Steps "host" $hostLog) + @(Get-Steps "guest" $guestLog) + @(Get-Steps "bridge" $coordinatorLog)
if ($timedOut) { $steps += "runner FAIL timeout: the games were still running after $TimeoutMinutes minutes" }
if ($runnerError) { $steps += "runner FAIL $runnerError" }
$steps | Write-Host
$passed = ($steps -match '^\w+ RESULT pass$').Count -eq 3 -and -not ($steps -match '^\w+ (FAIL|RESULT fail)')
# The logs stay for a failed run.
if ($passed) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
else { Write-Host "Logs: $work (the host's sf4e.log lines are in host.log)" }
Write-Host $(if ($passed) { "RESULT pass" } else { "RESULT fail" })
exit $(if ($passed) { 0 } else { 1 })
