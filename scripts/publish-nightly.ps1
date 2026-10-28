#Requires -Version 7
# Build and publish the designated Nightly checkout on the Windows build PC.
param([switch]$WhatIf, [switch]$Local, [switch]$Force, [switch]$SkipRoomHosts,
      [string]$VisualStudioPath = $env:SF4E_VISUAL_STUDIO_PATH,
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
# Check native exits explicitly, including the optional server1 SSH probe.
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'Packaging.ps1')
$checkoutRoot = Split-Path $PSScriptRoot -Parent
$outDirectory = Join-Path $checkoutRoot 'dist'
$logDirectory = Join-Path $outDirectory 'nightly-logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
# The build and packaging steps. The packaging functions return what they made; their console
# output stays out of the result.
$operations = @{
    Target = { param($SourceRoot) Get-EmberBuildTarget $SourceRoot }
    GameRunning = { [bool](Get-Process -Name SSFIV -ErrorAction SilentlyContinue) }
    Now = { [DateTime]::UtcNow }
    ToolPaths = { param($SourceRoot, $VisualStudio) Get-EmberToolPaths $SourceRoot $VisualStudio '' }
    Build = {
        param($VisualStudio, $DiscordSdk)
        & (Join-Path $PSScriptRoot 'build-current.ps1') -VisualStudioPath $VisualStudio -DiscordSdkArchive $DiscordSdk
    }
    Package = {
        param($OutDir, $Label)
        New-EmberPackage -OutDir $OutDir -VersionLabel $Label
    }
    Installer = {
        param($PackageDir, $Label, $OutDir, $VisualStudio)
        (New-EmberInstaller -PackageDir $PackageDir -VersionLabel $Label -OutDir $OutDir -VisualStudioPath $VisualStudio).installerPath
    }
    LinuxBuild = {
        param($Revision, $ArchivePath, $HostDirectory)
        Invoke-NightlyLinuxBuild (Join-Path $PSScriptRoot 'build-nightly-room-hosts.sh') $Revision $ArchivePath $HostDirectory
    }
}
$runLock = $null
$transcriptStarted = $false
$locationPushed = $false
try {
    # Also protect manually started runs, not just the scheduled task.
    $runLock = [IO.File]::Open((Join-Path $outDirectory 'nightly.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    Start-Transcript -LiteralPath (Join-Path $logDirectory "$((Get-Date).ToString('yyyyMMdd-HHmmss', [cultureinfo]::InvariantCulture)).log") -Append | Out-Null
    $transcriptStarted = $true
    Push-Location $checkoutRoot
    $locationPushed = $true
    . (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
    Invoke-NightlyPublish $checkoutRoot $outDirectory $operations -WhatIf:$WhatIf -Local:$Local -Force:$Force `
        -SkipRoomHosts:$SkipRoomHosts -VisualStudioPath $VisualStudioPath -DiscordSdkArchive $DiscordSdkArchive
} catch {
    Write-Host "Nightly FAILED: $($_.Exception.Message)"
    throw
} finally {
    if ($locationPushed) { Pop-Location }
    try { if ($transcriptStarted) { Stop-Transcript | Out-Null } }
    finally { if ($runLock) { $runLock.Dispose() } }
}
