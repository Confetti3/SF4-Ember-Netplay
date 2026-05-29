# SF4 Netplay Launcher — troubleshooting guide

> **Experimental unofficial port** — not production-ready. Use this guide if the launcher shows a black screen, the game crashes right away, or netplay will not connect.

For a short player guide see [USER_NETPLAY.md](USER_NETPLAY.md).

---

## Start here (60-second checklist)

Do these on **each PC** before deeper fixes:

1. Download the **full** release zip from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) (the **Assets** zip, not "Source code").
2. **Extract the entire zip** to one folder (for example `C:\Games\SF4-Netplay-Launcher\`). Do not copy only `Launcher.exe`.
3. Confirm these sit **next to each other** in that folder:
   - `Launcher.exe`, `Sidecar.dll`, `RelayHost.exe`, `WebView2Loader.dll`
   - Folder `launcher-ui\` with `index.html`, `app.js`, `styles.css`
4. Install once (if you have not already):
   - [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
   - [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)
5. Own **Ultra Street Fighter IV** on Steam and launch it at least once through Steam.
6. Both players use the **same version** — check the version in the launcher header (for example `v0.3.7`).
7. Optional: run `preflight.cmd` in the install folder for a quick sanity check.

---

## Launcher opens to a black or blank window

The launcher UI uses **Microsoft WebView2** (like a small browser window). A solid dark or empty window usually means WebView2 or the UI files failed to load — not a problem with USF4 itself.

Try these steps **in order**:

### 1. Install or repair WebView2 Runtime

1. Install [WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703).
2. **Restart your PC**.
3. Open `Launcher.exe` again.

If the launcher showed a message about WebView2, this step fixes it most of the time.

### 2. Confirm `launcher-ui` is present

1. Open your install folder (where `Launcher.exe` lives).
2. You must see a folder named **`launcher-ui`** with `index.html`, `app.js`, and `styles.css` inside.
3. If it is missing, **re-extract the full zip** — do not move only `Launcher.exe` out of the zip.

### 3. Reset the launcher display cache

A corrupted WebView2 profile can cause a permanent black window.

1. **Close** the launcher completely.
2. Press **Win + R**, paste this path, press Enter:

   ```
   %LOCALAPPDATA%\sf4e\launcher-webview2
   ```

3. Delete the **`launcher-webview2`** folder (or everything inside it).
4. Start `Launcher.exe` again.

Your netplay settings in `%APPDATA%\sf4e\config.json` are **not** removed by this step.

### 4. Update graphics drivers

WebView2 needs a working GPU and up-to-date drivers.

1. Update **NVIDIA / AMD / Intel** graphics drivers from the vendor site or Windows Update.
2. If you run the game in a **VM** or on very old hardware, WebView2 may not work — use a normal Windows gaming PC if possible.

### 5. See error messages (console + logs)

1. Open **Command Prompt** or PowerShell in your install folder.
2. Run:

   ```bat
   Launcher.exe --console
   ```

3. Note any red error text in the window.
4. Also check: `%APPDATA%\sf4e\logs\sf4e.log`

| Symptom | Likely cause |
|---------|----------------|
| Message about WebView2 | Install WebView2 Runtime (step 1) |
| `WebView2Loader.dll was not found` | Re-extract full zip |
| Blank window, no message | Reset `launcher-webview2` (step 3) or missing `launcher-ui` (step 2) |
| Launcher never appears | Run `--console`; check Defender did not remove `Launcher.exe` |

---

## Game crashes or closes immediately after Start game

This usually happens when **Sidecar.dll** (the netplay hook) cannot load, was removed by antivirus, or the game path is wrong.

Try these steps **in order**:

### 1. Check Windows Defender / antivirus (most common)

Unsigned netplay tools are often flagged as **false positives** (for example `Program:Win32/Wacapew.A!ml` on `Sidecar.dll`).

1. Open your install folder.
2. Confirm **`Sidecar.dll`** is still there next to `Launcher.exe`.
3. If it is **missing** or in quarantine:
   - Restore it from your antivirus, or
   - Re-extract the **full** zip from [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).
4. Read [WINDOWS_DEFENDER.md](WINDOWS_DEFENDER.md) for hash verification and reporting false positives.

We do **not** recommend turning off real-time protection permanently. Re-extract from the official release after restoring files.

### 2. Install VC++ Redistributable (x86)

Missing runtime libraries cause instant crashes.

1. Install [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe).
2. Restart the PC.
3. Try **Start game** again.

### 3. Re-extract the full zip (missing DLLs)

If Windows blocked files during extract, the game may crash immediately.

Confirm these DLLs sit beside `Launcher.exe`:

- `GameNetworkingSockets.dll`
- `GGPO.dll`
- `libcrypto-3.dll`
- `libprotobuf.dll`
- `abseil_dll.dll`
- `spdlog.dll`
- `fmt.dll`

Compare with `MANIFEST.txt` in your install folder. Re-download and extract the **whole** zip if anything is missing.

### 4. Steam cannot find the game

The launcher looks for USF4 under Steam automatically.

1. Launch **Ultra Street Fighter IV** once through **Steam** (normal play, no mod).
2. If it still fails, set the game folder before starting the launcher:

   ```bat
   set STEAM_APP_PATH=C:\Program Files (x86)\Steam\steamapps\common\Super Street Fighter IV - Arcade Edition
   Launcher.exe
   ```

   Use your actual Steam library path if the game is on another drive.

### 5. Disable conflicting overlays

Overlays can crash injected games.

Temporarily disable:

- Steam in-game overlay (Steam → Settings → In-Game)
- Discord overlay
- GeForce Experience overlay
- MSI Afterburner / RivaTuner Statistics Server

Then try **Start game** again.

### 6. Test Offline mode (isolate the problem)

1. In the launcher, choose **Offline** → **Start game**.
2. If USF4 **still** crashes, the problem is likely the base game install or Sidecar/Defender — not netplay or the broker.
3. If Offline works but Host/Join crashes, focus on Defender, matching builds, and broker steps below.

### 7. Same build on both players

Both PCs must use the **same release zip**. Mixed versions cause version mismatch or odd crashes.

- Check the version in the launcher header on both PCs.
- Use **Check for updates** on the home screen, or download the same zip manually.

| Symptom | Likely cause |
|---------|----------------|
| Crash within 1–2 seconds of Start game | Defender removed `Sidecar.dll`, or missing VC++ / DLLs |
| Error about version mismatch | Different zip / `Sidecar.dll` on each PC |
| "Install USF4 on Steam" | Game not installed or wrong `STEAM_APP_PATH` |
| Works in Offline, fails online | Netplay/broker path — see connection section below |

---

## Recommended settings (smooth netplay)

Use these defaults unless you have a good reason to change them.

### Simple mode + relay (recommended for internet)

| Setting | Recommendation |
|---------|----------------|
| Launcher mode | **Simple mode** (default) — use `SF4-XXXX` room codes |
| Host port forwarding | **Not needed** — traffic uses the shared VPS relay |
| Broker | Default `https://74-208-200-95.nip.io` (preconfigured) |

**Host flow:** Create relay room → share **current** `SF4-XXXX` → **Start game** → wait in lobby.  
**Joiner flow:** Paste code → wait until host connected → **Start game** → both **Ready**.

### Input delay (frames)

| Your connection | Suggested host input delay |
|-----------------|----------------------------|
| Same room / LAN | **2** |
| Typical internet (US) | **3** |
| High rollback / stutter | Raise by **1** and test again |

Set input delay on the **host** screen before **Start game**. Joiner inherits the session setting.

During fights, watch the bottom overlay: **RTT**, **LFB**, **RFB**. If LFB/RFB are often 3+, try raising host delay by 1. See [ROLLBACK_BENCHMARK.md](ROLLBACK_BENCHMARK.md) for internal tuning.

### In-game graphics

In USF4 options, turn **Smooth** frame rate **OFF**. Smooth frame rate can make rollback feel wrong.

### Keep versions matched

- Same zip on both PCs.
- Use **Check for updates** before a session.
- Read `BUILD_INFO.txt` **Git** line if something fails — both players should match.

---

## Connection problems (relay / join / in-game)

| Problem | What to try |
|---------|-------------|
| Joiner stuck before game | Host must click **Start game** first |
| Wrong opponent / empty lobby | Use the host's **current** `SF4-XXXX`, not an old code |
| "Cannot reach relay" / room expired | Host creates a **new** room; broker may be full — wait a few minutes |
| In-game "Still connecting" (relay) | Both on latest release; host and joiner both clicked Start game |
| Version mismatch | Same zip on both PCs |

**Host tip (v0.3.7+):** keep the launcher running during long matches so the room stays registered on the broker.

---

## Direct IP mode: firewall and port forwarding (Advanced only)

Use **Direct IP** only if you intentionally use **Advanced** mode and share `public.ip:port` — not `SF4-XXXX`.

### Who forwards what

| Role | Router port forward | Windows Firewall |
|------|---------------------|------------------|
| **Host** (Direct IP) | **Yes** — TCP **and** UDP on session port | Allow inbound on that port |
| **Joiner** | **No** | Usually nothing extra |
| **Relay (Simple mode)** | **No** on host PC | Usually nothing extra |

Default session port: **23456** (change only if you set a custom port in Advanced).

### Host setup (step by step)

1. In the launcher, switch to **Advanced** → **Direct IP** (not relay).
2. Note your **public** internet address on the host screen (use **Refresh**).  
   - Joiners on the internet need your **public** or **VPN** IP — not `192.168.x.x` unless they are on the same LAN.
3. On your **router**, create a port forward:
   - **Protocol:** TCP and UDP (both)
   - **External port:** `23456` (or your custom session port)
   - **Internal IP:** your gaming PC's LAN address (for example `192.168.1.50`)
   - **Internal port:** same as external
4. In **Windows Defender Firewall** → Advanced settings → **Inbound Rules**:
   - Allow **TCP** and **UDP** on port `23456`, or
   - Allow `Launcher.exe` and `RelayHost.exe` for private networks when prompted.
5. Share room code as **`your.public.ip:23456`** (example: `203.0.113.42:23456`).
6. Host clicks **Start game**, then joiner connects.

### Joiner setup

1. **Advanced** → **Direct IP**.
2. Paste **`host.public.ip:23456`** in the join field (not an `SF4-XXXX` code).
3. **Start game** after the host has started.

### If Direct IP keeps failing

| Check | Action |
|-------|--------|
| Joiner used `SF4-XXXX` | Direct IP needs `IP:port`, not a relay code |
| Host shared LAN IP to WAN friend | Share **public** IP |
| Only TCP forwarded | Forward **UDP** too — GGPO needs both |
| Mode resets to relay | Delete `%APPDATA%\sf4e\config.json` and pick Advanced again |
| CGNAT / no public IP | Use **Simple mode** relay instead — no port forward |

### Quick comparison

| Mode | Host port forward | Joiner address |
|------|-------------------|----------------|
| Simple (relay) | None | `SF4-XXXX` |
| Advanced Direct IP | TCP+UDP `23456` | `public.ip:23456` |

---

## Collecting logs for a bug report

When asking for help, include:

| Item | Location |
|------|----------|
| Build ID | `BUILD_INFO.txt` in install folder (**Git** line) |
| Game log | `%APPDATA%\sf4e\logs\sf4e.log` (both players if possible) |
| Launcher console | Run `Launcher.exe --console`, copy errors |
| Updater log | `%TEMP%\sf4-netplay-update.log` (if update failed) |
| Screenshot | Launcher or in-game overlay (RTT / LFB / RFB) |
| Steps | Host or Join, room code, when it failed |

Send from **both** PCs when reporting netplay issues.

---

## More help

| Topic | Document |
|-------|----------|
| Defender false positives | [WINDOWS_DEFENDER.md](WINDOWS_DEFENDER.md) |
| Tester quick reference | [BETA_TESTERS.md](BETA_TESTERS.md) |
| Full install walkthrough | [TEAM_QUICKSTART.md](TEAM_QUICKSTART.md) (also `START_HERE.md` in zip) |
| Player guide | [USER_NETPLAY.md](USER_NETPLAY.md) |

Report issues on [GitHub](https://github.com/Confetti3/SF4-Netplay-Launcher/issues) with the items above.
