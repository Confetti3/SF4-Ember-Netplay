# Assemble only the shared product inventory. Never reuse or mirror a previous package.
param([string]$BuildDir = "", [string]$InstallDir = "",
      [string]$OutDir = "dist", [string]$VersionLabel = "", [string]$QuickStartPath = "", [string]$Python = $env:SF4E_PYTHON)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'PackageContent.ps1')
if (!$Python) { $Python = (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source }
if (!(Test-Path -LiteralPath $Python -PathType Leaf)) { throw "Python executable does not exist: $Python" }
& $Python -c "import runpy,sys; runpy.run_path(sys.argv[1], run_name='package_docs_preflight')" (Join-Path $PSScriptRoot 'package-docs.py')
if ($LASTEXITCODE) { throw 'Install scripts/requirements-docs.txt with Python 3.10+ before packaging.' }
$designation = Get-EmberBuildTarget $repo
if (!$BuildDir) { $BuildDir = Join-Path $repo $designation.buildDirectory }
if (!$InstallDir) { $InstallDir = Join-Path $repo $designation.installDirectory }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
if ($BuildDir -ne (Join-Path $repo $designation.buildDirectory) -or $InstallDir -ne (Join-Path $repo $designation.installDirectory)) { throw 'Use the designated build and stage directories from build-target.json' }
$receipt = Assert-BuildReceipt $repo $BuildDir $InstallDir
if (!$receipt.testsPassed) { throw 'The current build has no passing test receipt' }
if (!$receipt.baseRevision -or $receipt.baseRevision -ne (& git -C $repo rev-parse HEAD)) { throw 'The build receipt is for a different source commit; rebuild the release commit.' }
if (!$VersionLabel) { $VersionLabel = (Get-Date).ToString("yyyyMMdd-HHmmss", [cultureinfo]::InvariantCulture) }
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
$documents = @(Copy-EmberPackageContent -Repo $repo -BuildDir $BuildDir -InstallDir $InstallDir -Destination $destination -Entries $entries -Generated $generated -QuickStartPath $QuickStartPath)
Copy-Item -LiteralPath $inventory -Destination (Join-Path $destination 'PackageInventory.inc')
Copy-Item -LiteralPath (Join-Path $BuildDir 'build-provenance.json') -Destination (Join-Path $destination 'build-provenance.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'tester-preflight.ps1') -Destination (Join-Path $destination 'preflight.ps1')
# %~dp0 ends in a backslash, which would escape the closing quote and hand PowerShell a path with a literal quote; the dot keeps it a plain directory.
# Players double-click it, so the window stays open to show the result.
Set-Content -LiteralPath (Join-Path $destination 'preflight.cmd') -Encoding ASCII -Value @(
    '@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0preflight.ps1" -PackageDir "%~dp0." -Interactive',
    '@pause')
& $Python (Join-Path $PSScriptRoot 'collect-notices.py') --build-dir $BuildDir --output (Join-Path $destination 'notices/THIRD_PARTY_LICENSES.txt')
if ($LASTEXITCODE -ne 0) { throw 'Dependency notice collection failed' }
$revision = git -C $repo rev-parse HEAD
$mapping = Join-Path $BuildDir ('package-documents-' + [Guid]::NewGuid().ToString('N') + '.json')
try {
    ConvertTo-Json -InputObject $documents -Depth 5 | Set-Content -LiteralPath $mapping -Encoding UTF8
    & $Python (Join-Path $PSScriptRoot 'package-docs.py') --repo $repo --package $destination --revision $revision --mapping $mapping
    if ($LASTEXITCODE -ne 0) { throw 'Package documentation validation failed' }
} finally {
    Remove-Item -LiteralPath $mapping -ErrorAction SilentlyContinue
}
Set-Content -LiteralPath (Join-Path $destination 'BUILD_INFO.txt') -Encoding UTF8 -Value "SF4 Ember Netplay`nRelease: $VersionLabel`nSource revision: $revision`nSee build-provenance.json for the exact source fingerprint and validation.`n"
Add-Content -LiteralPath (Join-Path $destination 'BUILD_INFO.txt') -Value "Source: $repo`nSource fingerprint: $($receipt.sourceFingerprint)`nFeatures: $($receipt.features -join ', ')"
$manifestPaths = [string[]]@(Get-ChildItem -LiteralPath $destination -File -Recurse | ForEach-Object { $_.FullName })
[Array]::Sort($manifestPaths, [StringComparer]::Ordinal)
$manifest = foreach ($path in $manifestPaths) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant(), $path.Substring($destination.Length + 1)
}
Set-Content -LiteralPath (Join-Path $destination 'MANIFEST.txt') -Encoding UTF8 -Value $manifest
& (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $destination -Strict
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "Package preflight failed" }
$validator = @((Join-Path $BuildDir 'PackageInstallerTest.exe'), (Join-Path $BuildDir 'candidate/PackageInstallerTest.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$validator) { throw 'Build PackageInstallerTest before packaging so the native update inventory can validate the output.' }
& $validator --validate $destination
if ($LASTEXITCODE -ne 0) { throw 'The native updater rejected the package inventory' }
Compress-Archive -LiteralPath $destination -DestinationPath "$destination.zip"
# The candidate zip must pass the same extraction and validation as a downloaded update.
& (Join-Path $PSScriptRoot 'test-updater-download-validation.ps1') -Validator $validator -PackageZip "$destination.zip"
$hash = (Get-FileHash -LiteralPath "$destination.zip" -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$destination.zip.sha256" -Encoding ASCII -Value "$hash  $([IO.Path]::GetFileName($destination)).zip"
$script:PackageZipPath = "$destination.zip"
$script:PackageFolderPath = $destination
$script:PackageGitRev = $revision
Write-Host "SF4 Ember Netplay package: $destination.zip"
