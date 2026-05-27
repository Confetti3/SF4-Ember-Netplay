# SF4 Netplay Launcher v0.2.3

Hotfix for relay host session connection and RelayHost startup reliability.

## What's new

- **Relay host connect fix:** host game client connects to local RelayHost via `127.0.0.1:<port>` instead of public IP (fixes infinite "Connecting to session server..." on most routers)
- **RelayHost startup:** launcher waits for RelayHost to bind the assigned UDP port before launching the game; detects early RelayHost exit
- **Connection errors:** session connect failures now appear in the in-game Network overlay
- **Network overlay:** wrapped status text and layout fixes for the player panel

## Upgrade from v0.2.2

Download and extract the new team zip on **both PCs**. Use **Check for updates** on the launcher home screen, or grab the asset from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).

Host relay flow: **Create relay room** → **Start game** → overlay should show **Connected: yes** within a few seconds.

Joiners still need the host to forward **TCP+UDP** on the broker-assigned port (shown in the host share card / room resolve).

## Prerequisites

- **Ultra Street Fighter IV** on Steam (app 45760)
- [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
- [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)

## Install

1. Download the **team zip** asset below (not "Source code" only).
2. Extract the **entire** zip to one folder.
3. Run `preflight.cmd`
4. Run `Launcher.exe` — **Host**, **Join**, or **Offline**

## Support

Include the **Git** line from `BUILD_INFO.txt` when reporting issues.
