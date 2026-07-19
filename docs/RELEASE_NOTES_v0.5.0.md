# SF4 Netplay Launcher v0.5.0

Adds the **Training room** host option, makes host lobby settings actually apply to relay matches, and fixes rollback audio double-triggers plus a crash after long rematch sets.

## Install

1. Download **`sf4-netplay-launcher-*-0.5.0.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).
2. Extract the full zip to a new folder.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.5.0** and host a fresh room.

Both players must use the same release.

## New

- **Training room (endless sparring)**: a checkbox on the Host screen. The match runs as an endless VS session — maximum rounds, no round timer — for both players. Experimental: if you see a round timer counting down in a training room, report it.
- Host settings (rounds, round time, edition select, training room) now reach the lobby on VPS-relay rooms. Previously relay rooms silently used server defaults and ignored the host's launcher settings.

## Fixes

- Hit sounds no longer stop-and-restart ("double hit" audio) when a rollback re-simulates across a retriggered sound; stops now target the exact sound instance.
- Stale sound-stop commands from rolled-back frames are discarded on state load instead of killing sounds that should keep playing.
- Fixed a crash after several consecutive rematches: sound metadata from a finished battle could attach to the next battle's audio players at reused memory addresses, and a failed sound replay then wrote far outside the player array. Metadata is now purged at battle teardown, reset at battle setup, and failed plays are handled instead of asserted.
- Fixed a potential null dereference in the sound-player overflow path and an unbounded memory leak in the sound stop queue.

## Server

- The VPS session relay has been updated to honor the new lobby-settings message (already deployed; no player action needed). Older clients on the new relay, and this client on an older relay, both degrade gracefully to normal matches.

## Verification

- GGPO UDP validation regression passes.
- Rollback audio and rematch-crash fixes verified by code path analysis; live netplay soak testing of the training room and audio behavior is the goal of this release.
