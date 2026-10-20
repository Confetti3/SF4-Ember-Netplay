# Capture real Ember UI with fictional sample session data. Does not launch SF4.
param([Parameter(Mandatory=$true)][string]$GameRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
$renderer = Join-Path $build 'UiRenderTest.exe'
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
# Each picture under docs/images and the render it is made from. The guides'
# step pictures are WebP to keep the repository small; cwebp reads PNG, not BMP.
$shots = [ordered]@{
    'ember-home.png'='home'
    'ember-room.png'='room'
    'ember-fighter-selection.png'='roster'
    'ember-costumes.png'='costumes'
    'guides/user_netplay/launch-recovery.webp'='launch-recovery'
    'guides/user_netplay/game-settings.webp'='game-settings-card'
    'guides/user_netplay/player-controller.webp'='player'
    'guides/user_netplay/assign-controller.webp'='controllers-capture-press'
    'guides/user_netplay/create-room.webp'='create'
    'guides/user_netplay/join-room.webp'='join'
    'guides/user_netplay/seat-chooser.webp'='room-seat-chooser'
    'guides/user_netplay/fighter-select.webp'='selection'
    'guides/user_netplay/ready-up.webp'='table-ready-up'
    'guides/user_netplay/interface.webp'='interface'
    'guides/user_netplay/replays.webp'='replays'
    'guides/user_netplay/replay-export.webp'='replay-export'
    'guides/user_netplay/leave-room.webp'='leave-confirmation'
    'guides/custom_rooms/members.webp'='room-members'
    'guides/custom_rooms/transfer-host.webp'='room-transfer-host-confirm'
    'guides/custom_rooms/room-settings.webp'='room-admin'
    'guides/controller_menus/profile.webp'='profile'
    'guides/training_lab/controls.webp'='training-home'
    'guides/training_lab/dummy-recording.webp'='training-recording'
    'guides/training_lab/reply-preset.webp'='training-reply-presets'
    'guides/saving_logs/help-about.webp'='about'
    'guides/saving_logs/report-comment.webp'='report-problem-comment'
    'guides/saving_logs/report-send.webp'='report-consent'
    'guides/saving_logs/report-sent.webp'='report-sent'
}
foreach ($shot in $shots.GetEnumerator()) {
    $output = Join-Path $destination $shot.Key
    New-Item -ItemType Directory -Path (Split-Path $output -Parent) -Force | Out-Null
    $png = Join-Path $captures "$($shot.Value).png"
    $bitmap = [Drawing.Bitmap]::new((Join-Path $captures "$($shot.Value)-en-1280-100.bmp"))
    try { $bitmap.Save($png,[Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
    if ($output.EndsWith('.png')) { Copy-Item -LiteralPath $png -Destination $output; continue }
    & $cwebp -quiet -q 85 $png -o $output
    if ($LASTEXITCODE) { throw "cwebp failed on $($shot.Value)" }
}
Write-Output "Fresh README and guide pictures: $destination. Original captures: $captures"
