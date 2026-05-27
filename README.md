# SF4 Netplay Launcher

> **Unofficial fork** of **[sf4e](https://codeberg.org/adanducci/sf4e)** by **[Anthony Danducci](https://codeberg.org/adanducci/sf4e)** and contributors (MIT). This is **not** the upstream sf4e project. See [ATTRIBUTION.md](ATTRIBUTION.md).

**SF4 Netplay Launcher** adds a WebView2 **Host / Join / Offline** launcher and **VPS relay room codes** (`SF4-XXXX`) on top of sf4e’s rollback netplay for _Ultra Street Fighter IV_ on Steam — so friends can play online without port forwarding on the host PC.

**Latest release:** [v0.2.8](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest)

**Download:** [GitHub Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) — get the **team zip** asset (not "Source code" only).

[TOC]

## Getting started

### 1. Prerequisites

Install once on each PC:

| Requirement | Link |
|-------------|------|
| **Ultra Street Fighter IV** (Steam, app 45760) | Not included in the zip |
| [Microsoft Edge WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703) | Required for the launcher UI |
| [VC++ Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe) | Required for sf4e binaries |

### 2. Install

1. Download the latest **team zip** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).
2. Extract the **entire** zip to one folder (e.g. `C:\Games\SF4-Netplay-Launcher\`). Keep all files together — do not copy only `Launcher.exe`.
3. Optional: run `preflight.cmd` to verify the package.
4. Double-click **`Launcher.exe`**.

Both players must use the **same release zip** (`Sidecar.dll` must match). The launcher header shows your installed version (e.g. `v0.2.7.3`). Use **Check for updates** on the home screen to upgrade.

### 3. Play online (Simple mode — recommended)

The launcher defaults to **Simple mode**. No router setup on the host PC — traffic goes through the VPS relay.

| Step | Host | Joiner |
|------|------|--------|
| 1 | Click **Host** → **Create relay room** | Wait |
| 2 | Copy the **`SF4-XXXX`** code shown on screen | Click **Join** → paste that exact code |
| 3 | Click **Start game** | Wait until host is in-game, then **Start game** |
| 4 | Press **Ready** in the in-game lobby | Press **Ready** |
| 5 | Pick characters and fight | Same |

**Tips**

- Share the **current** room code from the host screen — old codes point at empty or expired sessions.
- Stay in **Simple mode** for beta testing. **Find match** and **Open rooms** (Advanced only) are experimental.
- If USF4 is not detected automatically, set `STEAM_APP_PATH` to your `Super Street Fighter IV - Arcade Edition` folder before launching.

### 4. Advanced mode (Direct IP)

Switch to **Advanced** in the launcher for classic host/join with `IP:port`, local relay, or UPnP. The host must **port-forward TCP+UDP** on the session port (default **23456**). See [docs/USER_NETPLAY.md](docs/USER_NETPLAY.md).

Direct IP behavior is unchanged from v0.2.6 — use Advanced when you prefer port-forward over VPS room codes.

## Documentation

| Doc | Audience |
|-----|----------|
| [docs/BETA_TESTERS.md](docs/BETA_TESTERS.md) | Beta testers — quick checklist and bug reports |
| [docs/USER_NETPLAY.md](docs/USER_NETPLAY.md) | Player guide — Simple + Advanced flows |
| [docs/CASUAL_NETPLAY.md](docs/CASUAL_NETPLAY.md) | Casual WAN play overview |
| [docs/TEAM_QUICKSTART.md](docs/TEAM_QUICKSTART.md) | Packaged as `START_HERE.md` in the release zip |
| [docs/SMOKE_TEST.md](docs/SMOKE_TEST.md) | Manual test checklist |
| [ATTRIBUTION.md](ATTRIBUTION.md) | Upstream sf4e credit (Anthony Danducci) |
| [docs/RELEASE.md](docs/RELEASE.md) | Building and publishing releases |

## Troubleshooting

| Problem | What to try |
|---------|-------------|
| Blank launcher / WebView2 error | Install [WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703); keep `launcher-ui/` next to `Launcher.exe` |
| "Version mismatch" on join | Same zip on both PCs; use **Check for updates** |
| Empty lobby / wrong opponent | Same **`SF4-XXXX`** from host's **current** screen |
| Black screen after portraits | Update to **v0.2.7.3+** on **both** PCs |
| Join fails before game starts | Host must click **Start game** first |

**Logs:** `%APPDATA%\sf4e\logs\sf4e.log` · **Console:** `Launcher.exe --console` · **Build info:** `BUILD_INFO.txt`

**Report bugs:** include the Git line from `BUILD_INFO.txt`, both players' `sf4e.log` if possible, and steps to reproduce. See [docs/BETA_TESTERS.md](docs/BETA_TESTERS.md).

## Configuration

| Setting | How |
|---------|-----|
| Broker URL | Advanced → **Room broker URL**, or `set SF4E_BROKER_URL=http://your-broker:8787` |
| Developer overlay | `Launcher.exe --dev-overlay` or `set SF4E_NETPLAY_DEV=1` |
| Offline (no netplay) | **Offline** on the launcher home screen |
| Reset stuck settings | Delete or edit `%APPDATA%\sf4e\config.json` |

Default broker: `http://74.208.200.95:8787` (VPS relay — no host port forward in Simple mode).

## For developers

**Publish a release:**

```powershell
powershell -NoProfile -File scripts/github-release.ps1 -Tag v0.2.8 -NotesFile docs/RELEASE_NOTES_v0.2.8.md
```

See [docs/RELEASE.md](docs/RELEASE.md).

### Supported environments

* Windows: Windows 10 or later
* Linux: Fedora 40+, Steam Deck (via Proton)

### Running on Windows

Windows users with a working Steam installation can run sf4e by extracting a release then double-clicking on `Launcher.exe`. sf4e will attempt to detect your SF4 installation automatically. Windows users with uncommon or damaged Steam installations may run `Launcher.exe` with the `STEAM_APP_PATH` environment variable to the absolute path of the `Super Street Fighter IV - Arcade Edition` directory installed by Steam. You can navigate to this directory using the Steam library's context menu by right-clicking on Ultra Street Fighter IV's library entry, hovering over "Manage", then selecting "Browse local files", as shown below.

![The Steam right-click context menu, opened on the Ultra Street Fighter 4 library list entry](images/browse-local-files-context-menu.png)

### Running on Linux

The most straighforward way to launch sf4e on Linux is with [protontricks](https://github.com/Matoking/protontricks). Extract the release, then run `protontricks-launch Launcher.exe` and select SF4 from the popup UI. For convenience, `protontricks-launch --appid 45760 Launcher.exe` can be used to launch sf4e non-interactively, ex. from shell scripts or program shortcuts.

Linux users who do not install `protontricks` may set the `STEAM_APP_PATH` environment variable to the path of the the `Super Street Fighter IV - Arcade Edition` directory installed by Steam, as demonstrated above. Users should take care to ensure the variable points to a Windows-formatted path accessible from within the Proton container for SF4, and it may be helpful to take advantage of Wine providing the Linux system root as the `Z:` root inside Wine to specify the path. For example, if the local directory is available at `/home/steamdeck/.local/share/Steam/steamapps/common/Super Street Fighter IV - Arcade Edition`, the corresponding path through the Proton container would be `Z:\\home\\steamdeck\\.local\\share\\Steam\\steamapps\\common\\Super Street Fighter IV - Arcade Edition`.

## Building

`sf4e` is built primarily with [Visual Studio](https://visualstudio.microsoft.com/)
2019 16.10 or later with Visual C++. Other development environments will need
support for installing dependencies, ideally via [vcpkg](https://vcpkg.io/en/index.html)
and build file generation via [CMake](https://cmake.org/).

To build sf4e with VS2019 16.10+:

1. Follow steps 1 and 2 in [`vcpkg`'s Getting Started guide](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started),
   stopping after `vcpkg` has been bootstrapped.
   - You can stop at step 3- sf4e already has a manfest file.
2. Set up a local `CMakeUserPresets.json` to describe your environment.
   The following can be used as a quickstart, making sure to provide the
   path to the copy of `vcpkg` checked out in step 1: 
```
{
    "version": 2,
    "configurePresets": [
      {
        "name": "default",
        "inherits": "x86-msvc-ninja-relwithdebinfo",
        "environment": {
          "VCPKG_ROOT": "C:/Users/myuser/path/to/vcpkg"
        }
      }
    ]
  }
  
```
   - Since SF4 is a 32-bit executable, `sf4e` and its dependencies
     (most importantly Detours) also need to be built targeting a
     32-bit host to properly hook SF4's instructions.
3. Open `CMakeLists.txt` with VS2019's native CMake integration.
   - Ensure [CMakePresets.json integration in Visual Studio](https://learn.microsoft.com/en-us/cpp/build/cmake-presets-vs?view=msvc-170#enable-cmakepresets-json-integration) is enabled.
4. Run `Build All`. Confirm that `Launcher.exe` and `Sidecar.dll` are in
   the build output.
5. Run `Launcher.exe`.

To build sf4e with the CMake command line:

1. Set up `vcpkg`, as above in step 1.
2. Set up a local `CMakeUserPresets.json` to describe your environment,
   as above in step 2.
3. Using a CLI environment with CMake and a compiler prepared, run
   `cmake --preset default` from the root of the repository.
   - VS users may wish to use either the x86 Native Tools or the
     x64_x86 Cross Tools developer command prompts, as they already
     provide tools like Ninja and Cmake, and have the various environment
     variables used by CMake already prepared.
4. Build sf4e by running `cmake --build ./path-to-binary-dir/` from the root
   of the repository. Confirm that `Launcher.exe` and `Sidecar.dll` are in
   the build output.
   * If a `CMakeUserPresets.json` file like the one in step 2 is used, the
     the binary dir is `./msvc-build/default`.
5. Run `Launcher.exe`.

Builds generated with CMake that cannot take advantage of `vcpkg` will need to
provide the following dependencies:

* [Detours](https://github.com/microsoft/Detours). Detours is used to install
  custom netplay hooks at runtime.
* [ValveFileVDF](https://github.com/TinyTinni/ValveFileVDF). ValveFileVDF
  is used to parse Steam's configuration files, to automatically detect
  your installation of SF4.
* [Dear Imgui](https://github.com/ocornut/imgui). Dear Imgui is used to
  provide custom overlays for new features and non-durable,
  development-time debugging.
* [spdlog](https://github.com/gabime/spdlog). `spdlog` is used to provide
  durable file logging, both at development time and in release builds.
* [nlohmann/json](https://github.com/nlohmann/json). `json` is used for
  message serialization.
* [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets/).
  `GamenNetworkingSockets` provides a very helpful high-level API on top
  of message passing, and additionally supports NAT hole punching if
  a signalling server is run.
* [GGPO](https://github.com/pond3r/ggpo), used to provide rollback.

## License

This project is licensed under the MIT License - see the LICENSE.md file for details.

## External Licenses and Copyright Information

Street Fighter, Street Fighter 4, Ultra Street Fighter 4, and all related software
Copyright © CAPCOM.

Steam
Copyright © Valve Corporation.

Visual Studio, Visual Studio 2019, vcpkg, and Detours
Copyright © Microsoft Corporation.

CMake - Cross Platform Makefile Generator
Copyright © Kitware, Inc. and Contributors.

ValveFileVDF
Copyright © Matthias Möller.

Dear Imgui
Copyright © Omar Cornut

spdlog
Copyright © 2016 Gabi Melman.

nlohmann/json
Copyright © 2013-2022 Niels Lohmann

GameNetworkingSockets
Copyright © 2018, Valve Corporation

GGPO (Good Game Peace Out)
Copyright © GroundStorm Studios, LLC.
