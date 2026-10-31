#Requires -Version 7
# Publish an already-tagged, locally verified Ember release. Never rebuild in CI.
# The checkout must be the Stable channel's, on the release branch, and the release goes to the Stable repository:
# neither is a parameter. Invoke-StablePublish (NightlyOperations.ps1) holds the steps.
param([Parameter(Mandatory=$true)][string]$Tag, [string]$OutDir = 'dist',
      [string]$NotesFile = '', [string]$PreviousVersion = '')
$ErrorActionPreference = 'Stop'
# Native exits are checked explicitly by the shared operations.
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
. (Join-Path $PSScriptRoot 'Packaging.ps1')
$source = Split-Path $PSScriptRoot -Parent
# The packaging functions return what they made; their console output stays out of the result.
$operations = @{
    Target = { param($SourceRoot) Get-EmberBuildTarget $SourceRoot }
    Package = { param($OutDirectory, $Label) New-EmberPackage -OutDir $OutDirectory -VersionLabel $Label }
    Upgrade = {
        param($From, $To, $BaseZip, $TargetZip, $OutDirectory)
        (New-EmberUpgradePackage -FromVersion $From -ToVersion $To -BaseZip $BaseZip -TargetZip $TargetZip -OutDir $OutDirectory).zipPath
    }
    Installer = {
        param($PackageDir, $Label, $OutDirectory)
        (New-EmberInstaller -PackageDir $PackageDir -VersionLabel $Label -OutDir $OutDirectory).installerPath
    }
}
Push-Location $source
try {
    Invoke-StablePublish $source $Tag $OutDir $NotesFile $PreviousVersion $operations | Out-Null
} finally { Pop-Location }
