# Ember Stream Host: keeps a PC that streams an Ember public room all day
# running. It starts Ember with --host-public-room (Ember opens the room and
# opens it again whenever it closes), restarts the game when the room is empty
# and the game has run long or is low on memory, shows what the log says,
# and packs the logs and crash dumps to send.
#
# Put this file (and EmberStreamHost.cmd) in the Ember folder, beside
# Launcher.exe, and start EmberStreamHost.cmd. Windows PowerShell 5.1.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# The title screen learns from a real Start which pad or keyboard plays, so
# Enter is sent to the game's window the way scripts/run-selftest.ps1 does.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class EmberHostKeys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; public uint pad1, pad2; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public KEYBDINPUT key; }
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    static bool Key(ushort vk, ushort scan, uint flags) {
        var key = new INPUT[1]; key[0].type = 1; key[0].key.vk = vk; key[0].key.scan = scan; key[0].key.flags = flags;
        return SendInput(1, key, Marshal.SizeOf(typeof(INPUT))) == 1;
    }
    // A tap of Alt lets this process hand the foreground over.
    public static bool Front(IntPtr window) {
        if (GetForegroundWindow() == window) return true;
        Key(0x12, 0, 0); Key(0x12, 0, 2);
        SetForegroundWindow(window);
        System.Threading.Thread.Sleep(200);
        return GetForegroundWindow() == window;
    }
    public static bool Tap(ushort scan) {
        // Scan codes (0x0008), as the game reads them; then the key up (0x0002).
        bool pressed = Key(0, scan, 0x0008);
        System.Threading.Thread.Sleep(80);
        return Key(0, scan, 0x0008 | 0x0002) && pressed;
    }
}
"@
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$settingsPath = Join-Path $here 'EmberStreamHost.json'
$logs = Join-Path $env:APPDATA 'sf4e\logs'
$gameLog = Join-Path $logs 'sf4e.log'

# Settings: the Ember folder and the tick boxes and numbers on the window.
$settings = [ordered]@{ emberFolder = $here; restartHours = 12; memoryRestart = $true; memoryMB = 120; emptyMinutes = 10 }
if (Test-Path $settingsPath) {
    try { (Get-Content $settingsPath -Raw | ConvertFrom-Json).psobject.Properties | ForEach-Object { $settings[$_.Name] = $_.Value } } catch {}
}
function Save-Settings { $settings | ConvertTo-Json | Set-Content -Path $settingsPath -Encoding UTF8 }

# What the log has said about this game run.
$state = @{
    game = $null; startedAt = $null; wanted = $true; restarting = $false
    quickExits = 0; nextStartAt = $null
    offset = 0L; members = 0; room = 'none'; match = 'none'; emptySince = $null
    largestFree = $null; late = New-Object System.Collections.Generic.List[object]
    day = (Get-Date).Date; watched = 0; cancelled = 0; lastRestart = 'none'
    screen = ''; screenAt = $null; titleTaps = 0; atMenu = $false
}

# ---- window ----
$form = New-Object Windows.Forms.Form
$form.Text = 'Ember Stream Host'; $form.Size = New-Object Drawing.Size(470, 552); $form.FormBorderStyle = 'FixedSingle'; $form.MaximizeBox = $false
$font = New-Object Drawing.Font('Segoe UI', 10)
$form.Font = $font
$y = 10
function Add-Label([string]$text) {
    $label = New-Object Windows.Forms.Label
    $label.Location = New-Object Drawing.Point(12, $script:y); $label.Size = New-Object Drawing.Size(440, 22); $label.Text = $text
    $form.Controls.Add($label); $script:y += 24; $label
}
$gameLabel = Add-Label 'Game: not running'
$roomLabel = Add-Label 'Room: none'
$todayLabel = Add-Label 'Today: 0 games watched'
$memoryLabel = Add-Label 'Memory left: unknown'
$lateLabel = Add-Label 'Frames late (last hour): 0'
$restartLabel = Add-Label 'Last restart: none'
$y += 8

function Add-Number([int]$x, [int]$top, [decimal]$value, [int]$max) {
    $box = New-Object Windows.Forms.NumericUpDown
    $box.Location = New-Object Drawing.Point($x, $top); $box.Size = New-Object Drawing.Size(60, 24)
    $box.Minimum = 1; $box.Maximum = $max; $box.Value = $value
    $form.Controls.Add($box); $box
}
$hoursText = New-Object Windows.Forms.Label
$hoursText.Location = New-Object Drawing.Point(12, ($y + 3)); $hoursText.Size = New-Object Drawing.Size(220, 22); $hoursText.Text = 'Restart when empty after (hours)'
$form.Controls.Add($hoursText)
$hoursBox = Add-Number 240 $y $settings.restartHours 72
$y += 30
$memoryCheck = New-Object Windows.Forms.CheckBox
$memoryCheck.Location = New-Object Drawing.Point(12, $y); $memoryCheck.Size = New-Object Drawing.Size(225, 24)
$memoryCheck.Text = 'Also when memory below (MB)'; $memoryCheck.Checked = [bool]$settings.memoryRestart
$form.Controls.Add($memoryCheck)
$memoryBox = Add-Number 240 $y $settings.memoryMB 1000
$y += 34
$hoursBox.add_ValueChanged({ $settings.restartHours = [int]$hoursBox.Value; Save-Settings })
$memoryBox.add_ValueChanged({ $settings.memoryMB = [int]$memoryBox.Value; Save-Settings })
$memoryCheck.add_CheckedChanged({ $settings.memoryRestart = $memoryCheck.Checked; Save-Settings })

# Where Launcher.exe is: beside this script unless changed here.
$folderLabel = New-Object Windows.Forms.Label
$folderLabel.Location = New-Object Drawing.Point(12, ($y + 5)); $folderLabel.Size = New-Object Drawing.Size(330, 22); $folderLabel.AutoEllipsis = $true
$form.Controls.Add($folderLabel)
$folderButton = New-Object Windows.Forms.Button
$folderButton.Location = New-Object Drawing.Point(342, $y); $folderButton.Size = New-Object Drawing.Size(98, 30); $folderButton.Text = 'Change...'
$form.Controls.Add($folderButton)
function Show-Folder { $folderLabel.Text = "Ember: $($settings.emberFolder)" }
Show-Folder
$y += 38

$buttons = @{}
$x = 12
foreach ($name in 'Restart now', 'Stop', 'Start', 'Send logs') {
    $button = New-Object Windows.Forms.Button
    $button.Location = New-Object Drawing.Point($x, $y); $button.Size = New-Object Drawing.Size(104, 30); $button.Text = $name
    $form.Controls.Add($button); $buttons[$name] = $button; $x += 110
}
$y += 40
$history = New-Object Windows.Forms.ListBox
$history.Location = New-Object Drawing.Point(12, $y); $history.Size = New-Object Drawing.Size(430, (502 - $y))
$form.Controls.Add($history)

function Say([string]$text) {
    $line = '{0:HH:mm} {1}' -f (Get-Date), $text
    $history.Items.Insert(0, $line)
    while ($history.Items.Count -gt 200) { $history.Items.RemoveAt(200) }
    try { Add-Content -Path (Join-Path $here 'EmberStreamHost.log') -Value ('{0:yyyy-MM-dd} {1}' -f (Get-Date), $line) } catch {}
}

# ---- the game ----
function Find-Game { Get-Process -Name SSFIV -ErrorAction SilentlyContinue | Select-Object -First 1 }

# Asks for Launcher.exe and keeps its folder; false when nothing was picked.
function Select-Launcher {
    $dialog = New-Object Windows.Forms.OpenFileDialog
    $dialog.Title = 'Where is Ember''s Launcher.exe?'
    $dialog.Filter = 'Ember launcher (Launcher.exe)|Launcher.exe'
    if (Test-Path $settings.emberFolder) { $dialog.InitialDirectory = $settings.emberFolder }
    if ($dialog.ShowDialog($form) -ne 'OK') { return $false }
    $settings.emberFolder = Split-Path $dialog.FileName -Parent; Save-Settings; Show-Folder
    Say "Ember folder set to $($settings.emberFolder)"
    $true
}

# -Ask: started by hand or with the window, so a missing Launcher.exe is asked
# for. A restart after a crash never asks; the folder is in EmberStreamHost.json.
function Start-Ember([switch]$Ask) {
    $launcher = Join-Path $settings.emberFolder 'Launcher.exe'
    if (-not (Test-Path $launcher)) {
        Say "Launcher.exe not found in $($settings.emberFolder)"
        if (-not ($Ask -and (Select-Launcher))) { $state.wanted = $false; $state.nextStartAt = $null; Say 'not started: press Start or Change...'; return }
        $launcher = Join-Path $settings.emberFolder 'Launcher.exe'
    }
    # After a crash or a forced close the old launcher waits on its recovery
    # window; close it (the dumps stay in the logs folder for Send logs).
    foreach ($old in @(Get-Process -Name Launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $launcher })) {
        Say 'closing the launcher left over from the last run'
        [void]$old.CloseMainWindow()
        if (-not $old.WaitForExit(5000)) { $old.Kill() }
    }
    Start-Process -FilePath $launcher -ArgumentList '--host-public-room' -WorkingDirectory $settings.emberFolder
    $state.startedAt = Get-Date; $state.offset = 0L; $state.members = 0; $state.room = 'none'; $state.match = 'none'
    $state.emptySince = $null; $state.largestFree = $null; $state.screen = ''; $state.titleTaps = 0; $state.atMenu = $false
    Say 'Ember started'
}

# Close the game as the window's close button would; force it after 30 s.
function Stop-Game {
    $game = Find-Game
    if (-not $game) { return }
    $state.restarting = $true
    [void]$game.CloseMainWindow()
    if (-not $game.WaitForExit(30000)) { $game.Kill(); [void]$game.WaitForExit(10000) }
    $state.game = $null; $state.restarting = $false
}

function Restart-Game([string]$why) {
    Say "restarting: $why"
    $state.lastRestart = '{0:HH:mm} ({1})' -f (Get-Date), $why
    Stop-Game; $state.quickExits = 0; Start-Ember
}

# ---- the log ----
# Reads what sf4e.log gained since the last read. Ember keeps the file open,
# so it is opened with sharing; a new game run starts a new file.
function Read-Log {
    if (-not (Test-Path $gameLog)) { return }
    try {
        $stream = [IO.File]::Open($gameLog, 'Open', 'Read', 'ReadWrite, Delete')
        try {
            if ($stream.Length -lt $state.offset) { $state.offset = 0L }
            if ($stream.Length -eq $state.offset) { return }
            [void]$stream.Seek($state.offset, 'Begin')
            $reader = New-Object IO.StreamReader($stream)
            $text = $reader.ReadToEnd()
            $end = $text.LastIndexOf("`n")
            if ($end -lt 0) { return }
            $state.offset += [Text.Encoding]::UTF8.GetByteCount($text.Substring(0, $end + 1))
            foreach ($line in $text.Substring(0, $end).Split("`n")) { Read-Line $line.TrimEnd("`r") }
        } finally { $stream.Dispose() }
    } catch {}
}

function Read-Line([string]$line) {
    $now = Get-Date
    if ($now.Date -ne $state.day) { $state.day = $now.Date; $state.watched = 0; $state.cancelled = 0 }
    # Only today's lines count towards Today (the first read also sees older ones).
    $today = $line.StartsWith('[' + $now.ToString('yyyy-MM-dd'))
    # With --host-public-room Ember logs each screen change, even with no room.
    if ($line -match 'Room: native screen (\S+) \(was .*, room (\S+), match (\S+)$') {
        $state.screen = $Matches[1]; $state.room = $Matches[2]; $state.match = $Matches[3]
        $state.screenAt = [datetime]::ParseExact($line.Substring(1, 19), 'yyyy-MM-dd HH:mm:ss', $null)
        # The first read can see the last run's log, before the game starts a new one.
        if ($state.screen -eq 'MainMenu' -and $state.startedAt -and $state.screenAt -ge $state.startedAt) { $state.atMenu = $true }
    }
    elseif ($line -match 'Room: load over \d+ s members=(\d+)') { $state.members = [int]$Matches[1] }
    # The lines that say the room and match state, each from its one producer.
    elseif ($line -match 'Main menu: native menu \S+ \(room (\S+), match (\S+)\)$|Overlay: Ember menu \S+ \(.*\), room (\S+), match (\S+)$') {
        $state.room = if ($Matches[1]) { $Matches[1] } else { $Matches[3] }
        $state.match = if ($Matches[2]) { $Matches[2] } else { $Matches[4] }
    }
    elseif ($line -match 'Room: closing ') { $state.room = 'idle'; $state.match = 'none'; $state.members = 0 }
    elseif ($line -match 'Room: match ended table=\d+ generation=\d+ result=(\d)') {
        # 0-2 a played game, 3 cancelled, 4 aborted (Ember's MatchResult).
        if (-not $today) { return }
        if ([int]$Matches[1] -le 2) { $state.watched++ } else { $state.cancelled++ }
    }
    elseif ($line -match 'largest_free=(\d+)MB') { $state.largestFree = [int]$Matches[1] }
    elseif ($today -and $line -match 'Frame rate: .* (\d+) of \d+ frames late') {
        $at = [datetime]::ParseExact($line.Substring(1, 19), 'yyyy-MM-dd HH:mm:ss', $null)
        $state.late.Add(@($at, [int]$Matches[1]))
    }
}

# The room is empty when this PC is alone in it (it counts itself) or there
# is no room, and no game is on.
function Test-Empty { ($state.room -ne 'joined' -or $state.members -le 1) -and $state.match -in 'none', 'post-match' }

function Update-Rules {
    if (-not $state.wanted -or -not $state.startedAt) { return }
    if (-not (Test-Empty)) { $state.emptySince = $null; return }
    if (-not $state.emptySince) { $state.emptySince = Get-Date }
    if (((Get-Date) - $state.emptySince).TotalMinutes -lt $settings.emptyMinutes) { return }
    $up = (Get-Date) - $state.startedAt
    if ($up.TotalHours -ge $settings.restartHours) { Restart-Game ('empty, {0} h up' -f [int]$up.TotalHours) }
    elseif ($settings.memoryRestart -and $state.largestFree -and $state.largestFree -lt $settings.memoryMB) { Restart-Game ("empty, memory $($state.largestFree) MB") }
}

# A game that ended without being asked to starts again after 30 s; three in
# a row within 10 minutes of starting, and the host gives up until Start.
function Watch-Game {
    if (-not $state.game) { $state.game = Find-Game; if ($state.game -and -not $state.startedAt) { $state.startedAt = $state.game.StartTime } }
    if ($state.game -and $state.game.HasExited -and -not $state.restarting) {
        $state.game = $null
        if (-not $state.wanted) { return }
        $quick = $state.startedAt -and ((Get-Date) - $state.startedAt).TotalMinutes -lt 10
        $state.quickExits = if ($quick) { $state.quickExits + 1 } else { 1 }
        if ($state.quickExits -ge 3) { $state.wanted = $false; Say 'the game closed 3 times soon after starting; press Start to try again'; return }
        Say 'the game closed; starting it again in 30 s'
        $state.nextStartAt = (Get-Date).AddSeconds(30)
    }
    if ($state.nextStartAt -and (Get-Date) -ge $state.nextStartAt) { $state.nextStartAt = $null; if ($state.wanted -and -not (Find-Game)) { Start-Ember } }
}

# Before this run's main menu: the title screen, or the attract mode it falls
# into (a demo fight, the logos, the opening), whose Start goes back to the
# title. Each shown for 3 s gets Enter, then the window that was in front
# (OBS) gets the focus back. Six at most per run.
function Pass-Title {
    if ($state.atMenu -or $state.screen -notmatch '^(Title|DemoBattle|Opening|Logo)' -or -not $state.game -or $state.titleTaps -ge 6 -or -not $state.startedAt) { return }
    if ($state.screenAt -lt $state.startedAt -or ((Get-Date) - $state.screenAt).TotalSeconds -lt 3) { return }
    $state.game.Refresh()
    $window = $state.game.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { return }
    $before = [EmberHostKeys]::GetForegroundWindow()
    if (-not [EmberHostKeys]::Front($window)) { return }
    if ([EmberHostKeys]::Tap(0x1C)) { $state.titleTaps++; $state.screenAt = Get-Date; Say "pressed Start ($($state.screen))" }
    if ($before -ne [IntPtr]::Zero -and $before -ne $window) { [void][EmberHostKeys]::Front($before) }
}

function Update-Labels {
    $hourAgo = (Get-Date).AddHours(-1)
    $state.late.RemoveAll({ param($entry) $entry[0] -lt $hourAgo }) | Out-Null
    $late = 0; foreach ($entry in $state.late) { $late += $entry[1] }
    if ($state.game) {
        $up = (Get-Date) - $state.startedAt
        $gameLabel.Text = 'Game: running  |  up {0} h {1} m' -f [int][Math]::Floor($up.TotalHours), $up.Minutes
    } else { $gameLabel.Text = if ($state.wanted) { 'Game: not running' } else { 'Game: stopped' } }
    $roomLabel.Text = switch ($state.room) {
        'joined' { 'Room: open  |  {0} in room  |  match {1}' -f $state.members, $state.match }
        'opening' { 'Room: opening' }
        default { 'Room: none (Ember opens it again)' }
    }
    $todayLabel.Text = 'Today: {0} games watched  |  {1} cancelled' -f $state.watched, $state.cancelled
    $memoryLabel.Text = if ($state.largestFree) { 'Memory left: {0} MB  (restart below {1} MB)' -f $state.largestFree, $settings.memoryMB } else { 'Memory left: not logged yet' }
    $lateLabel.Text = "Frames late (last hour): $late"
    $restartLabel.Text = "Last restart: $($state.lastRestart)"
}

# ---- logs to send ----
# Today's and yesterday's logs (the ten newest session logs of them), the
# three newest small crash dumps (*-report.dmp) and the newest full dump
# (30-40 MB). The game keeps its logs open, so each is copied with sharing.
function New-LogZip([string]$zip) {
    $since = (Get-Date).Date.AddDays(-1)
    $recent = @(Get-ChildItem $logs -File -Filter *.log | Where-Object { $_.LastWriteTime -ge $since -or $_.Name -eq 'sf4e-crash.log' })
    $files = @($recent | Where-Object { $_.Name -notlike 'session-*' })
    $files += @($recent | Where-Object { $_.Name -like 'session-*' } | Sort-Object LastWriteTime -Descending | Select-Object -First 10)
    $dumps = @(Get-ChildItem $logs -File -Filter *.dmp | Sort-Object LastWriteTime -Descending)
    $files += @($dumps | Where-Object { $_.Name -like '*-report.dmp' } | Select-Object -First 3)
    $files += @($dumps | Where-Object { $_.Name -notlike '*-report.dmp' } | Select-Object -First 1)
    $files += @(Get-Item (Join-Path $here 'EmberStreamHost.log') -ErrorAction SilentlyContinue)
    $temp = Join-Path $env:TEMP ('ember-logs-' + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        foreach ($file in $files) {
            $in = [IO.File]::Open($file.FullName, 'Open', 'Read', 'ReadWrite, Delete')
            try { $out = [IO.File]::Create((Join-Path $temp $file.Name)); try { $in.CopyTo($out) } finally { $out.Dispose() } } finally { $in.Dispose() }
        }
        Compress-Archive -Path (Join-Path $temp '*') -DestinationPath $zip -Force
    } finally { Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue }
    (Get-Item $zip).Length
}

# Packs the zip on the desktop and shows it in Explorer.
function Send-Logs {
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ('ember-logs-{0}-{1:yyyyMMdd-HHmm}.zip' -f $env:COMPUTERNAME, (Get-Date))
    $size = New-LogZip $zip
    Say ('logs packed: {0} ({1:N1} MB)' -f (Split-Path $zip -Leaf), ($size / 1MB))
    Start-Process explorer.exe -ArgumentList "/select,`"$zip`""
}

$buttons['Restart now'].add_Click({ $state.wanted = $true; Restart-Game 'by hand' })
$buttons['Stop'].add_Click({ $state.wanted = $false; $state.nextStartAt = $null; Say 'stopping the game'; Stop-Game })
$buttons['Start'].add_Click({ $state.wanted = $true; $state.quickExits = 0; if (Find-Game) { Say 'the game is already running' } else { Start-Ember -Ask } })
$folderButton.add_Click({ [void](Select-Launcher) })
$buttons['Send logs'].add_Click({ try { Send-Logs } catch { Say "packing the logs failed: $($_.Exception.Message)" } })

# The log is read when Windows says it changed. Windows can hold that news
# back for a file another program keeps open, so the timer, which otherwise
# only redraws and asks the process handle whether the game is still there,
# also reads it every 30 s.
if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Path $logs | Out-Null }
$watcher = New-Object IO.FileSystemWatcher($logs, 'sf4e.log')
$watcher.NotifyFilter = 'Size, LastWrite, FileName'
$watcher.SynchronizingObject = $form
# An error in a handler is said in the history: the window has no console.
function Guard([scriptblock]$work) {
    try { & $work } catch { Say ("error: {0} (line {1})" -f $_.Exception.Message, $_.InvocationInfo.ScriptLineNumber) }
}
$onChange = { Guard { Read-Log; Update-Rules } }
$watcher.add_Changed($onChange); $watcher.add_Created($onChange)
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 2000
$ticks = 0
$timer.add_Tick({ Guard {
    Watch-Game
    # Until the main menu the log is read every tick: the title shows for ~10 s.
    if ($state.game -and -not $state.atMenu) { Read-Log }
    Pass-Title
    if ((++$script:ticks % 15) -eq 0) { Read-Log; Update-Rules }
    Update-Labels
} })

$form.add_Shown({
    $watcher.EnableRaisingEvents = $true
    $state.game = Find-Game
    if ($state.game) { $state.startedAt = $state.game.StartTime; Say 'Ember is already running; restarts need it started from here (--host-public-room)' }
    else { Start-Ember -Ask }
    Read-Log; Update-Labels; $timer.Start()
})
$form.add_FormClosed({ $timer.Stop(); $watcher.Dispose() })
[void]$form.ShowDialog()
