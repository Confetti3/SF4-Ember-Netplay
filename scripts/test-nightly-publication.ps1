#Requires -Version 7
# Direct operation tests: all git/gh/SSH/scp/WSL commands are mocked.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'NightlyOperations.ps1')
$originalCulture = [cultureinfo]::CurrentCulture
try {
    # A day-first culture for the whole run: nothing here may depend on the build PC's date format.
    [cultureinfo]::CurrentCulture = 'nl-BE'
    $fixture = Join-Path ([IO.Path]::GetTempPath()) ('nightly-publication-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $fixture | Out-Null
    $repository = 'Confetti3/SF4-Ember-Netplay-Nightly'
    $statePath = Join-Path $fixture 'pending.json'
    $cleanupPath = Join-Path $fixture 'cleanup.json'
    $snapshot = [pscustomobject]@{revision=('a' * 40);fingerprint=('B' * 64)}
    $source = Join-Path $fixture 'source'
    $passed = 0
    function Check([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
    function Reject([scriptblock]$Action, [string]$Expected) {
        try { & $Action | Out-Null } catch {
            if ($_.Exception.Message -like $Expected) { return }
            throw
        }
        throw "Expected rejection: $Expected"
    }
    function Pass([string]$Message) { $script:passed++; Write-Output "PASS $Message" }

    # Check the real command boundary before replacing it with a deterministic fake.
    function gh {
        $global:LASTEXITCODE = 1
        return $script:nativeError
    }
    try {
        $script:nativeError = 'gh: HTTP 404: Not Found'
        Check (!(Invoke-NightlyCommand gh @('api', 'fixture') 'query failed' -AllowNotFound)) '404 was not accepted'
        $script:nativeError = 'gh: HTTP 403: Forbidden'
        Reject { Invoke-NightlyCommand gh @('api', 'fixture') 'query failed' -AllowNotFound } '*HTTP 403*'
        $script:nativeError = 'network timed out'
        Reject { Invoke-NightlyCommand gh @('api', 'fixture') 'query failed' -AllowNotFound } '*network timed out*'
        Pass 'only explicit HTTP 404 is recoverable absence; auth/network failures propagate'
    } finally { Remove-Item Function:gh }

    function Reset-Fake {
        $script:commands = [Collections.Generic.List[object]]::new()
        $script:remoteReleases = @{}
        $script:remoteTags = @{}
        $script:remoteUploads = @{}
        $script:failCleanup = $false
        $script:dirty = @()
        $script:head = $snapshot.revision
        $script:fingerprint = $snapshot.fingerprint
        $script:failOn = ''
        $script:serverAvailable = $true
        $script:missingRepository = $false
        $script:receipt = [pscustomobject]@{sourceFingerprint=$snapshot.fingerprint;baseRevision=$snapshot.revision}
        foreach ($path in @($statePath, $cleanupPath)) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
    }
    function Get-SourceFingerprint([string]$SourceRoot) { return $script:fingerprint }
    function Assert-BuildReceipt([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot) { return $script:receipt }
    function Invoke-NightlyCommand([string]$Command, [string[]]$Arguments, [string]$Failure,
                                  [switch]$AllowNotFound, [string]$InputText) {
        $script:commands.Add([pscustomobject]@{command=$Command;argv=$Arguments;inputText=$InputText})
        $text = "$Command $($Arguments -join ' ')"
        if ($Command -eq 'git') {
            if ($Arguments -contains 'status') { return $script:dirty }
            if ($Arguments -contains 'rev-parse') { return $script:head }
            throw "Unexpected git operation: $text"
        }
        if ($Command -eq 'ssh' -and $Arguments[-1] -eq 'true') {
            if (!$script:serverAvailable) { throw 'server unavailable' }
            return
        }
        if ($Command -in @('ssh', 'scp')) {
            if ($Command -eq 'ssh') {
                Check ($Arguments[-1] -match '^bash -s -- (ember-rooms(?:-box)?) ([0-9a-f]{64}) [0-9a-f]{64} [0-9a-f]{64} ([0-9a-f]{32}) (prepare|commit|cleanup)$') 'Unexpected host staging command'
                $root, $build, $token, $operation = $Matches[1], $Matches[2], $Matches[3], $Matches[4]
                $key = "$($Arguments[-2]):~/$root/.incoming-$build-$token/"
                $record = Get-Content $statePath -Raw | ConvertFrom-Json
                $destination = $record.destinations | Where-Object hostName -CEQ $Arguments[-2]
                Check ($destination.uploadToken -ceq $token) 'Upload token was not persisted before remote mutation'
                if ($operation -eq 'prepare') {
                    Check (!$script:remoteUploads.ContainsKey($key)) 'Retry did not remove abandoned upload'
                    $script:remoteUploads[$key] = @()
                } elseif ($operation -eq 'cleanup') {
                    if ($script:failCleanup) { throw 'injected upload cleanup failure' }
                    $script:remoteUploads.Remove($key)
                    return
                }
            } else {
                $key = $Arguments[-1]
                Check ($script:remoteUploads.ContainsKey($key)) 'Upload directory was not prepared'
                $script:remoteUploads[$key] = @('partial host bytes')
            }
            if ($script:failOn -and $text -like $script:failOn) { $script:failOn = ''; throw 'injected staging failure' }
            if ($Command -eq 'ssh' -and $operation -eq 'commit') { $script:remoteUploads.Remove($key) }
            if ($Command -eq 'scp' -and $script:failOn -eq 'dirty-after-upload') { $script:failOn=''; $script:dirty=@(' M tracked.cxx') }
            return
        }
        if ($Command -ne 'gh') { throw "Unexpected external command: $text" }
        if ($Arguments[0] -eq 'release') {
            $repoIndex = [Array]::IndexOf($Arguments, '--repo')
            Check ($repoIndex -ge 0 -and $Arguments[$repoIndex + 1] -ceq $repository) 'Missing explicit --repo'
            if ($script:missingRepository) { throw 'HTTP 404: Not Found' }
            $tag = $Arguments[2]
            switch ($Arguments[1]) {
                list {
                    return ConvertTo-Json -InputObject @($script:remoteReleases.Values | Where-Object { !$_.draft } |
                        ForEach-Object { [pscustomobject]@{tagName=$_.tag_name;createdAt=$_.createdAt} }) -Depth 8
                }
                create {
                    Check ($Arguments -contains '--draft') 'Release must be created as a draft'
                    $notesIndex = [Array]::IndexOf($Arguments, '--notes-file')
                    $script:remoteReleases[$tag] = [pscustomobject]@{id=100;tag_name=$tag;draft=$true;prerelease=$true;
                        body=(Get-Content -LiteralPath $Arguments[$notesIndex + 1] -Raw);assets=@();createdAt='2026-10-08T12:00:00Z'}
                    # Simulate an uncertain result after GitHub accepted the mutation.
                    if ($script:failOn -eq 'after-create') { $script:failOn=''; throw 'injected failure after create' }
                }
                upload {
                    $artifact = Get-NightlyArtifact $Arguments[3]
                    if ($script:failOn -eq 'starter-upload') {
                        $script:remoteReleases[$tag].assets += [pscustomobject]@{id=501;name=$artifact.name;digest=$null;state='starter';size=0}
                        $script:failOn=''; throw 'injected failure during upload'
                    }
                    $script:remoteReleases[$tag].assets += [pscustomobject]@{name=$artifact.name;digest="sha256:$($artifact.sha256)"}
                    if ($script:failOn -eq 'after-upload') { $script:failOn=''; throw 'injected failure after upload' }
                }
                edit {
                    $script:remoteReleases[$tag].draft = $false
                    $script:remoteTags[$tag] = 'c' * 40
                    if ($script:failOn -eq 'after-publish') { $script:failOn=''; throw 'injected failure after public visibility' }
                }
                delete {
                    Check (Test-Path -LiteralPath $cleanupPath) 'Deletion intent was not persisted'
                    Check ($Arguments -notcontains '--cleanup-tag') 'Release/tag deletion must have separate recovery boundaries'
                    $script:remoteReleases.Remove($tag)
                    if ($script:failOn -eq 'after-delete-release') { $script:failOn=''; throw 'injected failure after release deletion' }
                }
                default { throw "Unexpected release command: $text" }
            }
            return
        }
        if ($Arguments[0] -eq 'api') {
            if ($Arguments[1] -eq 'graphql') {
                Check ($Arguments -contains 'owner=Confetti3' -and $Arguments -contains 'name=SF4-Ember-Netplay-Nightly') 'GraphQL escaped Nightly repository'
                $tag = ($Arguments | Where-Object { $_ -like 'tag=*' }).Substring(4)
                $release = if ($script:remoteReleases.ContainsKey($tag)) {
                    @{databaseId=$script:remoteReleases[$tag].id}
                } else { $null }
                return @{data=@{repository=@{release=$release}}} | ConvertTo-Json -Depth 5
            }
            $endpoint = $Arguments[-1]
            Check ($endpoint.StartsWith("repos/$repository/")) 'API escaped the Nightly repository'
            if ($endpoint -match '/releases/assets/([0-9]+)$' -and $Arguments -contains 'DELETE') {
                $id = $Matches[1]
                foreach ($release in $script:remoteReleases.Values) {
                    $release.assets = @($release.assets | Where-Object id -NE $id)
                }
                return
            } elseif ($endpoint -match '/releases/([0-9]+)$') {
                $id = $Matches[1]
                $release = $script:remoteReleases.Values | Where-Object id -EQ $id
                if ($release) { return $release | ConvertTo-Json -Depth 8 }
            } elseif ($endpoint -match '/git/ref(s)?/tags/(.+)$') {
                $tag = $Matches[2]
                if ($Arguments -contains 'DELETE') {
                    Check (Test-Path -LiteralPath $cleanupPath) 'Tag deletion intent was not persisted'
                    if ($script:failOn -eq 'delete-tag') { $script:failOn=''; throw 'injected tag cleanup failure' }
                    $script:remoteTags.Remove($tag)
                    if ($script:failOn -eq 'after-delete-tag') { $script:failOn=''; throw 'injected failure after tag deletion' }
                    return
                }
                if ($script:remoteTags.ContainsKey($tag)) { return @{object=@{sha=$script:remoteTags[$tag]}} | ConvertTo-Json }
            } else { throw "Unexpected API operation: $text" }
            if (!$AllowNotFound) { throw 'HTTP 404' }
            return
        }
        throw "Unexpected gh operation: $text"
    }
    function New-Pending {
        $assetPath = Join-Path $fixture 'package.zip'
        $notesPath = Join-Path $fixture 'notes.md'
        $hostPath = Join-Path $fixture 'sf4e-room-host'
        $netPath = Join-Path $fixture 'sf4-net'
        Set-Content -LiteralPath $assetPath -Value 'inert package fixture'
        Set-Content -LiteralPath $notesPath -Value "Source: $($snapshot.revision)"
        Set-Content -LiteralPath $hostPath -Value 'inert host fixture'
        Set-Content -LiteralPath $netPath -Value 'inert helper fixture'
        $pending = [pscustomobject]@{repository=$repository;tag='v1.2.0-nightly20261008';label='1.2.0-nightly20261008';
            phase='prepared';snapshot=$snapshot;buildId=('d' * 64);
            assets=@(Get-NightlyArtifact $assetPath);notes=(Get-NightlyArtifact $notesPath);
            binaries=@((Get-NightlyArtifact $hostPath), (Get-NightlyArtifact $netPath));
            destinations=@([pscustomobject]@{hostName='vps';root='ember-rooms'}, [pscustomobject]@{hostName='server1';root='ember-rooms-box'})}
        Save-NightlyState $statePath $pending
        return $pending
    }
    function Resume-Pending {
        $record = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        Complete-NightlyPublication $record $statePath $cleanupPath $source $snapshot | Out-Null
    }
    function Add-OldReleases([int]$Count = 15) {
        1..$Count | ForEach-Object {
            $tag = "v1.2.0-nightly202609$('{0:d2}' -f $_)"
            $script:remoteReleases[$tag] = [pscustomobject]@{id=$_;tag_name=$tag;draft=$false;prerelease=$true;
                createdAt="2026-09-$('{0:d2}' -f $_)T12:00:00Z";body='';assets=@()}
            $script:remoteTags[$tag] = 'e' * 40
        }
    }
    try {
        Reset-Fake
        Add-OldReleases 14
        1..3 | ForEach-Object {
            $script:remoteReleases["stable$_"] = [pscustomobject]@{tag_name="stable$_";draft=$false;createdAt="2026-10-0${_}T12:00:00Z"}
        }
        $releases = @(Get-NightlyReleases $repository)
        Check ($releases.Count -eq 14 -and $releases[0].tagName -ceq 'v1.2.0-nightly20260914') 'Mixed list selected wrong Nightly baseline'
        Invoke-NightlyRetention $repository $cleanupPath $source $snapshot | Out-Null
        Check ($script:remoteReleases.Count -eq 17) 'Mixed list pruned a retained Nightly'
        Add-OldReleases 15
        $pruned = @(Invoke-NightlyRetention $repository $cleanupPath $source $snapshot)
        Check ($pruned.Count -eq 1 -and $pruned[0] -ceq 'v1.2.0-nightly20260901') 'Mixed retention did not prune only oldest Nightly'
        Check ($script:remoteReleases.ContainsKey('stable3')) 'Unrelated release was removed'
        Pass 'mixed release lists use the Nightly baseline and retain exactly fourteen Nightlies'

        foreach ($failure in @('ssh *vps* prepare', 'ssh *server1* prepare', 'scp *', 'scp *server1:*', 'ssh *vps* commit', 'ssh *server1* commit')) {
            Reset-Fake
            $pending = New-Pending
            $script:failOn = $failure
            Reject { Resume-Pending } '*injected staging failure*'
            Check ($script:remoteReleases.Count -eq 0) 'Staging failure made a release visible'
            Check ((Get-Content $statePath -Raw | ConvertFrom-Json).phase -eq 'prepared') 'Failed staging lost prepared state'
            Check ($script:remoteUploads.Count -eq 0) 'Handled staging failure leaked partial upload files'
            Resume-Pending
            Check (!$script:remoteReleases[$pending.tag].draft -and !(Test-Path $statePath)) 'Staging retry did not finish same artifact identity'
            $scpCalls = @($script:commands | Where-Object command -eq 'scp')
            Check (@($scpCalls | Where-Object { $_.argv[-1] -notmatch '/\.incoming-[0-9a-f]{64}-[0-9a-f]{32}/$' }).Count -eq 0) 'scp wrote into scanned builds'
        }
        Pass 'upload and verification failures on either host prevent publication; unchanged-source retry uses the recorded identity'

        Reset-Fake
        $pending = New-Pending
        $script:failOn = 'scp *'
        $script:failCleanup = $true
        Reject { Resume-Pending } '*injected staging failure*'
        $record = Get-Content $statePath -Raw | ConvertFrom-Json
        $token = $record.destinations[0].uploadToken
        Check ($token -cmatch '^[0-9a-f]{32}$' -and $script:remoteUploads.Count -eq 1) 'Failed cleanup lost upload recovery identity'
        $abandoned = "vps:~/ember-rooms/.incoming-$($pending.buildId)-$token/"
        $unrelated = "vps:~/ember-rooms/.incoming-$($pending.buildId)-$([Guid]::NewGuid().ToString('N'))/"
        $script:remoteUploads[$unrelated] = @('unrelated partial upload')
        $script:failCleanup = $false
        Resume-Pending
        Check (!$script:remoteUploads.ContainsKey($abandoned) -and $script:remoteUploads.ContainsKey($unrelated)) 'Recovery leaked owned upload or swept another upload'
        Check ($script:remoteUploads.Count -eq 1) 'Recovery created another abandoned upload'
        Reset-Fake
        $pending = New-Pending
        $script:failCleanup = $true
        Reject { Resume-Pending } '*injected upload cleanup failure*'
        $record = Get-Content $statePath -Raw | ConvertFrom-Json
        Check ($record.phase -ceq 'prepared' -and $record.destinations[0].uploadToken -cmatch '^[0-9a-f]{32}$') 'Cleanup failure after commit lost recovery token'
        Check ($script:remoteReleases.Count -eq 0) 'Unconfirmed upload cleanup reached publication'
        $script:failCleanup = $false
        Resume-Pending
        Check ($script:remoteUploads.Count -eq 0) 'Cleanup retry after commit leaked upload files'
        Pass 'failed cleanup retains an exact upload token and retry removes only its partial files'

        foreach ($hostIndex in @(0, 1)) {
            Reset-Fake
            $pending = New-Pending
            $token = [Guid]::NewGuid().ToString('N')
            $destination = $pending.destinations[$hostIndex]
            $destination | Add-Member -NotePropertyName uploadToken -NotePropertyValue $token
            Save-NightlyState $statePath $pending
            # Abrupt process death skips finally: persisted token and remote partial bytes survive.
            $abandoned = "$($destination.hostName):~/$($destination.root)/.incoming-$($pending.buildId)-$token/"
            $script:remoteUploads[$abandoned] = @('interrupted scp bytes')
            Resume-Pending
            Check ($script:remoteUploads.Count -eq 0) 'Interrupted upload was not recovered'
        }
        Pass 'process interruption on either destination recovers persisted partial uploads before preparing again'

        Reset-Fake
        $pending = New-Pending
        $pending.destinations[0] | Add-Member -NotePropertyName uploadToken -NotePropertyValue '../unrelated'
        Save-NightlyState $statePath $pending
        Reject { Resume-Pending } '*Invalid room-host upload token*'
        Check (@($script:commands | Where-Object command -in @('ssh', 'scp')).Count -eq 0) 'Malformed recovery token reached a remote operation'
        Pass 'invalid persisted upload tokens are rejected before remote mutations'

        Reset-Fake
        $pending = New-Pending
        Reject { Publish-NightlyRelease $pending $source } '*requires verified staging*'
        $pending.destinations = @()
        Reject { Complete-NightlyPublication $pending $statePath $cleanupPath $source $snapshot } '*mandatory VPS*'
        Check ($script:commands.Count -eq 0) 'Unstaged/missing-destination work reached external commands'
        $pending.binaries = @() # Explicit -SkipRoomHosts preparation still works.
        Save-NightlyState $statePath $pending
        Resume-Pending
        Check (@($script:commands | Where-Object command -in @('ssh','scp')).Count -eq 0) 'Skipped host preparation performed staging'
        Pass 'callable publication requires the staged phase and host work requires its VPS destination'

        Reset-Fake
        $pending = New-Pending
        $script:failOn = 'dirty-after-upload'
        Reject { Resume-Pending } '*requires committed source*'
        Check ($script:remoteReleases.Count -eq 0) 'Source edited during staging reached publication'
        Check ($script:remoteUploads.Count -eq 0) 'Source edit during staging leaked upload files'
        $script:dirty = @()
        Resume-Pending
        Pass 'an editor save during upload blocks the next external mutation and remains recoverable'

        foreach ($failure in @('after-create', 'starter-upload', 'after-upload', 'after-publish')) {
            Reset-Fake
            $pending = New-Pending
            $script:failOn = $failure
            Reject { Resume-Pending } '*injected failure*'
            Check (Test-Path $statePath) 'Uncertain mutation lost its recovery record'
            Resume-Pending
            Check (!$script:remoteReleases[$pending.tag].draft -and $script:remoteReleases.Count -eq 1) 'Recovery created a second release'
            $creates = @($script:commands | Where-Object { $_.command -eq 'gh' -and $_.argv[0] -eq 'release' -and $_.argv[1] -eq 'create' })
            Check ($creates.Count -eq 1) 'Recovery recreated release instead of finishing it'
            $remoteMutations = @($script:commands | Where-Object { $_.command -in @('ssh','scp') -or ($_.command -eq 'gh' -and $_.argv[0] -eq 'release' -and $_.argv[1] -eq 'create') })
            Check ($remoteMutations[0].command -eq 'ssh') 'Publication preceded staging'
        }
        Pass 'draft creation, upload and public visibility followed by failure resume without another label or rebuild'

        Reset-Fake
        Add-OldReleases
        $pending = New-Pending
        $script:failOn = 'delete-tag'
        Reject { Resume-Pending } '*tag cleanup failure*'
        Check ((Get-Content $statePath -Raw | ConvertFrom-Json).phase -eq 'published') 'Post-publication retention failure lost completion phase'
        $before = $script:commands.Count
        # Published work only needs retention; source can now have advanced cleanly.
        $script:head = 'f' * 40
        $script:fingerprint = 'A' * 64
        $current = Get-NightlySourceSnapshot $source
        $record = Get-Content $statePath -Raw | ConvertFrom-Json
        Complete-NightlyPublication $record $statePath $cleanupPath $source $current | Out-Null
        $later = @($script:commands | Select-Object -Skip $before)
        Check (@($later | Where-Object command -in @('ssh','scp')).Count -eq 0) 'Published retry repeated host staging'
        Check (@($later | Where-Object { $_.command -eq 'gh' -and $_.argv[1] -in @('create','upload','edit') }).Count -eq 0) 'Published retry mutated its release'
        Check (!(Test-Path $statePath) -and !(Test-Path $cleanupPath)) 'Retention retry did not finish'
        Pass 'publication followed by retention failure resumes maintenance even after clean source advances'

        foreach ($failure in @('after-delete-release', 'delete-tag', 'after-delete-tag')) {
            Reset-Fake
            Add-OldReleases
            $script:remoteTags['v1.2.0-nightly20260101'] = 'f' * 40 # unrelated orphan must survive
            $script:failOn = $failure
            Reject { Invoke-NightlyRetention $repository $cleanupPath $source $snapshot } '*injected*'
            $cleanup = Get-Content $cleanupPath -Raw | ConvertFrom-Json
            Check ($cleanup.targets.Count -eq 1 -and $cleanup.targets[0].tag -ceq 'v1.2.0-nightly20260901') 'Cleanup target was not narrowly persisted'
            Invoke-NightlyRetention $repository $cleanupPath $source $snapshot | Out-Null
            Check (!$script:remoteTags.ContainsKey('v1.2.0-nightly20260901')) 'Orphan tag was not recovered'
            Check ($script:remoteTags.ContainsKey('v1.2.0-nightly20260101')) 'Unrecorded orphan tag was swept'
        }
        Pass 'interrupted release/tag deletions recover their exact recorded target without sweeping other orphans'

        Reset-Fake
        Add-OldReleases
        $script:failOn = 'delete-tag'
        Reject { Invoke-NightlyRetention $repository $cleanupPath $source $snapshot } '*tag cleanup failure*'
        $script:remoteTags['v1.2.0-nightly20260901'] = 'f' * 40
        Reject { Invoke-NightlyRetention $repository $cleanupPath $source $snapshot } '*tag identity changed*'
        Check ($script:remoteTags['v1.2.0-nightly20260901'] -ceq ('f' * 40)) 'Repointed cleanup tag was deleted'
        Save-NightlyState $cleanupPath ([pscustomobject]@{repository=$repository;targets=@([pscustomobject]@{tag='v1.2.0';releaseId=1;tagObjectSha=''})})
        Reject { Invoke-NightlyRetention $repository $cleanupPath $source $snapshot } '*non-Nightly*'
        Reject { Invoke-NightlyRetention 'someone/another-repo' $cleanupPath $source $snapshot } '*confined*'
        Pass 'mutation guards reject unrelated repositories/tags and changed cleanup identities'

        Reset-Fake
        $pending = New-Pending
        $script:dirty = @(' M tracked.cxx')
        Reject { Get-NightlySourceSnapshot $source } '*requires committed source*'
        Reject { Resume-Pending } '*requires committed source*'
        Check (@($script:commands | Where-Object command -ne 'git').Count -eq 0) 'Dirty source reached external operations'
        $script:dirty = @('?? untracked.cxx')
        Reject { Get-NightlySourceSnapshot $source } '*requires committed source*'
        $script:dirty = @()
        $script:head = 'f' * 40
        Reject { Resume-Pending } '*Source commit changed*'
        $script:head = $snapshot.revision
        $script:fingerprint = 'C' * 64
        Reject { Resume-Pending } '*fingerprint changed*'
        # A canonical receipt accepting an edited tree still cannot satisfy the clean snapshot.
        $script:receipt.sourceFingerprint = $script:fingerprint
        Reject { Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot } '*receipt does not match*'
        $script:fingerprint = $snapshot.fingerprint
        $script:receipt.sourceFingerprint = $snapshot.fingerprint
        $script:receipt.baseRevision = 'f' * 40
        Reject { Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot } '*receipt does not match*'
        $script:receipt.baseRevision = $snapshot.revision
        Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot
        $packageDirectory = Join-Path $fixture 'package'
        New-Item -ItemType Directory -Path $packageDirectory | Out-Null
        $script:receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $packageDirectory 'build-provenance.json')
        Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot $packageDirectory
        @{sourceFingerprint=('C' * 64);baseRevision=$snapshot.revision} | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path $packageDirectory 'build-provenance.json')
        Reject { Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot $packageDirectory } '*Packaged receipt does not match*'
        Pass 'dirty/untracked source, changed HEAD/fingerprint and receipts from a different snapshot are rejected'

        Reset-Fake
        $pending = New-Pending
        Set-Content -LiteralPath $pending.assets[0].path -Value 'changed after preparation'
        Reject { Resume-Pending } '*Prepared artifact changed*'
        Check ($script:remoteReleases.Count -eq 0) 'Changed local artifact reached publication'
        Reset-Fake
        $pending = New-Pending
        $script:failOn = 'after-upload'
        Reject { Resume-Pending } '*injected failure*'
        $script:remoteReleases[$pending.tag].assets[0].digest = 'sha256:' + ('f' * 64)
        Reject { Resume-Pending } '*Remote asset identity*'
        Check ($script:remoteReleases[$pending.tag].draft) 'Conflicting remote asset became public'
        Pass 'changed local bytes or conflicting remote asset hashes prevent visibility'

        Reset-Fake
        $script:serverAvailable = $false
        $destinations = @(Get-NightlyRoomDestinations)
        Check ($destinations.Count -eq 1 -and $destinations[0].hostName -ceq 'vps') 'Optional server probe removed mandatory VPS'
        $script:serverAvailable = $true
        Check (@(Get-NightlyRoomDestinations).Count -eq 2) 'Reachable server1 was not recorded'
        $script:missingRepository = $true
        Check (@(Get-NightlyReleases $repository -WhatIf).Count -eq 0) 'WhatIf missing repository fallback failed'
        Reject { Get-NightlyReleases $repository } '*initial default-branch commit*'
        Pass 'optional server1 selection and repository WhatIf fallback retain explicit boundaries'
        Write-Output "$passed/$passed Nightly publication test groups passed; no external commands executed"
    } finally {
        # The resolved recursive target must be the exact isolated fixture we created.
        $resolved = [IO.Path]::GetFullPath($fixture)
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path $resolved -Leaf) -notmatch '^nightly-publication-[0-9a-f]{32}$') { throw 'Unsafe fixture cleanup target' }
        Remove-Item -LiteralPath $resolved -Recurse
    }
} finally {
    [cultureinfo]::CurrentCulture = $originalCulture
}
