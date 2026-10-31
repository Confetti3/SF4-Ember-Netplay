#Requires -Version 7
# Import-safe operations. Invoke-NightlyPublish owns preparation and phase ordering;
# the entry script supplies the lock, transcript and the build/package operations.
# Callers also dot-source BuildEnvironment.ps1 (Invoke-StablePublish uses Assert-EmberChannel,
# Assert-EmberStableBranch and Get-EmberBuildTarget) and BuildProvenance.ps1 (Nightly snapshots use its fingerprint).
# The draft, verify, publish release machine is shared: Invoke-StablePublish (run by
# github-release.ps1) uses it for Stable, bound to the Stable repository by its channel.
# Retention stays Nightly-only.
# A publication record moves through three phases for either channel: 'prepared' (every
# local file made and pinned by its SHA-256: the package, installer, notes, symbol archive
# and, for Nightly, the room hosts), 'staged' (everything off GitHub confirmed: Nightly's
# room hosts and both channels' symbols, Complete-EmberStaging) and 'published'. A retry
# continues from the recorded phase with the recorded files and never rebuilds.
. (Join-Path $PSScriptRoot 'PackageFiles.ps1')
. (Join-Path $PSScriptRoot 'SymbolPublication.ps1')

function Invoke-NightlyNative([string]$Command, [string[]]$Arguments, [string]$InputText) {
    # Bytes, not the string: a piped string gets a CRLF after its last line,
    # which bash reading the script from ssh takes for a command named CR.
    if ($InputText) { $output = [Text.UTF8Encoding]::new($false).GetBytes($InputText) | & $Command @Arguments 2>&1 }
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

# The channel decides the repository; nothing else does. A channel's build-target.json entry
# and a pending record can only agree with this list, never widen it.
function Get-EmberReleaseRepository([string]$Channel) {
    switch -CaseSensitive ($Channel) {
        'stable' { return 'Confetti3/SF4-Ember-Netplay' }
        'nightly' { return 'Confetti3/SF4-Ember-Netplay-Nightly' }
        default { throw "Unknown release channel: $Channel" }
    }
}

function Get-EmberRepositoryChannel([string]$Repository) {
    foreach ($channel in @('stable', 'nightly')) {
        if ($Repository -ieq (Get-EmberReleaseRepository $channel)) { return $channel }
    }
    throw 'Release operations are confined to the Stable and Nightly repositories.'
}

function Get-EmberChannelName([string]$Channel) {
    if ($Channel -ceq 'nightly') { return 'Nightly' }
    return 'Stable'
}

# Product release tags: v1.4.1, or a pre-release such as v1.5.0-rc1 or v1.5.0-beta2.
function Test-StableTag([string]$Tag) {
    return $Tag -cmatch '^v[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}(-(rc|beta)[0-9]+)?$'
}

function Test-EmberTag([string]$Repository, [string]$Tag) {
    if ((Get-EmberRepositoryChannel $Repository) -ceq 'nightly') { return Test-NightlyTag $Tag }
    return Test-StableTag $Tag
}

# The one product VERSION line of a CMakeLists.txt, as text and numbers.
function Get-EmberCMakeVersion([string[]]$Lines) {
    $versionLines = @($Lines | Where-Object { $_ -match '^\s+VERSION ' })
    if ($versionLines.Count -ne 1 -or $versionLines[0].Trim() -notmatch '^VERSION (([0-9]{1,9})\.([0-9]{1,9})\.([0-9]{1,9}))$') {
        throw 'Expected one VERSION X.Y.Z line in CMakeLists.txt.'
    }
    return [pscustomobject]@{text=$Matches[1];major=[int]$Matches[2];minor=[int]$Matches[3];patch=[int]$Matches[4]}
}

# Nightly stays one minor version ahead of Stable, so every Stable release is followed by a renumber.
function Assert-NightlyMinorAhead([object]$Nightly, [object]$Stable) {
    if ($Nightly.major -ne $Stable.major -or $Nightly.minor -ne $Stable.minor + 1) {
        throw "Nightly CMake version $($Nightly.text) must be one minor version ahead of Stable $($Stable.text) (expected $($Stable.major).$($Stable.minor + 1).x); renumber nightly after the Stable release."
    }
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
function Invoke-EmberQuery([string]$Repository, [string]$Query, [hashtable]$Variables = @{}) {
    Get-EmberRepositoryChannel $Repository | Out-Null
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

function Invoke-NightlyQuery([string]$Repository, [string]$Query, [hashtable]$Variables = @{}) {
    Assert-NightlyRepository $Repository
    return Invoke-EmberQuery $Repository $Query $Variables
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
        foreach ($extension in @('', '.zip', '.zip.sha256', '-setup.exe', '-setup.exe.sha256', '-symbols.zip')) {
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

function Assert-NightlyCleanHead([string]$SourceRoot, [string]$Revision = '', [string]$Subject = 'Nightly') {
    $dirty = @(Invoke-NightlyCommand git @('-C', $SourceRoot, 'status', '--porcelain', '--untracked-files=all') 'Could not check worktree cleanliness.')
    if ($dirty.Count) { throw "$Subject requires committed source and no untracked non-ignored files:`n$($dirty -join "`n")" }
    $head = (Invoke-NightlyCommand git @('-C', $SourceRoot, 'rev-parse', 'HEAD') 'Could not resolve HEAD.').Trim()
    if ($head -cnotmatch '^[0-9a-f]{40}$' -or ($Revision -and $head -cne $Revision)) { throw "Source commit changed during the $Subject run." }
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
                                    [object]$Snapshot, [string]$PackageDirectory, [switch]$SkipFingerprint) {
    # The receipt check fingerprints the tree itself; tying the receipt to the snapshot
    # covers the content, so only HEAD is left to compare here. Returns the receipt.
    # -SkipFingerprint drops that fresh fingerprint and keeps every other check: the receipt must still
    # equal the snapshot, the packaged receipt too, the staged binaries must hash as recorded and HEAD
    # must be clean and unmoved. Assert-NightlySource fingerprints again before the pending record.
    $receipt = Assert-BuildReceipt $SourceRoot $BuildRoot $StageRoot -SkipFingerprint:$SkipFingerprint
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
    $receipt
}

function Get-NightlyArtifact([string]$Path) {
    [pscustomobject]@{path=[IO.Path]::GetFullPath($Path);name=(Split-Path $Path -Leaf);
        sha256=(Get-FileSha256 $Path)}
}

function Assert-NightlyArtifacts([object[]]$Artifacts) {
    foreach ($artifact in @($Artifacts | Where-Object { $_ })) {
        if ((Get-FileSha256 $artifact.path) -cne $artifact.sha256) {
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
    # The token outlives interrupted runs, so a retry finds and removes exactly its directory.
    Invoke-EmberStagedUpload $Destination.hostName "$($Destination.root) $BuildId $($Binaries[0].sha256) $($Binaries[1].sha256) $token" `
        (Read-NightlyPinnedText $StagingScript) @($Binaries | ForEach-Object path) "~/$($Destination.root)/.incoming-$BuildId-$token/" 'Nightly room-host pair' | Out-Null
}

# One staged upload over ssh, shared by the room hosts and the symbols: the pinned $Script runs on $HostName as
# `bash -s -- <$Arguments> <phase>`. cleanup removes whatever an earlier attempt left in the upload folder,
# prepare makes it, scp copies $Paths into $Incoming (an empty $Incoming means the folder path prepare printed),
# commit installs them, and cleanup runs again whatever happened. A cleanup failure after an earlier failure is
# only a warning, so the error that stopped the upload is the one thrown; the phase advances only after a
# confirmed cleanup. Returns commit's output.
function Invoke-EmberStagedUpload([string]$HostName, [string]$Arguments, [string]$Script, [string[]]$Paths, [string]$Incoming, [string]$What) {
    $remote = { param($Phase, $Failure) Invoke-NightlyCommand ssh @('-o', 'BatchMode=yes', $HostName, "bash -s -- $Arguments $Phase") $Failure -InputText $Script }
    $stageError = $null
    try {
        & $remote cleanup "Could not recover the previous $What upload." | Out-Null
        $prepared = @(& $remote prepare "Could not prepare the $What upload." | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
        if (!$Incoming) {
            if (!$prepared.Count -or $prepared[-1] -cnotmatch '^/\S+$') { throw "The $What upload did not report its folder." }
            $Incoming = "$($prepared[-1])/"
        }
        Invoke-NightlyCommand scp (@('-o', 'BatchMode=yes', '-r') + $Paths + @("${HostName}:$Incoming")) "Could not upload the $What." | Out-Null
        & $remote commit "Could not install the uploaded $What."
    } catch { $stageError = $_; throw }
    finally {
        try { & $remote cleanup "Could not clean up the $What upload." | Out-Null }
        catch {
            if (!$stageError) { throw }
            Write-Warning "The $What upload folder was not removed; the next attempt removes it first: $($_.Exception.Message)"
        }
    }
}

# The release and tag behind one tag of the Stable or Nightly repository. GraphQL finds draft releases
# too (REST's by-tag endpoint only finds published ones), then full REST metadata is read by
# database ID, as gh does: https://github.com/cli/cli/blob/trunk/pkg/cmd/release/shared/fetch.go
function Get-EmberTagState([string]$Repository, [string]$Tag) {
    $name = Get-EmberChannelName (Get-EmberRepositoryChannel $Repository)
    if (!(Test-EmberTag $Repository $Tag)) { throw "Invalid $name tag." }
    $query = 'query($owner: String!, $name: String!, $tag: String!, $ref: String!) { repository(owner: $owner, name: $name) { release(tagName: $tag) { databaseId } ref(qualifiedName: $ref) { target { oid } } } }'
    $outcome = Invoke-EmberQuery $Repository $query @{tag=$Tag;ref="refs/tags/$Tag"}
    if ($outcome.status -cne 'found') { throw "Could not resolve $name release identity ($($outcome.status)): $($outcome.detail)" }
    $release = $null
    $id = $outcome.data.repository.release.databaseId
    if ($id) {
        if ("$id" -notmatch '^[1-9][0-9]*$') { throw "Invalid $name release database ID." }
        $release = Invoke-NightlyCommand gh @('api', "repos/$Repository/releases/$id") "Could not inspect $name release." | ConvertFrom-Json
    }
    return [pscustomobject]@{release=$release;tagSha=[string]$outcome.data.repository.ref.target.oid}
}

# Retention and every other Nightly-only caller stay confined to the Nightly repository.
function Get-NightlyTagState([string]$Repository, [string]$Tag) {
    if (!(Test-NightlyTag $Tag)) { throw 'Invalid Nightly tag.' }
    Assert-NightlyRepository $Repository
    return Get-EmberTagState $Repository $Tag
}

# What a prepared record means for its channel. The repository comes from the channel, never from
# the record: the record must agree with it. Records written before Stable used this machine have
# no channel and are Nightly. Stable's tag already exists on the remote (the draft is made with
# --verify-tag) and it becomes Latest only when it is published; Nightly's tag is created by the
# publication itself and is never Latest.
function Get-EmberReleasePlan([object]$Pending) {
    $channel = [string]$Pending.channel
    if (!$channel) { $channel = 'nightly' }
    $repository = Get-EmberReleaseRepository $channel
    if ([string]$Pending.repository -ine $repository) {
        throw "A $channel release can only be published to $repository."
    }
    $name = Get-EmberChannelName $channel
    if ($channel -ceq 'nightly') {
        return [pscustomobject]@{channel=$channel;name=$name;repository=$repository;prerelease=$true;latest=$false;
            tagMustExist=$false;title="SF4 Ember Netplay Nightly $($Pending.label)"}
    }
    if (!(Test-StableTag $Pending.tag)) { throw 'Invalid Stable tag.' }
    $prerelease = [bool]$Pending.prerelease
    if ($prerelease -ne ($Pending.tag -cmatch '-(rc|beta)[0-9]+$')) { throw 'The Stable tag and its pre-release flag disagree.' }
    return [pscustomobject]@{channel=$channel;name=$name;repository=$repository;prerelease=$prerelease;latest=!$prerelease;
        tagMustExist=$true;title="SF4 Ember Netplay $($Pending.tag)"}
}

# The notes exactly as hashed when the record was prepared, read once per publication.
function Read-NightlyNotes([object]$Pending) {
    return (Read-NightlyPinnedText $Pending.notes).TrimStart([char]0xFEFF).Replace("`r`n", "`n").TrimEnd()
}

function Assert-EmberReleaseIdentity([object]$Release, [object]$Pending, [string]$Notes, [switch]$AllowIncomplete) {
    $plan = Get-EmberReleasePlan $Pending
    if ($Release.tag_name -cne $Pending.tag -or [bool]$Release.prerelease -ne $plan.prerelease -or
        ([string]$Release.body).Replace("`r`n", "`n").TrimEnd() -cne $Notes) { throw "Existing release does not match the prepared $($plan.name) identity." }
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
function Get-EmberNextStep([object]$State, [object]$Pending, [string]$Notes) {
    $plan = Get-EmberReleasePlan $Pending
    $release = $State.release
    if (!$release) {
        if ($plan.tagMustExist) {
            if (!$State.tagSha) { throw "The $($plan.name) release tag is not on the remote; push it before publishing." }
        } elseif ($State.tagSha) {
            # A tag without our release is a collision, never a recovery target.
            throw 'Prepared Nightly tag already exists without its release; refusing to reuse it.'
        }
        return [pscustomobject]@{action='create';target=$Pending.tag}
    }
    Assert-EmberReleaseIdentity $release $Pending $Notes -AllowIncomplete
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
        if (!$release.draft) { throw "Published $($plan.name) has missing assets; refusing to change public artifacts." }
        return [pscustomobject]@{action='upload';target=$asset.path}
    }
    if (@($release.assets).Count -ne @($Pending.assets).Count) { throw "$($plan.name) draft asset set is incomplete." }
    if ($release.draft) { return [pscustomobject]@{action='publish';target=$Pending.tag} }
    return [pscustomobject]@{action='done';target=$Pending.tag}
}

# Draft, upload, verify every asset digest against the record, then publish: for either channel.
function Publish-EmberRelease([object]$Pending) {
    $plan = Get-EmberReleasePlan $Pending
    $name = $plan.name
    if ($Pending.phase -cne 'staged') { throw "$name publication requires verified staging first." }
    Assert-NightlyArtifacts @($Pending.assets)
    # The draft is created from, and compared with, this verified text; the file is not read again.
    $notes = Read-NightlyNotes $Pending
    $repository = $plan.repository
    $previous = $null
    # Read, take one step, read back. The read after a mutation is its verification:
    # a step that is still due right after it ran was not confirmed.
    for ($pass = 0; $pass -lt 2 * @($Pending.assets).Count + 4; $pass++) {
        $step = Get-EmberNextStep (Get-EmberTagState $repository $Pending.tag) $Pending $notes
        if ($step.action -ceq 'done') { return "https://github.com/$repository/releases/tag/$($Pending.tag)" }
        if ($previous -and $previous.action -ceq $step.action -and $previous.target -ceq $step.target) {
            throw "$name $($step.action) of $($step.target) could not be confirmed by reading the release back."
        }
        switch ($step.action) {
            'create' {
                $arguments = @('release', 'create', $Pending.tag, '--repo', $repository, '--draft')
                if ($plan.prerelease) { $arguments += '--prerelease' }
                if ($plan.tagMustExist) { $arguments += '--verify-tag' }
                $arguments += @('--title', $plan.title)
                if ($plan.channel -ceq 'nightly') {
                    $arguments += @('--notes', $notes)
                    Invoke-NightlyCommand gh $arguments "Could not create the $name draft." | Out-Null
                } else {
                    # Stable notes are long enough to approach the command-line limit, so the verified
                    # text goes through a private file written from it, not from the release-notes file.
                    $notesFile = [IO.Path]::Combine([IO.Path]::GetTempPath(), "release-notes-$([Guid]::NewGuid().ToString('N')).md")
                    try {
                        [IO.File]::WriteAllText($notesFile, $notes, [Text.UTF8Encoding]::new($false))
                        Invoke-NightlyCommand gh ($arguments + @('--notes-file', $notesFile)) "Could not create the $name draft." | Out-Null
                    } finally { if (Test-Path -LiteralPath $notesFile) { Remove-Item -LiteralPath $notesFile } }
                }
            }
            'delete-asset' {
                Invoke-NightlyCommand gh @('api', '--method', 'DELETE',
                    "repos/$repository/releases/assets/$($step.target)") "Could not remove incomplete $name draft upload." | Out-Null
            }
            'upload' {
                Invoke-NightlyCommand gh @('release', 'upload', $Pending.tag, $step.target, '--repo', $repository) "Could not upload $name draft asset." | Out-Null
            }
            'publish' {
                $arguments = @('release', 'edit', $Pending.tag, '--repo', $repository, '--draft=false')
                if ($plan.latest) { $arguments += '--latest' }
                Invoke-NightlyCommand gh $arguments "$name GitHub publication failed." | Out-Null
            }
        }
        $previous = $step
    }
    throw "$name publication did not settle; inspect the draft before retrying."
}

# Complete-NightlyPublication publishes through this wrapper, so it stays Nightly-only.
function Publish-NightlyRelease([object]$Pending) {
    Assert-NightlyRepository $Pending.repository
    if ([string]$Pending.channel -and [string]$Pending.channel -cne 'nightly') { throw 'Nightly publication cannot publish another channel.' }
    return Publish-EmberRelease $Pending
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

# Everything a release needs off GitHub before it is announced, from the recorded files only: Nightly's room hosts
# (identical pairs are accepted), then the Symbols step (Publish-EmberSymbols). The record moves from 'prepared' to
# 'staged' only once all of it is confirmed, so a retry repeats exactly this, and a 'staged' record goes straight
# on to the release.
function Complete-EmberStaging([object]$Pending, [string]$StatePath) {
    if ($Pending.phase -cne 'prepared') { return }
    if ($Pending.symbols.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Pending work must include its symbol archive.' }
    foreach ($destination in $Pending.destinations) {
        Invoke-NightlyHostStage $destination $Pending.buildId $Pending.binaries $Pending.stagingScript
    }
    # A retry installs with the installer this release archived.
    Publish-EmberSymbols $Pending.symbols (Get-EmberReleasePlan $Pending).channel $Pending.label (Get-EmberArchivedSymbolInstaller $Pending.symbols)
    $Pending.phase = 'staged'
    Save-NightlyState $StatePath $Pending
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
        Assert-NightlyArtifacts (@($Pending.assets) + @($Pending.notes) + @($Pending.binaries) + @($Pending.stagingScript) + @($Pending.symbols))
        Complete-EmberStaging $Pending $StatePath
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
                Assert-NightlyArtifacts (@($pending.assets) + @($pending.notes) + @($pending.binaries) + @($pending.stagingScript) + @($pending.symbols))
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

    $nightlyVersion = Get-EmberCMakeVersion @(Get-Content -LiteralPath (Join-Path $CheckoutRoot 'CMakeLists.txt'))
    $baseLabel = "$($nightlyVersion.text)-nightly$((& $Operations.Now).ToUniversalTime().ToString('yyyyMMdd', [cultureinfo]::InvariantCulture))"
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
    $releaseFetched = $false
    if (!$baseline) {
        $baseline = 'origin/release'
        if ($Local) {
            Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'cat-file', '-e', "$baseline^{commit}") 'Local mode requires an existing origin/release ref for the initial changelog baseline; fetch it before running with -Local.' | Out-Null
        } else {
            Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'fetch', 'origin', 'release') 'Could not fetch the initial Stable changelog baseline.' | Out-Host
            $releaseFetched = $true
        }
    } else {
        Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'cat-file', '-e', "$baseline^{commit}") 'Previous Nightly Source commit is unavailable locally; restore its history before publishing.' | Out-Null
    }
    # The one-minor-ahead rule reads Stable's current version, so a stale origin/release must not be used.
    # -Local does not fetch; it fails closed when the ref or its CMakeLists.txt is missing.
    if (!$Local -and !$releaseFetched) {
        Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'fetch', 'origin', 'release') 'Could not fetch the Stable release branch for the Nightly version check.' | Out-Host
    }
    $stableLines = @(Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'show', 'origin/release:CMakeLists.txt') 'Could not read origin/release:CMakeLists.txt for the Nightly version check; fetch origin release first.')
    Assert-NightlyMinorAhead $nightlyVersion (Get-EmberCMakeVersion ($stableLines | ForEach-Object { "$_" }))
    $commitRange = "$baseline..$sourceRevision"
    $subjects = @(Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'log', '--first-parent', '--max-count=60', '--format=%s', $commitRange) 'Could not collect Nightly commit subjects.')
    $commitCount = [int](Invoke-NightlyCommand git @('-C', $CheckoutRoot, 'rev-list', '--first-parent', '--count', $commitRange) 'Could not count Nightly commits.')

    # Resolve before the child build script enters vcvarsall. Reuse its actual
    # VS installation for the installer instead of probing again afterwards.
    $VisualStudioPath = (& $Operations.ToolPaths $CheckoutRoot $VisualStudioPath).VisualStudioPath
    & $Operations.Build $VisualStudioPath $DiscordSdkArchive
    $buildDirectory = Join-Path $CheckoutRoot $nightlyTarget.buildDirectory
    $stageDirectory = Join-Path $CheckoutRoot $nightlyTarget.installDirectory
    $receipt = Assert-NightlyBuildSnapshot $CheckoutRoot $buildDirectory $stageDirectory $snapshot
    # A publication carries its symbols: a build without them stops here, before anything is packaged.
    Assert-ReceiptSymbols $receipt | Out-Null
    $package = & $Operations.Package $OutDirectory $nightlyLabel
    $releaseAssets = @($package.zipPath, "$($package.zipPath).sha256")
    $installerPath = & $Operations.Installer $package.folderPath $nightlyLabel $OutDirectory $VisualStudioPath
    $releaseAssets += @($installerPath, "$installerPath.sha256")
    if ($package.gitRev -cne $sourceRevision) { throw 'Packaged source revision changed.' }
    # Packaging fingerprinted the tree seconds ago and Assert-NightlySource does again before the pending record.
    Assert-NightlyBuildSnapshot $CheckoutRoot $buildDirectory $stageDirectory $snapshot $package.folderPath -SkipFingerprint | Out-Null
    $symbolArchive = Save-EmberSymbolArchive $CheckoutRoot $receipt 'nightly' $nightlyLabel $OutDirectory
    # GetHash reads every on-disk byte through BCrypt SHA256, formatting each
    # digest byte with %02hhx: 64 lowercase hex characters, no prefix or separators.
    $publicBuildId = Get-FileSha256 (Join-Path $stageDirectory 'Sidecar.dll')
    $packagedBuildId = Get-FileSha256 (Join-Path $package.folderPath 'Sidecar.dll')
    if ($publicBuildId -ne $packagedBuildId) { throw 'Staged and packaged Sidecar.dll build ids differ.' }
    if ($publicBuildId -cnotmatch '^[0-9a-f]{64}$') { throw 'Expected a SHA-256 public rooms build id.' }
    Write-Host "Public rooms build: $publicBuildId"
    $notes = @("SF4 Ember Netplay Nightly $nightlyLabel", '', "Source: $sourceRevision", '',
        'Nightly build from the nightly branch. Daily test builds may break; rooms only with other Nightly players.', '',
        "Public rooms build: $publicBuildId", '')
    if ($receipt.testsPassedOnRerun) { $notes += @("Passed only when run once more: $($receipt.testsPassedOnRerun -join ', ')", '') }
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
            repository=$releaseRepository;channel='nightly';tag=$nightlyTag;label=$nightlyLabel;phase='prepared'
            snapshot=$snapshot;buildId=$publicBuildId;destinations=@($destinations)
            assets=@($releaseAssets | ForEach-Object { Get-NightlyArtifact $_ })
            notes=(Get-NightlyArtifact $notesPath)
            binaries=@($hostBinaries | ForEach-Object { Get-NightlyArtifact $_ })
            stagingScript=$(if ($stagingScriptPath) { Get-NightlyArtifact $stagingScriptPath } else { $null })
            symbols=$symbolArchive
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
    Write-Host "Symbols (kept locally): $($symbolArchive.path)"
    foreach ($asset in $releaseAssets) {
        Write-Host "Asset: $asset  SHA-256: $(Get-FileSha256 $asset)"
    }
    Write-Host "Pruned tags: $(if ($prunedTags.Count) { $prunedTags -join ', ' } else { '(none)' })"
}

# The whole Stable release below github-release.ps1. $Operations supplies what packages
# (scriptblocks: Target, Package, Upgrade, Installer); git, gh, ssh and scp go through Invoke-NightlyCommand.
# The repository is the Stable channel's, never an argument. The tag is pushed first, the build's
# symbols are checked before anything is packaged and archived beside the package, the Symbols step
# uploads them, the release is made as a draft, every asset is read back against its recorded SHA-256,
# and only then is it published (and made Latest). The prepared record in the output folder lets a run
# that stopped partway, such as a failed symbol or large upload, continue with the same files; no other
# run may reuse a draft. $SourceRoot, $OutDirectory and $NotesFile are absolute or relative to $SourceRoot.
function Invoke-StablePublish([string]$SourceRoot, [string]$Tag, [string]$OutDirectory, [string]$NotesFile,
                              [string]$PreviousVersion, [hashtable]$Operations) {
    $target = & $Operations.Target $SourceRoot
    # Before any git or gh call: a Nightly checkout carries a build that trusts the staging bridge.
    Assert-EmberChannel $target 'stable' 'Stable publication'
    $repository = Get-EmberReleaseRepository 'stable'
    if ($target.githubRepo -and [string]$target.githubRepo -ine $repository) {
        throw "build-target.json names $($target.githubRepo); Stable publishes only to $repository."
    }
    # The first git call, before any gh call or build step: a checkout that reads as stable only
    # because it fell back to its own build-target.json is still refused off the release branch.
    $branch = Invoke-NightlyCommand git @('-C', $SourceRoot, 'branch', '--show-current') 'Could not read the current branch.'
    Assert-EmberStableBranch $branch 'Stable publication'
    # A pre-release (v1.1.0-rc1, v1.1.0-beta2) reaches only players on the
    # pre-release update channel and never becomes the latest release.
    if ($Tag -notmatch '^v([0-9]+\.[0-9]+\.[0-9]+)(-(?:rc|beta)[0-9]+)?$') { throw 'Use a full product release tag, such as v0.8.0 or v0.8.0-rc1.' }
    $productVersion = $Matches[1]
    $prerelease = [bool]$Matches[2]
    $version = $productVersion + $Matches[2]
    if (![IO.Path]::IsPathRooted($OutDirectory)) { $OutDirectory = Join-Path $SourceRoot $OutDirectory }
    $revision = Assert-NightlyCleanHead $SourceRoot '' 'Stable publication'
    $tagRevision = Invoke-NightlyNative git @('-C', $SourceRoot, 'rev-parse', "$Tag^{commit}")
    if ($tagRevision.exitCode -or "$($tagRevision.output)".Trim() -cne $revision) { throw 'The release tag must point at the current source commit.' }
    $cmakeVersion = Get-EmberCMakeVersion @(Get-Content -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt'))
    if ($cmakeVersion.text -cne $productVersion) { throw 'Tag and CMake product version differ.' }
    if (!$NotesFile) { $NotesFile = "docs/release-notes/RELEASE_NOTES_$Tag.md" }
    if (![IO.Path]::IsPathRooted($NotesFile)) { $NotesFile = Join-Path $SourceRoot $NotesFile }
    if (!(Test-Path -LiteralPath $NotesFile)) { throw "Missing release notes: $NotesFile" }
    $pendingPath = Join-Path $OutDirectory "release-pending-$Tag.json"
    if (Test-Path -LiteralPath $pendingPath) {
        $pending = Get-Content -LiteralPath $pendingPath -Raw | ConvertFrom-Json
        if ([string]$pending.channel -cne 'stable' -or $pending.tag -cne $Tag -or [string]$pending.repository -ine $repository -or
            $pending.snapshot.revision -cne $revision) {
            throw "The pending release record $pendingPath does not match this tag, repository and source commit; check the draft, then remove the record."
        }
        Assert-NightlyArtifacts (@($pending.assets) + @($pending.notes) + @($pending.symbols))
        Write-Host "Resuming $Tag from $pendingPath at $($pending.phase); nothing is repackaged."
    } else {
        $state = Get-EmberTagState $repository $Tag
        if ($state.release) { throw 'The release already exists; refusing to replace published assets.' }
        if (!$state.tagSha) { throw "The tag $Tag is not on $repository; push it before publishing." }
        # Pre-releases carry no upgrade package: their testers take the complete one.
        if (!$prerelease) {
            if (!$PreviousVersion) {
                $latest = (Invoke-NightlyCommand gh @('release', 'view', '--repo', $repository, '--json', 'tagName') 'Could not resolve the previous published version.' | ConvertFrom-Json).tagName
                if ($latest -notmatch '^v([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Could not resolve the previous published version.' }
                $PreviousVersion = $Matches[1]
            }
            if ($PreviousVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or $PreviousVersion -eq $version) { throw 'Invalid previous release version.' }
            $baseZip = Join-Path $OutDirectory "sf4-ember-netplay-$PreviousVersion.zip"
            if (!(Test-Path -LiteralPath $baseZip -PathType Leaf) -or !(Test-Path -LiteralPath ($baseZip + '.sha256') -PathType Leaf)) {
                throw "Keep the verified previous full package and checksum in ${OutDirectory}: sf4-ember-netplay-$PreviousVersion.zip"
            }
            $previousAssets = (Invoke-NightlyCommand gh @('release', 'view', "v$PreviousVersion", '--repo', $repository, '--json', 'assets') 'Could not read the previous release assets.' | ConvertFrom-Json).assets
            $published = @($previousAssets | Where-Object { $_.name -ceq "sf4-ember-netplay-$PreviousVersion.zip" }) | Select-Object -First 1
            $baseHash = Get-FileSha256 $baseZip
            if (!$published -or !$published.digest -or $published.digest -ine "sha256:$baseHash") { throw 'Local previous package does not match the published release asset.' }
        }
        # A publication carries its symbols (release-team-build.ps1 builds with them): a build without them stops
        # here, before anything is packaged. Packaging fingerprints the tree right after.
        $receipt = Assert-BuildReceipt $SourceRoot (Join-Path $SourceRoot $target.buildDirectory) (Join-Path $SourceRoot $target.installDirectory) -SkipFingerprint
        Assert-ReceiptSymbols $receipt | Out-Null
        $package = & $Operations.Package $OutDirectory $version
        $assets = @($package.zipPath, "$($package.zipPath).sha256")
        if (!$prerelease) {
            $upgradePath = & $Operations.Upgrade $PreviousVersion $version $baseZip $package.zipPath $OutDirectory
            $assets += @($upgradePath, "$upgradePath.sha256")
        }
        $installerPath = & $Operations.Installer $package.folderPath $version $OutDirectory
        $assets += @($installerPath, "$installerPath.sha256")
        if ($package.gitRev -cne $revision) { throw 'Packaged source revision changed.' }
        if ($receipt.sourceFingerprint -cne $package.sourceFingerprint) { throw 'The build receipt changed while packaging.' }
        Assert-NightlyCleanHead $SourceRoot $revision 'Stable publication' | Out-Null
        $pending = [pscustomobject]@{
            channel='stable';repository=$repository;tag=$Tag;label=$version;phase='prepared';prerelease=$prerelease
            snapshot=[pscustomobject]@{revision=$revision;fingerprint=[string]$package.sourceFingerprint}
            assets=@($assets | ForEach-Object { Get-NightlyArtifact $_ })
            notes=(Get-NightlyArtifact $NotesFile)
            symbols=(Save-EmberSymbolArchive $SourceRoot $receipt 'stable' $version $OutDirectory)
        }
        Save-NightlyState $pendingPath $pending
    }
    Complete-EmberStaging $pending $pendingPath
    $url = Publish-EmberRelease $pending
    Remove-Item -LiteralPath $pendingPath
    Write-Host "Published: $url"
    return $url
}
