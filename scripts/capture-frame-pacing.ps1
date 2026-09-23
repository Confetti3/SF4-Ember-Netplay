# Captures PresentMon frame timing for SSFIV.exe and writes a receipt. With
# -CsvPath, validates an existing capture instead (DurationSeconds is then the
# length that capture was meant to cover).
param([string]$PresentMonPath,
      [string]$CsvPath,
      [ValidateRange(10,3600)][int]$DurationSeconds=300,
      [string]$ProcessName='SSFIV.exe',
      [string]$Label='capture',
      [string]$OutputDirectory='build/current/performance')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if([bool]$PresentMonPath -eq [bool]$CsvPath){throw 'Pass either -PresentMonPath to capture or -CsvPath to validate an existing capture.'}
if($Label -notmatch '^[a-zA-Z0-9._-]+$'){throw 'Label may contain only letters, numbers, dot, underscore and dash.'}
if($CsvPath){
    $csv=(Resolve-Path -LiteralPath $CsvPath).Path
} else {
    $presentMon=(Resolve-Path -LiteralPath $PresentMonPath).Path
    $outputRoot=if([IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory}else{Join-Path $repo $OutputDirectory}
    $null=[IO.Directory]::CreateDirectory($outputRoot)
    $stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
    $csv=Join-Path $outputRoot "$Label-$stamp.csv"
    & $presentMon --process_name $ProcessName --timed $DurationSeconds --terminate_after_timed --output_file $csv --qpc_time_ms --no_console_stats
    if($LASTEXITCODE -ne 0){throw "PresentMon failed with exit code $LASTEXITCODE"}
    if(!(Test-Path -LiteralPath $csv -PathType Leaf)){throw 'PresentMon did not create a CSV capture.'}
}
$rows=@(Import-Csv -LiteralPath $csv)
if($rows.Count -lt 30){throw "Capture has only $($rows.Count) presentation samples."}
$columns=@($rows[0].PSObject.Properties.Name)
# Prefer millisecond timestamps: only they let the span check below run.
$qpcName=@('CPUStartQPCTime','CPUStartQPC')|Where-Object{$columns -contains $_}|Select-Object -First 1
if(!$qpcName){throw "Capture is missing absolute QPC timestamps. Columns: $($columns -join ', ')"}
if($columns -contains 'Application'){$rows=@($rows|Where-Object{$_.Application -ieq $ProcessName})}
if($columns -contains 'ProcessID' -and $columns -contains 'SwapChainAddress') {
    # Multiple instances/swap chains must never be blended into one percentile.
    $stream=$rows|Group-Object ProcessID,SwapChainAddress|Sort-Object Count -Descending|Select-Object -First 1
    $rows=@($stream.Group)
}
$intervals=@();$intervalName='';$displayAvailable=$false;$coveredSeconds=0.0;$spanSeconds=$null;$coverageFailures=@();$MinimumCoverage=0.9
foreach($candidate in @('DisplayedTime','MsBetweenDisplayChange','MsBetweenPresents')) {
    if($columns -notcontains $candidate){continue}
    $samples=@($rows|ForEach-Object{ $value=0.0;$qpc=0.0
        if([double]::TryParse([string]$_.$qpcName,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$qpc)-and$qpc-gt 0-and
           [double]::TryParse([string]$_.$candidate,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value)-and$value-gt 0-and ![double]::IsInfinity($value)){[pscustomobject]@{qpc=$qpc;value=$value}}
    })
    if($samples.Count-lt 30){continue}
    $usable=@($samples|ForEach-Object{$_.value}|Sort-Object)
    # The usable intervals must span the capture, or a long capture with a few
    # displayed frames would sign off pacing it never measured (ledger H-012).
    # Both their sum and, with millisecond timestamps, the clock span they were
    # taken over must cover it.
    $covered=($usable|Measure-Object -Sum).Sum/1000.0
    if($covered-lt $MinimumCoverage*$DurationSeconds){$coverageFailures+="$candidate covers $([Math]::Round($covered,1)) of $DurationSeconds s";continue}
    if($qpcName-eq'CPUStartQPCTime'){
        $qpcs=$samples|ForEach-Object{$_.qpc}|Measure-Object -Minimum -Maximum
        $span=($qpcs.Maximum-$qpcs.Minimum)/1000.0
        if($span-lt $MinimumCoverage*$DurationSeconds){$coverageFailures+="$candidate timestamps span $([Math]::Round($span,1)) of $DurationSeconds s";continue}
        $spanSeconds=$span
    }
    $intervals=$usable;$intervalName=$candidate;$displayAvailable=$candidate-ne'MsBetweenPresents';$coveredSeconds=$covered;break
}
if($intervals.Count -lt 30){throw ("Capture does not contain enough usable display or presentation intervals. "+($coverageFailures -join '; '))}
function Percentile([double[]]$Values,[double]$P){$Values[[Math]::Min($Values.Count-1,[Math]::Floor(($Values.Count-1)*$P))]}
$receipt=[ordered]@{
 schema=1;capture=$csv;process=$ProcessName;label=$Label;capturedUtc=[DateTime]::UtcNow.ToString('o');samples=$intervals.Count
 intervalColumn=$intervalName;qpcColumn=$qpcName;durationSeconds=$DurationSeconds;coveredSeconds=[Math]::Round($coveredSeconds,1);timestampSpanSeconds=if($null-ne$spanSeconds){[Math]::Round($spanSeconds,1)}else{'unchecked (raw QPC column)'};rowsWithoutInterval=$rows.Count-$intervals.Count;p50Ms=Percentile $intervals .50;p95Ms=Percentile $intervals .95
 displayMetricsAvailable=$displayAvailable;displayMetricsStatus=if($displayAvailable){'available'}else{'unavailable; presentation intervals only'}
 processId=if($rows.Count-and$columns-contains'ProcessID'){$rows[0].ProcessID}else{$null}
 swapChain=if($rows.Count-and$columns-contains'SwapChainAddress'){$rows[0].SwapChainAddress}else{$null}
 p99Ms=Percentile $intervals .99;over25Ms=@($intervals|Where-Object{$_-gt 25}).Count;over50Ms=@($intervals|Where-Object{$_-gt 50}).Count
 sourceRevision=(& git -C $repo rev-parse HEAD);sourceDirty=[bool](& git -C $repo status --porcelain)
}
$receiptPath=[IO.Path]::ChangeExtension($csv,'.json')
$receipt|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $receiptPath -Encoding UTF8
Write-Host "Frame capture: $csv"
Write-Host "Receipt: $receiptPath"
Write-Host ("samples={0} p50={1:N2}ms p95={2:N2}ms p99={3:N2}ms over25={4} over50={5}" -f $receipt.samples,$receipt.p50Ms,$receipt.p95Ms,$receipt.p99Ms,$receipt.over25Ms,$receipt.over50Ms)
