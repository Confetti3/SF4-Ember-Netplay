# Capture real Ember UI with fictional sample session data. Does not launch SF4.
param([Parameter(Mandatory=$true)][string]$GameRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
$renderer = Join-Path $build 'UiRenderTest.exe'
# The pictures show the source the renderer and the lab were built from, which the build's receipt names.
$receipt = Join-Path $build 'build-provenance.json'
if (!(Test-Path -LiteralPath $receipt)) { throw 'No build receipt next to the renderer: build with scripts/build-current.ps1 first.' }
$revision = (Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json).baseRevision
if (!$revision) { throw "$receipt names no baseRevision: build with scripts/build-current.ps1 first." }
$cwebp = (Get-Command cwebp -ErrorAction Stop).Source
if (!(Test-Path -LiteralPath (Join-Path $GameRoot 'SSFIV.exe'))) { throw 'GameRoot must be the owned Steam USF4 installation.' }
$captures = Join-Path $build ('readme-captures-' + (Get-Date).ToString('yyyyMMdd-HHmmss', [cultureinfo]::InvariantCulture))
New-Item -ItemType Directory -Path $captures | Out-Null
# Every capture below is English at 1280x720 and 100%, so the sweep renders only that size.
$quick = $env:SF4E_UI_RENDER_QUICK; $env:SF4E_UI_RENDER_QUICK = '720'
try { & $renderer "$captures/" $GameRoot (Join-Path $repo 'assets/selection') --readme-shots }
finally { $env:SF4E_UI_RENDER_QUICK = $quick }
if ($LASTEXITCODE) { throw 'README UI rendering failed.' }
Add-Type -AssemblyName System.Drawing
$destination = Join-Path $repo 'docs/images'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
# docs/images/manifest.tsv names the render each picture is made from. The guides'
# step pictures are WebP to keep the repository small; cwebp reads PNG, not BMP.
$rows = Import-Csv -LiteralPath (Join-Path $destination 'manifest.tsv') -Delimiter "`t"
foreach ($row in $rows | Where-Object source -like 'render *') {
    $render = $row.source.Substring(7)
    $name = $row.picture -replace '[/.]', '-'
    $bmp = Join-Path $captures "$render-en-1280-100.bmp"
    if (-not (Test-Path -LiteralPath $bmp)) { throw "the render test wrote no '$render' for $($row.picture)" }
    $output = Join-Path $destination $row.picture
    New-Item -ItemType Directory -Path (Split-Path $output -Parent) -Force | Out-Null
    $png = Join-Path $captures "$name.png"
    $bitmap = [Drawing.Bitmap]::new($bmp)
    try { $bitmap.Save($png,[Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
    if ($output.EndsWith('.png')) { Copy-Item -LiteralPath $png -Destination $output }
    else {
        & $cwebp -quiet -q 85 $png -o $output
        if ($LASTEXITCODE) { throw "cwebp failed on $($row.picture)" }
    }
}
Remove-Item -LiteralPath $captures -Recurse -Force
Write-Output "Fresh README and guide pictures: $destination"
# stale.py dates every picture captured here, changed or not, from this trailer of the commit that
# records the run, which a rebase keeps. It names the build's revision, not HEAD, which may have moved on.
Write-Output "End the message of the commit with these pictures with this line, even if no picture changed:"
Write-Output "Pictures-from: $revision"
