# Publish an already-tagged, locally verified Ember release. Never rebuild in CI.
param([Parameter(Mandatory=$true)][string]$Tag,
      [string]$Repo = 'Confetti3/SF4-Ember-Netplay', [string]$OutDir = 'dist',
      [string]$NotesFile = '', [string]$PreviousVersion = '')
$ErrorActionPreference = 'Stop'
$source = Split-Path $PSScriptRoot -Parent
$releaseRepository = $Repo
Push-Location $source
try {
    if ($Tag -notmatch '^v([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Use a full product release tag, such as v0.8.0.' }
    $version = $Matches[1]
    if (@(& git diff --name-only HEAD).Count -or @(& git ls-files --others --exclude-standard).Count) { throw 'Commit the reviewed release source before packaging.' }
    $revision = & git rev-parse HEAD
    $tagRevision = & git rev-parse "$Tag^{commit}"
    if ($LASTEXITCODE -or $tagRevision -ne $revision) { throw 'The release tag must point at the current source commit.' }
    $versionLine = Get-Content CMakeLists.txt | Where-Object { $_ -match '^\s+VERSION ' }
    if ($versionLine.Trim() -ne "VERSION $version") { throw 'Tag and CMake product version differ.' }
    if (!$NotesFile) { $NotesFile = "docs/release-notes/RELEASE_NOTES_$Tag.md" }
    if (!(Test-Path -LiteralPath $NotesFile)) { throw "Missing release notes: $NotesFile" }
    & gh release view $Tag --repo $releaseRepository 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { throw 'The release already exists; refusing to replace published assets.' }
    if (!$PreviousVersion) {
        $latest = (& gh release view --repo $releaseRepository --json tagName | ConvertFrom-Json).tagName
        if ($latest -notmatch '^v([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Could not resolve the previous published version.' }
        $PreviousVersion = $Matches[1]
    }
    if ($PreviousVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or $PreviousVersion -eq $version) { throw 'Invalid previous release version.' }
    $baseZip = Join-Path $OutDir "sf4-ember-netplay-$PreviousVersion.zip"
    if (!(Test-Path -LiteralPath $baseZip -PathType Leaf) -or !(Test-Path -LiteralPath ($baseZip + '.sha256') -PathType Leaf)) {
        throw "Keep the verified previous full package and checksum in ${OutDir}: sf4-ember-netplay-$PreviousVersion.zip"
    }
    $published = (& gh release view "v$PreviousVersion" --repo $releaseRepository --json assets | ConvertFrom-Json).assets |
        Where-Object { $_.name -ceq "sf4-ember-netplay-$PreviousVersion.zip" } | Select-Object -First 1
    $baseHash = (Get-FileHash -LiteralPath $baseZip -Algorithm SHA256).Hash.ToLowerInvariant()
    if (!$published -or !$published.digest -or $published.digest -ine "sha256:$baseHash") { throw 'Local previous package does not match the published release asset.' }
    . (Join-Path $PSScriptRoot 'package-team.ps1') -OutDir $OutDir -VersionLabel $version
    . (Join-Path $PSScriptRoot 'package-upgrade.ps1') -FromVersion $PreviousVersion -ToVersion $version -BaseZip $baseZip -TargetZip $script:PackageZipPath -OutDir $OutDir
    & gh release create $Tag $script:PackageZipPath "${script:PackageZipPath}.sha256" $script:UpgradeZipPath "${script:UpgradeZipPath}.sha256" --repo $releaseRepository --verify-tag --latest --title "SF4 Ember Netplay $Tag" --notes-file $NotesFile
    if ($LASTEXITCODE) { throw 'GitHub release publication failed.' }
} finally { Pop-Location }
