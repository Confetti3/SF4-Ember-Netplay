# SF4 Netplay Launcher v0.4.4

**Launcher + overlay UX release** — denser Simple Host/Join layout, clearer status text, sticky Start game, and a player-friendlier in-game lobby (Ready/Rematch + saved char/stage prefs).

> **Experimental unofficial port** — unsigned build. Download only from this GitHub release page.

## Install

1. Download **`sf4-netplay-launcher-*-v0.4.4.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (Assets — not Source code).
2. Extract the **entire** zip on **both** PCs.
3. Run **`preflight.cmd`** — you should see `[OK]` for each file and `RESULT: PASS`.
4. Run **`Launcher.exe`**. Confirm both players show **v0.4.4** in the launcher header (or matching `BUILD_INFO.txt` Git line).

## What's in v0.4.4

### Launcher
- **Compact Simple Host** — relay code + Get code on one row; Start game stays visible under settings; less empty space in Updates and Host.
- **Cleaner chrome** — experimental warning only on Home; status no longer clips into the Simple/Advanced tabs; shorter “Room live” status (share instructions stay in the hint).
- **Smaller controls** — tighter buttons, steppers, share cards, and form layout for name/delay.
- Removed the deprecated web-based launcher UI (Qt-only).

### In-game overlay
- **Ready / Rematch** without scrolling the lobby panel; toolbar Rematch uses the same Ready path.
- **Saved prefs** — last character, stage, super, and edition (and related overlay picks) persist under `%AppData%\sf4e\`.
- Player view keeps **GGPO ping/RB** stats; original sf4e debug menus/hash strip stay DEV-only.
- Slim player toolbar (**Network** + **Rematch**) when the mouse is near the top of the screen.

### Ops (optional)
- Room broker / VPS relay install units wait for `network-online` and the relay manager before starting.

## Netplay (VPS)

Unchanged session flow from v0.4.3:

- Default broker: `https://74-208-200-95.nip.io`
- Host → **Get code** → share `SF4-XXXX` → **Start game**
- Joiner → paste code → **Start game** after host starts
- Both **Ready** in the in-game lobby

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Upgrade from v0.4.3

Replace the **whole** extracted folder on both PCs. Do not copy only `Launcher.exe`. Both players must use the same zip.
