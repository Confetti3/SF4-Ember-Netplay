# Repeatable current-build measurement; no historical baseline installation required.
[CmdletBinding()]
param(
    [ValidateRange(240,36000)][int]$Frames = 1800,
    [switch]$RelayOnly,
    [switch]$ConnectionCheck,
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
$helper = Join-Path $build 'sf4-net.exe'
$harness = Join-Path $build 'RecoveryBenchmark.exe'
$ggpo = Join-Path $build 'GGPO.dll'
$check = Join-Path $build 'IrohAuthorizedMatchTest.exe'
$paths = @($helper,$harness,$ggpo)
if ($ConnectionCheck) { $paths += $check }
$hashes = @($paths | ForEach-Object {
    [pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}
})
if (!$OutputDirectory) { $OutputDirectory=Join-Path $build ('network-benchmark-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh benchmark output directory.' }
$directory=(New-Item -ItemType Directory -Path $OutputDirectory).FullName
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'artifacts.json')
$output=Join-Path $directory 'ggpo.json'
$arguments=@("--helper=$helper","--frames=$Frames","--timeout-ms=$([Math]::Max(90000,$Frames*40))","--output=$output",'--skip-ggpo')
if ($RelayOnly) { $arguments+='--relay-only' }
$nativePreference=$ErrorActionPreference
try {
    $ErrorActionPreference='Continue'
    & $harness @arguments *> (Join-Path $directory 'harness.log')
    $exitCode=$LASTEXITCODE
} finally { $ErrorActionPreference=$nativePreference }
if ($exitCode) { throw "GGPO benchmark failed; results retained at $directory" }
$result=Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
if (!$result.native.measured -or $result.native.error -or
    !$result.native.state_checksums.final_equal -or
    $result.native.confirmation.host_input_frame -ne ($Frames-1) -or
    $result.native.confirmation.guest_input_frame -ne ($Frames-1) -or
    $result.native.accepted_frame_counts.host -ne $Frames -or
    $result.native.accepted_frame_counts.guest -ne $Frames -or
    $result.native.ggpo_dll.sha256 -ine $hashes[2].sha256) {
    throw "GGPO benchmark did not pass its measurement gates; see $directory"
}
if ($ConnectionCheck) {
    $arguments=@($helper,'--benchmark')
    if ($RelayOnly) { $arguments+='--relay-only' }
    try {
        $ErrorActionPreference='Continue'
        & $check @arguments *> (Join-Path $directory 'connection-check.log')
        $exitCode=$LASTEXITCODE
    } finally { $ErrorActionPreference=$nativePreference }
    if ($exitCode -or !(Select-String -LiteralPath (Join-Path $directory 'connection-check.log') -SimpleMatch -Quiet -Pattern 'Four participants, three fresh authorized GGPO matches')) { throw "Connection-check/match lifecycle failed; see $directory" }
}
foreach ($artifact in $hashes) {
    if ((Get-FileHash -LiteralPath $artifact.path).Hash -cne $artifact.sha256) { throw 'Benchmark artifacts changed during measurement.' }
}
[pscustomobject]@{
    passed=$true; framesPerPlayer=$Frames; forcedRelay=[bool]$RelayOnly
    connectionBenchmark=[bool]$ConnectionCheck; outputDirectory=$directory
    predictionStalls=$result.native.prediction_stalls
    maximumRollbackFrames=$result.native.max_replay_depth
    evidence='Local helpers and synthetic GGPO state; not native SF4 gameplay or remote-network acceptance.'
}
