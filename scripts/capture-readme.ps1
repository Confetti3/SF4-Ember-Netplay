# Capture real Ember UI with fictional sample session data. Does not launch SF4.
param([Parameter(Mandatory=$true)][string]$GameRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
$renderer = Join-Path $build 'UiRenderTest.exe'
if (!(Test-Path -LiteralPath (Join-Path $GameRoot 'SSFIV.exe'))) { throw 'GameRoot must be the owned Steam USF4 installation.' }
$captures = Join-Path $build ('readme-captures-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $captures | Out-Null
& $renderer "$captures/" $GameRoot (Join-Path $repo 'assets/selection') --readme-shots
if ($LASTEXITCODE) { throw 'README UI rendering failed.' }
Add-Type -AssemblyName System.Drawing
$destination = Join-Path $repo 'docs/images'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$shots = @(
    @{source='home';name='ember-home';crop=$null},
    @{source='room';name='ember-room';crop=$null},
    @{source='roster';name='ember-fighter-selection';crop=$null},
    @{source='costumes';name='ember-costumes';crop=$null}
)
foreach ($shot in $shots) {
    $bitmap = [Drawing.Bitmap]::new((Join-Path $captures "$($shot.source)-1280-100.bmp"))
    try {
        $output = Join-Path $destination "$($shot.name).png"
        if ($shot.crop) {
            $r = $shot.crop
            $cropped = $bitmap.Clone([Drawing.Rectangle]::new($r[0],$r[1],$r[2],$r[3]),[Drawing.Imaging.PixelFormat]::Format24bppRgb)
            try { $cropped.Save($output,[Drawing.Imaging.ImageFormat]::Png) } finally { $cropped.Dispose() }
        } else { $bitmap.Save($output,[Drawing.Imaging.ImageFormat]::Png) }
    } finally { $bitmap.Dispose() }
}
Write-Output "Fresh README PNGs: $destination. Original captures: $captures"
