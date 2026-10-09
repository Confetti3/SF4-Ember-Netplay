#Requires -Version 7
# Direct operation tests: all git/gh/SSH/scp/WSL commands and build steps are mocked.
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
    $dist = Join-Path $fixture 'dist'
    $statePath = Join-Path $dist 'nightly-pending.json'
    $cleanupPath = Join-Path $dist 'nightly-cleanup.json'
    $snapshot = [pscustomobject]@{revision=('a' * 40);fingerprint=('B' * 64)}
    $source = Join-Path $fixture 'checkout'
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
    function New-StdErr([string]$Text) { [Management.Automation.ErrorRecord]::new([Exception]::new($Text), 'NativeCommandError', 'NotSpecified', $null) }
    function ConvertTo-FakeJson([object]$Value) { $Value | ConvertTo-Json -Depth 8 -Compress }

    # Check the real command boundary and query classification before replacing them.
    function gh {
        $global:LASTEXITCODE = $script:nativeExit
        return $script:nativeOutput
    }
    try {
        function Set-Native([int]$Exit, [object[]]$Output) { $script:nativeExit = $Exit; $script:nativeOutput = $Output }
        $missing = ConvertTo-FakeJson @{data=@{repository=$null};errors=@(@{type='NOT_FOUND';path=@('repository');message='Could not resolve to a Repository'})}
        Set-Native 1 @($missing, (New-StdErr 'gh: Could not resolve to a Repository'))
        Check ((Get-NightlyRepositoryState $repository).status -ceq 'absent') 'Missing repository was not absent'
        Set-Native 0 @((ConvertTo-FakeJson @{data=@{repository=@{defaultBranchRef=$null}}}))
        Check ((Get-NightlyRepositoryState $repository).status -ceq 'absent') 'Empty repository was not absent'
        Set-Native 0 @((ConvertTo-FakeJson @{data=@{repository=@{defaultBranchRef=@{name='main'}}}}))
        Check ((Get-NightlyRepositoryState $repository).status -ceq 'found') 'Existing repository was not found'
        $forbidden = ConvertTo-FakeJson @{data=@{repository=$null};errors=@(@{type='FORBIDDEN';path=@('repository');message='Resource not accessible'})}
        $deeper = ConvertTo-FakeJson @{data=@{repository=@{release=$null}};errors=@(@{type='NOT_FOUND';path=@('repository','release');message='Not found'})}
        $errors = @(
            @(1, @((New-StdErr 'gh: HTTP 404: Not Found'))),
            @(1, @('Not Found (HTTP 404)', (New-StdErr 'gh: Could not resolve to a Repository'))),
            @(4, @((New-StdErr 'gh: authentication required'))),
            @(1, @((New-StdErr 'network timed out'))),
            @(1, @($forbidden)),
            @(1, @($deeper)))
        foreach ($case in $errors) {
            Set-Native $case[0] $case[1]
            Check ((Get-NightlyRepositoryState $repository).status -ceq 'error') "Failure was classified as absence: $($case[1] -join ' ')"
            Reject { Get-NightlyTagState $repository 'v1.2.0-nightly20261008' } '*Could not resolve Nightly release identity (error)*'
        }
        Set-Native 0 @((ConvertTo-FakeJson @{data=@{repository=@{release=$null;ref=$null}}}))
        $state = Get-NightlyTagState $repository 'v1.2.0-nightly20261008'
        Check (!$state.release -and !$state.tagSha) 'Absent release and tag were not empty fields'
        Set-Native 1 @((New-StdErr 'gh: HTTP 404: Not Found'))
        Reject { Invoke-NightlyCommand gh @('api', 'fixture') 'query failed' } '*query failed (exit 1)*HTTP 404*'
        Pass 'absence comes only from GraphQL JSON; 404 text, auth, network and other errors stay errors'
    } finally { Remove-Item Function:gh; Remove-Item Function:Set-Native }

    # From here on a real external command must never run, whatever replaces what.
    function gh { throw 'A Nightly test reached the real gh.' }
    function git { throw 'A Nightly test reached the real git.' }
    function ssh { throw 'A Nightly test reached the real ssh.' }
    function scp { throw 'A Nightly test reached the real scp.' }
    function wsl { throw 'A Nightly test reached the real wsl.' }

    function Reset-Fake {
        $script:commands = [Collections.Generic.List[object]]::new()
        $script:remoteReleases = @{}
        $script:remoteTags = @{}
        $script:remoteUploads = @{}
        $script:failCleanupAfter = ''
        $script:dirty = @()
        $script:head = $snapshot.revision
        $script:fingerprint = $snapshot.fingerprint
        $script:fingerprintCalls = 0
        $script:failOn = ''
        $script:serverAvailable = $true
        $script:repositoryMode = ''
        $script:gameRunning = $false
        $script:receipt = [pscustomobject]@{sourceFingerprint=$snapshot.fingerprint;baseRevision=$snapshot.revision}
        if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse }
        New-Item -ItemType Directory -Path $dist | Out-Null
    }
    function Get-SourceFingerprint([string]$SourceRoot) { $script:fingerprintCalls++; return $script:fingerprint }
    function Assert-BuildReceipt([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot) { return $script:receipt }
    function New-FakeExit([int]$Code, [object[]]$Output) { [pscustomobject]@{PSTypeName='NightlyFakeExit';exitCode=$Code;output=$Output} }
    # The single native boundary: fake failures become a non-zero exit, as a real command's would.
    function Invoke-NightlyNative([string]$Command, [string[]]$Arguments, [string]$InputText) {
        try { $output = @(Invoke-FakeCommand $Command $Arguments $InputText) }
        catch { return [pscustomobject]@{exitCode=1;output=@($_.Exception.Message)} }
        if ($output.Count -eq 1 -and $output[0].PSObject.TypeNames -contains 'NightlyFakeExit') { return $output[0] }
        return [pscustomobject]@{exitCode=0;output=$output}
    }
    function Invoke-FakeCommand([string]$Command, [string[]]$Arguments, [string]$InputText) {
        $script:commands.Add([pscustomobject]@{command=$Command;argv=$Arguments;inputText=$InputText})
        $text = "$Command $($Arguments -join ' ')"
        if ($Command -eq 'git') {
            if ($Arguments -contains 'status') { return $script:dirty }
            if ($Arguments -contains 'rev-parse') { return $script:head }
            if ($Arguments -contains 'branch') { return 'nightly' }
            if ($Arguments -contains 'fetch' -or $Arguments -contains 'merge' -or $Arguments -contains 'cat-file') { return }
            if ($Arguments -contains 'log') { return @('Add a fixture feature', 'Fix a fixture bug') }
            if ($Arguments -contains 'rev-list') { return '2' }
            if ($Arguments -contains 'archive') {
                Set-Content -LiteralPath $Arguments[[Array]::IndexOf($Arguments, '-o') + 1] -Value 'inert archive fixture'
                return
            }
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
                Check ($InputText -ceq $script:expectedStager) 'SSH did not receive the pinned staging script'
                if ($operation -eq 'prepare') {
                    Check (!$script:remoteUploads.ContainsKey($key)) 'Retry did not remove abandoned upload'
                    $script:remoteUploads[$key] = @()
                } elseif ($operation -eq 'cleanup') {
                    $previous = $script:commands[$script:commands.Count - 2]
                    if ($script:failCleanupAfter -and "$($previous.command) $($previous.argv -join ' ')" -like $script:failCleanupAfter) { throw 'injected upload cleanup failure' }
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
            if ($script:repositoryMode) { throw 'Could not resolve to a Repository' }
            $tag = $Arguments[2]
            switch ($Arguments[1]) {
                list {
                    return ConvertTo-Json -InputObject @($script:remoteReleases.Values | Where-Object { !$_.draft } |
                        ForEach-Object { [pscustomobject]@{tagName=$_.tag_name;createdAt=$_.createdAt} }) -Depth 8
                }
                view { return @{body=$script:remoteReleases[$tag].body} | ConvertTo-Json }
                create {
                    Check ($Arguments -contains '--draft') 'Release must be created as a draft'
                    if ($script:failOn -eq 'ignore-create') { return }
                    Check ($Arguments -notcontains '--notes-file') 'Draft notes were read from disk again'
                    $notesIndex = [Array]::IndexOf($Arguments, '--notes')
                    $script:remoteReleases[$tag] = [pscustomobject]@{id=100;tag_name=$tag;draft=$true;prerelease=$true;
                        body=$Arguments[$notesIndex + 1];assets=@();createdAt='2026-10-08T12:00:00Z'}
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
                    if ($script:failOn -eq 'ignore-publish') { return }
                    $script:remoteReleases[$tag].draft = $false
                    $script:remoteTags[$tag] = 'c' * 40
                    if ($script:failOn -eq 'after-publish') { $script:failOn=''; throw 'injected failure after public visibility' }
                }
                default { throw "Unexpected release command: $text" }
            }
            return
        }
        if ($Arguments[0] -eq 'api' -and $Arguments[1] -eq 'graphql') {
            $fields = @{}
            for ($index = 2; $index -lt $Arguments.Count; $index += 2) {
                Check ($Arguments[$index] -ceq '-f') 'GraphQL variables must be raw string fields'
                $name, $value = $Arguments[$index + 1].Split('=', 2)
                $fields[$name] = $value
            }
            Check ($fields.owner -ceq 'Confetti3' -and $fields.name -ceq 'SF4-Ember-Netplay-Nightly') 'GraphQL escaped Nightly repository'
            switch ($script:repositoryMode) {
                missing {
                    return New-FakeExit 1 @((ConvertTo-FakeJson @{data=@{repository=$null};errors=@(@{type='NOT_FOUND';path=@('repository');message='Could not resolve to a Repository'})}),
                        (New-StdErr 'gh: Could not resolve to a Repository'))
                }
                denied { return New-FakeExit 4 @((New-StdErr 'gh: authentication required')) }
            }
            if ($fields.query -like '*defaultBranchRef*') {
                $branch = if ($script:repositoryMode -eq 'empty') { $null } else { @{name='main'} }
                return ConvertTo-FakeJson @{data=@{repository=@{defaultBranchRef=$branch}}}
            }
            $tag = $fields.tag
            Check ($fields.ref -ceq "refs/tags/$tag") 'Release and tag queries name different tags'
            # The first read inside publication comes after the notes were verified.
            if ($script:failOn -eq 'notes-before-create') { $script:failOn=''; Set-Content -LiteralPath $script:notesPath -Value 'edited after verification' }
            $release = if ($script:remoteReleases.ContainsKey($tag)) { @{databaseId=$script:remoteReleases[$tag].id} } else { $null }
            $ref = if ($script:remoteTags.ContainsKey($tag)) { @{target=@{oid=$script:remoteTags[$tag]}} } else { $null }
            return ConvertTo-FakeJson @{data=@{repository=@{release=$release;ref=$ref}}}
        }
        if ($Arguments[0] -eq 'api') {
            $endpoint = @($Arguments | Where-Object { $_ -like 'repos/*' })[0]
            Check ($endpoint.StartsWith("repos/$repository/")) 'API escaped the Nightly repository'
            if ($endpoint -match '/releases/assets/([0-9]+)$' -and $Arguments -contains 'DELETE') {
                $id = $Matches[1]
                foreach ($release in $script:remoteReleases.Values) {
                    $release.assets = @($release.assets | Where-Object id -NE $id)
                }
                return
            } elseif ($endpoint -match '/releases/([0-9]+)$' -and $Arguments -contains 'DELETE') {
                $id = [int]$Matches[1]
                Check (Test-Path -LiteralPath $cleanupPath) 'Deletion intent was not persisted'
                $release = $script:remoteReleases.Values | Where-Object id -EQ $id
                if ($script:failOn -eq 'replace-release') {
                    # Another release takes the tag between inspection and deletion.
                    $script:failOn = ''
                    $script:remoteReleases[$release.tag_name] = [pscustomobject]@{id=777;tag_name=$release.tag_name;draft=$false;prerelease=$true;
                        createdAt=$release.createdAt;body='replacement';assets=@()}
                    $release = $null
                }
                if (!$release) { throw 'gh: Not Found (HTTP 404)' }
                if ($script:failOn -eq 'ignore-delete-release') { return }
                $script:remoteReleases.Remove($release.tag_name)
                if ($script:failOn -eq 'after-delete-release') { $script:failOn=''; throw 'injected failure after release deletion' }
                if ($script:failOn -eq 'arrive-during-retention') { $script:failOn=''; Add-OldReleases 1 '08' }
                return
            } elseif ($endpoint -match '/releases/([0-9]+)$') {
                $id = $Matches[1]
                $release = $script:remoteReleases.Values | Where-Object id -EQ $id
                if ($release) { return $release | ConvertTo-Json -Depth 8 }
                throw 'gh: Not Found (HTTP 404)'
            } elseif ($endpoint -match '/git/matching-refs/tags/(.+)$') {
                $prefix = $Matches[1]
                return @($script:remoteTags.Keys | Where-Object { $_.StartsWith($prefix) } | ForEach-Object { "refs/tags/$_" })
            } elseif ($endpoint -match '/git/refs/tags/(.+)$' -and $Arguments -contains 'DELETE') {
                $tag = $Matches[1]
                Check (Test-Path -LiteralPath $cleanupPath) 'Tag deletion intent was not persisted'
                if ($script:failOn -eq 'delete-tag') { $script:failOn=''; throw 'injected tag cleanup failure' }
                $script:remoteTags.Remove($tag)
                if ($script:failOn -eq 'after-delete-tag') { $script:failOn=''; throw 'injected failure after tag deletion' }
                return
            }
            throw "Unexpected API operation: $text"
        }
        throw "Unexpected gh operation: $text"
    }
    function Write-Fixture([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false)) }
    function New-Pending {
        $assetPath = Join-Path $fixture 'package.zip'
        $notesPath = Join-Path $fixture 'notes.md'
        $script:notesPath = $notesPath
        $hostPath = Join-Path $fixture 'sf4e-room-host'
        $netPath = Join-Path $fixture 'sf4-net'
        $stagerPath = Join-Path $fixture 'stage-nightly-room-hosts.sh'
        Set-Content -LiteralPath $assetPath -Value 'inert package fixture'
        Set-Content -LiteralPath $notesPath -Value "Source: $($snapshot.revision)"
        Set-Content -LiteralPath $hostPath -Value 'inert host fixture'
        Set-Content -LiteralPath $netPath -Value 'inert helper fixture'
        $script:expectedStager = "#!/usr/bin/env bash`n# inert pinned staging fixture`n"
        Write-Fixture $stagerPath $script:expectedStager
        $pending = [pscustomobject]@{repository=$repository;tag='v1.2.0-nightly20261008';label='1.2.0-nightly20261008';
            phase='prepared';snapshot=$snapshot;buildId=('d' * 64);
            assets=@(Get-NightlyArtifact $assetPath);notes=(Get-NightlyArtifact $notesPath);
            binaries=@((Get-NightlyArtifact $hostPath), (Get-NightlyArtifact $netPath));
            stagingScript=(Get-NightlyArtifact $stagerPath);
            destinations=@([pscustomobject]@{hostName='vps';root='ember-rooms';uploadToken=[Guid]::NewGuid().ToString('N')},
                           [pscustomobject]@{hostName='server1';root='ember-rooms-box';uploadToken=[Guid]::NewGuid().ToString('N')})}
        Save-NightlyState $statePath $pending
        return $pending
    }
    # A resumed run: capture the clean snapshot, then finish the recorded work.
    function Resume-Pending {
        $current = Get-NightlySourceSnapshot $source
        $record = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        Complete-NightlyPublication $record $statePath $cleanupPath $current | Out-Null
    }
    function Add-OldReleases([int]$Count = 15, [string]$Month = '09') {
        1..$Count | ForEach-Object {
            $tag = "v1.2.0-nightly2026$Month$('{0:d2}' -f $_)"
            $script:remoteReleases[$tag] = [pscustomobject]@{id=([int]$Month * 100 + $_);tag_name=$tag;draft=$false;prerelease=$true;
                createdAt="2026-$Month-$('{0:d2}' -f $_)T12:00:00Z";body='';assets=@()}
            $script:remoteTags[$tag] = 'e' * 40
        }
    }
    function Get-CommandLabels { @($script:commands | ForEach-Object { "$($_.command) $($_.argv -join ' ')" }) }
    function Find-Command([string]$Pattern) {
        $labels = Get-CommandLabels
        for ($index = 0; $index -lt $labels.Count; $index++) { if ($labels[$index] -like $Pattern) { return $index } }
        return -1
    }
    function Test-Ran([string]$Pattern) { return (Find-Command $Pattern) -ge 0 }

    # A checkout and build steps for the whole run; each step is recorded with the commands.
    New-Item -ItemType Directory -Path (Join-Path $source 'scripts') -Force | Out-Null
    Write-Fixture (Join-Path $source 'CMakeLists.txt') "project(SF4E`n    VERSION 1.2.0`n)`n"
    Write-Fixture (Join-Path $source 'scripts/stage-nightly-room-hosts.sh') "#!/usr/bin/env bash`r`n# inert checkout staging fixture`r`n"
    function Add-Step([string]$Name) { $script:commands.Add([pscustomobject]@{command='op';argv=@($Name);inputText=''}) }
    $operations = @{
        Target = { param($Root) [pscustomobject]@{channel='nightly';branch='nightly';githubRepo=$repository;buildDirectory='build';installDirectory='build/stage'} }
        GameRunning = { $script:gameRunning }
        Now = { [DateTime]::new(2026, 10, 8, 12, 0, 0, [DateTimeKind]::Utc) }
        ToolPaths = { param($Root, $VisualStudio) Add-Step 'ToolPaths'; [pscustomobject]@{VisualStudioPath='C:\fixture\VS'} }
        Build = {
            param($VisualStudio, $DiscordSdk)
            Add-Step 'Build'
            New-Item -ItemType Directory -Path (Join-Path $source 'build/stage') -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $source 'build/stage/Sidecar.dll') -Value 'inert sidecar fixture'
        }
        Package = {
            param($OutDir, $Label)
            Add-Step 'Package'
            $folder = Join-Path $OutDir "sf4-ember-netplay-$Label"
            New-Item -ItemType Directory -Path $folder | Out-Null
            Copy-Item -LiteralPath (Join-Path $source 'build/stage/Sidecar.dll') -Destination $folder
            $script:receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'build-provenance.json')
            Set-Content -LiteralPath "$folder.zip" -Value 'inert zip fixture'
            Set-Content -LiteralPath "$folder.zip.sha256" -Value 'inert zip hash fixture'
            [pscustomobject]@{zipPath="$folder.zip";folderPath=$folder;gitRev=$script:head}
        }
        Installer = {
            param($PackageDir, $Label, $OutDir, $VisualStudio)
            Add-Step 'Installer'
            Check ($VisualStudio -ceq 'C:\fixture\VS') 'Installer did not reuse the resolved Visual Studio'
            $installer = Join-Path $OutDir "sf4-ember-netplay-$Label-setup.exe"
            Set-Content -LiteralPath $installer -Value 'inert installer fixture'
            Set-Content -LiteralPath "$installer.sha256" -Value 'inert installer hash fixture'
            $installer
        }
        LinuxBuild = {
            param($Revision, $ArchivePath, $HostDirectory)
            Add-Step 'LinuxBuild'
            Check ($Revision -ceq $snapshot.revision -and (Test-Path -LiteralPath $ArchivePath)) 'Linux build lacks the committed source archive'
            foreach ($name in @('sf4e-room-host', 'sf4-net')) { Set-Content -LiteralPath (Join-Path $HostDirectory $name) -Value "inert $name fixture" }
        }
    }
    function Invoke-Publish([hashtable]$Switches = @{}) {
        Invoke-NightlyPublish $source $dist $operations @Switches | Out-Null
    }
    try {
        Reset-Fake
        # Both dates parse without throwing under nl-BE, but Parse swaps their order.
        $script:remoteReleases = [ordered]@{
            'v1.2.0-nightly20260910' = [pscustomobject]@{tag_name='v1.2.0-nightly20260910';draft=$false;createdAt='2026-09-10T12:00:00Z'}
            'v1.2.0-nightly20261008' = [pscustomobject]@{tag_name='v1.2.0-nightly20261008';draft=$false;createdAt='2026-10-08T12:00:00Z'}
        }
        $releases = @(Get-NightlyReleases $repository)
        Check (($releases.tagName -join ',') -ceq 'v1.2.0-nightly20261008,v1.2.0-nightly20260910') 'Ambiguous dates selected wrong descending Nightly tag order'
        Pass 'ambiguous day/month dates use the complete descending Nightly tag order'

        Reset-Fake
        Add-OldReleases 14
        1..3 | ForEach-Object {
            $script:remoteReleases["stable$_"] = [pscustomobject]@{tag_name="stable$_";draft=$false;createdAt="2026-10-0${_}T12:00:00Z"}
        }
        $releases = @(Get-NightlyReleases $repository)
        Check ($releases.Count -eq 14 -and $releases[0].tagName -ceq 'v1.2.0-nightly20260914') 'Mixed list selected wrong Nightly baseline'
        Invoke-NightlyRetention $repository $cleanupPath | Out-Null
        Check ($script:remoteReleases.Count -eq 17) 'Mixed list pruned a retained Nightly'
        Add-OldReleases 15
        $pruned = @(Invoke-NightlyRetention $repository $cleanupPath)
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
            $before = $script:commands.Count
            Resume-Pending
            Check (!$script:remoteReleases[$pending.tag].draft -and !(Test-Path $statePath)) 'Staging retry did not finish same artifact identity'
            $scpCalls = @($script:commands | Where-Object command -eq 'scp')
            Check (@($scpCalls | Where-Object { $_.argv[-1] -notmatch '/\.incoming-[0-9a-f]{64}-[0-9a-f]{32}/$' }).Count -eq 0) 'scp wrote into scanned builds'
            $vps = @($script:commands | Select-Object -Skip $before | Where-Object { $_.command -in @('ssh', 'scp') -and ($_.argv -join ' ') -match '\bvps\b' } |
                ForEach-Object { if ($_.command -eq 'scp') { 'scp' } else { ($_.argv[-1] -split ' ')[-1] } })
            Check (($vps -join ',') -ceq 'cleanup,prepare,scp,commit,cleanup') "Staging order was $($vps -join ',')"
        }
        Pass 'upload and verification failures on either host prevent publication; retries clean, prepare, upload and commit with the recorded identity'

        Reset-Fake
        $pending = New-Pending
        $script:failOn = 'scp *'
        $script:failCleanupAfter = 'scp *'
        Reject { Resume-Pending } '*injected staging failure*'
        $record = Get-Content $statePath -Raw | ConvertFrom-Json
        $token = $record.destinations[0].uploadToken
        Check ($token -ceq $pending.destinations[0].uploadToken -and $script:remoteUploads.Count -eq 1) 'Failed cleanup lost upload recovery identity'
        $abandoned = "vps:~/ember-rooms/.incoming-$($pending.buildId)-$token/"
        $unrelated = "vps:~/ember-rooms/.incoming-$($pending.buildId)-$([Guid]::NewGuid().ToString('N'))/"
        $script:remoteUploads[$unrelated] = @('unrelated partial upload')
        $script:failCleanupAfter = ''
        Resume-Pending
        Check (!$script:remoteUploads.ContainsKey($abandoned) -and $script:remoteUploads.ContainsKey($unrelated)) 'Recovery leaked owned upload or swept another upload'
        Check ($script:remoteUploads.Count -eq 1) 'Recovery created another abandoned upload'
        Reset-Fake
        $pending = New-Pending
        $script:failCleanupAfter = 'ssh * commit'
        Reject { Resume-Pending } '*injected upload cleanup failure*'
        $record = Get-Content $statePath -Raw | ConvertFrom-Json
        Check ($record.phase -ceq 'prepared' -and $record.destinations[0].uploadToken -cmatch '^[0-9a-f]{32}$') 'Cleanup failure after commit lost recovery token'
        Check ($script:remoteReleases.Count -eq 0) 'Unconfirmed upload cleanup reached publication'
        $script:failCleanupAfter = ''
        Resume-Pending
        Check ($script:remoteUploads.Count -eq 0) 'Cleanup retry after commit leaked upload files'
        Pass 'failed cleanup keeps the recorded upload token and retry removes only its partial files'

        foreach ($hostIndex in @(0, 1)) {
            Reset-Fake
            $pending = New-Pending
            $destination = $pending.destinations[$hostIndex]
            # Abrupt process death skips finally: the recorded token and remote partial bytes survive.
            $abandoned = "$($destination.hostName):~/$($destination.root)/.incoming-$($pending.buildId)-$($destination.uploadToken)/"
            $script:remoteUploads[$abandoned] = @('interrupted scp bytes')
            Resume-Pending
            Check ($script:remoteUploads.Count -eq 0) 'Interrupted upload was not recovered'
        }
        Pass 'process interruption on either destination recovers persisted partial uploads before preparing again'

        foreach ($token in @('../unrelated', '')) {
            Reset-Fake
            $pending = New-Pending
            $pending.destinations[0].uploadToken = $token
            Save-NightlyState $statePath $pending
            Reject { Resume-Pending } '*Invalid room-host upload token*'
            Check (@($script:commands | Where-Object command -in @('ssh', 'scp')).Count -eq 0) 'Malformed or missing upload token reached a remote operation'
        }
        Pass 'invalid or missing recorded upload tokens are rejected before remote mutations'

        Reset-Fake
        $pending = New-Pending
        Reject { Publish-NightlyRelease $pending } '*requires verified staging*'
        $saved = $pending.stagingScript
        $pending.stagingScript = $null
        Reject { Complete-NightlyPublication $pending $statePath $cleanupPath $snapshot } '*prepared staging script*'
        $pending.stagingScript = $saved
        $pending.destinations = @()
        Reject { Complete-NightlyPublication $pending $statePath $cleanupPath $snapshot } '*mandatory VPS*'
        Check ($script:commands.Count -eq 0) 'Unstaged/missing-destination work reached external commands'
        $pending.binaries = @() # Explicit -SkipRoomHosts preparation still works.
        $pending.stagingScript = $null
        Save-NightlyState $statePath $pending
        Resume-Pending
        Check (@($script:commands | Where-Object command -in @('ssh','scp')).Count -eq 0) 'Skipped host preparation performed staging'
        Pass 'callable publication requires the staged phase and host work requires its VPS destination and staging script'

        Reset-Fake
        $pending = New-Pending
        $script:failOn = 'dirty-after-upload'
        Resume-Pending
        $firstRemote = Find-Command 'ssh *vps* cleanup'
        Check (!(Test-Path $statePath) -and !$script:remoteReleases[$pending.tag].draft) 'A source edit after preparation stopped pinned publication'
        Check (@(Get-CommandLabels | Select-Object -Skip $firstRemote | Where-Object { $_ -like 'git *' }).Count -eq 0) 'Source was consulted between remote operations'
        Reset-Fake
        $pending = New-Pending
        Write-Fixture $pending.stagingScript.path "#!/usr/bin/env bash`nrm -rf ~`n"
        Reject { Resume-Pending } '*Prepared artifact changed*'
        Check (@($script:commands | Where-Object command -in @('ssh', 'scp')).Count -eq 0) 'A changed staging script reached SSH'
        Pass 'remote steps use only the pinned files: later source edits are ignored and a changed staging script is refused'

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

        foreach ($ignored in @('ignore-create', 'ignore-publish')) {
            Reset-Fake
            $pending = New-Pending
            $script:failOn = $ignored
            Reject { Resume-Pending } '*could not be confirmed by reading the release back*'
            foreach ($action in @('create', 'edit')) {
                Check (@($script:commands | Where-Object { $_.command -eq 'gh' -and $_.argv[1] -eq $action }).Count -le 1) "An unconfirmed $action was repeated"
            }
            Check ((Get-Content $statePath -Raw | ConvertFrom-Json).phase -ceq 'staged') 'Unconfirmed publication advanced its phase'
        }
        Pass 'a release step that the read-back does not show stops the run instead of repeating it'

        Reset-Fake
        $pending = New-Pending
        $script:failOn = 'notes-before-create'
        Resume-Pending
        $release = $script:remoteReleases[$pending.tag]
        Check (!$release.draft -and $release.body -ceq "Source: $($snapshot.revision)") 'Notes edited after verification reached the draft'
        Pass 'draft notes and the body comparison use the verified notes text, not a later read of the file'

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
        Resume-Pending
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
            Reject { Invoke-NightlyRetention $repository $cleanupPath } '*injected*'
            $cleanup = Get-Content $cleanupPath -Raw | ConvertFrom-Json
            Check ($cleanup.targets.Count -eq 1 -and $cleanup.targets[0].tag -ceq 'v1.2.0-nightly20260901') 'Cleanup target was not narrowly persisted'
            # Deletion never depends on the working tree.
            $script:dirty = @(' M tracked.cxx')
            Invoke-NightlyRetention $repository $cleanupPath | Out-Null
            Check (!$script:remoteTags.ContainsKey('v1.2.0-nightly20260901')) 'Orphan tag was not recovered'
            Check ($script:remoteTags.ContainsKey('v1.2.0-nightly20260101')) 'Unrecorded orphan tag was swept'
            Check (!(Test-Ran 'git *')) 'Retention consulted the working tree'
        }
        Pass 'interrupted release/tag deletions recover their exact recorded target without sweeping other orphans or reading the tree'

        Reset-Fake
        Add-OldReleases
        $script:failOn = 'delete-tag'
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*tag cleanup failure*'
        $script:remoteTags['v1.2.0-nightly20260901'] = 'f' * 40
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*tag identity changed*'
        Check ($script:remoteTags['v1.2.0-nightly20260901'] -ceq ('f' * 40)) 'Repointed cleanup tag was deleted'
        Save-NightlyState $cleanupPath ([pscustomobject]@{repository=$repository;targets=@([pscustomobject]@{tag='v1.2.0';releaseId=1;tagObjectSha=''})})
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*non-Nightly*'
        Reject { Invoke-NightlyRetention 'someone/another-repo' $cleanupPath } '*confined*'
        Pass 'mutation guards reject unrelated repositories/tags and changed cleanup identities'

        $oldest = 'v1.2.0-nightly20260901'
        Reset-Fake
        Add-OldReleases
        Save-NightlyState $cleanupPath ([pscustomobject]@{repository=$repository;targets=@([pscustomobject]@{tag=$oldest;releaseId=901;tagObjectSha=('e' * 40)})})
        $script:remoteTags[$oldest] = 'f' * 40
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*tag identity changed*'
        Check ($script:remoteReleases.ContainsKey($oldest) -and !(Test-Ran 'gh api --method DELETE *')) 'A repointed tag lost its release before the identity check'
        Reset-Fake
        Add-OldReleases
        $script:failOn = 'replace-release'
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*Could not prune Nightly release*'
        Check ($script:remoteReleases[$oldest].id -eq 777 -and $script:remoteTags.ContainsKey($oldest)) 'A replacement release or its tag was deleted'
        Check ((Get-Content $cleanupPath -Raw | ConvertFrom-Json).targets[0].tag -ceq $oldest) 'Failed release deletion retired its target'
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*release identity changed*'
        Check ($script:remoteReleases[$oldest].id -eq 777) 'A replacement release was deleted on retry'
        Check (!(Test-Ran 'gh release delete *')) 'A release was deleted by its mutable tag'
        Pass 'retention checks release and tag identity before deleting, and deletes the release by its recorded ID'

        Reset-Fake
        Add-OldReleases
        $script:failOn = 'ignore-delete-release'
        Reject { Invoke-NightlyRetention $repository $cleanupPath } '*could not be confirmed by reading it back*'
        Check ((Get-Content $cleanupPath -Raw | ConvertFrom-Json).targets[0].tag -ceq $oldest) 'An unconfirmed deletion retired its target'
        Check (@(Get-CommandLabels | Where-Object { $_ -like 'gh api --method DELETE */releases/901' }).Count -eq 1) 'An unconfirmed deletion was repeated'
        Reset-Fake
        Add-OldReleases
        $script:failOn = 'arrive-during-retention'
        $pruned = @(Invoke-NightlyRetention $repository $cleanupPath)
        Check ($pruned.Count -eq 1 -and $pruned[0] -ceq $oldest -and !(Test-Path $cleanupPath)) 'Retention did not finish its saved plan'
        Check ($script:remoteReleases.ContainsKey('v1.2.0-nightly20260801')) 'A release arriving mid-run joined the running plan'
        $pruned = @(Invoke-NightlyRetention $repository $cleanupPath)
        Check ($pruned.Count -eq 1 -and $pruned[0] -ceq 'v1.2.0-nightly20260801') 'The next run did not take the newly arrived work'
        Pass 'a target is retired only after read-back shows it gone, and one run finishes one saved plan'

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
        Check (@($script:commands | Where-Object command -ne 'git').Count -eq 0) 'Changed source reached external operations'
        # A canonical receipt accepting an edited tree still cannot satisfy the clean snapshot.
        $script:receipt.sourceFingerprint = $script:fingerprint
        Reject { Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot } '*receipt does not match*'
        $script:fingerprint = $snapshot.fingerprint
        $script:receipt.sourceFingerprint = $snapshot.fingerprint
        $script:receipt.baseRevision = 'f' * 40
        Reject { Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot } '*receipt does not match*'
        $script:receipt.baseRevision = $snapshot.revision
        Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot
        $script:head = 'f' * 40
        Reject { Assert-NightlyBuildSnapshot $source 'build' 'stage' $snapshot } '*Source commit changed*'
        $script:head = $snapshot.revision
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
        $destinations = @(Get-NightlyRoomDestinations)
        Check ($destinations.Count -eq 2) 'Reachable server1 was not recorded'
        Check (@($destinations | Where-Object { $_.uploadToken -cmatch '^[0-9a-f]{32}$' }).Count -eq 2 -and
               $destinations[0].uploadToken -cne $destinations[1].uploadToken) 'Destinations lack their own upload tokens'
        Pass 'optional server1 selection keeps the VPS and gives each destination its own upload token'

        # The publisher's whole run, with build steps and every external command faked.
        Reset-Fake
        $script:expectedStager = "#!/usr/bin/env bash`n# inert checkout staging fixture`n"
        Invoke-Publish
        $order = @('git * fetch origin nightly', 'git * merge --ff-only origin/nightly', 'gh api graphql *defaultBranchRef*',
            'gh release list *', 'gh api repos/*/git/matching-refs/tags/v1.2.0-nightly20261008 *', 'git * fetch origin release',
            'git * log *', 'op Build', 'op Package', 'op Installer', 'git * archive *', 'op LinuxBuild', 'ssh * server1 true',
            'ssh *vps* cleanup', 'ssh *vps* prepare', 'scp *', 'ssh *vps* commit', 'ssh *server1* commit',
            'gh release create v1.2.0-nightly20261008 *', 'gh release upload *', 'gh release edit *')
        $last = -1
        foreach ($pattern in $order) {
            $index = Find-Command $pattern
            Check ($index -gt $last) "Publisher order broke at: $pattern"
            $last = $index
        }
        $release = $script:remoteReleases['v1.2.0-nightly20261008']
        Check (!$release.draft -and @($release.assets).Count -eq 4 -and !(Test-Path $statePath)) 'Fresh run did not publish its four assets'
        Check ($release.body -like "*Source: $($snapshot.revision)*- Fix a fixture bug*") 'Release notes lack the source and subjects'
        Check ($script:fingerprintCalls -eq 2) "Fresh run fingerprinted the source $($script:fingerprintCalls) times"
        Check (@(Get-CommandLabels | Select-Object -Skip (Find-Command 'ssh *vps* cleanup') | Where-Object { $_ -like 'git *' }).Count -eq 0) 'Fresh run consulted the source between remote operations'
        Pass 'a fresh run syncs, builds, packages, prepares hosts, then stages and publishes in order with one prepare-time source check'

        Reset-Fake
        $script:expectedStager = "#!/usr/bin/env bash`n# inert checkout staging fixture`n"
        Invoke-Publish @{WhatIf=$true}
        Check ((Test-Ran 'op Build') -and (Test-Ran 'op Package') -and (Test-Ran 'op Installer') -and (Test-Ran 'op LinuxBuild')) 'Dry run skipped preparation'
        Check (!(Test-Ran 'ssh *') -and !(Test-Ran 'scp *')) 'Dry run reached a host'
        Check (!(Test-Ran 'gh release create*') -and !(Test-Ran 'gh release delete*') -and !(Test-Ran 'gh api --method*')) 'Dry run changed GitHub'
        Check (!(Test-Path $statePath) -and $script:remoteReleases.Count -eq 0) 'Dry run left a pending record or release'
        Pass 'a dry run prepares everything locally and records or changes nothing remote'

        Reset-Fake
        Invoke-Publish @{SkipRoomHosts=$true}
        Check (!(Test-Ran 'op LinuxBuild') -and !(Test-Ran 'git * archive *') -and !(Test-Ran 'ssh *') -and !(Test-Ran 'scp *')) 'Skipped room hosts were built or staged'
        Check (!$script:remoteReleases['v1.2.0-nightly20261008'].draft -and !(Test-Path $statePath)) 'A run without room hosts did not publish'
        Pass 'a run without room hosts publishes without building, pinning or staging them'

        Reset-Fake
        $script:repositoryMode = 'missing'
        Invoke-Publish @{WhatIf=$true}
        Check (!(Test-Ran 'gh release *') -and !(Test-Ran 'gh api repos/*')) 'Dry run read history from a missing repository'
        Check ((Test-Ran 'git * fetch origin release') -and (Test-Ran 'op Package')) 'Dry run without history did not use the Stable baseline'
        Check ((Get-ChildItem -LiteralPath $dist -Filter 'nightly-1.2.0-nightly20261008.md').Count -eq 1) 'Dry run did not keep the base label'
        foreach ($mode in @('missing', 'empty')) {
            Reset-Fake
            $script:repositoryMode = $mode
            Reject { Invoke-Publish } '*must exist with an initial default-branch commit*'
            Check (!(Test-Ran 'op Build')) "A $mode repository reached the build"
        }
        Reset-Fake
        $script:repositoryMode = 'denied'
        Reject { Invoke-Publish @{WhatIf=$true} } '*Could not inspect Nightly repository*'
        Check (!(Test-Ran 'op Build')) 'An authentication failure was treated as an empty history'
        Pass 'only a dry run treats a missing or empty repository as no history; query errors stop every run'

        Reset-Fake
        $pending = New-Pending
        Invoke-Publish @{WhatIf=$true; Force=$true}
        Check ((Test-Path $statePath) -and !(Test-Ran 'ssh *') -and !(Test-Ran 'gh *')) 'Dry run changed pending work'
        Invoke-Publish @{Force=$true}
        Check (!(Test-Ran 'git * fetch *') -and !(Test-Ran 'op *')) 'Pending resume fetched or rebuilt'
        Check (!(Test-Path $statePath) -and !$script:remoteReleases[$pending.tag].draft) 'Pending resume did not finish the recorded release'
        Check ($script:fingerprintCalls -eq 2) 'Pending resume fingerprinted the source more than once per run'
        Pass 'pending work resumes from its record without fetch or rebuild, and a dry run leaves it untouched'

        Reset-Fake
        Add-OldReleases 15
        $script:remoteReleases['v1.2.0-nightly20261007'] = [pscustomobject]@{id=99;tag_name='v1.2.0-nightly20261007';draft=$false;prerelease=$true;
            createdAt='2026-10-07T12:00:00Z';body="Notes`nSource: $($snapshot.revision)`n";assets=@()}
        Invoke-Publish
        Check (!(Test-Ran 'op *') -and !$script:remoteReleases.ContainsKey('v1.2.0-nightly20260902')) 'Unchanged source rebuilt or skipped retention'
        Check ($script:remoteReleases.Count -eq 14) 'Unchanged source retention kept the wrong set'
        Reset-Fake
        $script:gameRunning = $true
        Invoke-Publish
        Check (!(Test-Ran 'git * fetch *') -and !(Test-Ran 'op *') -and !(Test-Ran 'gh *')) 'A running game did not skip the run'
        Pass 'unchanged source only runs retention, and a running game skips the run'
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
