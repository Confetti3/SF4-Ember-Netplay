#Requires -Version 7
# Import-safe operations. Invoke-NightlyPublish owns preparation and phase ordering;
# the entry script supplies the lock, transcript and the build/package operations.
function Invoke-NightlyNative([string]$Command, [string[]]$Arguments, [string]$InputText) {
    if ($InputText) { $output = $InputText | & $Command @Arguments 2>&1 }
    else { $output = & $Command @Arguments 2>&1 }
    return [pscustomobject]@{exitCode=$LASTEXITCODE;output=@($output)}
}

function Invoke-NightlyCommand([string]$Command, [string[]]$Arguments, [string]$Failure, [string]$InputText) {
    $result = Invoke-NightlyNative $Command $Arguments $InputText
    if ($result.exitCode -ne 0) {
        throw "$Failure (exit $($result.exitCode)): $(($result.output | ForEach-Object { "$_" }) -join "`n")"
    }
    return $result.output
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

# One GraphQL read of the Nightly repository, classified as found, absent or error.
# Absence is decided from the response JSON, never from error text: GraphQL returns null
# fields for a missing tag or release, and a NOT_FOUND error on the repository field
# when the repository itself is missing. Authentication, network and any other
# failures stay errors, so they can never be mistaken for an empty history.
function Invoke-NightlyQuery([string]$Repository, [string]$Query, [hashtable]$Variables = @{}) {
    Assert-NightlyRepository $Repository
    $owner, $name = $Repository.Split('/')
    $arguments = @('api', 'graphql', '-f', "query=$Query", '-f', "owner=$owner", '-f', "name=$name")
    foreach ($key in @($Variables.Keys | Sort-Object)) { $arguments += @('-f', "$key=$($Variables[$key])") }
    $result = Invoke-NightlyNative gh $arguments
    $stdout = @($result.output | Where-Object { $_ -isnot [Management.Automation.ErrorRecord] } | ForEach-Object { "$_" }) -join "`n"
    $response = $null
    if ($stdout.Trim()) { try { $response = $stdout | ConvertFrom-Json } catch { $response = $null } }
    $status = 'error'
    if ($response -and $response.data) {
        $errors = @($response.errors | Where-Object { $_ })
        if ($result.exitCode -eq 0 -and !$errors.Count -and $null -ne $response.data.repository) { $status = 'found' }
        elseif ($null -eq $response.data.repository -and $errors.Count -and
                !@($errors | Where-Object { $_.type -cne 'NOT_FOUND' -or "$($_.path)" -cne 'repository' }).Count) { $status = 'absent' }
    }
    return [pscustomobject]@{status=$status;data=$response.data;
        detail="exit $($result.exitCode): $(($result.output | ForEach-Object { "$_" }) -join "`n")"}
}

# A repository without a default-branch commit has no history to read and cannot take tags.
function Get-NightlyRepositoryState([string]$Repository) {
    $outcome = Invoke-NightlyQuery $Repository 'query($owner: String!, $name: String!) { repository(owner: $owner, name: $name) { defaultBranchRef { name } } }'
    if ($outcome.status -ceq 'found' -and !$outcome.data.repository.defaultBranchRef) {
        return [pscustomobject]@{status='absent';detail='The repository has no default-branch commit.'}
    }
    return $outcome
}

function Get-NightlyReleases([string]$Repository) {
    Assert-NightlyRepository $Repository
    $json = Invoke-NightlyCommand gh @('release', 'list', '--repo', $Repository,
        '--exclude-drafts', '--limit', '10000', '--json', 'tagName,createdAt') 'Could not list Nightly releases.'
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

# Compares a snapshot already taken in this run with the one a pending record was prepared from.
function Assert-NightlyPendingSource([object]$Pending, [object]$Snapshot) {
    if ($Snapshot.revision -cne $Pending.snapshot.revision) { throw 'Source commit changed from the pending Nightly snapshot.' }
    if ($Snapshot.fingerprint -cne $Pending.snapshot.fingerprint) { throw 'Source fingerprint changed from the clean Nightly snapshot.' }
}

function Assert-NightlyBuildSnapshot([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot,
                                    [object]$Snapshot, [string]$PackageDirectory) {
    # The receipt check fingerprints the tree itself; tying the receipt to the snapshot
    # covers the content, so only HEAD is left to compare here.
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
    Assert-NightlyCleanHead $SourceRoot $Snapshot.revision | Out-Null
}

function Get-NightlyArtifact([string]$Path) {
    [pscustomobject]@{path=[IO.Path]::GetFullPath($Path);name=(Split-Path $Path -Leaf);
        sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}

function Assert-NightlyArtifacts([object[]]$Artifacts) {
    foreach ($artifact in @($Artifacts | Where-Object { $_ })) {
        if ((Get-FileHash -LiteralPath $artifact.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $artifact.sha256) {
            throw "Prepared artifact changed: $($artifact.path)"
        }
    }
}

# Hashes the bytes it returns, so nothing can change between the check and the use.
function Read-NightlyPinnedText([object]$Artifact) {
    $bytes = [IO.File]::ReadAllBytes($Artifact.path)
    if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant() -cne $Artifact.sha256) {
        throw "Prepared artifact changed: $($Artifact.path)"
    }
    return [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
}

function Save-NightlyState([string]$Path, [object]$State) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, ($State | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $Path, $true)
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}

# Each destination gets its upload token here, so it is persisted with the pending
# record before any remote operation and owns one exact temporary directory for the
# lifetime of that record.
function Get-NightlyRoomDestinations {
    [pscustomobject]@{hostName='vps';root='ember-rooms';uploadToken=[Guid]::NewGuid().ToString('N')}
    try {
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', 'server1', 'true') 'server1 SSH is unavailable.' | Out-Null
        [pscustomobject]@{hostName='server1';root='ember-rooms-box';uploadToken=[Guid]::NewGuid().ToString('N')}
    } catch { Write-Host 'Skipped server1 staging: BatchMode SSH is unavailable.' }
}

function Invoke-NightlyHostStage([object]$Destination, [string]$BuildId, [object[]]$Binaries, [object]$StagingScript) {
    if (($Destination.hostName -ceq 'vps' -and $Destination.root -ceq 'ember-rooms') -or
        ($Destination.hostName -ceq 'server1' -and $Destination.root -ceq 'ember-rooms-box')) { }
    else { throw 'Unexpected Nightly room-host destination.' }
    if ($BuildId -cnotmatch '^[0-9a-f]{64}$' -or $Binaries.Count -ne 2 -or
        $Binaries[0].name -cne 'sf4e-room-host' -or $Binaries[1].name -cne 'sf4-net' -or
        @($Binaries | Where-Object { $_.sha256 -cnotmatch '^[0-9a-f]{64}$' }).Count) { throw 'Invalid room-host pair identity.' }
    $token = $Destination.uploadToken
    if ($token -cnotmatch '^[0-9a-f]{32}$') { throw 'Invalid room-host upload token.' }
    Assert-NightlyArtifacts $Binaries
    # The prepared copy, not the checkout: the source may move on once the record exists.
    $payload = Read-NightlyPinnedText $StagingScript
    $remote = "bash -s -- $($Destination.root) $BuildId $($Binaries[0].sha256) $($Binaries[1].sha256) $token"
    $stageError = $null
    try {
        # The token outlives interrupted runs, so start by removing whatever an earlier
        # attempt left in its directory. Cleanup is idempotent.
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote cleanup") 'Could not recover the previous host upload.' -InputText $payload | Out-Null
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote prepare") 'Could not prepare host upload.' -InputText $payload | Out-Null
        Invoke-NightlyCommand scp (@('-o', 'BatchMode=yes') + @($Binaries | ForEach-Object path) +
            @("$($Destination.hostName):~/$($Destination.root)/.incoming-$BuildId-$token/")) 'Could not upload the Nightly room-host pair.' | Out-Null
        Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote commit") 'Remote pair verification or atomic staging failed.' -InputText $payload | Out-Null
    } catch { $stageError = $_; throw }
    finally {
        # The phase advances only after this confirmed cleanup; until then the recorded
        # token lets a retry find and remove exactly this directory.
        try {
            Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $Destination.hostName, "$remote cleanup") 'Could not clean the host upload.' -InputText $payload | Out-Null
        } catch {
            if (!$stageError) { throw }
            Write-Warning "Upload cleanup failed; a retry removes it using the recorded token: $($_.Exception.Message)"
        }
    }
}

# The release and tag behind one Nightly tag. GraphQL finds draft releases too (REST's
# by-tag endpoint only finds published ones), then full REST metadata is read by
# database ID, as gh does: https://github.com/cli/cli/blob/trunk/pkg/cmd/release/shared/fetch.go
function Get-NightlyTagState([string]$Repository, [string]$Tag) {
    if (!(Test-NightlyTag $Tag)) { throw 'Invalid Nightly tag.' }
    $query = 'query($owner: String!, $name: String!, $tag: String!, $ref: String!) { repository(owner: $owner, name: $name) { release(tagName: $tag) { databaseId } ref(qualifiedName: $ref) { target { oid } } } }'
    $outcome = Invoke-NightlyQuery $Repository $query @{tag=$Tag;ref="refs/tags/$Tag"}
    if ($outcome.status -cne 'found') { throw "Could not resolve Nightly release identity ($($outcome.status)): $($outcome.detail)" }
    $release = $null
    $id = $outcome.data.repository.release.databaseId
    if ($id) {
        if ("$id" -notmatch '^[1-9][0-9]*$') { throw 'Invalid Nightly release database ID.' }
        $release = Invoke-NightlyCommand gh @('api', "repos/$Repository/releases/$id") 'Could not inspect Nightly release.' | ConvertFrom-Json
    }
    return [pscustomobject]@{release=$release;tagSha=[string]$outcome.data.repository.ref.target.oid}
}

# The notes exactly as hashed when the record was prepared, read once per publication.
function Read-NightlyNotes([object]$Pending) {
    return (Read-NightlyPinnedText $Pending.notes).TrimStart([char]0xFEFF).Replace("`r`n", "`n").TrimEnd()
}

function Assert-NightlyReleaseIdentity([object]$Release, [object]$Pending, [string]$Notes, [switch]$AllowIncomplete) {
    if ($Release.tag_name -cne $Pending.tag -or !$Release.prerelease -or
        ([string]$Release.body).Replace("`r`n", "`n").TrimEnd() -cne $Notes) { throw 'Existing release does not match the prepared Nightly identity.' }
    foreach ($asset in $Release.assets) {
        $expected = @($Pending.assets | Where-Object name -CEQ $asset.name)
        if ($expected.Count -ne 1) { throw "Unexpected remote asset: $($asset.name)" }
        if ($AllowIncomplete -and $Release.draft -and $asset.state -ceq 'starter' -and $asset.size -eq 0) { continue }
        if ($asset.digest -cne "sha256:$($expected[0].sha256)") {
            throw "Remote asset identity is missing or different: $($asset.name)"
        }
    }
}

# Decides the one remote step that moves the observed release towards the prepared one.
function Get-NightlyNextStep([object]$State, [object]$Pending, [string]$Notes) {
    $release = $State.release
    if (!$release) {
        # A tag without our release is a collision, never a recovery target.
        if ($State.tagSha) { throw 'Prepared Nightly tag already exists without its release; refusing to reuse it.' }
        return [pscustomobject]@{action='create';target=$Pending.tag}
    }
    Assert-NightlyReleaseIdentity $release $Pending $Notes -AllowIncomplete
    # GitHub can leave a zero-byte starter asset when an upload is interrupted.
    # Repair only that incomplete asset in our draft; never replace public bytes.
    foreach ($asset in @($release.assets)) {
        if ($release.draft -and $asset.state -ceq 'starter' -and $asset.size -eq 0) {
            if ("$($asset.id)" -notmatch '^[1-9][0-9]*$') { throw 'Invalid incomplete draft asset ID.' }
            return [pscustomobject]@{action='delete-asset';target="$($asset.id)"}
        }
    }
    foreach ($asset in $Pending.assets) {
        if (@($release.assets | Where-Object name -CEQ $asset.name).Count) { continue }
        if (!$release.draft) { throw 'Published Nightly has missing assets; refusing to change public artifacts.' }
        return [pscustomobject]@{action='upload';target=$asset.path}
    }
    if (@($release.assets).Count -ne @($Pending.assets).Count) { throw 'Nightly draft asset set is incomplete.' }
    if ($release.draft) { return [pscustomobject]@{action='publish';target=$Pending.tag} }
    return [pscustomobject]@{action='done';target=$Pending.tag}
}

function Publish-NightlyRelease([object]$Pending) {
    Assert-NightlyRepository $Pending.repository
    if ($Pending.phase -cne 'staged') { throw 'Nightly publication requires verified staging first.' }
    Assert-NightlyArtifacts @($Pending.assets)
    # The draft is created from, and compared with, this verified text; the file is not read again.
    $notes = Read-NightlyNotes $Pending
    $repository = $Pending.repository
    $previous = $null
    # Read, take one step, read back. The read after a mutation is its verification:
    # a step that is still due right after it ran was not confirmed.
    for ($pass = 0; $pass -lt 2 * @($Pending.assets).Count + 4; $pass++) {
        $step = Get-NightlyNextStep (Get-NightlyTagState $repository $Pending.tag) $Pending $notes
        if ($step.action -ceq 'done') { return "https://github.com/$repository/releases/tag/$($Pending.tag)" }
        if ($previous -and $previous.action -ceq $step.action -and $previous.target -ceq $step.target) {
            throw "Nightly $($step.action) of $($step.target) could not be confirmed by reading the release back."
        }
        switch ($step.action) {
            'create' {
                Invoke-NightlyCommand gh @('release', 'create', $Pending.tag, '--repo', $repository,
                    '--draft', '--prerelease', '--title', "SF4 Ember Netplay Nightly $($Pending.label)", '--notes', $notes) 'Could not create the Nightly draft.' | Out-Null
            }
            'delete-asset' {
                Invoke-NightlyCommand gh @('api', '--method', 'DELETE',
                    "repos/$repository/releases/assets/$($step.target)") 'Could not remove incomplete Nightly draft upload.' | Out-Null
            }
            'upload' {
                Invoke-NightlyCommand gh @('release', 'upload', $Pending.tag, $step.target, '--repo', $repository) 'Could not upload Nightly draft asset.' | Out-Null
            }
            'publish' {
                Invoke-NightlyCommand gh @('release', 'edit', $Pending.tag, '--repo', $repository, '--draft=false') 'Nightly GitHub publication failed.' | Out-Null
            }
        }
        $previous = $step
    }
    throw 'Nightly publication did not settle; inspect the draft before retrying.'
}

# Both recorded identities are checked on every read, before any deletion: a replaced
# release or a repointed tag stops the run with nothing deleted.
function Get-NightlyRetentionStep([object]$State, [object]$Target) {
    $release = $State.release
    if ($release -and ($release.id -ne $Target.releaseId -or $release.tag_name -cne $Target.tag -or $release.draft)) {
        throw 'Pending cleanup release identity changed.'
    }
    if ($State.tagSha -and $State.tagSha -cne $Target.tagObjectSha) { throw 'Pending cleanup tag identity changed.' }
    if ($release) { return 'delete-release' }
    if ($State.tagSha) { return 'delete-tag' }
    return 'done'
}

# Read, delete one thing, read back, until the target is confirmed absent.
function Remove-NightlyRetentionTarget([string]$Repository, [object]$Target) {
    if (!(Test-NightlyTag $Target.tag)) { throw 'Refusing to prune a non-Nightly tag.' }
    if ("$($Target.releaseId)" -notmatch '^[1-9][0-9]*$') { throw 'Invalid pending cleanup release ID.' }
    $previous = ''
    for ($pass = 0; $pass -lt 4; $pass++) {
        $step = Get-NightlyRetentionStep (Get-NightlyTagState $Repository $Target.tag) $Target
        if ($step -ceq 'done') { return }
        if ($step -ceq $previous) { throw "Nightly retention $step of $($Target.tag) could not be confirmed by reading it back." }
        if ($step -ceq 'delete-release') {
            # By the recorded ID, never by tag: a release that replaced it is not ours to delete.
            Invoke-NightlyCommand gh @('api', '--method', 'DELETE', "repos/$Repository/releases/$($Target.releaseId)") 'Could not prune Nightly release.' | Out-Null
        } else {
            Invoke-NightlyCommand gh @('api', '--method', 'DELETE', "repos/$Repository/git/refs/tags/$($Target.tag)") 'Could not prune Nightly tag.' | Out-Null
        }
        $previous = $step
    }
    throw "Nightly retention of $($Target.tag) did not settle."
}

# Works only from recorded identities and the remote state, never the local checkout.
# One run finishes one saved plan; releases that arrive meanwhile wait for the next run.
function Invoke-NightlyRetention([string]$Repository, [string]$CleanupPath) {
    Assert-NightlyRepository $Repository
    if (Test-Path -LiteralPath $CleanupPath) {
        # Replay only explicitly recorded targets, including tags whose release is gone.
        $cleanup = Get-Content -LiteralPath $CleanupPath -Raw | ConvertFrom-Json
        if ($cleanup.repository -cne $Repository) { throw 'Pending cleanup repository mismatch.' }
    } else {
        $targets = @(foreach ($item in @(Get-NightlyReleases $Repository | Select-Object -Skip 14)) {
            if (!(Test-NightlyTag $item.tagName)) { throw 'Refusing to prune a non-Nightly tag.' }
            $state = Get-NightlyTagState $Repository $item.tagName
            if (!$state.release -or $state.release.draft) { throw 'Retention candidate changed; retry with a fresh release list.' }
            [pscustomobject]@{tag=$item.tagName;releaseId=$state.release.id;tagObjectSha=$state.tagSha}
        })
        if (!$targets.Count) { return @() }
        # Save the entire narrow deletion intent before the first external deletion.
        $cleanup = [pscustomobject]@{repository=$Repository;targets=$targets}
        Save-NightlyState $CleanupPath $cleanup
    }
    $pruned = @()
    foreach ($target in @($cleanup.targets)) {
        Remove-NightlyRetentionTarget $Repository $target
        # Retired only once the read-back shows neither the release nor the tag.
        $cleanup.targets = @($cleanup.targets | Where-Object tag -CNE $target.tag)
        Save-NightlyState $CleanupPath $cleanup
        $pruned += $target.tag
    }
    Remove-Item -LiteralPath $CleanupPath
    return $pruned
}

# $Snapshot is the clean snapshot this run captured. Unfinished work is checked against
# it once here; after that every remote step uses only the pinned, hashed files.
function Complete-NightlyPublication([object]$Pending, [string]$StatePath, [string]$CleanupPath, [object]$Snapshot) {
    Assert-NightlyRepository $Pending.repository
    if (!(Test-NightlyTag $Pending.tag) -or $Pending.tag -cne "v$($Pending.label)" -or
        $Pending.phase -notin @('prepared', 'staged', 'published')) { throw 'Invalid pending Nightly state.' }
    if ($Pending.phase -ne 'published') {
        if (@($Pending.binaries).Count -and
            @($Pending.destinations | Where-Object { $_.hostName -ceq 'vps' -and $_.root -ceq 'ember-rooms' }).Count -ne 1) {
            throw 'Pending room-host work must include the mandatory VPS destination.'
        }
        if (@($Pending.binaries).Count -and $Pending.stagingScript.sha256 -cnotmatch '^[0-9a-f]{64}$') {
            throw 'Pending room-host work must include its prepared staging script.'
        }
        Assert-NightlyPendingSource $Pending $Snapshot
        Assert-NightlyArtifacts (@($Pending.assets) + @($Pending.notes) + @($Pending.binaries) + @($Pending.stagingScript))
        # Repeat staging after an interruption. Identical pairs are accepted.
        foreach ($destination in $Pending.destinations) {
            Invoke-NightlyHostStage $destination $Pending.buildId $Pending.binaries $Pending.stagingScript
        }
        $Pending.phase = 'staged'
        Save-NightlyState $StatePath $Pending
        $url = Publish-NightlyRelease $Pending
        $Pending.phase = 'published'
        Save-NightlyState $StatePath $Pending
        Write-Host "Published: $url"
    }
    $pruned = @(Invoke-NightlyRetention $Pending.repository $CleanupPath)
    Remove-Item -LiteralPath $StatePath
    return $pruned
}

# The whole Nightly run below the lock and transcript. $Operations supplies everything
# that builds, packages or inspects the PC (scriptblocks: Target, GameRunning, Now,
# ToolPaths, Build, Package, Installer, LinuxBuild); git, gh, ssh and scp go through
# Invoke-NightlyCommand.
function Invoke-NightlyPublish([string]$CheckoutRoot, [string]$OutDirectory, [hashtable]$Operations,
                               [switch]$WhatIf, [switch]$Local, [switch]$Force, [switch]$SkipRoomHosts,
                               [string]$VisualStudioPath, [string]$DiscordSdkArchive) {
    $statePath = Join-Path $OutDirectory 'nightly-pending.json'
    $cleanupPath = Join-Path $OutDirectory 'nightly-cleanup.json'
    $nightlyTarget = & $Operations.Target $CheckoutRoot
    if ($nightlyTarget.channel -ne 'nightly') {
        throw 'Not the Nightly checkout: build-target.json channels.nightly must be the designated target, and this script must run from it.'
    }
    $nightlyBranch = [string]$nightlyTarget.branch
    if ($nightlyBranch -ne 'nightly') { throw 'channels.nightly.branch must be nightly.' }
    $releaseRepository = [string]$nightlyTarget.githubRepo
    # Keep every publication and deletion confined to the assets-only repository.
    if ($releaseRepository -ine 'Confetti3/SF4-Ember-Netplay-Nightly') { throw 'channels.nightly.githubRepo must be Confetti3/SF4-Ember-Netplay-Nightly; refusing to touch another repository.' }
    $branch = Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'branch', '--show-current') 'Could not read the current branch.'
    if ($branch -ne $nightlyBranch) { throw "Checkout must be on $nightlyBranch, currently: $branch" }
    if (& $Operations.GameRunning) {
        Write-Host 'Skipped: SSFIV.exe is running; close the game before packaging.'
        return
    }
    Assert-NightlyCleanHead $CheckoutRoot | Out-Null
    $pending = if (Test-Path -LiteralPath $statePath) {
        Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    } else { $null }
    if ($pending -and $pending.phase -ne 'published') {
        # Do not advance HEAD beyond an artifact set that still needs completion.
        Write-Host "Resuming $($pending.tag) at $($pending.phase); no fetch or rebuild (including -Force)."
    } elseif ($Local) {
        Write-Host 'Local: using checkout HEAD as-is; no fetch or fast-forward.'
    } else {
        Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'fetch', 'origin', $nightlyBranch) 'Fetching the Nightly branch failed.' | Out-Host
        Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'merge', '--ff-only', "origin/$nightlyBranch") 'Nightly cannot fast-forward; resolve the divergence by hand.' | Out-Host
    }
    # Capture only after synchronization, and check cleanliness around hashing.
    $snapshot = Get-NightlySourceSnapshot $CheckoutRoot
    $sourceRevision = $snapshot.revision
    Write-Host "Nightly source: $sourceRevision"
    if ($pending) {
        if ($WhatIf) {
            if ($pending.phase -ne 'published') {
                Assert-NightlyPendingSource $pending $snapshot
                Assert-NightlyArtifacts (@($pending.assets) + @($pending.notes) + @($pending.binaries) + @($pending.stagingScript))
            }
            Write-Host "WhatIf: would finish $($pending.tag) and retention; preserving pending work."
        } else {
            $pruned = @(Complete-NightlyPublication $pending $statePath $cleanupPath $snapshot)
            Write-Host "Completed $($pending.tag). Pruned tags: $($pruned -join ', ')"
        }
        return
    }

    # Only a dry run may continue without the repository; it then has no remote
    # history or tags. Authentication and network failures are errors either way.
    $repositoryState = Get-NightlyRepositoryState $releaseRepository
    $repositoryFound = $repositoryState.status -ceq 'found'
    if ($repositoryState.status -ceq 'error') { throw "Could not inspect Nightly repository $releaseRepository. $($repositoryState.detail)" }
    if (!$repositoryFound) {
        if (!$WhatIf) { throw "Nightly repository $releaseRepository must exist with an initial default-branch commit. $($repositoryState.detail)" }
        Write-Host "WhatIf: $releaseRepository is unavailable; continuing without remote history/tags."
    }
    $previousSource = ''
    $publishedReleases = if ($repositoryFound) { @(Get-NightlyReleases $releaseRepository) } else { @() }
    if ($publishedReleases.Count) {
        $previousRelease = $publishedReleases[0]
        $bodyJson = Invoke-NightlyCommand gh @('release', 'view', $previousRelease.tagName, '--repo', $releaseRepository, '--json', 'body') 'Could not read the previous Nightly release.'
        $body = ($bodyJson | ConvertFrom-Json).body
        $sourceLines = [regex]::Matches($body, '(?m)^Source: ([0-9a-fA-F]{40})\r?$')
        if ($sourceLines.Count -ne 1) { throw "Previous Nightly $($previousRelease.tagName) must have one Source: <full sha> line." }
        $previousSource = $sourceLines[0].Groups[1].Value.ToLowerInvariant()
        if ($previousSource -eq $sourceRevision -and !$Force) {
            if (!$WhatIf) {
                $pruned = @(Invoke-NightlyRetention $releaseRepository $cleanupPath)
                Write-Host "Retention complete. Pruned tags: $($pruned -join ', ')"
            }
            Write-Host "Skipped build: nothing new since $($previousRelease.tagName) ($sourceRevision)."
            return
        }
    } else { Write-Host 'No previous Nightly release; collecting notes since origin/release.' }

    $versionLines = @(Get-Content -LiteralPath (Join-Path $CheckoutRoot 'CMakeLists.txt') | Where-Object { $_ -match '^\s+VERSION ' })
    if ($versionLines.Count -ne 1 -or $versionLines[0].Trim() -notmatch '^VERSION ([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9})$') {
        throw 'Expected one VERSION X.Y.Z line in CMakeLists.txt.'
    }
    $baseLabel = "$($Matches[1])-nightly$((& $Operations.Now).ToUniversalTime().ToString('yyyyMMdd', [cultureinfo]::InvariantCulture))"
    # Include tags without releases as well.
    $existingTags = @()
    if ($repositoryFound) {
        $tagRefs = Invoke-NightlyCommand gh @('api', "repos/$releaseRepository/git/matching-refs/tags/v$baseLabel", '--paginate', '--jq', '.[].ref') 'Could not check existing Nightly tags.'
        $existingTags = @($tagRefs | ForEach-Object { "$_" -replace '^refs/tags/', '' })
    }
    $existingTags += @($publishedReleases | ForEach-Object tagName)
    # Dry runs and failed publication can leave outputs without a remote tag.
    # Select a free label before building; preserve every existing local output.
    $nightlyLabel = Get-NightlyLabel $baseLabel $existingTags $OutDirectory
    # ParseVersion: three numbers (1..9 digits), word nightly, eight-digit
    # prerelease number, optional .N remainder; see github_release_validation.cxx.
    if ($nightlyLabel -cnotmatch '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}-nightly[0-9]{8}(\.[0-9]+)?$') { throw "Invalid Nightly version: $nightlyLabel" }
    $nightlyTag = "v$nightlyLabel"
    $notesPath = Join-Path $OutDirectory "nightly-$nightlyLabel.md"
    $baseline = $previousSource
    if (!$baseline) {
        $baseline = 'origin/release'
        if ($Local) {
            Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'cat-file', '-e', "$baseline^{commit}") 'Local mode requires an existing origin/release ref for the initial changelog baseline; fetch it before running with -Local.' | Out-Null
        } else {
            Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'fetch', 'origin', 'release') 'Could not fetch the initial Stable changelog baseline.' | Out-Host
        }
    } else {
        Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'cat-file', '-e', "$baseline^{commit}") 'Previous Nightly Source commit is unavailable locally; restore its history before publishing.' | Out-Null
    }
    $commitRange = "$baseline..$sourceRevision"
    $subjects = @(Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'log', '--first-parent', '--max-count=60', '--format=%s', $commitRange) 'Could not collect Nightly commit subjects.')
    $commitCount = [int](Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'rev-list', '--first-parent', '--count', $commitRange) 'Could not count Nightly commits.')

    # Resolve before the child build script enters vcvarsall. Reuse its actual
    # VS installation for the installer instead of probing again afterwards.
    $VisualStudioPath = (& $Operations.ToolPaths $CheckoutRoot $VisualStudioPath).VisualStudioPath
    & $Operations.Build $VisualStudioPath $DiscordSdkArchive
    $buildDirectory = Join-Path $CheckoutRoot $nightlyTarget.buildDirectory
    $stageDirectory = Join-Path $CheckoutRoot $nightlyTarget.installDirectory
    Assert-NightlyBuildSnapshot $CheckoutRoot $buildDirectory $stageDirectory $snapshot
    $package = & $Operations.Package $OutDirectory $nightlyLabel
    $releaseAssets = @($package.zipPath, "$($package.zipPath).sha256")
    $installerPath = & $Operations.Installer $package.folderPath $nightlyLabel $OutDirectory $VisualStudioPath
    $releaseAssets += @($installerPath, "$installerPath.sha256")
    if ($package.gitRev -cne $sourceRevision) { throw 'Packaged source revision changed.' }
    Assert-NightlyBuildSnapshot $CheckoutRoot $buildDirectory $stageDirectory $snapshot $package.folderPath
    # GetHash reads every on-disk byte through BCrypt SHA256, formatting each
    # digest byte with %02hhx: 64 lowercase hex characters, no prefix or separators.
    $publicBuildId = (Get-FileHash -LiteralPath (Join-Path $stageDirectory 'Sidecar.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    $packagedBuildId = (Get-FileHash -LiteralPath (Join-Path $package.folderPath 'Sidecar.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
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
    $stagingScriptPath = ''
    if (!$SkipRoomHosts) {
        $hostDirectory = Join-Path $OutDirectory "nightly-room-hosts/$nightlyLabel"
        New-Item -ItemType Directory -Path $hostDirectory -Force | Out-Null
        $archivePath = Join-Path $hostDirectory 'room-host-src.tgz'
        Invoke-NightlyCommand git @('-C', $CheckoutRoot, '-c', 'core.autocrlf=false', 'archive', '--format=tar.gz', '-o', $archivePath, $sourceRevision, 'src', 'rust', 'server') 'Could not archive the committed room-host sources.' | Out-Host
        & $Operations.LinuxBuild $sourceRevision $archivePath $hostDirectory | Out-Host
        $hostBinaries = @((Join-Path $hostDirectory 'sf4e-room-host'), (Join-Path $hostDirectory 'sf4-net'))
        foreach ($binary in $hostBinaries) {
            if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "WSL did not produce $binary" }
        }
        # Pin the exact bytes that SSH sends as the staging script; retries use this copy.
        $stagingScriptPath = Join-Path $hostDirectory 'stage-nightly-room-hosts.sh'
        $stagingScript = (Get-Content -LiteralPath (Join-Path $CheckoutRoot 'scripts/stage-nightly-room-hosts.sh') -Raw).Replace("`r`n", "`n")
        [IO.File]::WriteAllText($stagingScriptPath, $stagingScript, [Text.UTF8Encoding]::new($false))
        if ($WhatIf) { Write-Host 'WhatIf: room hosts built locally; no SSH staging or scp.' }
    } else { Write-Host 'Skipped room-host build and staging (-SkipRoomHosts).' }

    # Preparation is complete. This is the one source check before the pending record;
    # from here on only the hashed files recorded in it are used.
    Assert-NightlySource $CheckoutRoot $snapshot
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
            stagingScript=$(if ($stagingScriptPath) { Get-NightlyArtifact $stagingScriptPath } else { $null })
        }
        Save-NightlyState $statePath $pending
        $prunedTags = @(Complete-NightlyPublication $pending $statePath $cleanupPath $snapshot)
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
}
