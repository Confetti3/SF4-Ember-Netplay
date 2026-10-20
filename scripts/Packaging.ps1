# The three packaging steps as functions that return what they made. Import-safe: dot-source it.
#   New-EmberPackage         complete package from the current build (package-team.ps1)
#   New-EmberUpgradePackage  hash-addressed upgrade between two complete packages (package-upgrade.ps1)
#   New-EmberInstaller       per-user installer around a verified package folder (package-installer.ps1)
# Each pins its own strictness and error preference, so nothing leaks into the caller. Console output goes
# to the host; the only pipeline output is the returned object.
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
. (Join-Path $PSScriptRoot 'PackageFiles.ps1')
. (Join-Path $PSScriptRoot 'PackageGuide.ps1')

# Assemble only the shared product inventory. Never reuse or mirror a previous package.
# Returns zipPath, folderPath, gitRev and the source fingerprint of the build receipt.
function New-EmberPackage([string]$BuildDir = '', [string]$InstallDir = '',
                          [string]$OutDir = 'dist', [string]$VersionLabel = '', [string]$QuickStartPath = '') {
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Off
    $repo = Split-Path $PSScriptRoot -Parent
    $designation = Get-EmberBuildTarget $repo
    if (!$BuildDir) { $BuildDir = Join-Path $repo $designation.buildDirectory }
    if (!$InstallDir) { $InstallDir = Join-Path $repo $designation.installDirectory }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir)
    $InstallDir = [IO.Path]::GetFullPath($InstallDir)
    if ($BuildDir -ne (Join-Path $repo $designation.buildDirectory) -or $InstallDir -ne (Join-Path $repo $designation.installDirectory)) { throw 'Use the designated build and stage directories from build-target.json' }
    $receipt = Assert-BuildReceipt $repo $BuildDir $InstallDir
    # Both channels package through this function; the receipt must still belong to this checkout's channel.
    Assert-ReceiptChannel $receipt $designation.channel
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
        if ($entry.Path -like '*.md') { [IO.File]::WriteAllText($target, (Get-PackageGuide $source)) }
        else { Copy-Item -LiteralPath $source -Destination $target }
    }
    Copy-Item -LiteralPath $inventory -Destination (Join-Path $destination 'PackageInventory.inc')
    Copy-Item -LiteralPath (Join-Path $BuildDir 'build-provenance.json') -Destination (Join-Path $destination 'build-provenance.json')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'tester-preflight.ps1') -Destination (Join-Path $destination 'preflight.ps1')
    # %~dp0 ends in a backslash, which would escape the closing quote and hand PowerShell a path with a literal quote; the dot keeps it a plain directory.
    # Players double-click it, so the window stays open to show the result.
    Set-Content -LiteralPath (Join-Path $destination 'preflight.cmd') -Encoding ASCII -Value @(
        '@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0preflight.ps1" -PackageDir "%~dp0." -Interactive',
        '@pause')
    if (!$QuickStartPath) { $QuickStartPath = Join-Path $repo 'docs/guides/USER_NETPLAY.md' }
    if (!(Test-Path -LiteralPath $QuickStartPath -PathType Leaf)) { throw "Quick-start guide missing: $QuickStartPath" }
    $quickStart = (Resolve-Path -LiteralPath $QuickStartPath).Path
    # The guide is renamed inside the package and, like the docs, loses its step
    # pictures. Maintainer references are carried separately through the optional
    # docs in PackageInventory.inc.
    [IO.File]::WriteAllText((Join-Path $destination 'START_HERE.md'), (Get-PackageGuide $quickStart))
    # Preserved photographs (color-N.jpg/.png) are masking inputs kept in the
    # repository for provenance. The runtime draws the cutout beside each one, so
    # only the cutouts ship; a photograph without its cutout stops packaging.
    $art = (Resolve-Path -LiteralPath (Join-Path $repo 'assets/selection')).Path
    foreach ($file in Get-ChildItem -LiteralPath $art -File -Recurse) {
        if ($file.Name -match '^color-\d+\.(jpg|png)$') {
            if (!(Test-Path -LiteralPath (Join-Path $file.DirectoryName ($file.BaseName + '-cutout.png')))) { throw "Selection photograph has no cutout: $($file.FullName)" }
            continue
        }
        $target = Join-Path (Join-Path $destination 'assets/selection') $file.FullName.Substring($art.Length + 1)
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
    & python (Join-Path $PSScriptRoot 'collect-notices.py') --build-dir $BuildDir --output (Join-Path $destination 'notices/THIRD_PARTY_LICENSES.txt') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Dependency notice collection failed' }
    $revision = git -C $repo rev-parse HEAD
    Set-Content -LiteralPath (Join-Path $destination 'BUILD_INFO.txt') -Encoding UTF8 -Value "SF4 Ember Netplay`nRelease: $VersionLabel`nSource revision: $revision`nSee build-provenance.json for the exact source fingerprint and validation.`n"
    Add-Content -LiteralPath (Join-Path $destination 'BUILD_INFO.txt') -Value "Source: $repo`nSource fingerprint: $($receipt.sourceFingerprint)`nFeatures: $($receipt.features -join ', ')"
    Write-HashManifest $destination 'MANIFEST.txt'
    & (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $destination -Strict | Out-Host
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "Package preflight failed" }
    $validator = @((Join-Path $BuildDir 'PackageInstallerTest.exe'), (Join-Path $BuildDir 'candidate/PackageInstallerTest.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$validator) { throw 'Build PackageInstallerTest before packaging so the native update inventory can validate the output.' }
    & $validator --validate $destination | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'The native updater rejected the package inventory' }
    Compress-Archive -LiteralPath $destination -DestinationPath "$destination.zip"
    # The candidate zip must pass the same extraction and validation as a downloaded update.
    & (Join-Path $PSScriptRoot 'test-updater-download-validation.ps1') -Validator $validator -PackageZip "$destination.zip" | Out-Host
    $null = Write-Sha256Sidecar "$destination.zip"
    Write-Host "SF4 Ember Netplay package: $destination.zip"
    [pscustomobject]@{zipPath="$destination.zip";folderPath=$destination;gitRev=$revision;sourceFingerprint=$receipt.sourceFingerprint}
}

function Get-ExtractedPackageRoot([string]$Extracted) {
    $items = @(Get-ChildItem -LiteralPath $Extracted -Force)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { return $items[0].FullName }
    return $Extracted
}

function Copy-PackageFile([string]$Source, [string]$Destination) {
    $null = New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force
    Copy-Item -LiteralPath $Source -Destination $Destination
}

# Build and validate a small hash-addressed upgrade between two complete packages.
# Returns zipPath and folderPath.
function New-EmberUpgradePackage([Parameter(Mandatory=$true)][string]$FromVersion,
                                 [Parameter(Mandatory=$true)][string]$ToVersion,
                                 [Parameter(Mandatory=$true)][string]$BaseZip,
                                 [Parameter(Mandatory=$true)][string]$TargetZip,
                                 [string]$OutDir = 'dist') {
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version Latest
    $repo = Split-Path $PSScriptRoot -Parent
    if ($FromVersion -notmatch '^\d+\.\d+\.\d+$' -or $ToVersion -notmatch '^\d+\.\d+\.\d+$' -or $FromVersion -eq $ToVersion) {
        throw 'Use distinct semantic versions such as 0.8.2 and 0.8.3.'
    }
    $baseZipPath = [IO.Path]::GetFullPath($BaseZip)
    $targetZipPath = [IO.Path]::GetFullPath($TargetZip)
    $basePackageHash = Assert-Sha256Sidecar $baseZipPath
    $targetPackageHash = Assert-Sha256Sidecar $targetZipPath
    $outputRoot = if ([IO.Path]::IsPathRooted($OutDir)) { [IO.Path]::GetFullPath($OutDir) } else { [IO.Path]::GetFullPath((Join-Path $repo $OutDir)) }
    $destination = Join-Path $outputRoot "upgrade-sf4-ember-netplay-$FromVersion-to-$ToVersion"
    if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination + '.zip'))) { throw "Upgrade destination already exists: $destination" }
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('sf4-ember-upgrade-build-' + [Guid]::NewGuid().ToString('N'))
    $baseExtract = Join-Path $tempRoot 'base'
    $targetExtract = Join-Path $tempRoot 'target'
    try {
        $null = New-Item -ItemType Directory -Path $baseExtract,$targetExtract,$destination -Force
        Expand-Archive -LiteralPath $baseZipPath -DestinationPath $baseExtract
        Expand-Archive -LiteralPath $targetZipPath -DestinationPath $targetExtract
        $baseRoot = Get-ExtractedPackageRoot $baseExtract
        $targetRoot = Get-ExtractedPackageRoot $targetExtract
        $baseManifestPath = Join-Path $baseRoot 'MANIFEST.txt'
        $targetManifestPath = Join-Path $targetRoot 'MANIFEST.txt'
        if (!(Test-Path -LiteralPath $baseManifestPath) -or !(Test-Path -LiteralPath $targetManifestPath)) { throw 'A complete package manifest is missing.' }
        $before = Read-HashManifest $baseManifestPath
        $after = Read-HashManifest $targetManifestPath
        $baseManifestHash = (Get-FileHash -LiteralPath $baseManifestPath -Algorithm SHA256).Hash
        $targetManifestHash = (Get-FileHash -LiteralPath $targetManifestPath -Algorithm SHA256).Hash
        $before['MANIFEST.txt'] = $baseManifestHash
        $after['MANIFEST.txt'] = $targetManifestHash
        $changed = [string[]]@($after.Keys | Where-Object { !$before.ContainsKey($_) -or $before[$_] -ine $after[$_] })
        $removed = [string[]]@($before.Keys | Where-Object { !$after.ContainsKey($_) })
        [Array]::Sort($changed, [StringComparer]::Ordinal)
        [Array]::Sort($removed, [StringComparer]::Ordinal)
        if (!$changed.Count) { throw 'The package manifests contain no upgrade work.' }
        $unchanged = @($after.Keys | Where-Object { $before.ContainsKey($_) -and $before[$_] -ieq $after[$_] }).Count

        Copy-Item -LiteralPath $baseManifestPath -Destination (Join-Path $destination 'base-manifest.txt')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'upgrade\Install-Upgrade.ps1') -Destination (Join-Path $destination 'Install-Upgrade.ps1')
        Set-Content -LiteralPath (Join-Path $destination 'Install Upgrade.cmd') -Encoding ASCII -Value '@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Upgrade.ps1"'
        foreach ($optional in @('ATTRIBUTION.md','LICENSE')) {
            $source = Join-Path $targetRoot $optional
            if (Test-Path -LiteralPath $source -PathType Leaf) { Copy-Item -LiteralPath $source -Destination (Join-Path $destination $optional) }
        }
        $notices = Join-Path $targetRoot 'notices'
        if (Test-Path -LiteralPath $notices -PathType Container) { Copy-Item -LiteralPath $notices -Destination $destination -Recurse }
        $payload = Join-Path $destination 'payload'
        foreach ($relative in $changed) { Copy-PackageFile (Join-Path $targetRoot $relative) (Join-Path $payload $relative) }
        $installerHash = (Get-FileHash -LiteralPath (Join-Path $destination 'Install-Upgrade.ps1') -Algorithm SHA256).Hash
        $targetReceipt = Get-Content -LiteralPath (Join-Path $targetRoot 'build-provenance.json') -Raw | ConvertFrom-Json
        [ordered]@{
            schema=1;from=$FromVersion;to=$ToVersion;baseManifestSha256=$baseManifestHash;targetManifestSha256=$targetManifestHash
            basePackageSha256=$basePackageHash;targetPackageSha256=$targetPackageHash;changedFiles=$changed;removedFiles=$removed
            unchangedFiles=$unchanged;payloadSourceRevision=$targetReceipt.baseRevision;installerSha256=$installerHash
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'upgrade.json') -Encoding UTF8
        $guide = @"
# Upgrade SF4 Ember Netplay $FromVersion to $ToVersion

This smaller download upgrades an intact published **Ember $FromVersion** folder to **$ToVersion** without downloading the unchanged artwork and other large files. It replaces $($changed.Count) files, removes $($removed.Count) obsolete files and reuses $unchanged unchanged files. It is not a fresh installation or an upgrade from the legacy launcher.

## Install

1. Close Ultra Street Fighter IV and Ember.
2. Extract this upgrade ZIP into a separate folder outside your Ember installation.
3. Double-click **Install Upgrade.cmd**.
4. Select your existing **Ember $FromVersion folder containing Launcher.exe**.
5. Wait for the verified completion message, then run Launcher.exe from the upgraded folder.

The installer verifies the complete existing package and upgrade payload before changing anything. It constructs and preflights the complete $ToVersion layout locally, creates a backup beside the Ember folder, changes only the declared product files, and verifies the final package. Personal files and preferences under **%APPDATA%\sf4e** are preserved. It does not launch or stop the game.

For a non-modifying check:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Upgrade.ps1 -InstallDir "C:\Games\Ember" -CheckOnly
~~~

If the check reports an interrupted transaction, keep the backup (either the sibling Ember-backup folder or the installation's .ember-update-backups folder) and run recovery from this extracted package:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Upgrade.ps1 -InstallDir "C:\Games\Ember" -RecoverOnly
~~~

Recovery verifies the saved bytes before restoring them. If it reports missing or damaged evidence, preserve the installation, transaction and backup instead of retrying a normal update.

If the selected folder is modified or is not the exact published $FromVersion package, use the complete $ToVersion package instead. Everyone in a room must use the same Ember version.

Release notes: https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v$ToVersion
"@
        Set-Content -LiteralPath (Join-Path $destination 'START_HERE.md') -Encoding UTF8 -Value $guide
        Write-HashManifest $destination 'UPGRADE_MANIFEST.txt'

        # Exercise both the non-mutating check and a real upgrade against temporary
        # extraction of the exact base package. Extra user files must survive.
        $extra = Join-Path $baseRoot 'user-file-preservation-check.txt'
        Set-Content -LiteralPath $extra -Encoding ASCII -Value 'preserve'
        & (Join-Path $destination 'Install-Upgrade.ps1') -InstallDir $baseRoot -CheckOnly | Out-Host
        & (Join-Path $destination 'Install-Upgrade.ps1') -InstallDir $baseRoot | Out-Host
        if (!(Test-Path -LiteralPath $extra -PathType Leaf)) { throw 'Upgrade removed an unrelated user file.' }
        foreach ($relative in $after.Keys) {
            $path = Join-Path $baseRoot $relative
            if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ine $after[$relative]) {
                throw "Upgraded validation tree differs from target: $relative"
            }
        }
        foreach ($relative in $removed) { if (Test-Path -LiteralPath (Join-Path $baseRoot $relative)) { throw "Removed file survived validation: $relative" } }

        Compress-Archive -LiteralPath $destination -DestinationPath ($destination + '.zip')
        $null = Write-Sha256Sidecar ($destination + '.zip')
        Write-Host "SF4 Ember Netplay upgrade package: $($destination + '.zip')"
        Write-Host "Upgrade payload: $($changed.Count) changed, $($removed.Count) removed, $unchanged unchanged files reused"
        [pscustomobject]@{zipPath=($destination + '.zip');folderPath=$destination}
    } finally {
        $resolved = [IO.Path]::GetFullPath($tempRoot)
        if ((Split-Path $resolved -Leaf) -like 'sf4-ember-upgrade-build-*' -and $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
            if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
        }
    }
}

# Wrap an already verified package folder in the per-user installer. Never rebuild or repackage here.
# Returns installerPath.
function New-EmberInstaller([Parameter(Mandatory=$true)][string]$PackageDir, [Parameter(Mandatory=$true)][string]$VersionLabel,
                            [string]$OutDir = 'dist', [string]$VisualStudioPath = '', [string]$VcpkgRoot = '',
                            [string]$InnoCompiler = $env:SF4E_ISCC, [string[]]$InnoDefines = @()) {
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Off
    $repo = Split-Path $PSScriptRoot -Parent
    if ($VersionLabel -notmatch '^[a-zA-Z0-9._-]+$') { throw 'Invalid version label' }
    $PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path.TrimEnd('\','/')
    if (!(Test-Path -LiteralPath (Join-Path $PackageDir 'MANIFEST.txt') -PathType Leaf)) { throw "Not a package folder: $PackageDir" }
    & (Join-Path $PSScriptRoot 'tester-preflight.ps1') -PackageDir $PackageDir -Strict | Out-Host
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Package preflight failed' }
    # The compiler is pinned like the other build inputs: a private copy under
    # build\tools, fetched once and checked against installer\innosetup-pin.json,
    # the download on arrival and the kept ISCC.exe on every run.
    # /PORTABLE=1 leaves no uninstall entry, shortcut or file association.
    if (!$InnoCompiler) {
        $pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installer\innosetup-pin.json') -Raw | ConvertFrom-Json
        $tool = Join-Path $repo "build\tools\innosetup-$($pin.version)"
        $InnoCompiler = Join-Path $tool 'ISCC.exe'
        $cached = (Test-Path -LiteralPath $InnoCompiler -PathType Leaf) -and (Get-FileHash -LiteralPath $InnoCompiler -Algorithm SHA256).Hash -eq $pin.isccSha256
        if (!$cached) {
            if (Test-Path -LiteralPath $tool) { Remove-Item -LiteralPath $tool -Recurse -Force }
            $download = Join-Path ([IO.Path]::GetTempPath()) "innosetup-$($pin.version)-$PID.exe"
            try {
                $ProgressPreference = 'SilentlyContinue'
                Invoke-WebRequest -Uri $pin.url -OutFile $download -UseBasicParsing
                if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $pin.sha256) { throw 'The Inno Setup download does not match installer\innosetup-pin.json' }
                $setup = Start-Process -FilePath $download -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/PORTABLE=1',"/DIR=`"$tool`"" -Wait -PassThru
                if ($setup.ExitCode -ne 0) { throw "Inno Setup installation exited with $($setup.ExitCode)" }
            } finally { Remove-Item -LiteralPath $download -ErrorAction SilentlyContinue }
            if ((Get-FileHash -LiteralPath $InnoCompiler -Algorithm SHA256).Hash -ne $pin.isccSha256) { throw 'The installed ISCC.exe does not match installer\innosetup-pin.json' }
        }
    }
    if (!(Test-Path -LiteralPath $InnoCompiler -PathType Leaf)) { throw "Inno Setup compiler missing: $InnoCompiler" }
    # The runtime of the toolset that built the package: Launcher.exe refuses to
    # start on an older one, so the installer carries exactly this one.
    $tools = Get-EmberToolPaths $repo $VisualStudioPath $VcpkgRoot
    Enter-EmberVcEnvironment $tools.VisualStudioPath
    $redist = Join-Path $env:VCToolsRedistDir 'vc_redist.x86.exe'
    if (!(Test-Path -LiteralPath $redist -PathType Leaf)) { throw "Visual C++ runtime installer missing: $redist" }
    $signature = Get-AuthenticodeSignature -LiteralPath $redist
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw "The Visual C++ runtime installer is not signed by Microsoft: $redist" }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $OutDir = (Resolve-Path -LiteralPath $OutDir).Path
    $installer = Join-Path $OutDir "sf4-ember-netplay-$VersionLabel-setup.exe"
    if (Test-Path -LiteralPath $installer) { throw "Installer destination already exists: $installer" }
    & $InnoCompiler /Qp "/DPackageDir=$PackageDir" "/DAppVersion=$VersionLabel" "/DRedist=$redist" @InnoDefines "/O$OutDir" (Join-Path $PSScriptRoot 'installer\ember-setup.iss') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed' }
    $null = Write-Sha256Sidecar $installer
    Write-Host "SF4 Ember Netplay installer: $installer"
    [pscustomobject]@{installerPath=$installer}
}
