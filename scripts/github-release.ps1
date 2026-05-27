# Build, package, and publish a GitHub Release for SF4 Netplay Launcher.
# Usage: powershell -NoProfile -File scripts/github-release.ps1 -Tag v0.1.0-testers

param(
    [Parameter(Mandatory = $true)]
    [string]$Tag,
    [string]$VersionLabel = "",
    [string]$OutDir = "dist",
    [string]$Repo = "Confetti3/SF4-netplay-launcher",
    [string]$NotesFile = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $VersionLabel) {
    $VersionLabel = $Tag -replace '^v', ''
}

Push-Location $RepoRoot
try {
    $releaseArgs = @{
        OutDir = $OutDir
        VersionLabel = $VersionLabel
    }
    if ($SkipBuild) {
        . (Join-Path $RepoRoot "scripts\package-team.ps1") @releaseArgs
    } else {
        $buildArgs = @{ OutDir = $OutDir; VersionLabel = $VersionLabel }
        . (Join-Path $RepoRoot "scripts\release-team-build.ps1") @buildArgs
    }

    $zip = $script:PackageZipPath
    if (-not $zip -or -not (Test-Path $zip)) {
        $candidates = Get-ChildItem -Path (Join-Path $RepoRoot $OutDir) -Filter "sf4-netplay-launcher-*.zip" |
            Sort-Object LastWriteTime -Descending
        if ($candidates.Count -eq 0) {
            $candidates = Get-ChildItem -Path (Join-Path $RepoRoot $OutDir) -Filter "sf4e-netplay-team-*.zip" |
                Sort-Object LastWriteTime -Descending
        }
        if ($candidates.Count -eq 0) {
            Write-Error "No package zip found under $OutDir"
        }
        $zip = $candidates[0].FullName
    }

    if (-not $NotesFile) {
        $NotesFile = Join-Path $RepoRoot "docs\RELEASE_NOTES_TEMPLATE.md"
    }
    if (-not (Test-Path $NotesFile)) {
        Write-Error "Release notes file not found: $NotesFile"
    }

    $releaseExists = $false
    try {
        gh release view $Tag --repo $Repo 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $releaseExists = $true }
    } catch {
        $releaseExists = $false
    }

    if ($releaseExists) {
        Write-Host "Release $Tag exists; uploading asset..."
        gh release upload $Tag $zip --repo $Repo --clobber
    } else {
        Write-Host "Creating release $Tag on $Repo..."
        gh release create $Tag $zip `
            --repo $Repo `
            --title "SF4 Netplay Launcher $Tag" `
            --notes-file $NotesFile
    }

    if ($LASTEXITCODE -ne 0) {
        throw "gh release failed with exit $LASTEXITCODE"
    }

    Write-Host ""
    Write-Host "Release published: https://github.com/$Repo/releases/tag/$Tag"
    Write-Host "Latest:            https://github.com/$Repo/releases/latest"
    Write-Host "Asset:             $zip"
}
finally {
    Pop-Location
}
