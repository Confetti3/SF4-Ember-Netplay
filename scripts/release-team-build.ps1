# One-command team release: build, install, package, validate.

# Usage (from repo root):

#   powershell -NoProfile -File scripts/release-team-build.ps1

# Optional: -VersionLabel beta1, -OutDir dist, -SkipBuild



param(

    [string]$VersionLabel = "",

    [string]$OutDir = "dist",

    [switch]$SkipBuild

)



$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

$BuildDir = Join-Path $RepoRoot "msvc-build\default"



Push-Location $RepoRoot

try {

    if (-not $SkipBuild) {

        Write-Host "==> Building RelWithDebInfo..."

        & cmake --build $BuildDir --config RelWithDebInfo

        if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit $LASTEXITCODE" }



        Write-Host "==> Installing to msvc-out..."

        & cmake --install $BuildDir --config RelWithDebInfo

        if ($LASTEXITCODE -ne 0) { throw "cmake install failed with exit $LASTEXITCODE" }

    }

    Write-Host "==> Testing GGPO UDP validation..."

    & ctest --test-dir $BuildDir -C RelWithDebInfo --output-on-failure -R GgpoUdpValidation

    if ($LASTEXITCODE -ne 0) { throw "GGPO UDP validation test failed with exit $LASTEXITCODE" }



    Write-Host "==> Packaging team zip..."

    $packArgs = @{

        OutDir = $OutDir

    }

    if ($VersionLabel) { $packArgs["VersionLabel"] = $VersionLabel }

    . (Join-Path $RepoRoot "scripts\package-team.ps1") @packArgs

    $zip = $script:PackageZipPath
    $folder = $script:PackageFolderPath
    $git = $script:PackageGitRev



    Write-Host ""

    Write-Host "========================================"

    Write-Host "  Team release ready to share"

    Write-Host "========================================"

    Write-Host "Zip:     $zip"

    Write-Host "Folder:  $folder"

    if ($git) { Write-Host "Git:     $git (both players must use this exact zip)" }

    Write-Host ""

    Write-Host "Tester instructions (paste to Discord/email):"

    Write-Host "  1. Extract the FULL zip to one folder"

    Write-Host "  2. Run: preflight.cmd"

    Write-Host "  3. Install VC++ x86 if preflight warns (Qt DLLs are in the zip)"

    Write-Host "  4. Run Launcher.exe - Host / Join / Offline"

    Write-Host "  5. Same zip on both PCs for netplay"

    Write-Host ""

}

finally {

    Pop-Location

}


