# Runs an in-game self-test (src/sf4e/sf4e__NetplayRuntime__SelfTest.cxx):
# starts the game through a package's Launcher.exe with SF4E_SELFTEST set,
# waits for the game to close itself and prints what each step did. Exit code
# 0 when every step passed.
#
# The game has to be closed and Steam running and signed in. The game's
# window takes the focus for the few seconds its Start is pressed, with a real
# Enter key, and gives it back; the rest runs behind other windows. Do not
# type during those seconds. The saves are copied to
# %APPDATA%\sf4e\selftest-backups before each run. The replays
# test adds a replay from %APPDATA%\sf4e\replays to the game's match slots,
# as Add to game does.
param(
    [string]$PackageDir = "",
    [string]$Test = "replays",
    [int]$TimeoutMinutes = 20
)

$ErrorActionPreference = "Stop"

if (-not $PackageDir) { $PackageDir = Split-Path $PSScriptRoot -Parent }
$launcher = Join-Path $PackageDir "Launcher.exe"
if (-not (Test-Path $launcher)) { throw "Launcher.exe not found in $PackageDir" }
if (Get-Process SSFIV -ErrorAction SilentlyContinue) { throw "Close Ultra Street Fighter IV first." }

# The test writes into the signed-in account's saves, so they are copied first,
# all of them, and nothing runs when that cannot be done.
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
$user = (Get-ItemProperty 'HKCU:\Software\Valve\Steam\ActiveProcess' -ErrorAction SilentlyContinue).ActiveUser
if (-not $steam -or -not $user) { throw "Steam is not running with an account signed in." }
$saves = Join-Path $steam "userdata\$user\45760\remote\capcom\superstreetfighteriv\ssf4_savedata"
if (-not (Test-Path (Join-Path $saves "save.dat"))) { throw "No save data for the signed-in account in $saves" }
$backup = Join-Path $env:APPDATA ("sf4e\selftest-backups\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
Copy-Item -Path (Join-Path $saves "*") -Destination $backup -Recurse -Force
if ((Get-ChildItem $backup -File).Count -ne (Get-ChildItem $saves -File).Count) { throw "The saves could not be copied to $backup; not running." }
Write-Host "Saves copied to $backup"

# The title screen learns from its Start which device, and so which profile,
# plays, so it gets a real key: when the test says the game is at the title
# screen its window is brought to the front and Enter is sent once, with its
# scan code. When the game has read its saves the window that was in front
# before gets the focus back. Nothing else is ever sent: a message the game
# shows is left for a person.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class EmberSelfTestKeys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; public uint pad1, pad2; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public KEYBDINPUT key; }
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    // Windows lets a process take the foreground while it is the one giving
    // input: a tap of Alt is the usual way to be that process.
    public static bool Front(IntPtr window) {
        if (GetForegroundWindow() == window) return true;
        keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
        SetForegroundWindow(window);
        System.Threading.Thread.Sleep(200);
        return GetForegroundWindow() == window;
    }
    public static void Tap(ushort scan) {
        var key = new INPUT[1]; key[0].type = 1; key[0].key.scan = scan; key[0].key.flags = 0x0008;
        SendInput(1, key, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(80);
        key[0].key.flags = 0x0008 | 0x0002;
        SendInput(1, key, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

# How many lines of this run's log match: the log of the run before is still
# on disk until the game starts, so a line counts by its time.
function Get-RunLog([string]$pattern, [datetime]$since) {
    $log = Join-Path $env:APPDATA "sf4e\logs\sf4e.log"
    if (-not (Test-Path $log)) { return @() }
    @(Select-String -Path $log -Pattern $pattern | Where-Object { $_.Line -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' -and [datetime]$Matches[1] -ge $since })
}

$result = Join-Path $env:TEMP ("ember-selftest-" + [guid]::NewGuid().ToString("N") + ".txt")
$env:SF4E_SELFTEST = $Test
$env:SF4E_SELFTEST_RESULT = $result
try {
    $began = (Get-Date).AddSeconds(-1)
    $process = Start-Process -FilePath $launcher -WorkingDirectory $PackageDir -PassThru
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $started = $false; $taps = 0; $before = [IntPtr]::Zero
    while (-not $process.HasExited) {
        if ((Get-Date) -gt $deadline) {
            Get-Process SSFIV -ErrorAction SilentlyContinue | Stop-Process -Force
            Write-Host "FAIL timeout: the game was still running after $TimeoutMinutes minutes"
            exit 2
        }
        Start-Sleep -Seconds 1
        if ($started) { continue }
        $game = Get-Process SSFIV -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if (-not $game) { continue }
        # The game reads its saves as Start is taken, and logs the replay table then.
        $done = (Get-RunLog "Replay: the game's replay table|SelfTest: (PASS main menu|FAIL|RESULT)" $began).Count -gt 0
        # One Enter for each time the test says the game arrived at the title screen, three seconds
        # after it, and three at most. A message the game shows there is not an arrival: nothing answers it.
        $arrivals = @(Get-RunLog "SelfTest: at the title screen" $began)
        if ($done -or $taps -ge 3) {
            $started = $true
            if ($before -ne [IntPtr]::Zero -and $before -ne $game.MainWindowHandle) { [void][EmberSelfTestKeys]::Front($before) }
            Write-Host "Start was sent $taps times; the window that was in front has the focus back."
            continue
        }
        if ($arrivals.Count -le $taps) { continue }
        $arrivals[-1].Line -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' | Out-Null
        if (((Get-Date) - [datetime]$Matches[1]).TotalSeconds -lt 3) { continue }
        if ($before -eq [IntPtr]::Zero) { $before = [EmberSelfTestKeys]::GetForegroundWindow() }
        if ([EmberSelfTestKeys]::Front($game.MainWindowHandle)) { [EmberSelfTestKeys]::Tap(0x1C); $taps++ }
    }
}
finally {
    Remove-Item Env:SF4E_SELFTEST, Env:SF4E_SELFTEST_RESULT -ErrorAction SilentlyContinue
}

if (-not (Test-Path $result)) {
    Write-Host "FAIL no result: the game closed without writing one. See $env:APPDATA\sf4e\logs\sf4e.log"
    exit 2
}
$lines = Get-Content $result
$lines | ForEach-Object { Write-Host $_ }
Remove-Item $result
if ($lines -contains "RESULT pass") { exit 0 }
exit 1
