# Packages Launcher + Sidecar + runtime DLLs + docs for team testing.

# Usage (from repo root):

#   powershell -NoProfile -File scripts/package-team.ps1

# Optional: -BuildDir, -InstallDir, -OutDir, -VersionLabel



param(

    [string]$BuildDir = "",

    [string]$InstallDir = "",

    [string]$OutDir = "dist",

    [string]$VersionLabel = ""

)



$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "msvc-build\default" }

if (-not $InstallDir) { $InstallDir = Join-Path $RepoRoot "msvc-out\relwithdebinfo" }



$Launcher = Join-Path $InstallDir "Launcher.exe"

$Sidecar = Join-Path $InstallDir "Sidecar.dll"

if (-not (Test-Path $Launcher) -or -not (Test-Path $Sidecar)) {

    Write-Error "Missing install artifacts. Build and install first:`n  cmake --preset default`n  cmake --build msvc-build/default --config RelWithDebInfo`n  cmake --install msvc-build/default --config RelWithDebInfo"

}



$RuntimeDlls = @(

    "spdlog.dll",

    "fmt.dll",

    "GameNetworkingSockets.dll",

    "GGPO.dll",

    "libcrypto-3.dll",

    "libprotobuf.dll",

    "abseil_dll.dll",

    "Qt6Core.dll",

    "Qt6Gui.dll",

    "Qt6Widgets.dll"

)

$QtDeps = @(

    "icudt78.dll",

    "icuin78.dll",

    "icuuc78.dll",

    "double-conversion.dll",

    "pcre2-16.dll",

    "md4c.dll",

    "zlib1.dll"

)



$RequiredPackagePaths = @(

    "Launcher.exe",

    "Sidecar.dll",

    "RelayHost.exe",

    "Updater.exe",

    "qt.conf",

    "plugins\platforms\qwindows.dll",

    "START_HERE.md",

    "BUILD_INFO.txt",

    "preflight.ps1",

    "preflight.cmd"

) + $RuntimeDlls



$Stamp = Get-Date -Format "yyyyMMdd"

$PackageName = "sf4-netplay-launcher-$Stamp"

if ($VersionLabel) {

    $safeLabel = ($VersionLabel -replace '[^\w\-.]', '-')

    $PackageName = "$PackageName-$safeLabel"

}

$PackageRoot = Join-Path $RepoRoot (Join-Path $OutDir $PackageName)

$DocsDir = Join-Path $PackageRoot "docs"



if (Test-Path $PackageRoot) { Remove-Item -Recurse -Force $PackageRoot }

New-Item -ItemType Directory -Path $PackageRoot -Force | Out-Null

New-Item -ItemType Directory -Path $DocsDir -Force | Out-Null



Copy-Item $Launcher $PackageRoot

Copy-Item $Sidecar $PackageRoot

$Updater = Join-Path $InstallDir "Updater.exe"
if (-not (Test-Path $Updater)) {
    Write-Error "Missing Updater.exe in $InstallDir. Build Updater target and install first."
}
Copy-Item $Updater $PackageRoot

$RelayHost = Join-Path $InstallDir "RelayHost.exe"
if (-not (Test-Path $RelayHost)) {
    Write-Error "Missing RelayHost.exe in $InstallDir. Build RelayHost target and install first."
}
Copy-Item $RelayHost $PackageRoot



foreach ($dll in $QtDeps) {

    $srcInstall = Join-Path $InstallDir $dll

    $srcBuild = Join-Path $BuildDir $dll

    if (Test-Path $srcInstall) {

        Copy-Item $srcInstall $PackageRoot

    } elseif (Test-Path $srcBuild) {

        Copy-Item $srcBuild $PackageRoot

    }

}

$QtConfInstall = Join-Path $InstallDir "qt.conf"

$QtConfBuild = Join-Path $BuildDir "generated\qt.conf"

if (Test-Path $QtConfInstall) {

    Copy-Item $QtConfInstall $PackageRoot

} elseif (Test-Path $QtConfBuild) {

    Copy-Item $QtConfBuild $PackageRoot

} else {

    Set-Content -Path (Join-Path $PackageRoot "qt.conf") -Value "[Paths]`nPrefix=.`nPlugins=plugins`n" -Encoding ASCII

}

foreach ($pluginsRoot in @($InstallDir, $BuildDir)) {

    $pluginsSrc = Join-Path $pluginsRoot "plugins"

    if (Test-Path $pluginsSrc) {

        Copy-Item -Recurse $pluginsSrc (Join-Path $PackageRoot "plugins") -Force

        Get-ChildItem (Join-Path $PackageRoot "plugins") -Recurse -Filter "*.pdb" -File -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue

        break

    }

}



$MissingDll = @()

foreach ($dll in $RuntimeDlls) {

    $srcInstall = Join-Path $InstallDir $dll

    $srcBuild = Join-Path $BuildDir $dll

    if (Test-Path $srcInstall) {

        Copy-Item $srcInstall $PackageRoot

    } elseif (Test-Path $srcBuild) {

        Copy-Item $srcBuild $PackageRoot

    } else {

        $MissingDll += $dll

    }

}

if ($MissingDll.Count -gt 0) {

    Write-Error "Missing runtime DLL(s) in InstallDir or BuildDir: $($MissingDll -join ', ')"

}



$DocFiles = @(

    "docs\TEAM_QUICKSTART.md",

    "docs\BETA_TESTERS.md",

    "docs\SCOPE_AND_LIMITATIONS.md",

    "docs\USER_NETPLAY.md",

    "docs\SMOKE_TEST.md",

    "docs\NETPLAY_INVARIANTS.md",

    "docs\WINDOWS_DEFENDER.md",

    "docs\TROUBLESHOOTING.md",

    "docs\CODE_SIGNING.md"

)

foreach ($rel in $DocFiles) {

    $src = Join-Path $RepoRoot $rel

    if (Test-Path $src) {

        Copy-Item $src (Join-Path $DocsDir (Split-Path -Leaf $rel))

    }

}

$QuickStart = Join-Path $RepoRoot "docs\TEAM_QUICKSTART.md"

if (Test-Path $QuickStart) {

    Copy-Item $QuickStart (Join-Path $PackageRoot "START_HERE.md")

}



$PreflightSrc = Join-Path $RepoRoot "scripts\tester-preflight.ps1"

if (Test-Path $PreflightSrc) {

    Copy-Item $PreflightSrc (Join-Path $PackageRoot "preflight.ps1")

} else {

    Write-Warning "scripts\tester-preflight.ps1 not found; package will fail manifest validation."

}

$PreflightCmdSrc = Join-Path $RepoRoot "preflight.cmd"
if (Test-Path $PreflightCmdSrc) {
    Copy-Item $PreflightCmdSrc (Join-Path $PackageRoot "preflight.cmd")
} else {
    Write-Warning "preflight.cmd not found; package will fail manifest validation."
}

# Build metadata

$GitRev = ""

Push-Location $RepoRoot

try {

    $null = git rev-parse --short HEAD 2>$null

    if ($LASTEXITCODE -eq 0) { $GitRev = (git rev-parse --short HEAD).Trim() }

} finally { Pop-Location }



$Attribution = Join-Path $RepoRoot "ATTRIBUTION.md"
if (Test-Path $Attribution) {
    Copy-Item $Attribution $PackageRoot
}

# SECURITY.md is linked from README on GitHub; omit from zip so in-app updater allowlists match.

$BuildInfo = @"

SF4 Netplay Launcher package — EXPERIMENTAL UNOFFICIAL PORT (not official sf4e)
NOT PRODUCTION-READY SOFTWARE. Friends-only testing; sessions may fail.
This is a community experiment. It is NOT maintained or endorsed by Anthony Danducci.
Official sf4e by Anthony Danducci: https://codeberg.org/adanducci/sf4e
This port: https://github.com/Confetti3/SF4-Netplay-Launcher

Scope: USF4 Steam / Windows 10+ / experimental rollback for small friend groups.
Limits: not finished software; shared broker (~20 rooms); failed sessions expected.
See docs/SCOPE_AND_LIMITATIONS.md

Generated: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz")

$(if ($VersionLabel) { "Release: $VersionLabel`n" })

Git: $(if ($GitRev) { $GitRev } else { "(not a git repo or git unavailable)" })

Config: RelWithDebInfo x86

$(if ($VersionLabel) { "Label: $VersionLabel`n" })



Prerequisites (not included):

- Steam install of Ultra Street Fighter IV: Arcade Edition (USF4)

- Qt 6 runtime DLLs (included in this zip — Qt6Core/Gui/Widgets.dll and plugins/)

- Microsoft Visual C++ 2015-2022 Redistributable (x86)

  https://aka.ms/vs/17/release/vc_redist.x86.exe



Start here: START_HERE.md

Run preflight: double-click preflight.cmd (or run preflight.ps1 with PowerShell)

"@

Set-Content -Path (Join-Path $PackageRoot "BUILD_INFO.txt") -Value $BuildInfo -Encoding UTF8



# Pre-zip manifest validation

$MissingInPackage = @()

foreach ($rel in $RequiredPackagePaths) {

    $full = Join-Path $PackageRoot $rel

    if (-not (Test-Path $full)) {

        $MissingInPackage += $rel

    }

}

if ($MissingInPackage.Count -gt 0) {

    Write-Error "Package manifest incomplete. Missing in $PackageRoot :`n  $($MissingInPackage -join "`n  ")"

}



# MANIFEST.txt (all files with sizes)

$manifestLines = @(

    "SF4 Netplay Launcher package manifest",

    "Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",

    "Git: $(if ($GitRev) { $GitRev } else { 'unknown' })",

    ""

)

Get-ChildItem -Path $PackageRoot -Recurse -File | Sort-Object FullName | ForEach-Object {

    $rel = $_.FullName.Substring($PackageRoot.Length + 1)

    $manifestLines += ("{0,-48} {1,12}" -f $rel, $_.Length)

}

Set-Content -Path (Join-Path $PackageRoot "MANIFEST.txt") -Value $manifestLines -Encoding UTF8



$ZipPath = Join-Path $RepoRoot "$OutDir\$PackageName.zip"

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }

Compress-Archive -Path $PackageRoot -DestinationPath $ZipPath -Force



Write-Host ""

Write-Host "Package folder: $PackageRoot"

Write-Host "Zip archive:    $ZipPath"

if ($GitRev) { Write-Host "Build git:      $GitRev" }

Write-Host ""

Get-ChildItem $PackageRoot | Format-Table Name, Length -AutoSize



# Return paths for release-team-build.ps1

$script:PackageZipPath = $ZipPath

$script:PackageFolderPath = $PackageRoot

$script:PackageGitRev = $GitRev


