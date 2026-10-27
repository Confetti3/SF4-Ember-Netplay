# Assemble only the shared product inventory. Never reuse or mirror a previous package.
# The steps are New-EmberPackage in Packaging.ps1, which returns the zip, folder and revision.
param([string]$BuildDir = "", [string]$InstallDir = "",
      [string]$OutDir = "dist", [string]$VersionLabel = "", [string]$QuickStartPath = "")
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'Packaging.ps1')
$null = New-EmberPackage @PSBoundParameters
