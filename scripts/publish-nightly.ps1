# Build and publish the designated Nightly checkout on the Windows build PC.
param([switch]$WhatIf, [switch]$Local, [switch]$Force, [switch]$SkipRoomHosts,
      [string]$VisualStudioPath = $env:SF4E_VISUAL_STUDIO_PATH,
      [string]$DiscordSdkArchive = $env:SF4E_DISCORD_SDK_ARCHIVE)
$ErrorActionPreference = 'Stop'
# Check native exits explicitly, including the optional server1 SSH probe.
$PSNativeCommandUseErrorActionPreference = $false
$checkoutRoot = Split-Path $PSScriptRoot -Parent
$outDirectory = Join-Path $checkoutRoot 'dist'
$logDirectory = Join-Path $outDirectory 'nightly-logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
Start-Transcript -LiteralPath (Join-Path $logDirectory "$(Get-Date -Format yyyyMMdd-HHmmss).log") -Append | Out-Null
Push-Location $checkoutRoot

function Invoke-NightlyCommand([string]$Command, [string[]]$Arguments, [string]$Failure) {
    $result = & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Failure (exit $LASTEXITCODE)" }
    return $result
}

function Invoke-NightlyRepositoryQuery([string[]]$Arguments, [string]$Failure, [string]$DryRunFallback) {
    $result = & gh @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $detail = ($result | ForEach-Object { "$_" }) -join "`n"
        if ($detail -match 'HTTP 404|Could not resolve to a Repository|Git Repository is empty') {
            if ($WhatIf) {
                Write-Host "WhatIf: Nightly repository $releaseRepository is unavailable; $DryRunFallback"
                return
            }
            throw "Nightly repository $releaseRepository was not found or has no initial commit. Create it with one initial commit on its default branch before publishing. $detail"
        }
        throw "$Failure (exit $exitCode): $detail"
    }
    return $result
}

function Get-NightlyReleases {
    $json = Invoke-NightlyRepositoryQuery @('release', 'list', '--repo', $releaseRepository,
        '--exclude-drafts', '--limit', '10000', '--json', 'tagName,createdAt') 'Could not list Nightly releases.' 'Continuing with no previous Nightly.'
    $releases = @($json | ConvertFrom-Json)
    if ($releases.Count -ge 10000) { throw 'Nightly release list was truncated; refusing to choose or prune from an incomplete list.' }
    return $releases | Sort-Object { [DateTimeOffset]::Parse($_.createdAt) } -Descending
}

function ConvertTo-NightlyWslPath([string]$WindowsPath) {
    # Ubuntu uses WSL's default /mnt/<drive> mounts. Convert here so no
    # Windows backslashes ever cross a shell boundary. Reject UNC/relative
    # paths rather than silently inventing a mount for them.
    if ($WindowsPath -notmatch '^[a-zA-Z]:[\\/]') {
        throw "Expected an absolute drive path for WSL Ubuntu: $WindowsPath"
    }
    $fullPath = [IO.Path]::GetFullPath($WindowsPath).Replace('\', '/')
    return '/mnt/' + $fullPath.Substring(0, 1).ToLowerInvariant() + $fullPath.Substring(2)
}

try {
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
    $dirty = @(Invoke-NightlyCommand git @('status', '--porcelain', '--untracked-files=all') 'Could not check worktree cleanliness.')
    if ($dirty.Count) { throw "Nightly requires committed source and no untracked non-ignored files:`n$($dirty -join "`n")" }

    if ($Local) {
        Write-Host 'Local: using checkout HEAD as-is; no fetch or fast-forward.'
    } else {
        Invoke-NightlyCommand git @('fetch', 'origin', $nightlyBranch) 'Fetching the Nightly branch failed.' | Out-Host
        Invoke-NightlyCommand git @('merge', '--ff-only', "origin/$nightlyBranch") 'Nightly cannot fast-forward; resolve the divergence by hand.' | Out-Host
    }
    $sourceRevision = (Invoke-NightlyCommand git @('rev-parse', 'HEAD') 'Could not resolve HEAD.').Trim()
    if ($sourceRevision -notmatch '^[0-9a-f]{40}$') { throw "Expected a full source SHA: $sourceRevision" }
    Write-Host "Nightly source: $sourceRevision"

    $previousSource = ''
    $publishedReleases = @(Get-NightlyReleases)
    if ($publishedReleases.Count) {
        $previousRelease = $publishedReleases[0]
        $bodyJson = Invoke-NightlyCommand gh @('release', 'view', $previousRelease.tagName, '--repo', $releaseRepository, '--json', 'body') 'Could not read the previous Nightly release.'
        $body = ($bodyJson | ConvertFrom-Json).body
        $sourceLines = [regex]::Matches($body, '(?m)^Source: ([0-9a-fA-F]{40})\r?$')
        if ($sourceLines.Count -ne 1) { throw "Previous Nightly $($previousRelease.tagName) must have one Source: <full sha> line." }
        $previousSource = $sourceLines[0].Groups[1].Value.ToLowerInvariant()
        if ($previousSource -eq $sourceRevision -and !$Force) {
            Write-Host "Skipped: nothing new since $($previousRelease.tagName) ($sourceRevision)."
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
    $tagRefs = Invoke-NightlyRepositoryQuery @('api', "repos/$releaseRepository/git/matching-refs/tags/v$baseLabel", '--paginate', '--jq', '.[].ref') 'Could not check existing Nightly tags.' 'Skipping the remote tag-collision check; the planned tag must be rechecked before publishing.'
    $existingTags = @($tagRefs | ForEach-Object { $_ -replace '^refs/tags/', '' })
    $existingTags += @($publishedReleases | ForEach-Object tagName)
    $nightlyLabel = $baseLabel
    $suffix = 2
    while ($existingTags -contains "v$nightlyLabel") { $nightlyLabel = "$baseLabel.$suffix"; $suffix++ }
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
    . (Join-Path $PSScriptRoot 'package-team.ps1') -OutDir $outDirectory -VersionLabel $nightlyLabel
    $releaseAssets = @($script:PackageZipPath, "${script:PackageZipPath}.sha256")
    . (Join-Path $PSScriptRoot 'package-installer.ps1') -PackageDir $script:PackageFolderPath -VersionLabel $nightlyLabel -OutDir $outDirectory -VisualStudioPath $VisualStudioPath
    $releaseAssets += @($script:InstallerPath, "${script:InstallerPath}.sha256")
    $headAfterBuild = Invoke-NightlyCommand git @('rev-parse', 'HEAD') 'Could not recheck the source revision.'
    if ($headAfterBuild -ne $sourceRevision -or $script:PackageGitRev -ne $sourceRevision) { throw 'Source commit changed during the Nightly build/package run.' }
    $stageDirectory = Join-Path $checkoutRoot $nightlyTarget.installDirectory
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

    if (!$SkipRoomHosts) {
        $hostDirectory = Join-Path $outDirectory "nightly-room-hosts/$nightlyLabel"
        New-Item -ItemType Directory -Path $hostDirectory -Force | Out-Null
        $archivePath = Join-Path $hostDirectory 'room-host-src.tgz'
        Invoke-NightlyCommand git @('-c', 'core.autocrlf=false', 'archive', '--format=tar.gz', '-o', $archivePath, $sourceRevision, 'src', 'rust', 'server') 'Could not archive the committed room-host sources.' | Out-Host
        $linuxArchive = ConvertTo-NightlyWslPath $archivePath
        $linuxArtifacts = ConvertTo-NightlyWslPath $hostDirectory
        $linuxScript = @'
set -euo pipefail
export PATH="$HOME/.cargo/bin:$PATH"
build_root="$HOME/ember-nightly-build/$1"
export CARGO_TARGET_DIR="$HOME/ember-nightly-build/cargo-target"
mkdir -p "$build_root"
tar -xzf "$2" -C "$build_root"
cd "$build_root"
bash server/roomhost/build-linux.sh . ./sf4e-room-host
(cd rust/sf4-net && cargo build --profile server --locked)
dependencies=$(ldd ./sf4e-room-host)
printf '%s\n' "$dependencies"
while IFS= read -r library; do
    if [[ ! "$library" =~ ^(linux-vdso\.so\.[0-9]+|/[^[:space:]]*/ld-linux[^/[:space:]]*\.so(\.[0-9]+)?|lib(c|m|pthread|rt|dl|resolv|util|anl)\.so\.[0-9]+)$ ]]; then
        echo "Unexpected room-host dependency: $library (only libc-family libraries allowed)" >&2
        exit 1
    fi
done < <(printf '%s\n' "$dependencies" | awk 'NF { print $1 }')
test -x ./sf4e-room-host
test -x "$CARGO_TARGET_DIR/server/sf4-net"
cp ./sf4e-room-host "$CARGO_TARGET_DIR/server/sf4-net" "$3/"
'@
        $linuxScriptPath = Join-Path $hostDirectory 'build-wsl.sh'
        [IO.File]::WriteAllText($linuxScriptPath, $linuxScript.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
        $linuxScriptFile = ConvertTo-NightlyWslPath $linuxScriptPath
        # --exec bypasses WSL's default shell. Bash reads the script directly;
        # paths and the validated SHA are argv, never interpolated shell text.
        Invoke-NightlyCommand wsl @('-d', 'Ubuntu', '-u', 'kate', '--exec', 'bash', '--',
            $linuxScriptFile, $sourceRevision, $linuxArchive, $linuxArtifacts) 'Nightly room-host/helper build or libc dependency check failed in WSL Ubuntu.' | Out-Host
        $hostBinaries = @((Join-Path $hostDirectory 'sf4e-room-host'), (Join-Path $hostDirectory 'sf4-net'))
        foreach ($binary in $hostBinaries) {
            if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "WSL did not produce $binary" }
        }
        if ($WhatIf) { Write-Host 'WhatIf: room hosts built locally; no SSH staging or scp.' }
    } else { Write-Host 'Skipped room-host build and staging (-SkipRoomHosts).' }

    $prunedTags = @()
    $releaseUrl = "https://github.com/$releaseRepository/releases/tag/$nightlyTag"
    if (!$WhatIf) {
        # The Nightly repository owns the tag on its default branch; source
        # provenance is the Source line, not that assets-only repository's commit.
        Invoke-NightlyCommand gh (@('release', 'create', $nightlyTag) + $releaseAssets + @('--repo', $releaseRepository,
            '--prerelease', '--title', "SF4 Ember Netplay Nightly $nightlyLabel", '--notes-file', $notesPath)) 'Nightly GitHub release publication failed.' | Out-Host
        $releasesToPrune = @(Get-NightlyReleases | Select-Object -Skip 14 | Where-Object { $_.tagName -cmatch '^v\d+\.\d+\.\d+-nightly\d{8}(\.\d+)?$' })
        foreach ($releaseToPrune in $releasesToPrune) {
            Invoke-NightlyCommand gh @('release', 'delete', $releaseToPrune.tagName, '--repo', $releaseRepository, '--yes', '--cleanup-tag') "Could not prune Nightly $($releaseToPrune.tagName)." | Out-Host
            $prunedTags += $releaseToPrune.tagName
        }
    } else { Write-Host 'WhatIf: no release publication or pruning; the release URL below is planned.' }

    if (!$SkipRoomHosts -and !$WhatIf) {
        # SSH runs a remote shell (and legacy scp may too). The only variable
        # in these fixed remote paths is the validated lowercase SHA-256 above.
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', 'vps', "mkdir -p ~/ember-rooms/builds/$publicBuildId") 'Could not create the VPS staging directory.' | Out-Host
        Invoke-NightlyCommand scp (@('-o', 'BatchMode=yes') + $hostBinaries + @("vps:~/ember-rooms/builds/$publicBuildId/")) 'Could not stage the Nightly room-host pair on vps.' | Out-Host
        & ssh -o BatchMode=yes server1 true
        if ($LASTEXITCODE -eq 0) {
            Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', 'server1', "mkdir -p ~/ember-rooms-box/builds/$publicBuildId") 'Could not create the server1 staging directory.' | Out-Host
            Invoke-NightlyCommand scp (@('-o', 'BatchMode=yes') + $hostBinaries + @("server1:~/ember-rooms-box/builds/$publicBuildId/")) 'Could not stage the Nightly room-host pair on server1.' | Out-Host
        } else { Write-Host 'Skipped server1 staging: BatchMode SSH is unavailable.' }
        Write-Host 'Owner installation command: ssh -t vps "sudo bash ~/ember-rooms/setup.sh"'
    }

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
    Pop-Location
    Stop-Transcript | Out-Null
}
