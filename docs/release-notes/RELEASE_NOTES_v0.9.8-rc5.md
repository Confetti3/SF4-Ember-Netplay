# SF4 Ember Netplay v0.9.8-rc5 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is a pre-release for testers.** It has everything in rc4 plus the changes below. If you would rather not test, stay on v0.9.7, the current full release.

## Changes since v0.9.8-rc4

### Connections

- **Ember now prefers UDP port 45760 for matches.** It used a random port on every start, so there was nothing to forward in a router. A second copy on the same PC takes 45761 to 45763, and if all are busy Ember uses a random port as before. Nothing else changes: Ember still tries a direct connection first and falls back to the relay.
- **If Check connection says Relayed, you can now forward the port.** Forward UDP 45760 to your PC in your router and allow `sf4-net.exe` in Windows Firewall. On strict routers this usually gives other players a direct path they could not reach before. Players whose internet provider shares one public address between many customers cannot forward a port and may stay relayed.
- **Check connection explains a Relayed result.** It says what Relayed means (matches still work through a relay server) and what can make it direct. The Direct/Relayed label is now translated.
- **Help & About > Export diagnostics shows the UDP port in use.**

## Compatibility

Everyone in the room, including spectators, must use this same package. A room checks that every game runs the same build, so rc5 cannot play rc4, rc3 or v0.9.7.

## What to report

The launcher and updater still report 0.9.7, so that the updater moves you to the real 0.9.8 when it ships.

After a session, send `sf4e.log` and the newest `session-*.log` from the same folder (see [saving logs](../guides/SAVING_LOGS.md)), from every player in the room if you can. If the room closes on you, the lines just before `Room: closing` matter most.

If you were Relayed before: run Check connection and tell us whether it now says Direct, and whether you forwarded UDP 45760. In `sf4e.log`, the lines that start with `Room: helper UDP port` and `Room: game_ready` show which port Ember used and whether the match connected through it (`fixed_port=yes`).

Linux (Proton) players: please keep telling us whether rematches work and the room stays open after a match.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the latest Microsoft Visual C++ x86 runtime from https://aka.ms/vc14/vc_redist.x86.exe. Extract the entire `sf4-ember-netplay-v0.9.8-rc5` ZIP into a new, empty folder (do not extract over an older version), run `preflight.cmd`, then `Launcher.exe`. There is no upgrade package for this test build.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md), or ask in the [Ember Discord](https://discord.gg/uPNqF5A5uq). Keep invitations private.
