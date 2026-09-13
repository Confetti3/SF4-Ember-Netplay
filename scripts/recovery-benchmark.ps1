[CmdletBinding()]
param(
    [string]$Harness = (Join-Path $PSScriptRoot '..\build\current\RecoveryBenchmark.exe'),
    [string]$BaselineHelper = (Join-Path $PSScriptRoot '..\build\recovery-baseline-observable-target-static\x86_64-pc-windows-msvc\release\sf4-net.exe'),
    [string]$CandidateHelper = (Join-Path $PSScriptRoot '..\build\current\sf4-net.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\recovery-benchmark-results'),
    [UInt32]$Seed = 0x5f4e2026,
    [int]$Frames = 240,
    [int]$DropEvery = 17,
    [int]$DelayMs = 2,
    [int]$TimeoutMs = 30000,
    [string]$BaselineGgpoDll = '',
    [string]$CandidateGgpoDll = '',
    [switch]$RelayOnly,
    [switch]$SkipNative,
    [switch]$SkipGgpo
)

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
if (-not $SkipNative -and $Frames -lt 240) {
    throw 'A comparative workload requires at least 240 frames. Run the harness directly for short diagnostic checks.'
}
function Resolve-BenchmarkPath([string]$value) {
    if ([IO.Path]::IsPathRooted($value)) { return [IO.Path]::GetFullPath($value) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $value))
}
$harnessPath = Resolve-BenchmarkPath $Harness
$baselinePath = Resolve-BenchmarkPath $BaselineHelper
$candidatePath = Resolve-BenchmarkPath $CandidateHelper
$resultDirectory = Resolve-BenchmarkPath $OutputDirectory

function Get-Artifact([string]$path, [string]$label) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "$label was not found: $path"
    }
    $item = Get-Item -LiteralPath $path
    $resolved = $item.FullName
    $hash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    return [ordered]@{
        path = $resolved
        sha256 = $hash
        length_bytes = [int64]$item.Length
    }
}

function Get-HelperArtifacts([string]$path, [string]$label, [string]$explicitDll) {
    $helper = Get-Artifact $path "$label helper"
    $dllPath = if (-not [string]::IsNullOrWhiteSpace($explicitDll)) {
        Resolve-BenchmarkPath $explicitDll
    } else {
        $adjacent = Join-Path ([IO.Path]::GetDirectoryName($helper.path)) 'GGPO.dll'
        $current = Join-Path $repoRoot 'build\current\GGPO.dll'
        $vcpkg = Join-Path $repoRoot 'build\current\dependencies\x86-windows-wchar-filenames\bin\GGPO.dll'
        @($adjacent, $current, $vcpkg) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
        throw "$label helper has no resolvable GGPO.dll; pass -${label}GgpoDll or place the runtime DLL beside the helper"
    }
    return [ordered]@{
        helper = $helper
        ggpo_dll = Get-Artifact $dllPath "$label GGPO.dll"
    }
}

if (-not (Test-Path -LiteralPath $harnessPath -PathType Leaf)) {
    throw "Benchmark harness was not found: $harnessPath. Build the root-owned recovery benchmark target first."
}
if (-not $SkipNative -and -not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) { throw "Baseline helper was not found: $baselinePath" }
if (-not $SkipNative -and -not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) { throw "Candidate helper was not found: $candidatePath" }
New-Item -ItemType Directory -Force -Path $resultDirectory | Out-Null
$harnessArtifact = Get-Artifact $harnessPath 'Benchmark harness'
$baselineArtifacts = if ($SkipNative) { $null } else { Get-HelperArtifacts $baselinePath 'Baseline' $BaselineGgpoDll }
$candidateArtifacts = if ($SkipNative) { $null } else { Get-HelperArtifacts $candidatePath 'Candidate' $CandidateGgpoDll }

function Invoke-Benchmark([string]$label, [string]$helper, [string]$outputPath, [object]$artifacts) {
    # The executable imports GGPO.dll at process startup. Isolate each runtime
    # beside its selected DLL, so an explicit artifact choice controls loading.
    $runtimeDirectory = Join-Path $resultDirectory "$label-$stamp-runtime"
    if (Test-Path -LiteralPath $runtimeDirectory) { throw "Runtime path already exists: $runtimeDirectory" }
    New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null
    $runtimeHarness = Join-Path $runtimeDirectory 'recovery-benchmark.exe'
    Copy-Item -LiteralPath $harnessPath -Destination $runtimeHarness
    if ($null -ne $artifacts) {
        $runtimeDll = Join-Path $runtimeDirectory 'GGPO.dll'
        Copy-Item -LiteralPath $artifacts.ggpo_dll.path -Destination $runtimeDll
        $artifacts['loaded_ggpo_dll'] = Get-Artifact $runtimeDll "$label staged GGPO.dll"
        if ($artifacts.loaded_ggpo_dll.sha256 -ne $artifacts.ggpo_dll.sha256) { throw "$label DLL staging hash mismatch" }
    } else {
        # The optional loopback diagnostic still uses the executable's import.
        $diagnosticArtifacts = Get-HelperArtifacts $harnessPath 'Diagnostic' $CandidateGgpoDll
        Copy-Item -LiteralPath $diagnosticArtifacts.ggpo_dll.path -Destination (Join-Path $runtimeDirectory 'GGPO.dll')
    }
    $arguments = @(
        "--seed=$Seed", "--frames=$Frames", "--drop-every=$DropEvery", "--delay-ms=$DelayMs",
        "--timeout-ms=$TimeoutMs", "--output=$outputPath"
    )
    if ($SkipNative) { $arguments += '--skip-native' } else { $arguments += "--helper=$helper" }
    if ($SkipGgpo) { $arguments += '--skip-ggpo' }
    if ($RelayOnly) { $arguments += '--relay-only' }
    & $runtimeHarness @arguments *> "$outputPath.process.log"
    if ($LASTEXITCODE -ne 0) { throw "$label benchmark exited with code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) { throw "$label benchmark did not produce $outputPath" }
    if ((Get-Item -LiteralPath $outputPath).Length -eq 0) { throw "$label benchmark produced an empty result: $outputPath" }
    try { return (Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "$label benchmark result was not valid JSON: $outputPath : $($_.Exception.Message)" }
}

function Assert-NativeMeasurement($result, [string]$label) {
    if ($null -eq $result.native) { throw "$label has no native phase" }
    if ($result.native.measured -ne $true -or $result.native.phase_status -ne 'complete') {
        throw "$label native phase was not complete/measured: $($result.native.error)"
    }
    if ($result.native.wire_proxy.measured -ne $true -or $result.native.helper_statistics_fresh -ne $true -or
        $result.native.helper_statistics_matched -ne $true) {
        throw "$label native wire/statistics evidence was incomplete"
    }
    if ($result.native.teardown.measured -ne $true -or $result.native.runtime_errors.Count -ne 0 -or
        [int64]$result.native.disconnected_events -ne 0) {
        throw "$label native teardown/runtime evidence was incomplete"
    }
    if ($result.native.resources.measured -ne $true) { throw "$label native interval resources were not measured" }
    if ($result.native.confirmation.measured -ne $true -or $result.native.state_checksums.matches_oracle -ne $true -or
        $result.native.forwarding_cost.measured -ne $true) {
        throw "$label did not confirm the final input/state boundary or measure forwarding cost"
    }
    if ($result.native.route_measured -ne $true -or $result.native.route_stable -ne $true -or
        $result.native.route_comparable -ne $true -or $null -eq $result.native.selected_route -or
        [string]::IsNullOrWhiteSpace([string]$result.native.selected_route.host) -or
        [string]::IsNullOrWhiteSpace([string]$result.native.selected_route.guest)) {
        throw "$label native phase did not expose unchanged periodic selected-path observations"
    }
    $target = [int64]$result.frames
    if ([int64]$result.native.accepted_frame_counts.host -lt $target -or
        [int64]$result.native.accepted_frame_counts.guest -lt $target) {
        throw "$label native accepted-frame counts did not reach the requested workload"
    }
    if ($result.native.state_checksums.available -ne $true -or
        $result.native.state_checksums.initial_equal -ne $true -or
        $result.native.state_checksums.final_equal -ne $true) {
        throw "$label native GGPO initial/final state checksum equality was unavailable or failed"
    }
    if ($result.native.ggpo_dll.loaded -ne $true -or [string]::IsNullOrWhiteSpace([string]$result.native.ggpo_dll.path) -or
        [string]::IsNullOrWhiteSpace([string]$result.native.ggpo_dll.sha256)) {
        throw "$label did not report the loaded GGPO.dll path and SHA256"
    }
}

function Assert-LoadedGgpo([object]$result, [object]$artifact, [string]$label) {
    $loadedPath = [IO.Path]::GetFullPath([string]$result.native.ggpo_dll.path)
    $selectedPath = [IO.Path]::GetFullPath([string]$artifact.loaded_ggpo_dll.path)
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($loadedPath, $selectedPath) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals([string]$result.native.ggpo_dll.sha256, [string]$artifact.ggpo_dll.sha256)) {
        throw "$label loaded GGPO.dll does not match the selected artifact: loaded=$loadedPath selected=$selectedPath"
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$baselineResultPath = Join-Path $resultDirectory "baseline-$stamp.json"
$candidateResultPath = Join-Path $resultDirectory "candidate-$stamp.json"
$baseline = Invoke-Benchmark 'baseline' $baselinePath $baselineResultPath $baselineArtifacts
$candidate = Invoke-Benchmark 'candidate' $candidatePath $candidateResultPath $candidateArtifacts
if (-not $SkipNative) {
    Assert-NativeMeasurement $baseline 'Baseline'
    Assert-NativeMeasurement $candidate 'Candidate'
    Assert-LoadedGgpo $baseline $baselineArtifacts 'Baseline'
    Assert-LoadedGgpo $candidate $candidateArtifacts 'Candidate'
    if ($baselineArtifacts.ggpo_dll.sha256 -ne $candidateArtifacts.ggpo_dll.sha256) {
        throw 'GGPO artifacts differ; a helper-only performance comparison requires the same GGPO build.'
    }
    foreach ($side in @('host_class', 'guest_class')) {
        if ([string]$baseline.native.selected_route.$side -cne [string]$candidate.native.selected_route.$side) {
            throw "Selected paths differ between baseline and candidate ($side); results are preserved, but no comparative deltas will be published."
        }
    }
}

function Get-Number($object, [string]$path) {
    $current = $object
    $parts = $path -split '\.'
    $phase = $parts[0]
    if ($null -eq $object.$phase -or $object.$phase.measured -ne $true) { return $null }
    foreach ($part in $parts) {
        if ($null -eq $current -or $null -eq $current.$part) { return $null }
        $current = $current.$part
    }
    if ($current -is [ValueType] -and $current -isnot [bool]) { return [double]$current }
    return $null
}

$metricPaths = @(
    'native.ggpo_rtt_estimate.p50_us', 'native.ggpo_rtt_estimate.p95_us', 'native.ggpo_rtt_estimate.p99_us',
    'native.ggpo_rtt_estimate.max_us', 'native.resources.helper_cpu_ms', 'native.resources.working_set_growth_bytes',
    'native.resources.sampled_peak_working_set_bytes', 'native.forwarding_cost.helper_cpu_us_per_operation',
    'native.prediction_stalls', 'native.replay_loads',
    'native.max_replay_depth', 'native.not_synchronized', 'native.frames_advanced'
)
$delta = [ordered]@{}
foreach ($path in $metricPaths) {
    $before = Get-Number $baseline $path
    $after = Get-Number $candidate $path
    $delta[$path] = [ordered]@{
        baseline = $before
        candidate = $after
        delta = if ($null -ne $before -and $null -ne $after) { $after - $before } else { $null }
        measured = ($null -ne $before -and $null -ne $after)
    }
}

$comparison = [ordered]@{
    schema = 'sf4.recovery-benchmark-comparison.v2'
    generated_at = [DateTime]::UtcNow.ToString('o')
    baseline_source_snapshot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\recovery-baseline-observable'))
    baseline_original_source_snapshot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\repair-baseline-20260909-152753\source'))
    baseline_package = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist\sf4-netplay-launcher-ember-current-postmatch-ultra-20260908'))
    candidate_source = $repoRoot
    harness = $harnessArtifact
    baseline_artifacts = $baselineArtifacts
    candidate_artifacts = $candidateArtifacts
    seed = $Seed
    frames = $Frames
    schedule = [ordered]@{ seed = $Seed; frames = $Frames; application_delay_ms = $DelayMs; application_drop_every = $DropEvery; native_packet_proxy = $true; synthetic_ggpo_packet_proxy = (-not $SkipGgpo); relay_only = [bool]$RelayOnly; pacing_interval_us = 16667 }
    baseline_result = $baselineResultPath
    candidate_result = $candidateResultPath
    metrics = $delta
    interpretation = 'Real GGPO datagrams cross each selected helper path through a seeded per-direction proxy. Both peers must confirm the final input frame and match an independent input-driven state oracle. Byte/packet counters reconcile both proxy/helper boundaries before and after work. CPU brackets forward stepping, confirmation and bounded drain, excluding startup and the final statistics wait. Forwarding cost is total helper CPU amortized per successful enqueue/local delivery, including coordination overhead; it is not isolated bridge latency. GGPO RTT is a cached estimate. Periodic path samples must remain unchanged and baseline/candidate paths must match after removing ephemeral UDP ports; changes between samples are not observable. Working-set peak is sampled, while OS lifetime peaks are separately labeled. This synthetic callback workload does not launch SF4. Exact hashes identify helpers, staged GGPO DLLs, and the harness.'
}
$comparisonPath = Join-Path $resultDirectory "comparison-$stamp.json"
$comparison | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $comparisonPath -Encoding UTF8
$comparison | ConvertTo-Json -Depth 12
Write-Host "Wrote $comparisonPath"
