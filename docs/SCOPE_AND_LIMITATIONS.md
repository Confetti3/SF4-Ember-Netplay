# Scope and limitations

This document describes what **SF4 Netplay Launcher** is for, what it is **not**, and what to expect while using it.

## What this project is

| In scope | Details |
|----------|---------|
| **Game** | _Ultra Street Fighter IV_ on **Steam** (app 45760) only — you must own the game |
| **Platform** | **Windows 10+** is the primary target |
| **Netplay** | Rollback online play built on upstream [sf4e](https://codeberg.org/adanducci/sf4e) (MIT) |
| **This port adds** | WebView2 launcher (Host / Join / Offline), **VPS relay room codes** (`SF4-XXXX`), team zip packaging |
| **Recommended path** | **Simple mode** — Host creates a relay room, joiner pastes the same code; no host port forward |
| **Also supported** | **Advanced mode** — Direct IP, UPnP, custom broker URL (classic sf4e-style setup) |
| **Audience** | Friends / small groups testing casual WAN rollback — not a commercial matchmaking service |

## What this project is not

- **Not** the official [sf4e](https://codeberg.org/adanducci/sf4e) project by Anthony Danducci
- **Not** maintained, endorsed, or supported by Anthony Danducci, Capcom, or Valve
- **Not** a replacement for Steam's built-in USF4 online while the sidecar is loaded
- **Does not** include the game, Steam, or any copyrighted game assets
- **Not** console or macOS native support
- **Not** guaranteed stable production software — treat this as a **community beta port**

For official sf4e updates and support, use [Codeberg](https://codeberg.org/adanducci/sf4e).

## Known limitations

### Setup and versions

- Both players must use the **same release zip** (`Sidecar.dll` / build must match)
- Install **WebView2 Runtime** and **VC++ x86 redistributable** on each PC
- Keep the whole extracted folder together — do not copy only `Launcher.exe`

### Simple mode (VPS relay)

- Uses a **shared public broker** by default (`http://74.208.200.95:8787`) — suitable for testing, not private production scale
- Relay supports up to **~20 concurrent rooms** on the default broker; idle rooms expire after **~15 minutes**
- Host must **Start game** before the joiner; both need the **current** `SF4-XXXX` from the host screen
- Requires **v0.2.7.3+** on both PCs for VPS relay fights (fixes black screen after character select)

### Experimental features

- **Find match** and **Open rooms** exist but are **experimental** — use **Host + room code** for reliable play
- **Rematch**, **disconnect recovery**, and **spectator mode** need more testing — expect rough edges

### Advanced mode (Direct IP)

- Host must **port-forward TCP and UDP** on the session port (default **23456**) unless UPnP succeeds
- WAN Direct IP is less validated than Simple VPS relay in current beta testing

### Platform and upstream behavior

- **Linux**: Proton/Wine only (same x86 build); not a native Linux port
- While the sidecar is active, **vanilla Steam matchmaking for USF4 is replaced** by sf4e session + GGPO
- Smooth FPS / VSync settings can affect rollback feel (upstream sf4e behavior)
- Logs: `%APPDATA%\sf4e\logs\sf4e.log` — in-app updater logs: `%TEMP%\sf4-netplay-update.log`

## Support boundaries

| Report here | Do not report here |
|-------------|-------------------|
| This port: [GitHub Issues](https://github.com/Confetti3/SF4-Netplay-Launcher/issues) | Upstream sf4e / Anthony Danducci (unless you are testing vanilla sf4e on Codeberg) |
| Include `BUILD_INFO.txt`, both players' logs if possible, room code, steps | Capcom / Steam account or game ownership problems |

See also [BETA_TESTERS.md](BETA_TESTERS.md) and [USER_NETPLAY.md](USER_NETPLAY.md).
