# Build and validate a small hash-addressed upgrade between two complete packages.
# The steps are New-EmberUpgradePackage in Packaging.ps1, which returns the zip and folder.
param([Parameter(Mandatory=$true)][string]$FromVersion,
      [Parameter(Mandatory=$true)][string]$ToVersion,
      [Parameter(Mandatory=$true)][string]$BaseZip,
      [Parameter(Mandatory=$true)][string]$TargetZip,
      [string]$OutDir = 'dist')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Packaging.ps1')
$null = New-EmberUpgradePackage @PSBoundParameters
