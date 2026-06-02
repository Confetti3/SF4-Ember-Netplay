# Steam P2P experiment



This branch tests Steam Friends + SteamNetworkingSockets as a full netplay path (invite → P2P session → lobby → GGPO rollback) without the VPS room broker.



## Scope



- **Not** the default production path (`main` still uses `SF4-XXXX` VPS relay).

- Steam replaces signaling and session transport only; **GGPO rollback** remains required.

- `NetplayConfig` version **6** adds `useCentralSession = 3` (Steam P2P), `peerSteamId64`, `steamVirtualPort`, and `steamSessionToken`.



## Build



Requires **vcpkg** with `qtbase` (widgets) for the `x86-windows-wchar-filenames` triplet. First configure may take a while while Qt is built.



```powershell

cmake -B msvc-build/steam-p2p `

  -DSF4E_ENABLE_STEAMWORKS_EXPERIMENT=ON `

  -DSF4E_LAUNCHER_QT_UI=ON `

  -DSF4E_STEAMWORKS_SDK_DIR="C:/path/to/steamworks_sdk/sdk"

cmake --build msvc-build/steam-p2p --target Launcher Sidecar -j 8

```



CMake runs `windeployqt` after linking `Launcher.exe` so Qt DLLs and `platforms/qwindows.dll` sit next to the exe.



When `SF4E_ENABLE_STEAMWORKS_EXPERIMENT=ON`, `SF4E_LAUNCHER_QT_UI` defaults to **ON** (native Qt Widgets UI). No WebView2 Runtime, no Electron, no `launcher-ui/` folder.



## Download for testers (GitHub)



**Latest pre-release:** [steam-p2p-test-20260601](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/steam-p2p-test-20260601) (invite fixes + synchronized launch)

**Previous:** [steam-p2p-test-20260531](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/tag/steam-p2p-test-20260531)



1. Download **`sf4-netplay-p2p-steam-20260601-2125.zip`** from the latest release (Assets — not Source code; not older `2100` / `20260531` zips).
2. Extract on both PCs, run **`preflight.cmd`**, then **`Launcher.exe`**.
3. **Joiner opens Join tab first**, then host sends invite; both press **Ready to launch game** before USF4 starts (see release notes).
4. See [RELEASE_NOTES_STEAM_P2P_TEST_20260601.md](RELEASE_NOTES_STEAM_P2P_TEST_20260601.md) for install steps and bug-report checklist.



## Package for developers



```powershell

.\scripts\package-steam-qt.ps1

```



Produces `dist/sf4-netplay-p2p-steam-<timestamp>/` and a zip.

**Package layout (root stays minimal on purpose):**

| Path | Contents |
|------|----------|
| Package root | `Launcher.exe` (stub), `Updater.exe`, `qt.conf`, `plugins/`, `steam_appid.txt`, `preflight.cmd`, short `START_HERE.txt` |
| `dll/` | `LauncherApp.exe` (Qt UI), `Sidecar.dll`, Steam/Qt/netplay `*.dll` |
| `readme/` | `STEAM_P2P_EXPERIMENT.md`, `TROUBLESHOOTING.md`, full `START_HERE.txt`, `BUILD_INFO.txt` |
| `scripts/` | `preflight.ps1` |
| `tools/` | `SteamP2PProbe.exe`, `SteamP2PPayloadTest.exe`, `run-tests.ps1`, `run-offline-test.ps1`, `tools/dll/` for probe runtimes |

Root `Launcher.exe` is a tiny stub that starts `dll\LauncherApp.exe` (where Windows loads Qt and runtime DLLs from the same folder). `Updater.exe` remains at the root for the existing update backend. WebView2 and optional DirectX compiler DLLs from `windeployqt` are not packaged.

Tester packages include `steam_appid.txt` in both the package root and `dll/`. Runtime creation is disabled by default; developers can opt into the old local fallback with `SF4E_ALLOW_STEAM_APPID_WRITE=1`.

Run `preflight.cmd`, optionally run `tools\run-offline-test.ps1` for a local Sidecar/overlay smoke test, then **double-click `Launcher.exe`**.



## Launcher flow (relay-parity)



| Relay step | Steam experiment |

|------------|------------------|

| Create relay room | Host: **Send invite + listen** (`steamPrepareHost`) |

| Share `SF4-XXXX` | Steam friend message (invite payload) |

| Joiner enters code | Joiner: **Accept invite + connect** (`steamPrepareJoin`) |

| Start game | **Start game** (`steamStart` → `NetplayConfig` → USF4 launch) |

| Session | In-game Steam P2P (`useCentralSession == 3`) |

| GGPO | Legacy session tunnel (`ggpoTransport = 0`, `GgpoRelay`) |



## Relay vs Steam Qt launcher (parity review)



Both paths share **`NetplayLaunchController`**, **`launcher.cxx`** game launch (`CreateSF4Process`), and **Sidecar** injection. The Qt Steam window only exposes the Steam P2P message types; relay/VPS UI is not shipped in this experiment package.



| Area | Main / relay launcher | Steam Qt experiment |

|------|----------------------|---------------------|

| UI shell | WebView2 + `launcher-ui/` or team Electron | Qt Widgets (`SteamP2pMainWindow`) |

| Signaling / room | VPS broker, `createRelayRoom`, `SF4-XXXX` code | Steam friend invite + `steamPrepareHost` / `steamPrepareJoin` |

| Transport | UDP via `RelayHost.exe` / broker | SteamNetworkingSockets P2P (`useCentralSession = 3`) |

| Host flow | Create room → share code → start | Friend list → invite → listen → start |

| Join flow | Enter room code → start | Accept Steam invite → connect → start |

| `saveSettings` / broker URL / UPnP | Exposed in relay UI | Not exposed (Steam path only) |

| `setUiMode` / combined Steam+relay UI | N/A on main relay HTML | Not implemented (Steam-only tabs) |

| Update check (`github_release_client`) | Available in controller | Same backend; no update UI in Steam window |

| Headless / CI | `--electron-ipc` JSON on stdin/stdout | Same protocol via `--headless-test-handshake` |

| Packaged binaries | `RelayHost.exe`, WebView2, `launcher-ui/` | Root: `Launcher.exe` + DLLs; `readme/`, `scripts/`, `tools/` |



**Intentionally relay-only (not in Steam Qt UI):** room codes, broker URL editing, UPnP toggle, direct-IP join, relay host spawn from UI, and `start` without Steam prepare. These remain in `NetplayLaunchController` for other builds but are not wired to the Steam experiment window.



**Shared game launch:** `steamStart` and relay `start` both end in the same config write + USF4 process creation; only session/signaling fields differ (`peerSteamId64`, `steamVirtualPort`, `steamSessionToken` vs relay room metadata).



### Host



1. Open `Launcher.exe` from the experiment build folder (Steam running).

2. **Host** tab → pick a friend → **Send invite + listen**.

3. Wait until connection shows **Connected** (joiner must accept on their PC).

4. **Start game** → USF4 → in-game lobby → **Ready**.



### Join



1. Wait for Steam invite (log shows invite received).

2. **Join** tab → **Accept invite + connect** → wait for **Connected**.

3. **Start game** → USF4 → lobby → **Ready**.


### Offline Test


Use this before inviting the small test group, and ask testers to try it once before their first Steam P2P run.


1. Run `preflight.cmd`.

2. Run `tools\run-offline-test.ps1`, or open `Launcher.exe` and choose **Offline Test** → **Launch offline test**.

3. USF4 should launch with Sidecar injected and the in-game overlay should show **Offline Test**.

4. Confirm the overlay reports Sidecar loaded, Steam P2P disabled, GGPO inactive, config version, game phase, and log locations.

5. If anything fails, collect `%APPDATA%\sf4e\logs\launcher.log`, `%APPDATA%\sf4e\logs\sidecar_bootstrap.log`, and `%APPDATA%\sf4e\logs\sf4e.log`.



### Friend search



Search matches persona name, SteamID64, or `USF4` status. **only USF4** filters the list.



## UI implementation



- Qt Widgets in `src/launcher/qt/` (`SteamP2pMainWindow`, `ControllerBridge`). Other launcher code: `netplay/`, `steam/`, `ipc/`, `relay/`, `update/`, `webview/` (relay UI only).

- Same JSON message protocol as the former WebView UI, handled in-process via `NetplayLaunchController::HandleWebMessage`.

- Automation smoke test: `Launcher.exe --headless-test-handshake` (stdin/stdout JSON, no window).



## Automated Offline Checks



```powershell

ctest --test-dir msvc-build/steam-p2p -R SteamP2PPayloadTest --output-on-failure

.\scripts\run-package-tests.ps1 -PackageDir .\dist\sf4-netplay-p2p-steam-<timestamp> -BuildDir .\msvc-build\steam-p2p

```



## CLI probe (diagnostics)



```powershell

SteamP2PProbe.exe --write-appid 45760 --list-friends --poll-seconds 5

SteamP2PProbe.exe --listen --virtual-port 7 --poll-seconds 60

SteamP2PProbe.exe --connect 7656119xxxxxxxxxx --virtual-port 7 --poll-seconds 60

```



## Manual test matrix



Two Steam accounts, same build zip:



| Scenario | Expected |

|----------|----------|

| Same LAN | Invite → Connected → Start game → lobby → match |

| Different networks | P2P Connected without port forward |

| Mismatched Sidecar hash | `steamPrepareJoin` or Start game rejects invite |

| Host Start before joiner connects | Error until P2P Connected |

| GGPO rollback gate | Overlay `GGPO: Running`, RTT / LFB / RFB visible |



## Rollback gate



Feature is not production-viable until:



- `GGPO: Running` is reached over Steam session tunnel.

- Overlay RTT / LFB / RFB remain visible under jitter/loss.

- Quality is comparable to VPS UDP relay for the same pair.



## Legacy UI shells



Electron and WebView2 are **not** used on this branch. See [ELECTRON_MIGRATION_NOTES.md](ELECTRON_MIGRATION_NOTES.md) for historical context only.

