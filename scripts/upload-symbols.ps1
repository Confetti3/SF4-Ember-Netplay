#Requires -Version 7
# Copies the designated build's Breakpad symbols (build-current.ps1 -Symbols) into the report server's symbol
# store, so crash reports from this release get function names (server/ember-reports/deploy/README.md,
# Symbols). publish-nightly.ps1 and github-release.ps1 call it once the release has its label. The build's
# receipt must hold Sidecar's symbols, and the build must still be the one staged.
#
# server/ember-reports/deploy/install-symbols.sh runs on the server over ssh, from this checkout's copy: it
# makes an upload folder inside the store, then, after scp, moves each complete <debug file>/<DEBUGID>
# folder in, records which ones this release uses and deletes Nightly symbols older than 45 days.
param([Parameter(Mandatory = $true)][string]$Label, [string]$Server = 'vps')
$ErrorActionPreference = 'Stop'
# Native exits are checked by Invoke-NightlyCommand.
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
if ($Label -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') { throw "Not a release label: $Label" }
$repo = Split-Path $PSScriptRoot -Parent
$target = Get-EmberBuildTarget $repo
$receipt = Assert-BuildReceipt $repo (Join-Path $repo $target.buildDirectory) (Join-Path $repo $target.installDirectory)
$store = Assert-ReceiptSymbols $receipt
# The bytes ssh sends as the script: LF endings, as bash reads them.
$installer = (Get-Content -LiteralPath (Join-Path $repo 'server/ember-reports/deploy/install-symbols.sh') -Raw).Replace("`r`n", "`n")
$remote = "bash -s -- $($target.channel) $Label"
$ssh = { param($Phase, $Failure) Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Server, "$remote $Phase") $Failure -InputText $installer }
$incoming = "$(& $ssh prepare 'Could not prepare the symbol upload.' | Select-Object -Last 1)".Trim()
if ($incoming -cnotmatch '^/[^\s]+/\.incoming-(stable|nightly)-[0-9A-Za-z.-]+$') { throw "Unexpected symbol upload folder: $incoming" }
try {
    $modules = @(Get-ChildItem -LiteralPath $store -Directory | ForEach-Object FullName)
    Invoke-NightlyCommand scp (@('-o', 'BatchMode=yes', '-r') + $modules + @("${Server}:$incoming/")) 'Could not upload the symbols.' | Out-Null
    & $ssh commit 'Could not install the uploaded symbols.' | Out-Host
} finally {
    & $ssh cleanup 'Could not remove the symbol upload folder.' | Out-Null
}
Write-Host "Symbols for $Label ($($target.channel)) are on ${Server}: $(@($receipt.symbols.modules) -join ', ')"
