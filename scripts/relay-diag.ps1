# SF4 Netplay Launcher relay diagnostics - broker health, room create/resolve, relay reachability.
# Usage: powershell -NoProfile -File scripts\relay-diag.ps1 [-BrokerUrl URL] [-PackageDir path]

param(
    [string]$BrokerUrl = "",
    [string]$PackageDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $BrokerUrl) {
    $BrokerUrl = $env:SF4E_BROKER_URL
}
if (-not $BrokerUrl) {
    $BrokerUrl = "https://74-208-200-95.nip.io"
    Write-Host "Note: using default production broker. Override with SF4E_BROKER_URL."
}

if (-not $PackageDir) {
    $PackageDir = Join-Path (Split-Path $PSScriptRoot -Parent) "msvc-out\relwithdebinfo"
}

Write-Host "SF4 Netplay Launcher relay diagnostics"
Write-Host "Broker: $BrokerUrl"
Write-Host "Package: $PackageDir"
Write-Host ""

function Fail($msg) {
    Write-Host "[FAIL] $msg"
    exit 1
}

function Ok($msg) {
    Write-Host "[OK]   $msg"
}

function Get-SidecarHash([string]$Dir) {
    $sidecarPath = Join-Path $Dir "Sidecar.dll"
    if (-not (Test-Path $sidecarPath)) {
        Fail "Sidecar.dll not found at $sidecarPath"
    }
    return (Get-FileHash -Path $sidecarPath -Algorithm SHA256).Hash.ToLower()
}

try {
    $health = Invoke-RestMethod -Uri "$BrokerUrl/v1/health" -TimeoutSec 10
    if (-not $health.ok) { Fail "Broker health returned ok=false" }
    $forceVpsRelay = [bool]$health.forceVpsRelay
    $relayHost = if ($health.relayHost) { $health.relayHost } else { "127.0.0.1" }
    Ok "Broker health (rooms=$($health.rooms)/$($health.maxRooms), forceVpsRelay=$forceVpsRelay, relayHost=$relayHost)"
} catch {
    Fail "Broker health request failed: $_"
}

if ($forceVpsRelay) {
    $sidecarHash = Get-SidecarHash $PackageDir
    Ok "Sidecar hash $($sidecarHash.Substring(0, 12))..."
    $createBody = @{ displayName = "RelayDiag"; sidecarHash = $sidecarHash } | ConvertTo-Json
} else {
    $relayHostPath = Join-Path $PackageDir "RelayHost.exe"
    if (-not (Test-Path $relayHostPath)) {
        Fail "RelayHost.exe not found at $relayHostPath"
    }
    Ok "RelayHost.exe present"
    try {
        $publicIp = Invoke-RestMethod -Uri "https://api.ipify.org" -TimeoutSec 10
        Ok "Public IP: $publicIp"
    } catch {
        Fail "Could not detect public IP: $_"
    }
    $createBody = @{ displayName = "RelayDiag"; relayHost = $publicIp } | ConvertTo-Json
}

try {
    $created = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms" -Method POST -ContentType "application/json" -Body $createBody -TimeoutSec 15
} catch {
    Fail "Room create failed: $_"
}

if (-not $created.code -or -not $created.port) {
    Fail "Room create returned incomplete data"
}
Ok "Created room $($created.code) on port $($created.port)"

try {
    $resolved = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms/$($created.code)" -TimeoutSec 10
} catch {
    Fail "Room resolve failed: $_"
}

$expectedHost = if ($forceVpsRelay) { $relayHost } else { $publicIp }
if ($resolved.host -ne $expectedHost -or [int]$resolved.port -ne [int]$created.port) {
    Fail "Resolve mismatch: expected ${expectedHost}:$($created.port), got $($resolved.host):$($resolved.port)"
}
Ok "Resolved room to $($resolved.host):$($resolved.port)"

try {
    $hbBody = @{ hostSecret = $created.hostSecret } | ConvertTo-Json
    $heartbeat = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms/$($created.code)/heartbeat" -Method POST -ContentType "application/json" -Body $hbBody -TimeoutSec 10
    if (-not $heartbeat.ok) { Fail "Heartbeat returned ok=false" }
    if ($heartbeat.heartbeatOk -eq $false) { Fail "Heartbeat ok but relay not listening (heartbeatOk=false)" }
    Ok "Heartbeat accepted (relay listening=$($heartbeat.relayListening))"
} catch {
    if ($_.Exception.Response.StatusCode.value__ -eq 404) {
        Write-Host "[WARN] Heartbeat endpoint missing on broker - deploy updated services/room-broker/server.js"
    } else {
        Fail "Heartbeat failed: $_"
    }
}

if ($forceVpsRelay) {
    try {
        $roomHealth = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms/$($created.code)/health" -TimeoutSec 10
        if (-not $roomHealth.relayOk) {
            Fail "Room health check failed: relay not listening on port $($created.port)"
        }
        Ok "Room health relayOk=true on port $($created.port)"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -eq 404) {
            Write-Host "[WARN] GET /v1/rooms/:code/health missing - deploy updated broker"
        } else {
            Fail "Room health check failed: $_"
        }
    }
}

if ($forceVpsRelay) {
    $udp = New-Object System.Net.Sockets.UdpClient
    try {
        $bytes = [byte[]]@(0)
        $null = $udp.Send($bytes, 1, $resolved.host, [int]$resolved.port)
        Ok "VPS relay UDP reachable on $($resolved.host):$($resolved.port)"
    } catch {
        Fail "Cannot send UDP to VPS relay at $($resolved.host):$($resolved.port) - check VPS firewall (session relay UDP range from broker health / MAX_ROOMS)"
    } finally {
        $udp.Close()
    }
} else {
    $relayHostPath = Join-Path $PackageDir "RelayHost.exe"
    $relayProc = Start-Process -FilePath $relayHostPath -ArgumentList @("--port", $created.port) -WorkingDirectory $PackageDir -PassThru
    Start-Sleep -Seconds 2

    if ($relayProc.HasExited) {
        Fail "RelayHost exited immediately with code $($relayProc.ExitCode)"
    }
    Ok "RelayHost running (pid $($relayProc.Id)) on port $($created.port)"

    $listeners = netstat -ano | Select-String ":$($created.port)\s"
    if (-not $listeners) {
        Stop-Process -Id $relayProc.Id -Force -ErrorAction SilentlyContinue
        Fail "Nothing listening on port $($created.port)"
    }
    Ok "netstat shows listener on port $($created.port)"

    Stop-Process -Id $relayProc.Id -Force -ErrorAction SilentlyContinue
    Ok "RelayHost stopped cleanly"
}

Write-Host ""
Write-Host "RESULT: PASS"
Write-Host "Next: run Launcher, create relay room, Start game, then have joiner use $($created.code)."
if ($forceVpsRelay) {
    Write-Host "VPS relay mode - no host port forward required."
    Write-Host "If in-game connect still fails: open provider inbound UDP for the broker session+GGPO ranges (ufw alone is not enough). Default MAX_ROOMS=50 => 23456-23505 and 24456-24505."
} else {
    Write-Host "Ensure router forwards TCP+UDP $($created.port) if testing across the internet."
}
