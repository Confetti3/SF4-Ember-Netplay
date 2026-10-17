# Resolve each packaged file once; retain that selected source for Markdown links.
function Copy-EmberPackageContent(
    [string]$Repo, [string]$BuildDir, [string]$InstallDir, [string]$Destination,
    [object[]]$Entries, [string[]]$Generated, [string]$QuickStartPath,
    [switch]$SkipSelectionArt, [switch]$StagedBinariesOnly) {
    $records = [Collections.Generic.List[object]]::new()
    function Add-Record([string]$Source, [string]$Target, [string[]]$Aliases = @(), [bool]$Preferred = $false) {
        $records.Add([ordered]@{
            source = [IO.Path]::GetFullPath($Source)
            destination = $Target.Substring($Destination.TrimEnd('\','/').Length + 1).Replace('\','/')
            aliases = @($Aliases)
            preferred = $Preferred
        })
    }
    foreach ($entry in $Entries) {
        if ($Generated -contains $entry.Path) { continue }
        $relative = $entry.Path.Replace('\', [IO.Path]::DirectorySeparatorChar)
        $repositorySources = @((Join-Path $Repo $relative), (Join-Path $Repo ".github/$relative"))
        if ($relative -match '^docs[/\\]') {
            $repositorySources += @(Get-ChildItem -LiteralPath (Join-Path $Repo 'docs') -Directory |
                ForEach-Object { Join-Path $_.FullName (Split-Path $relative -Leaf) })
        }
        $aliases = @($repositorySources | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            ForEach-Object { [IO.Path]::GetFullPath($_) } | Select-Object -Unique)
        if ($aliases.Count -gt 1) { throw "Ambiguous repository inventory file: $relative" }
        $candidates = @((Join-Path $InstallDir $relative), (Join-Path $BuildDir "candidate/$relative"),
            (Join-Path $BuildDir $relative)) + $aliases
        # Private packages may only contain the binaries covered by the staged receipt.
        if ($StagedBinariesOnly -and [IO.Path]::GetExtension($relative) -in @('.dll','.exe')) {
            $candidates = @((Join-Path $InstallDir $relative))
        }
        $source = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if (!$source) { if ($entry.Required) { throw "Required package file missing: $relative" }; continue }
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
        Add-Record $source $target $aliases
    }
    if (!$QuickStartPath) { $QuickStartPath = Join-Path $Repo 'docs/guides/USER_NETPLAY.md' }
    if (!(Test-Path -LiteralPath $QuickStartPath -PathType Leaf)) { throw "Quick-start guide missing: $QuickStartPath" }
    $quickStart = (Resolve-Path -LiteralPath $QuickStartPath).Path
    $start = Join-Path $Destination 'START_HERE.md'
    Copy-Item -LiteralPath $quickStart -Destination $start
    Add-Record $quickStart $start @((Join-Path $Repo 'docs/guides/USER_NETPLAY.md')) $true
    if (!$SkipSelectionArt) {
        $art = (Resolve-Path -LiteralPath (Join-Path $Repo 'assets/selection')).Path
        foreach ($file in Get-ChildItem -LiteralPath $art -File -Recurse) {
            if ($file.Name -match '^color-\d+\.(jpg|png)$') {
                if (!(Test-Path -LiteralPath (Join-Path $file.DirectoryName ($file.BaseName + '-cutout.png')))) {
                    throw "Selection photograph has no cutout: $($file.FullName)"
                }
                continue
            }
            $target = Join-Path (Join-Path $Destination 'assets/selection') $file.FullName.Substring($art.Length + 1)
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
            Add-Record $file.FullName $target
        }
    }
    return $records.ToArray()
}
