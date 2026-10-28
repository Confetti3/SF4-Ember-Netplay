# Wrap an already verified package folder in the per-user installer. Never rebuild or repackage here.
# The steps are New-EmberInstaller in Packaging.ps1, which returns the installer path.
param([Parameter(Mandatory=$true)][string]$PackageDir, [Parameter(Mandatory=$true)][string]$VersionLabel,
      [string]$OutDir = 'dist', [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
      [string]$InnoCompiler = $env:SF4E_ISCC, [string[]]$InnoDefines = @())
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Packaging.ps1')
$null = New-EmberInstaller @PSBoundParameters
