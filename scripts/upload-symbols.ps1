#Requires -Version 7
# Uploads a release's Breakpad symbols to the report server's store again, from the archive its publication kept
# beside the package (dist/sf4-ember-netplay-<label>-symbols.zip, SymbolPublication.ps1), for example to restore
# the store or after a publication you stopped by hand. Publishing does this itself, in its Symbols step; this is
# only the manual way in. Run it from the checkout that published the release: its channel names the symbols.
# Nothing is rebuilt and the build folder is not read.
param([Parameter(Mandatory = $true)][string]$Label, [string]$OutDir = 'dist', [string]$Server = 'vps')
$ErrorActionPreference = 'Stop'
# Native exits are checked by Invoke-NightlyCommand.
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
$repo = Split-Path $PSScriptRoot -Parent
if (![IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repo $OutDir }
$archive = Get-EmberSymbolArchivePath $OutDir $Label
if (!(Test-Path -LiteralPath $archive -PathType Leaf)) { throw "No symbol archive for $Label at $archive." }
# The archive's manifest holds every file's SHA-256; the archive itself is taken as it is now.
Publish-EmberSymbols (Get-NightlyArtifact $archive) (Get-EmberBuildTarget $repo).channel $Label $Server
