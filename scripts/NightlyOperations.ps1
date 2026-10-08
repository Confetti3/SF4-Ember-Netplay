#Requires -Version 7
# Import-safe operations. The entry script owns preparation and phase ordering.
function Invoke-NightlyCommand([string]$Command, [string[]]$Arguments, [string]$Failure,
                              [switch]$AllowNotFound, [string]$InputText) {
    if ($InputText) { $result = $InputText | & $Command @Arguments 2>&1 }
    else { $result = & $Command @Arguments 2>&1 }
    if ($LASTEXITCODE -ne 0) {
        $detail = ($result | ForEach-Object { "$_" }) -join "`n"
        if ($AllowNotFound -and $detail -match 'HTTP 404') { return }
        throw "$Failure (exit $LASTEXITCODE): $detail"
    }
    return $result
}

function Assert-NightlyRepository([string]$Repository) {
    if ($Repository -ine 'Confetti3/SF4-Ember-Netplay-Nightly') {
        throw 'Nightly operations are confined to Confetti3/SF4-Ember-Netplay-Nightly.'
    }
}

function Test-NightlyTag([string]$Tag) {
    return $Tag -cmatch '^v[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}-nightly[0-9]{8}(\.[0-9]+)?$'
}

function Select-NightlyReleases([object[]]$Releases) {
    return $Releases | Where-Object { Test-NightlyTag $_.tagName } |
        # A cast, not Parse: ConvertFrom-Json hands a DateTime, and Parse would read
        # its text by the PC's culture, which swaps or refuses day and month where days come first.
        Sort-Object { [DateTimeOffset]$_.createdAt } -Descending
}

function Invoke-NightlyRepositoryQuery([string]$Repository, [string[]]$Arguments,
                                     [string]$Failure, [switch]$WhatIf) {
    Assert-NightlyRepository $Repository
    try { Invoke-NightlyCommand gh $Arguments $Failure }
    catch {
        if ($_.Exception.Message -match 'HTTP 404|Could not resolve to a Repository|Git Repository is empty') {
            if ($WhatIf) { Write-Host "WhatIf: $Repository is unavailable; continuing without remote history/tags."; return }
            throw "Nightly repository $Repository must exist with an initial default-branch commit. $($_.Exception.Message)"
        }
        throw
    }
}

function Get-NightlyReleases([string]$Repository, [switch]$WhatIf) {
    $json = Invoke-NightlyRepositoryQuery $Repository @('release', 'list', '--repo', $Repository,
        '--exclude-drafts', '--limit', '10000', '--json', 'tagName,createdAt') 'Could not list Nightly releases.' -WhatIf:$WhatIf
    $releases = @($json | ConvertFrom-Json)
    if ($releases.Count -ge 10000) { throw 'Nightly release list was truncated; refusing to choose or prune from an incomplete list.' }
    Select-NightlyReleases $releases
}

function Get-NightlyLabel([string]$BaseLabel, [string[]]$ExistingTags, [string]$OutDirectory) {
    $label = $BaseLabel
    $suffix = 2
    while ($true) {
        $destination = Join-Path $OutDirectory "sf4-ember-netplay-$label"
        $localCollision = $false
        foreach ($extension in @('', '.zip', '.zip.sha256', '-setup.exe', '-setup.exe.sha256')) {
            if (Test-Path -LiteralPath "$destination$extension") { $localCollision = $true; break }
        }
        if ($ExistingTags -notcontains "v$label" -and !$localCollision) { return $label }
        $label = "$BaseLabel.$suffix"
        $suffix++
    }
}

function ConvertTo-NightlyWslPath([string]$WindowsPath) {
    if ($WindowsPath -notmatch '^[a-zA-Z]:[\\/]') { throw "Expected an absolute drive path for WSL Ubuntu: $WindowsPath" }
    $fullPath = [IO.Path]::GetFullPath($WindowsPath).Replace('\', '/')
    return '/mnt/' + $fullPath.Substring(0, 1).ToLowerInvariant() + $fullPath.Substring(2)
}

function Invoke-NightlyLinuxBuild([string]$ScriptPath, [string]$Revision, [string]$ArchivePath, [string]$ArtifactsPath) {
    Invoke-NightlyCommand wsl @('-d', 'Ubuntu', '-u', 'kate', '--exec', 'bash', '--',
        (ConvertTo-NightlyWslPath $ScriptPath), $Revision,
        (ConvertTo-NightlyWslPath $ArchivePath), (ConvertTo-NightlyWslPath $ArtifactsPath)) 'Nightly Linux build or libc dependency check failed.'
}

function Assert-NightlyCleanHead([string]$SourceRoot, [string]$Revision = '') {
    $dirty = @(Invoke-NightlyCommand git @('-C', $SourceRoot, 'status', '--porcelain', '--untracked-files=all') 'Could not check worktree cleanliness.')
    if ($dirty.Count) { throw "Nightly requires committed source and no untracked non-ignored files:`n$($dirty -join "`n")" }
    $head = (Invoke-NightlyCommand git @('-C', $SourceRoot, 'rev-parse', 'HEAD') 'Could not resolve HEAD.').Trim()
    if ($head -cnotmatch '^[0-9a-f]{40}$' -or ($Revision -and $head -cne $Revision)) { throw 'Source commit changed during the Nightly run.' }
    return $head
}

function Get-NightlySourceSnapshot([string]$SourceRoot) {
    $revision = Assert-NightlyCleanHead $SourceRoot
    $fingerprint = Get-SourceFingerprint $SourceRoot
    Assert-NightlyCleanHead $SourceRoot $revision | Out-Null
    return [pscustomobject]@{revision=$revision;fingerprint=$fingerprint}
}

function Assert-NightlySource([string]$SourceRoot, [object]$Snapshot) {
    Assert-NightlyCleanHead $SourceRoot $Snapshot.revision | Out-Null
    if ((Get-SourceFingerprint $SourceRoot) -cne $Snapshot.fingerprint) { throw 'Source fingerprint changed from the clean Nightly snapshot.' }
    Assert-NightlyCleanHead $SourceRoot $Snapshot.revision | Out-Null
}

function Assert-NightlyBuildSnapshot([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot,
                                    [object]$Snapshot, [string]$PackageDirectory) {
    $receipt = Assert-BuildReceipt $SourceRoot $BuildRoot $StageRoot
    if ($receipt.sourceFingerprint -cne $Snapshot.fingerprint -or $receipt.baseRevision -cne $Snapshot.revision) {
        throw 'Build receipt does not match the clean Nightly snapshot.'
    }
    if ($PackageDirectory) {
        $packaged = Get-Content -LiteralPath (Join-Path $PackageDirectory 'build-provenance.json') -Raw | ConvertFrom-Json
        if ($packaged.sourceFingerprint -cne $Snapshot.fingerprint -or $packaged.baseRevision -cne $Snapshot.revision) {
            throw 'Packaged receipt does not match the clean Nightly snapshot.'
        }
    }
    Assert-NightlySource $SourceRoot $Snapshot
}

function Get-NightlyArtifact([string]$Path) {
    [pscustomobject]@{path=[IO.Path]::GetFullPath($Path);name=(Split-Path $Path -Leaf);
        sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}

function Assert-NightlyArtifacts([object[]]$Artifacts) {
    foreach ($artifact in $Artifacts) {
        if ((Get-FileHash -LiteralPath $artifact.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $artifact.sha256) {
            throw "Prepared artifact changed: $($artifact.path)"
        }
    }
}

function Save-NightlyState([string]$Path, [object]$State) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, ($State | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $Path, $true)
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}

function Get-NightlyRoomDestinations {
    [pscustomobject]@{hostName='vps';root='ember-rooms'}
    try {
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', 'server1', 'true') 'server1 SSH is unavailable.' | Out-Null
        [pscustomobject]@{hostName='server1';root='ember-rooms-box'}
    } catch { Write-Host 'Skipped server1 staging: BatchMode SSH is unavailable.' }
}

function Invoke-NightlyHostStage([object]$Destination, [string]$BuildId, [object[]]$Binaries,
                                 [string]$SourceRoot, [object]$Snapshot, [object]$Pending, [string]$StatePath) {
    if (($Destination.hostName -ceq 'vps' -and $Destination.root -ceq 'ember-rooms') -or
        ($Destination.hostName -ceq 'server1' -and $Destination.root -ceq 'ember-rooms-box')) { }
    else { throw 'Unexpected Nightly room-host destination.' }
    if ($BuildId -cnotmatch '^[0-9a-f]{64}$' -or $Binaries.Count -ne 2 -or
        $Binaries[0].name -cne 'sf4e-room-host' -or $Binaries[1].name -cne 'sf4-net' -or
        @($Binaries | Where-Object { $_.sha256 -cnotmatch '^[0-9a-f]{64}$' }).Count) { throw 'Invalid room-host pair identity.' }
    Assert-NightlyArtifacts $Binaries
    $recoverUpload = !!$Destination.uploadToken
    if ($recoverUpload -and $Destination.uploadToken -cnotmatch '^[0-9a-f]{32}$') { throw 'Invalid room-host upload token.' }
    if (!$recoverUpload) {
        $Destination | Add-Member -NotePropertyName uploadToken -NotePropertyValue ([Guid]::NewGuid().ToString('N')) -Force
        # Persist ownership before prepare; process death can skip all local cleanup.
        Save-NightlyState $StatePath $Pending
    }
    $token = $Destination.uploadToken
    $payload = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'stage-nightly-room-hosts.sh') -Raw).Replace("`r`n", "`n")
    $remote = "bash -s -- $($Destination.root) $BuildId $($Binaries[0].sha256) $($Binaries[1].sha256) $token"
    $stageError = $null
    try {
        Assert-NightlySource $SourceRoot $Snapshot
        if ($recoverUpload) {
            Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote cleanup") 'Could not recover the previous host upload.' -InputText $payload | Out-Null
            Assert-NightlySource $SourceRoot $Snapshot
        }
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote prepare") 'Could not prepare host upload.' -InputText $payload | Out-Null
        Assert-NightlySource $SourceRoot $Snapshot
        Invoke-NightlyCommand scp (@('-o', 'BatchMode=yes') + @($Binaries | ForEach-Object path) +
            @("$($Destination.hostName):~/$($Destination.root)/.incoming-$BuildId-$token/")) 'Could not upload the Nightly room-host pair.' | Out-Null
        Assert-NightlySource $SourceRoot $Snapshot
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote commit") 'Remote pair verification or atomic staging failed.' -InputText $payload | Out-Null
    } catch { $stageError = $_; throw }
    finally {
        # Removing our exact temporary resource also applies when source checks fail.
        # Keep the persisted token until remote cleanup has been confirmed.
        try {
            Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote cleanup") 'Could not clean the host upload.' -InputText $payload | Out-Null
            $Destination.uploadToken = ''
            Save-NightlyState $StatePath $Pending
        } catch {
            if (!$stageError) { throw }
            Write-Warning "Upload cleanup failed; retry using the recorded token in ${StatePath}: $($_.Exception.Message)"
        }
    }
}

function Get-NightlyRelease([string]$Repository, [string]$Tag) {
    Assert-NightlyRepository $Repository
    if (!(Test-NightlyTag $Tag)) { throw 'Invalid Nightly tag.' }
    # REST's by-tag endpoint finds published releases only. Resolve pending draft
    # tags through GraphQL, then read full REST metadata by database ID, as gh does:
    # https://github.com/cli/cli/blob/trunk/pkg/cmd/release/shared/fetch.go
    $owner, $name = $Repository.Split('/')
    $query = 'query($owner: String!, $name: String!, $tag: String!) { repository(owner: $owner, name: $name) { release(tagName: $tag) { databaseId } } }'
    $json = Invoke-NightlyCommand gh @('api', 'graphql', '-f', "query=$query", '-f', "owner=$owner",
        '-f', "name=$name", '-f', "tag=$Tag") 'Could not resolve Nightly release identity.'
    $lookup = $json | ConvertFrom-Json
    if ($lookup.errors -or !$lookup.data.repository) { throw 'Nightly repository/release lookup failed.' }
    $id = $lookup.data.repository.release.databaseId
    if (!$id) { return }
    if ("$id" -notmatch '^[1-9][0-9]*$') { throw 'Invalid Nightly release database ID.' }
    $json = Invoke-NightlyCommand gh @('api', "repos/$Repository/releases/$id") 'Could not inspect Nightly release.'
    return $json | ConvertFrom-Json
}

function Assert-NightlyReleaseIdentity([object]$Release, [object]$Pending, [switch]$AllowIncomplete) {
    $notes = (Get-Content -LiteralPath $Pending.notes.path -Raw).Replace("`r`n", "`n").TrimEnd()
    if ($Release.tag_name -cne $Pending.tag -or !$Release.prerelease -or
        ([string]$Release.body).Replace("`r`n", "`n").TrimEnd() -cne $notes) { throw 'Existing release does not match the prepared Nightly identity.' }
    foreach ($asset in $Release.assets) {
        $expected = @($Pending.assets | Where-Object name -CEQ $asset.name)
        if ($expected.Count -ne 1) { throw "Unexpected remote asset: $($asset.name)" }
        if ($AllowIncomplete -and $Release.draft -and $asset.state -ceq 'starter' -and $asset.size -eq 0) { continue }
        if ($asset.digest -cne "sha256:$($expected[0].sha256)") {
            throw "Remote asset identity is missing or different: $($asset.name)"
        }
    }
}

function Publish-NightlyRelease([object]$Pending, [string]$SourceRoot) {
    Assert-NightlyRepository $Pending.repository
    if ($Pending.phase -cne 'staged') { throw 'Nightly publication requires verified staging first.' }
    Assert-NightlyArtifacts (@($Pending.assets) + @($Pending.notes))
    $release = Get-NightlyRelease $Pending.repository $Pending.tag
    if (!$release) {
        # A tag without our release is a collision, never a recovery target.
        $ref = Invoke-NightlyCommand gh @('api', "repos/$($Pending.repository)/git/ref/tags/$($Pending.tag)") 'Could not inspect Nightly tag.' -AllowNotFound
        if ($ref) { throw 'Prepared Nightly tag already exists without its release; refusing to reuse it.' }
        Assert-NightlySource $SourceRoot $Pending.snapshot
        Invoke-NightlyCommand gh @('release', 'create', $Pending.tag, '--repo', $Pending.repository,
            '--draft', '--prerelease', '--title', "SF4 Ember Netplay Nightly $($Pending.label)", '--notes-file', $Pending.notes.path) 'Could not create the Nightly draft.' | Out-Null
        $release = Get-NightlyRelease $Pending.repository $Pending.tag
        if (!$release) { throw 'Created Nightly draft could not be read back.' }
    }
    Assert-NightlyReleaseIdentity $release $Pending -AllowIncomplete
    # GitHub can leave a zero-byte starter asset when an upload is interrupted.
    # Repair only that incomplete asset in our draft; never replace public bytes.
    foreach ($asset in @($release.assets)) {
        if ($release.draft -and $asset.state -ceq 'starter' -and $asset.size -eq 0 -and
            @($Pending.assets | Where-Object name -CEQ $asset.name).Count -eq 1) {
            if ("$($asset.id)" -notmatch '^[1-9][0-9]*$') { throw 'Invalid incomplete draft asset ID.' }
            Assert-NightlySource $SourceRoot $Pending.snapshot
            Invoke-NightlyCommand gh @('api', '--method', 'DELETE',
                "repos/$($Pending.repository)/releases/assets/$($asset.id)") 'Could not remove incomplete Nightly draft upload.' | Out-Null
        }
    }
    $release = Get-NightlyRelease $Pending.repository $Pending.tag
    Assert-NightlyReleaseIdentity $release $Pending
    foreach ($asset in $Pending.assets) {
        if (@($release.assets | Where-Object name -CEQ $asset.name).Count) { continue }
        if (!$release.draft) { throw 'Published Nightly has missing assets; refusing to change public artifacts.' }
        Assert-NightlySource $SourceRoot $Pending.snapshot
        Invoke-NightlyCommand gh @('release', 'upload', $Pending.tag, $asset.path, '--repo', $Pending.repository) 'Could not upload Nightly draft asset.' | Out-Null
    }
    $release = Get-NightlyRelease $Pending.repository $Pending.tag
    Assert-NightlyReleaseIdentity $release $Pending
    if (@($release.assets).Count -ne @($Pending.assets).Count) { throw 'Nightly draft asset set is incomplete.' }
    if ($release.draft) {
        Assert-NightlySource $SourceRoot $Pending.snapshot
        Invoke-NightlyCommand gh @('release', 'edit', $Pending.tag, '--repo', $Pending.repository, '--draft=false') 'Nightly GitHub publication failed.' | Out-Null
        $release = Get-NightlyRelease $Pending.repository $Pending.tag
        Assert-NightlyReleaseIdentity $release $Pending
        if ($release.draft -or @($release.assets).Count -ne @($Pending.assets).Count) { throw 'Nightly publication could not be confirmed.' }
    }
    return "https://github.com/$($Pending.repository)/releases/tag/$($Pending.tag)"
}

function Invoke-NightlyRetention([string]$Repository, [string]$CleanupPath,
                                 [string]$SourceRoot, [object]$Snapshot) {
    Assert-NightlyRepository $Repository
    $cleanup = [pscustomobject]@{repository=$Repository;targets=@()}
    if (Test-Path -LiteralPath $CleanupPath) { $cleanup = Get-Content -LiteralPath $CleanupPath -Raw | ConvertFrom-Json }
    if ($cleanup.repository -cne $Repository) { throw 'Pending cleanup repository mismatch.' }
    # Replay only explicitly recorded targets, including tags whose release is gone.
    $pruned = @()
    while ($true) {
        foreach ($target in @($cleanup.targets)) {
            if (!(Test-NightlyTag $target.tag)) { throw 'Refusing to prune a non-Nightly tag.' }
            $release = Get-NightlyRelease $Repository $target.tag
            if ($release) {
                if ($release.id -ne $target.releaseId -or $release.tag_name -cne $target.tag -or $release.draft) { throw 'Pending cleanup release identity changed.' }
                Assert-NightlySource $SourceRoot $Snapshot
                Invoke-NightlyCommand gh @('release', 'delete', $target.tag, '--repo', $Repository, '--yes') 'Could not prune Nightly release.' | Out-Null
            }
            $ref = Invoke-NightlyCommand gh @('api', "repos/$Repository/git/ref/tags/$($target.tag)") 'Could not inspect pending cleanup tag.' -AllowNotFound
            if ($ref) {
                if (($ref | ConvertFrom-Json).object.sha -cne $target.tagObjectSha) { throw 'Pending cleanup tag identity changed.' }
                Assert-NightlySource $SourceRoot $Snapshot
                Invoke-NightlyCommand gh @('api', '--method', 'DELETE', "repos/$Repository/git/refs/tags/$($target.tag)") 'Could not prune Nightly tag.' | Out-Null
            }
            $cleanup.targets = @($cleanup.targets | Where-Object tag -CNE $target.tag)
            Save-NightlyState $CleanupPath $cleanup
            $pruned += $target.tag
        }
        $older = @(Get-NightlyReleases $Repository | Select-Object -Skip 14)
        if (!$older.Count) { break }
        foreach ($item in $older) {
            if (!(Test-NightlyTag $item.tagName)) { throw 'Refusing to prune a non-Nightly tag.' }
            $release = Get-NightlyRelease $Repository $item.tagName
            if (!$release -or $release.draft) { throw 'Retention candidate changed; retry with a fresh release list.' }
            $ref = Invoke-NightlyCommand gh @('api', "repos/$Repository/git/ref/tags/$($item.tagName)") 'Could not read retention tag identity.' -AllowNotFound
            $tagSha = if ($ref) { ($ref | ConvertFrom-Json).object.sha } else { '' }
            $cleanup.targets += [pscustomobject]@{tag=$item.tagName;releaseId=$release.id;tagObjectSha=$tagSha}
        }
        # Save the entire narrow deletion intent before the first external deletion.
        Save-NightlyState $CleanupPath $cleanup
    }
    if (Test-Path -LiteralPath $CleanupPath) { Remove-Item -LiteralPath $CleanupPath }
    return $pruned
}

function Complete-NightlyPublication([object]$Pending, [string]$StatePath, [string]$CleanupPath,
                                     [string]$SourceRoot, [object]$CurrentSnapshot) {
    Assert-NightlyRepository $Pending.repository
    if (!(Test-NightlyTag $Pending.tag) -or $Pending.tag -cne "v$($Pending.label)" -or
        $Pending.phase -notin @('prepared', 'staged', 'published')) { throw 'Invalid pending Nightly state.' }
    if ($Pending.phase -ne 'published') {
        if (@($Pending.binaries).Count -and
            @($Pending.destinations | Where-Object { $_.hostName -ceq 'vps' -and $_.root -ceq 'ember-rooms' }).Count -ne 1) {
            throw 'Pending room-host work must include the mandatory VPS destination.'
        }
        Assert-NightlySource $SourceRoot $Pending.snapshot
        Assert-NightlyArtifacts (@($Pending.assets) + @($Pending.notes) + @($Pending.binaries))
        # Repeat staging after an interruption. Identical pairs are accepted.
        foreach ($destination in $Pending.destinations) {
            Invoke-NightlyHostStage $destination $Pending.buildId $Pending.binaries $SourceRoot $Pending.snapshot $Pending $StatePath
        }
        $Pending.phase = 'staged'
        Save-NightlyState $StatePath $Pending
        $url = Publish-NightlyRelease $Pending $SourceRoot
        $Pending.phase = 'published'
        Save-NightlyState $StatePath $Pending
        Write-Host "Published: $url"
    }
    $pruned = @(Invoke-NightlyRetention $Pending.repository $CleanupPath $SourceRoot $CurrentSnapshot)
    Remove-Item -LiteralPath $StatePath
    return $pruned
}
