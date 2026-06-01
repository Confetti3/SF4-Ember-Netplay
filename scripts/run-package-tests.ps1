# Run automated checks for Steam P2P package and native build.

param(

    [string]$PackageDir = "",

    [string]$BuildDir = "",

    [switch]$SmokeOfflineLaunch

)



$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent

if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "msvc-build\steam-p2p" }

if (-not $PackageDir) {

    $latest = Get-ChildItem (Join-Path $RepoRoot "dist") -Directory -Filter "sf4e-steam-p2p-*" | Sort-Object LastWriteTime -Descending | Select-Object -First 1

    if ($latest) { $PackageDir = $latest.FullName }

}



$failed = 0



function Test-Step($name, [scriptblock]$block) {

    Write-Host ""

    Write-Host "--- $name ---"

    try {

        & $block

        Write-Host "[PASS] $name"

    } catch {

        Write-Host "[FAIL] $name : $($_.Exception.Message)"

        $script:failed++

    }

}



Write-Host "=== SF4e package tests ==="

Write-Host "Package: $PackageDir"

Write-Host "Build:   $BuildDir"



Test-Step "SteamP2PPayloadTest (offline)" {

    $exe = Join-Path $BuildDir "SteamP2PPayloadTest.exe"

    if (-not (Test-Path $exe)) { throw "SteamP2PPayloadTest.exe not built" }

    $p = Start-Process -FilePath $exe -Wait -PassThru -NoNewWindow

    if ($p.ExitCode -ne 0) { throw "exit code $($p.ExitCode)" }

}



Test-Step "Package preflight (steam Qt)" {

    if (-not $PackageDir -or -not (Test-Path $PackageDir)) { throw "PackageDir missing" }

    $preflight = Join-Path $RepoRoot "scripts\preflight-steam.ps1"

    & powershell -NoProfile -File $preflight -PackageDir $PackageDir

    if ($LASTEXITCODE -ne 0) { throw "preflight exit $LASTEXITCODE" }

}



Test-Step "Launcher --headless-test-handshake" {

    $launcher = Join-Path $PackageDir "Launcher.exe"

    if (-not (Test-Path $launcher)) { $launcher = Join-Path $BuildDir "Launcher.exe" }

    if (-not (Test-Path $launcher)) { throw "Launcher.exe not found" }

    $psi = New-Object System.Diagnostics.ProcessStartInfo

    $psi.FileName = $launcher

    $psi.Arguments = "--headless-test-handshake"

    $psi.WorkingDirectory = Split-Path $launcher -Parent

    $psi.UseShellExecute = $false

    $psi.RedirectStandardInput = $true

    $psi.RedirectStandardOutput = $true

    $psi.RedirectStandardError = $true

    $psi.CreateNoWindow = $true

    $p = [System.Diagnostics.Process]::Start($psi)

    Start-Sleep -Milliseconds 800

    $p.StandardInput.WriteLine('{"v":1,"type":"getState"}')

    $p.StandardInput.Flush()

    $deadline = [DateTime]::UtcNow.AddSeconds(8)

    $gotState = $false

    while ([DateTime]::UtcNow -lt $deadline) {

        if ($p.StandardOutput.Peek() -ge 0) {

            $resp = $p.StandardOutput.ReadLine()

            if ($resp -and $resp -match '"type"\s*:\s*"state"') {

                $gotState = $true

                break

            }

        }

        if ($p.HasExited) { break }

        Start-Sleep -Milliseconds 100

    }

    try {

        $p.StandardInput.WriteLine('{"v":1,"type":"cancel"}')

        $p.StandardInput.Flush()

    } catch { }

    if (-not $p.HasExited) {

        $p.Kill()

        $p.WaitForExit(3000)

    }

    if (-not $gotState) { throw "no state response on stdin/stdout IPC" }

}



Test-Step "Qt6Widgets.dll in package" {

    $qt = Join-Path $PackageDir "dll\Qt6Widgets.dll"
    if (-not (Test-Path $qt)) { $qt = Join-Path $PackageDir "Qt6Widgets.dll" }

    if (-not (Test-Path $qt)) { throw "Qt6Widgets.dll missing from package (expected dll\Qt6Widgets.dll)" }

}

Test-Step "qwindows.dll is x86 (not x64 windeployqt)" {
    $qw = Join-Path $PackageDir "plugins\platforms\qwindows.dll"
    if (-not (Test-Path $qw)) { throw "plugins\platforms\qwindows.dll missing" }
    $bytes = [System.IO.File]::ReadAllBytes($qw)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x14C) { throw "qwindows.dll machine=0x{0:X} expected 0x14C (x86)" -f $machine }
}

Test-Step "Launcher.exe exists" {

    $launcher = Join-Path $PackageDir "Launcher.exe"

    if (-not (Test-Path $launcher)) { throw "Launcher.exe missing from package" }

    $launcherApp = Join-Path $PackageDir "dll\LauncherApp.exe"
    if (-not (Test-Path $launcherApp)) { $launcherApp = $PackageDir }
    if ((Test-Path (Join-Path $PackageDir "dll\LauncherApp.exe")) -and (Get-Item (Join-Path $PackageDir "dll\LauncherApp.exe")).Length -lt 100KB) {
        throw "dll\LauncherApp.exe suspiciously small"
    }

}

Test-Step "Offline tester script exists" {
    $script = Join-Path $PackageDir "tools\run-offline-test.ps1"
    if (-not (Test-Path $script)) { throw "tools\run-offline-test.ps1 missing" }
    $body = Get-Content $script -Raw
    if ($body -notmatch "--offline" -or $body -notmatch "--dev-overlay") {
        throw "offline tester script must launch with --offline --dev-overlay"
    }
}

Test-Step "Package file allowlist" {
    $allowed = @(
        "Launcher.exe",
        "Updater.exe",
        "START_HERE.txt",
        "preflight.cmd",
        "qt.conf",
        "steam_appid.txt",
        "dll\LauncherApp.exe",
        "dll\Sidecar.dll",
        "dll\steam_api.dll",
        "dll\steam_appid.txt",
        "dll\spdlog.dll",
        "dll\fmt.dll",
        "dll\GameNetworkingSockets.dll",
        "dll\GGPO.dll",
        "dll\libcrypto-3.dll",
        "dll\libprotobuf.dll",
        "dll\abseil_dll.dll",
        "dll\Qt6Core.dll",
        "dll\Qt6Gui.dll",
        "dll\Qt6Widgets.dll",
        "dll\Qt6Network.dll",
        "dll\icudt78.dll",
        "dll\icuin78.dll",
        "dll\icuuc78.dll",
        "dll\double-conversion.dll",
        "dll\pcre2-16.dll",
        "dll\md4c.dll",
        "dll\zlib1.dll",
        "dll\qt.conf",
        "plugins\generic\qtuiotouchplugin.dll",
        "plugins\imageformats\qgif.dll",
        "plugins\imageformats\qico.dll",
        "plugins\imageformats\qjpeg.dll",
        "plugins\networkinformation\qnetworklistmanager.dll",
        "plugins\platforms\qwindows.dll",
        "plugins\styles\qmodernwindowsstyle.dll",
        "plugins\tls\qcertonlybackend.dll",
        "plugins\tls\qschannelbackend.dll",
        "readme\STEAM_P2P_EXPERIMENT.md",
        "readme\TROUBLESHOOTING.md",
        "readme\START_HERE.txt",
        "readme\BUILD_INFO.txt",
        "scripts\preflight.ps1",
        "tools\SteamP2PProbe.exe",
        "tools\SteamP2PPayloadTest.exe",
        "tools\run-tests.ps1",
        "tools\run-offline-test.ps1",
        "tools\steam_appid.txt",
        "tools\dll\steam_api.dll",
        "tools\dll\spdlog.dll",
        "tools\dll\fmt.dll",
        "tools\dll\GameNetworkingSockets.dll",
        "tools\dll\GGPO.dll",
        "tools\dll\libcrypto-3.dll",
        "tools\dll\libprotobuf.dll",
        "tools\dll\abseil_dll.dll"
    )
    $allowedSet = @{}
    foreach ($rel in $allowed) { $allowedSet[$rel.ToLowerInvariant()] = $true }

    $unexpected = @()
    Get-ChildItem $PackageDir -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($PackageDir.Length).TrimStart([char[]]@('\', '/')).Replace('/', '\')
        if (-not $allowedSet.ContainsKey($rel.ToLowerInvariant())) {
            $unexpected += $rel
        }
    }

    if ($unexpected.Count -gt 0) {
        throw "unexpected package files: $($unexpected -join ', ')"
    }
}

if ($SmokeOfflineLaunch) {
    Test-Step "Offline launch smoke (opt-in)" {
        $launcher = Join-Path $PackageDir "Launcher.exe"
        if (-not (Test-Path $launcher)) { throw "Launcher.exe not found" }
        $p = Start-Process -FilePath $launcher -ArgumentList @("--offline", "--dev-overlay") -WorkingDirectory $PackageDir -PassThru
        Start-Sleep -Seconds 10
        if ($p.HasExited -and $p.ExitCode -ne 0) {
            throw "offline launcher exited early with code $($p.ExitCode)"
        }
        if (-not $p.HasExited) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
}



Write-Host ""

if ($failed -eq 0) {

    Write-Host "=== All tests PASSED ==="

    exit 0

}

Write-Host "=== $failed test(s) FAILED ==="

exit 1

