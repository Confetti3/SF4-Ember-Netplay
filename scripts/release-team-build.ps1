# Build and package locally. Never install into the game or publish.
param([string]$VersionLabel = '', [string]$OutDir = 'dist', [switch]$SkipBuild,
      [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
Push-Location $repo
try {
    Assert-EmberChannel (Get-EmberBuildTarget $repo) 'stable' 'Stable release packaging'
    $branch = & git -C $repo branch --show-current
    if ($LASTEXITCODE) { throw 'Could not read the current branch.' }
    Assert-EmberStableBranch $branch 'Stable release packaging'
    if (!$SkipBuild) {
        & (Join-Path $PSScriptRoot 'build-current.ps1') -VisualStudioPath $VisualStudioPath -VcpkgRoot $VcpkgRoot -DiscordSdkArchive $DiscordSdkArchive
    }
    . (Join-Path $PSScriptRoot 'package-team.ps1') -OutDir $OutDir -VersionLabel $VersionLabel
    Write-Host "Verified local package: $script:PackageZipPath"
} finally { Pop-Location }
