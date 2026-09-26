# Explicit public-network acceptance for the helper/transport, never SF4.
param(
    [switch]$IncludeLargeRooms,
    [ValidateSet('room','rejoin','game','authorized','recovery','queue-acks','terminal-recovery','four-tables','spectators')]
    [string[]]$Cases = @(),
    [ValidateSet('default','relay')][ValidateCount(1,2)][string[]]$Routes = @('default','relay'),
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
$helper = Join-Path $build 'sf4-net.exe'
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $build ('network-tests-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "Run directory already exists: $OutputDirectory" }
$OutputDirectory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$plan = @(
    @{name='room';executable='IrohRoomIntegrationTest';arguments=@()},
    @{name='rejoin';executable='IrohRoomIntegrationTest';arguments=@('--rejoin')},
    @{name='game';executable='IrohGameIntegrationTest';arguments=@()},
    @{name='authorized';executable='IrohAuthorizedMatchTest';arguments=@()},
    @{name='recovery';executable='IrohRecoveryIntegrationTest';arguments=@()},
    @{name='queue-acks';executable='IrohRoomIntegrationTest';arguments=@('--queue-acks')},
    @{name='terminal-recovery';executable='IrohAuthorizedMatchTest';arguments=@('--terminal-recovery')}
)
if ($IncludeLargeRooms -or @($Cases | Where-Object { $_ -in 'four-tables','spectators' }).Count) {
    $plan += @{name='four-tables';executable='CustomRoomGameTest';arguments=@()}
    $plan += @{name='spectators';executable='CustomRoomGameTest';arguments=@('--single-table')}
}
if ($Cases.Count) { $plan = @($plan | Where-Object { $_.name -in $Cases }) }
$artifacts = @($helper) + @($plan | ForEach-Object { Join-Path $build ($_.executable + '.exe') } | Select-Object -Unique)
$artifacts += @(Get-ChildItem -LiteralPath $build -File -Filter '*.dll' | ForEach-Object { $_.FullName })
$hashes = foreach ($artifact in $artifacts) {
    [ordered]@{path=$artifact;sha256=(Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash}
}
$hashes | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'artifacts.json')
$completionMarkers=@{
    'room'='C++ SessionClient/SessionServer over two Iroh helpers passed:'
    'rejoin'='Rejoined the same room through the same Discord ticket after four departures'
    'queue-acks'='192 identical terminal retries retained one queued action'
    'game'='C++ raw UDP bridge:'
    'authorized'='Four participants, three fresh authorized GGPO matches'
    'terminal-recovery'='Four participants, three fresh authorized GGPO matches'
    'recovery'='Helper recovery integration passed.'
    'four-tables'='six games without automatic rotation and spectator input verification passed.'
    'spectators'='six games without automatic rotation and spectator input verification passed.'
}
$results = @()
foreach ($case in $plan) {
    foreach ($route in ($Routes | Select-Object -Unique)) {
        $arguments = @($helper) + $case.arguments
        if ($route -eq 'relay') { $arguments += '--relay-only' }
        $log = Join-Path $OutputDirectory ($case.name + '-' + $route + '.log')
        $started = [DateTime]::UtcNow
        Write-Output "RUN $($case.name) ($route). Log: $log"
        # Windows PowerShell wraps redirected native stderr as ErrorRecords.
        # Stop would unwind the pipeline on the first diagnostic, before the
        # fixture can finish its failure dump or we can record its exit code.
        # Keep the native process running to completion, then enforce failure
        # using its exit status and the artifact checks below.
        $nativeErrorPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & (Join-Path $build ($case.executable + '.exe')) @arguments *> $log
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $nativeErrorPreference
        }
        $changed = @($hashes | Where-Object { (Get-FileHash -LiteralPath $_.path -Algorithm SHA256).Hash -ne $_.sha256 })
        $completed=Select-String -LiteralPath $log -SimpleMatch -Quiet -Pattern $completionMarkers[$case.name]
        $result = [pscustomobject]@{test=$case.name;executable=$case.executable;routePolicy=$route;
            exitCode=$exitCode;completed=[bool]$completed;artifactsUnchanged=($changed.Count -eq 0);startedUtc=$started.ToString('o');
            elapsedSeconds=([DateTime]::UtcNow-$started).TotalSeconds;log=$log}
        $results += $result
        $results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json')
        if ($changed.Count) { throw "Binaries changed during acceptance; see $OutputDirectory" }
        if ($exitCode -or !$completed) { throw "Failed $($case.name) ($route); see $log" }
        Write-Output "PASS $($case.name) ($route). Synthetic transport/GGPO only. Log: $log"
    }
}
