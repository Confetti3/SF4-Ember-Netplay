#Requires -Version 7
# Publish an already-tagged, locally verified Ember release. Never rebuild in CI.
# The checkout must be the Stable channel's, and the release goes to the Stable repository:
# neither is a parameter. Invoke-StablePublish (NightlyOperations.ps1) holds the steps.
param([Parameter(Mandatory=$true)][string]$Tag, [string]$OutDir = 'dist',
      [string]$NotesFile = '', [string]$PreviousVersion = '')
$ErrorActionPreference = 'Stop'
# Native exits are checked explicitly by the shared operations.
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
$source = Split-Path $PSScriptRoot -Parent
# The package scripts are dot-sourced, as before, and report their outputs through variables.
$operations = @{
    Target = { param($SourceRoot) Get-EmberBuildTarget $SourceRoot }
    Package = {
        param($OutDirectory, $Label)
        . (Join-Path $PSScriptRoot 'package-team.ps1') -OutDir $OutDirectory -VersionLabel $Label | Out-Host
        [pscustomobject]@{zipPath=$script:PackageZipPath;folderPath=$script:PackageFolderPath;gitRev=$script:PackageGitRev;
            sourceFingerprint=$receipt.sourceFingerprint}
    }
    Upgrade = {
        param($From, $To, $BaseZip, $TargetZip, $OutDirectory)
        . (Join-Path $PSScriptRoot 'package-upgrade.ps1') -FromVersion $From -ToVersion $To -BaseZip $BaseZip -TargetZip $TargetZip -OutDir $OutDirectory | Out-Host
        $script:UpgradeZipPath
    }
    Installer = {
        param($PackageDir, $Label, $OutDirectory)
        . (Join-Path $PSScriptRoot 'package-installer.ps1') -PackageDir $PackageDir -VersionLabel $Label -OutDir $OutDirectory | Out-Host
        $script:InstallerPath
    }
}
Push-Location $source
try {
    Invoke-StablePublish $source $Tag $OutDir $NotesFile $PreviousVersion $operations | Out-Null
} finally { Pop-Location }
