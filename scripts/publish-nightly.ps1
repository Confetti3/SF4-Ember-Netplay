# Build and publish the designated Nightly checkout on the Windows build PC.
param([switch]$WhatIf, [switch]$Local, [switch]$Force, [switch]$SkipRoomHosts,
      [string]$VisualStudioPath = $env:SF4E_VISUAL_STUDIO_PATH,
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
# Check native exits explicitly, including the optional server1 SSH probe.
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$checkoutRoot = Split-Path $PSScriptRoot -Parent
$outDirectory = Join-Path $checkoutRoot 'dist'
$logDirectory = Join-Path $outDirectory 'nightly-logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$statePath = Join-Path $outDirectory 'nightly-pending.json'
$cleanupPath = Join-Path $outDirectory 'nightly-cleanup.json'
$runLock = $null
$transcriptStarted = $false
$locationPushed = $false
try {
    # Also protect manually started runs, not just the scheduled task.
    $runLock = [IO.File]::Open((Join-Path $outDirectory 'nightly.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    Start-Transcript -LiteralPath (Join-Path $logDirectory "$(Get-Date -Format yyyyMMdd-HHmmss).log") -Append | Out-Null
    $transcriptStarted = $true
    Push-Location $checkoutRoot
    $locationPushed = $true
    . (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
    $nightlyTarget = Get-EmberBuildTarget $checkoutRoot
    if ($nightlyTarget.channel -ne 'nightly') {
        throw 'Get-EmberBuildTarget must return .channel = nightly. Use the channel-aware BuildEnvironment.ps1 and the designated Nightly checkout.'
    }
    $nightlyBranch = [string]$nightlyTarget.branch
    if ($nightlyBranch -ne 'nightly') { throw 'channels.nightly.branch must be nightly.' }
    $releaseRepository = [string]$nightlyTarget.githubRepo
    # Keep every publication and deletion confined to the assets-only repository.
    if ($releaseRepository -ine 'Confetti3/SF4-Ember-Netplay-Nightly') { throw 'channels.nightly.githubRepo must be Confetti3/SF4-Ember-Netplay-Nightly; refusing to touch another repository.' }
    $branch = Invoke-NightlyCommand git @('branch', '--show-current') 'Could not read the current branch.'
    if ($branch -ne $nightlyBranch) { throw "Checkout must be on $nightlyBranch, currently: $branch" }
    if (Get-Process -Name SSFIV -ErrorAction SilentlyContinue) {
        Write-Host 'Skipped: SSFIV.exe is running; close the game before packaging.'
        return
    }
    Assert-NightlyCleanHead $checkoutRoot | Out-Null
    $pending = if (Test-Path -LiteralPath $statePath) {
        Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    } else { $null }
    if ($pending -and $pending.phase -ne 'published') {
        # Do not advance HEAD beyond an artifact set that still needs completion.
        Write-Host "Resuming $($pending.tag) at $($pending.phase); no fetch or rebuild (including -Force)."
    } elseif ($Local) {
        Write-Host 'Local: using checkout HEAD as-is; no fetch or fast-forward.'
    } else {
        Invoke-NightlyCommand git @('fetch', 'origin', $nightlyBranch) 'Fetching the Nightly branch failed.' | Out-Host
        Invoke-NightlyCommand git @('merge', '--ff-only', "origin/$nightlyBranch") 'Nightly cannot fast-forward; resolve the divergence by hand.' | Out-Host
    }
    # Capture only after synchronization, and check cleanliness around hashing.
    $snapshot = Get-NightlySourceSnapshot $checkoutRoot
    $sourceRevision = $snapshot.revision
    Write-Host "Nightly source: $sourceRevision"
    if ($pending) {
        if ($WhatIf) {
            if ($pending.phase -ne 'published') {
                Assert-NightlySource $checkoutRoot $pending.snapshot
                Assert-NightlyArtifacts (@($pending.assets) + @($pending.notes) + @($pending.binaries))
            }
            Write-Host "WhatIf: would finish $($pending.tag) and retention; preserving pending work."
        } else {
            $pruned = @(Complete-NightlyPublication $pending $statePath $cleanupPath $checkoutRoot $snapshot)
            Write-Host "Completed $($pending.tag). Pruned tags: $($pruned -join ', ')"
        }
        return
    }

    $previousSource = ''
    $publishedReleases = @(Get-NightlyReleases $releaseRepository -WhatIf:$WhatIf)
    if ($publishedReleases.Count) {
        $previousRelease = $publishedReleases[0]
        $bodyJson = Invoke-NightlyCommand gh @('release', 'view', $previousRelease.tagName, '--repo', $releaseRepository, '--json', 'body') 'Could not read the previous Nightly release.'
        $body = ($bodyJson | ConvertFrom-Json).body
        $sourceLines = [regex]::Matches($body, '(?m)^Source: ([0-9a-fA-F]{40})\r?$')
        if ($sourceLines.Count -ne 1) { throw "Previous Nightly $($previousRelease.tagName) must have one Source: <full sha> line." }
        $previousSource = $sourceLines[0].Groups[1].Value.ToLowerInvariant()
        if ($previousSource -eq $sourceRevision -and !$Force) {
            if (!$WhatIf) {
                $pruned = @(Invoke-NightlyRetention $releaseRepository $cleanupPath $checkoutRoot $snapshot)
                Write-Host "Retention complete. Pruned tags: $($pruned -join ', ')"
            }
            Write-Host "Skipped build: nothing new since $($previousRelease.tagName) ($sourceRevision)."
            return
        }
    } else { Write-Host 'No previous Nightly release; collecting notes since origin/release.' }

    $versionLines = @(Get-Content CMakeLists.txt | Where-Object { $_ -match '^\s+VERSION ' })
    if ($versionLines.Count -ne 1 -or $versionLines[0].Trim() -notmatch '^VERSION ([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9})$') {
        throw 'Expected one VERSION X.Y.Z line in CMakeLists.txt.'
    }
    $baseLabel = "$($Matches[1])-nightly$([DateTime]::UtcNow.ToString('yyyyMMdd'))"
    # Include tags without releases as well. Only a missing or empty repository is
    # tolerated under WhatIf; authentication/network failures still fail.
    $tagRefs = Invoke-NightlyRepositoryQuery $releaseRepository @('api', "repos/$releaseRepository/git/matching-refs/tags/v$baseLabel", '--paginate', '--jq', '.[].ref') 'Could not check existing Nightly tags.' -WhatIf:$WhatIf
    $existingTags = @($tagRefs | ForEach-Object { $_ -replace '^refs/tags/', '' })
    $existingTags += @($publishedReleases | ForEach-Object tagName)
    # Dry runs and failed publication can leave outputs without a remote tag.
    # Select a free label before building; preserve every existing local output.
    $nightlyLabel = Get-NightlyLabel $baseLabel $existingTags $outDirectory
    # ParseVersion: three numbers (1..9 digits), word nightly, eight-digit
    # prerelease number, optional .N remainder; see github_release_validation.cxx.
    if ($nightlyLabel -cnotmatch '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}-nightly[0-9]{8}(\.[0-9]+)?$') { throw "Invalid Nightly version: $nightlyLabel" }
    $nightlyTag = "v$nightlyLabel"
    $notesPath = Join-Path $outDirectory "nightly-$nightlyLabel.md"
    $baseline = $previousSource
    if (!$baseline) {
        $baseline = 'origin/release'
        if ($Local) {
            Invoke-NightlyCommand git @('cat-file', '-e', "$baseline^{commit}") 'Local mode requires an existing origin/release ref for the initial changelog baseline; fetch it before running with -Local.' | Out-Null
        } else {
            Invoke-NightlyCommand git @('fetch', 'origin', 'release') 'Could not fetch the initial Stable changelog baseline.' | Out-Host
        }
    } else {
        Invoke-NightlyCommand git @('cat-file', '-e', "$baseline^{commit}") 'Previous Nightly Source commit is unavailable locally; restore its history before publishing.' | Out-Null
    }
    $commitRange = "$baseline..$sourceRevision"
    $subjects = @(Invoke-NightlyCommand git @('log', '--first-parent', '--max-count=60', '--format=%s', $commitRange) 'Could not collect Nightly commit subjects.')
    $commitCount = [int](Invoke-NightlyCommand git @('rev-list', '--first-parent', '--count', $commitRange) 'Could not count Nightly commits.')

    # Resolve before the child build script enters vcvarsall. Reuse its actual
    # VS installation for the installer instead of probing again afterwards.
    $nightlyTools = Get-EmberToolPaths $checkoutRoot $VisualStudioPath ''
    $VisualStudioPath = $nightlyTools.VisualStudioPath
    & (Join-Path $PSScriptRoot 'build-current.ps1') -VisualStudioPath $VisualStudioPath -DiscordSdkArchive $DiscordSdkArchive
    $buildDirectory = Join-Path $checkoutRoot $nightlyTarget.buildDirectory
    $stageDirectory = Join-Path $checkoutRoot $nightlyTarget.installDirectory
    Assert-NightlyBuildSnapshot $checkoutRoot $buildDirectory $stageDirectory $snapshot
    . (Join-Path $PSScriptRoot 'package-team.ps1') -OutDir $outDirectory -VersionLabel $nightlyLabel
    $releaseAssets = @($script:PackageZipPath, "${script:PackageZipPath}.sha256")
    . (Join-Path $PSScriptRoot 'package-installer.ps1') -PackageDir $script:PackageFolderPath -VersionLabel $nightlyLabel -OutDir $outDirectory -VisualStudioPath $VisualStudioPath
    $releaseAssets += @($script:InstallerPath, "${script:InstallerPath}.sha256")
    if ($script:PackageGitRev -cne $sourceRevision) { throw 'Packaged source revision changed.' }
    Assert-NightlyBuildSnapshot $checkoutRoot $buildDirectory $stageDirectory $snapshot $script:PackageFolderPath
    # GetHash reads every on-disk byte through BCrypt SHA256, formatting each
    # digest byte with %02hhx: 64 lowercase hex characters, no prefix or separators.
    $publicBuildId = (Get-FileHash -LiteralPath (Join-Path $stageDirectory 'Sidecar.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    $packagedBuildId = (Get-FileHash -LiteralPath (Join-Path $script:PackageFolderPath 'Sidecar.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($publicBuildId -ne $packagedBuildId) { throw 'Staged and packaged Sidecar.dll build ids differ.' }
    if ($publicBuildId -cnotmatch '^[0-9a-f]{64}$') { throw 'Expected a SHA-256 public rooms build id.' }
    Write-Host "Public rooms build: $publicBuildId"
    $notes = @("SF4 Ember Netplay Nightly $nightlyLabel", '', "Source: $sourceRevision", '',
        'Nightly build from the nightly branch. Daily test builds may break; rooms only with other Nightly players.', '',
        "Public rooms build: $publicBuildId", '')
    $notes += @($subjects | ForEach-Object { "- $_" })
    if ($commitCount -gt 60) { $notes += "- and $($commitCount - 60) more" }
    if (!$commitCount) { $notes += '- No commits since the changelog baseline.' }
    Set-Content -LiteralPath $notesPath -Encoding utf8 -Value $notes

    $hostBinaries = @()
    if (!$SkipRoomHosts) {
        $hostDirectory = Join-Path $outDirectory "nightly-room-hosts/$nightlyLabel"
        New-Item -ItemType Directory -Path $hostDirectory -Force | Out-Null
        $archivePath = Join-Path $hostDirectory 'room-host-src.tgz'
        Invoke-NightlyCommand git @('-c', 'core.autocrlf=false', 'archive', '--format=tar.gz', '-o', $archivePath, $sourceRevision, 'src', 'rust', 'server') 'Could not archive the committed room-host sources.' | Out-Host
        Invoke-NightlyLinuxBuild (Join-Path $PSScriptRoot 'build-nightly-room-hosts.sh') $sourceRevision $archivePath $hostDirectory | Out-Host
        $hostBinaries = @((Join-Path $hostDirectory 'sf4e-room-host'), (Join-Path $hostDirectory 'sf4-net'))
        foreach ($binary in $hostBinaries) {
            if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "WSL did not produce $binary" }
        }
        if ($WhatIf) { Write-Host 'WhatIf: room hosts built locally; no SSH staging or scp.' }
    } else { Write-Host 'Skipped room-host build and staging (-SkipRoomHosts).' }

    # Preparation is complete. Persist exact bytes before the first remote mutation.
    Assert-NightlyBuildSnapshot $checkoutRoot $buildDirectory $stageDirectory $snapshot $script:PackageFolderPath
    $prunedTags = @()
    $releaseUrl = "https://github.com/$releaseRepository/releases/tag/$nightlyTag"
    if (!$WhatIf) {
        $destinations = if (!$SkipRoomHosts) { @(Get-NightlyRoomDestinations) } else { @() }
        $pending = [pscustomobject]@{
            repository=$releaseRepository;tag=$nightlyTag;label=$nightlyLabel;phase='prepared'
            snapshot=$snapshot;buildId=$publicBuildId;destinations=@($destinations)
            assets=@($releaseAssets | ForEach-Object { Get-NightlyArtifact $_ })
            notes=(Get-NightlyArtifact $notesPath)
            binaries=@($hostBinaries | ForEach-Object { Get-NightlyArtifact $_ })
        }
        Save-NightlyState $statePath $pending
        $prunedTags = @(Complete-NightlyPublication $pending $statePath $cleanupPath $checkoutRoot $snapshot)
        if (!$SkipRoomHosts) { Write-Host 'Owner installation command: ssh -t vps "sudo bash ~/ember-rooms/setup.sh"' }
    } else { Write-Host 'WhatIf: no staging, publication, pruning or pending record; the release URL below is planned.' }

    Write-Host "Nightly label: $nightlyLabel"
    Write-Host "Tag: $nightlyTag"
    Write-Host "Release URL: $releaseUrl"
    Write-Host "Public rooms build: $publicBuildId"
    Write-Host "Notes: $notesPath"
    foreach ($asset in $releaseAssets) {
        Write-Host "Asset: $asset  SHA-256: $((Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant())"
    }
    Write-Host "Pruned tags: $(if ($prunedTags.Count) { $prunedTags -join ', ' } else { '(none)' })"
} catch {
    Write-Host "Nightly FAILED: $($_.Exception.Message)"
    throw
} finally {
    if ($locationPushed) { Pop-Location }
    try { if ($transcriptStarted) { Stop-Transcript | Out-Null } }
    finally { if ($runLock) { $runLock.Dispose() } }
}
