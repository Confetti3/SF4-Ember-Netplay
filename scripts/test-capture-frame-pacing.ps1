# Ledger H-012: the capture validator must refuse a capture whose usable
# intervals cover only a small part of its intended length.
$ErrorActionPreference='Stop'
$script=Join-Path $PSScriptRoot 'capture-frame-pacing.ps1'
$root=Join-Path ([IO.Path]::GetTempPath()) ('ember-frame-capture-test-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root -Force
function WriteCapture([string]$Path,[int]$Frames,[double]$IntervalMs){
    $lines=@('Application,ProcessID,SwapChainAddress,CPUStartQPC,MsBetweenDisplayChange')
    for($i=0;$i -lt $Frames;$i++){ $lines+=('SSFIV.exe,42,0x1,{0},{1}' -f (1000+$i*$IntervalMs),$IntervalMs) }
    Set-Content -LiteralPath $Path -Encoding ASCII -Value $lines
}
try {
    # 60 s at 60 fps: 3600 displayed intervals cover the whole capture.
    $full=Join-Path $root 'full.csv'; WriteCapture $full 3600 16.667
    & $script -CsvPath $full -DurationSeconds 60 | Out-Null
    $receipt=Get-Content -LiteralPath ([IO.Path]::ChangeExtension($full,'.json')) -Raw | ConvertFrom-Json
    if($receipt.coveredSeconds -lt 59 -or $receipt.durationSeconds -ne 60){throw "Full capture receipt is wrong: covered=$($receipt.coveredSeconds)"}

    # A nominal 300 s capture with 40 displayed frames must be refused.
    $sparse=Join-Path $root 'sparse.csv'; WriteCapture $sparse 40 16.667
    $refused=$false
    try { & $script -CsvPath $sparse -DurationSeconds 300 | Out-Null } catch { $refused=$_.Exception.Message -match 'covers' }
    if(!$refused){throw 'A capture covering under 1 s of 300 s was accepted.'}
    Write-Host 'Frame capture coverage validation passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
