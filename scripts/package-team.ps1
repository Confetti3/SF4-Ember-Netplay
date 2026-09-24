# Assemble only the shared product inventory. Never reuse or mirror a previous package.
param([string]$BuildDir = "", [string]$InstallDir = "",
      [string]$OutDir = "dist", [string]$VersionLabel = "", [string]$QuickStartPath = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$designation = Get-EmberBuildTarget $repo
if (!$BuildDir) { $BuildDir = Join-Path $repo $designation.buildDirectory }
if (!$InstallDir) { $InstallDir = Join-Path $repo $designation.installDirectory }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
if ($BuildDir -ne (Join-Path $repo $designation.buildDirectory) -or $InstallDir -ne (Join-Path $repo $designation.installDirectory)) { throw 'Use the designated build and stage directories from build-target.json' }
$receipt = Assert-BuildReceipt $repo $BuildDir $InstallDir
if (!$receipt.testsPassed) { throw 'The current build has no passing test receipt' }
if (!$receipt.baseRevision -or $receipt.baseRevision -ne (& git -C $repo rev-parse HEAD)) { throw 'The build receipt is for a different source commit; rebuild the release commit.' }
if (!$VersionLabel) { $VersionLabel = Get-Date -Format "yyyyMMdd-HHmmss" }
if ($VersionLabel -notmatch '^[a-zA-Z0-9._-]+$') { throw "Invalid version label" }
$discordBuild = Join-Path $InstallDir 'discord-build.json'
if (!(Test-Path -LiteralPath $discordBuild -PathType Leaf)) { throw 'Discord-enabled packaging requires an installed, configured Discord build.' }
$discord = Get-Content -LiteralPath $discordBuild -Raw | ConvertFrom-Json
if ($discord.applicationId -notmatch '^[1-9][0-9]+$' -or $discord.sdkSha256 -notmatch '^[0-9a-f]{64}$' -or !$discord.sdkVersion) {
    throw 'Discord build identity is missing or invalid.'
}
$destination = Join-Path $OutDir "sf4-ember-netplay-$VersionLabel"
if (Test-Path -LiteralPath $destination) { throw "Package destination already exists: $destination" }
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$destination = (Resolve-Path -LiteralPath $destination).Path
$inventory = Join-Path $repo "src/common/PackageInventory.inc"
$entries = foreach ($line in Get-Content -LiteralPath $inventory) {
    if ($line -match '^SF4E_PACKAGE_(REQUIRED|OPTIONAL)\("(.*)"\)') {
        [pscustomobject]@{ Required = $Matches[1] -eq 'REQUIRED'; Path = $Matches[2].Replace('\\','\') }
    }
}
$generated = @('PackageInventory.inc','preflight.ps1','preflight.cmd','START_HERE.md','MANIFEST.txt','BUILD_INFO.txt','build-provenance.json','notices\THIRD_PARTY_LICENSES.txt')
foreach ($entry in $entries) {
    if ($generated -contains $entry.Path) { continue }
    $candidates = @((Join-Path $InstallDir $entry.Path), (Join-Path $BuildDir "candidate/$($entry.Path)"),
        (Join-Path $BuildDir $entry.Path), (Join-Path $repo $entry.Path), (Join-Path $repo ".github/$($entry.Path)"))
    # Package docs stay flat under docs\; the repository keeps them in topic folders.
    if ($entry.Path -like 'docs\*') { $candidates += @(Get-ChildItem -LiteralPath (Join-Path $repo 'docs') -Directory | ForEach-Object { Join-Path $_.FullName (Split-Path $entry.Path -Leaf) }) }
    $source = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$source) { if ($entry.Required) { throw "Required package file missing: $($entry.Path)" }; continue }
    $target = Join-Path $destination $entry.Path
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
Copy-Item -LiteralPath $inventory -Destination (Join-Path $destination 'PackageInventory.inc')
Copy-Item -LiteralPath (Join-Path $BuildDir 'build-provenance.json') -Destination (Join-Path $destination 'build-provenance.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'tester-preflight.ps1') -Destination (Join-Path $destination 'preflight.ps1')
# %~dp0 ends in a backslash, which would escape the closing quote and hand PowerShell a path with a literal quote; the dot keeps it a plain directory.
Set-Content -LiteralPath (Join-Path $destination 'preflight.cmd') -Encoding ASCII -Value '@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0preflight.ps1" -PackageDir "%~dp0."'
if (!$QuickStartPath) { $QuickStartPath = Join-Path $repo 'docs/guides/USER_NETPLAY.md' }
if (!(Test-Path -LiteralPath $QuickStartPath -PathType Leaf)) { throw "Quick-start guide missing: $QuickStartPath" }
$quickStart = (Resolve-Path -LiteralPath $QuickStartPath).Path
# The guide is renamed inside the package. Keep it free of repository-relative
# links; maintainer references are carried separately through the optional docs
# in PackageInventory.inc.
Copy-Item -LiteralPath $quickStart -Destination (Join-Path $destination 'START_HERE.md')
$art = Join-Path $repo 'assets/selection'
New-Item -ItemType Directory -Path (Join-Path $destination 'assets') -Force | Out-Null
Copy-Item -LiteralPath $art -Destination (Join-Path $destination 'assets') -Recurse
& python (Join-Path $PSScriptRoot 'collect-notices.py') --build-dir $BuildDir --output (Join-Path $destination 'notices/THIRD_PARTY_LICENSES.txt')
if ($LASTEXITCODE -ne 0) { throw 'Dependency notice collection failed' }
$revision = git -C $repo rev-parse HEAD
Set-Content -LiteralPath (Join-Path $destination 'BUILD_INFO.txt') -Encoding UTF8 -Value "SF4 Ember Netplay`nRelease: $VersionLabel`nSource revision: $revision`nSee build-provenance.json for the exact source fingerprint and validation.`n"
Add-Content -LiteralPath (Join-Path $destination 'BUILD_INFO.txt') -Value "Source: $repo`nSource fingerprint: $($receipt.sourceFingerprint)`nFeatures: $($receipt.features -join ', ')"
$manifest = Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($destination.Length + 1)
}
Set-Content -LiteralPath (Join-Path $destination 'MANIFEST.txt') -Encoding UTF8 -Value $manifest
& (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $destination -Strict
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "Package preflight failed" }
$validator = @((Join-Path $BuildDir 'PackageInstallerTest.exe'), (Join-Path $BuildDir 'candidate/PackageInstallerTest.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$validator) { throw 'Build PackageInstallerTest before packaging so the native update inventory can validate the output.' }
& $validator $destination
if ($LASTEXITCODE -ne 0) { throw 'The native updater rejected the package inventory' }
Compress-Archive -LiteralPath $destination -DestinationPath "$destination.zip"
$hash = (Get-FileHash -LiteralPath "$destination.zip" -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$destination.zip.sha256" -Encoding ASCII -Value "$hash  $([IO.Path]::GetFileName($destination)).zip"
$script:PackageZipPath = "$destination.zip"
$script:PackageFolderPath = $destination
$script:PackageGitRev = $revision
Write-Host "SF4 Ember Netplay package: $destination.zip"
