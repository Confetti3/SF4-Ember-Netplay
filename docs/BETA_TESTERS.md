# SF4 Netplay Launcher — experimental tester guide

> **Experimental unofficial port** — not the official [sf4e](https://codeberg.org/adanducci/sf4e) project by Anthony Danducci. **Not production-ready software.** Report bugs for this port on GitHub, not to upstream sf4e.

Thank you for helping test this **friends-only experiment**. Sessions may fail — that is expected.

## Windows Defender / “Wacapew.A!ml”

If Windows blocks **`Sidecar.dll`** or **`Launcher.exe`**, that is usually a **false positive** (game hook + unsigned build). Choose **Allow on device** or add your install folder as an exclusion. See [`docs/WINDOWS_DEFENDER.md`](WINDOWS_DEFENDER.md).

## Before you play

1. Download the latest team zip from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (currently **v0.3.2**).
2. Extract the **entire** zip to one folder on each PC — keep `Launcher.exe`, `Sidecar.dll`, and `launcher-ui/` together.
3. Run **`preflight.cmd`** once per machine (optional sanity check).
4. Confirm both players show the **same version** in the launcher header (e.g. `v0.3.2`). Use **Check for updates** if versions differ.

## Recommended flow (Simple mode — experimental)

| Step | Host | Joiner |
|------|------|--------|
| 1 | **Host** → **Create relay room** | Wait |
| 2 | Copy the **current** `SF4-XXXX` from the host screen (not an old code) | Paste that exact code on **Join** |
| 3 | **Start game** | Wait until host shows **Connected**, then **Start game** |
| 4 | Both press **Ready** in the in-game lobby | Same |
| 5 | Play | Same |

Stay in **Simple mode** (default). Do not use **Find match** or **Open rooms** unless asked — those paths are even less tested.

## Scope and limits (summary)

- **Experimental port** — Windows + Steam USF4 only; you must own the game; **not finished software**
- **Same zip** on both PCs; default broker has **~20 rooms** and **~15 min** idle expiry
- **Not tested enough yet:** rematch, disconnect recovery, spectator mode
- Full details: [`docs/SCOPE_AND_LIMITATIONS.md`](SCOPE_AND_LIMITATIONS.md)

## If something goes wrong

| Problem | What to check |
|---------|----------------|
| Defender blocks zip / `Wacapew.A!ml` | False positive — **Allow on device**; see `docs/WINDOWS_DEFENDER.md` |
| Empty lobby / wrong opponent | Host and joiner must use the **same** `SF4-XXXX` from the host's **current** screen |
| Version mismatch in-game | Reinstall the same zip on both PCs |
| Black screen after portraits | Update to **v0.2.7.3** or newer on **both** PCs |
| Join blocked before game starts | Host must click **Start game** first |

## Reporting bugs

Include:

- Version from launcher header or `BUILD_INFO.txt` (**Git** line)
- `%APPDATA%\sf4e\logs\sf4e.log` from **both** players (if possible)
- Screenshot of launcher or in-game overlay
- Steps: Host or Join, room code used, when it failed

Send logs from: `%APPDATA%\sf4e\logs\sf4e.log`

More detail: [USER_NETPLAY.md](USER_NETPLAY.md), [TEAM_QUICKSTART.md](TEAM_QUICKSTART.md) (packaged as `START_HERE.md` in the zip).
