# Runs an in-game self-test (src/sf4e/sf4e__SelfTest.cxx): starts the game
# through a package's Launcher.exe with SF4E_SELFTEST set, waits for the game
# to close itself and prints what each step did. Exit code 0 when every step
# passed, 1 when one failed, 2 when the game left no result.
#
# The game has to be closed and Steam running and signed in. The game's
# window takes the focus for the few seconds its Start is pressed, with a real
# Enter key, and gives it back; the rest runs behind other windows. Do not
# type during those seconds. The saves are copied to
# %APPDATA%\sf4e\selftest-backups before each run. The training test plays
# an offline battle and writes nothing to them.
param(
    [string]$PackageDir = "",
    # The names sf4e__SelfTest.hxx knows (selftest::Test).
    [ValidateSet("training", "room-host", "room-guest")]
    [string]$Test = "training",
    # The room tests: the file the host writes its invitation to, and the
    # guest reads it from once the runner has copied it across.
    [string]$Invite = "",
    [int]$TimeoutMinutes = 20
)

$ErrorActionPreference = "Stop"

if (-not $PackageDir) { $PackageDir = Split-Path $PSScriptRoot -Parent }
$launcher = Join-Path $PackageDir "Launcher.exe"
if (-not (Test-Path $launcher)) { throw "Launcher.exe not found in $PackageDir" }
if (Get-Process SSFIV -ErrorAction SilentlyContinue) { throw "Close Ultra Street Fighter IV first." }

# A test runs in the signed-in account's game, so its saves are copied first,
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
    // True when Windows took both the press and the release.
    public static bool Tap(ushort scan) {
        var key = new INPUT[1]; key[0].type = 1; key[0].key.scan = scan; key[0].key.flags = 0x0008;
        bool pressed = SendInput(1, key, Marshal.SizeOf(typeof(INPUT))) == 1;
        System.Threading.Thread.Sleep(80);
        key[0].key.flags = 0x0008 | 0x0002;
        return SendInput(1, key, Marshal.SizeOf(typeof(INPUT))) == 1 && pressed;
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

# Hosting, joining and the controller a room needs write Ember's settings, so
# a room test puts them back as they were.
$settings = @()
if ($Test -like "room-*") {
    if (-not $Invite) { throw "-Invite is needed for $Test" }
    $settings = @("settings.json", "ui-preferences.json") | ForEach-Object { Join-Path $env:APPDATA "sf4e\$_" }
    $settings | Where-Object { Test-Path $_ } | ForEach-Object { Copy-Item $_ "$_.selftest" -Force }
    $env:SF4E_SELFTEST_INVITE = $Invite
}

$result = Join-Path $env:TEMP ("ember-selftest-" + [guid]::NewGuid().ToString("N") + ".txt")
$env:SF4E_SELFTEST = $Test
$env:SF4E_SELFTEST_RESULT = $result
try {
    $began = (Get-Date).AddSeconds(-1)
    $process = Start-Process -FilePath $launcher -WorkingDirectory $PackageDir -PassThru
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    # The game this launcher started, which is the only one given a key or ended.
    $game = $null
    $started = $false; $taps = 0; $tapped = 0; $before = [IntPtr]::Zero
    # The launcher until it has started the game, the game after that.
    while (-not $(if ($game) { $game.HasExited } else { $process.HasExited })) {
        if ((Get-Date) -gt $deadline) {
            if ($game) { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            Write-Host "FAIL timeout: the game was still running after $TimeoutMinutes minutes"
            exit 2
        }
        Start-Sleep -Seconds 1
        if (-not $game) {
            $child = Get-CimInstance Win32_Process -Filter "Name='SSFIV.exe' AND ParentProcessId=$($process.Id)" | Select-Object -First 1
            if ($child) { $game = Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue }
            continue
        }
        if ($started) { continue }
        $game.Refresh()
        if ($game.MainWindowHandle -eq 0) { continue }
        # The game reads its saves as Start is taken; the main menu follows.
        $done = (Get-RunLog "SelfTest: (PASS main menu|FAIL|RESULT)" $began).Count -gt 0
        if ($done -or $taps -ge 3) {
            $started = $true
            if ($before -ne [IntPtr]::Zero -and $before -ne $game.MainWindowHandle) { [void][EmberSelfTestKeys]::Front($before) }
            Write-Host "Start was sent $taps times; the window that was in front has the focus back."
            continue
        }
        # The test logs every change of the screen in front. One Enter for an
        # arrival at the title screen, once the newest line has named it for
        # three seconds and is still the newest as the key goes. Three at most.
        $screen = @(Get-RunLog "SelfTest: screen " $began) | Select-Object -Last 1
        if (-not $screen -or $screen.Line -notmatch 'SelfTest: screen Title$' -or $screen.LineNumber -eq $tapped) { continue }
        $screen.Line -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' | Out-Null
        if (((Get-Date) - [datetime]$Matches[1]).TotalSeconds -lt 3) { continue }
        if ($before -eq [IntPtr]::Zero) { $before = [EmberSelfTestKeys]::GetForegroundWindow() }
        if (-not [EmberSelfTestKeys]::Front($game.MainWindowHandle)) { continue }
        if ((@(Get-RunLog "SelfTest: screen " $began) | Select-Object -Last 1).LineNumber -ne $screen.LineNumber) { continue }
        if ([EmberSelfTestKeys]::Tap(0x1C)) { $taps++; $tapped = $screen.LineNumber }
    }
    # A launcher that stays up after its game, on a screen of its own, is closed.
    if (-not $process.WaitForExit(10000)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
}
finally {
    Remove-Item Env:SF4E_SELFTEST, Env:SF4E_SELFTEST_RESULT, Env:SF4E_SELFTEST_INVITE -ErrorAction SilentlyContinue
    # A file the run created is removed; one it changed is put back.
    $settings | ForEach-Object { if (Test-Path "$_.selftest") { Move-Item "$_.selftest" $_ -Force } else { Remove-Item $_ -ErrorAction SilentlyContinue } }
}

if (-not (Test-Path $result) -or -not (Get-Content $result | Select-String -Quiet '^RESULT ')) {
    Write-Host "FAIL no result: the game closed without writing one. See $env:APPDATA\sf4e\logs\sf4e.log"
    exit 2
}
$lines = Get-Content $result
$lines | ForEach-Object { Write-Host $_ }
Remove-Item $result
if ($lines -contains "RESULT pass") { exit 0 }
exit 1
