# Ember Stream Host

For a PC that streams an Ember public room all day. It keeps the room open
and the game fresh without anyone at the PC.

- Starts Ember with `Launcher.exe --host-public-room`. Ember then opens the
  last public room created on its Create screen again (its settings, type and
  template), whenever the room closes.
- Ember watches the room's games by itself: table 1's first, else any other,
  else it waits at table 1.
- Restarts the game when the room is empty (only this PC in it, no game on)
  for 10 minutes and the game has run longer than the hours set, or, if ticked,
  its memory is below the MB set. The game is a 32-bit program and runs short
  of memory after a day or so.
- Starts the game again if it closes or crashes, after 30 s. After three
  closes within 10 minutes of starting it stops and waits for **Start**.
- Shows the room, today's games and cancelled games, memory left, late frames
  and the last restart, read from `%APPDATA%\sf4e\logs\sf4e.log`.
- **Send logs** packs the last two days of logs, the three newest small crash
  dumps and the newest full one into a zip on the desktop and shows it in
  Explorer, to send by hand.

## Setup

1. In Ember, create the public room once from **Public rooms → Create**, with
   a template if you like. That room is what the host opens.
2. Ember carries the control panel inside `Launcher.exe`, not as a file of its
   folder. Run `Launcher.exe --write-stream-host` (a shortcut to it with that
   added to the Target does it) and Ember writes the panel to
   `%APPDATA%\sf4e\stream-host` and shows that folder. The folder survives
   updates; run the switch again after an update to get the new panel.
   `EmberFolder.txt` there names the Ember folder it was run from. If that is
   not where Ember lives now, **Change...** picks another `Launcher.exe`.
3. Close Ember, then start `EmberStreamHost.cmd` from that folder. It starts
   Ember itself; an Ember started another way is watched but does not reopen
   the room.
4. In OBS, capture the game with **Game Capture** set to the `SSFIV.exe`
   window, so the stream picks the game up again after a restart.

Settings are kept in `EmberStreamHost.json`, what it did in
`EmberStreamHost.log`, both beside the script.
