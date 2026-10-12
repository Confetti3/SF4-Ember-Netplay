# SF4 Ember Netplay on Linux

Ember runs under Proton inside the game's own Steam prefix. Steam then counts Ember sessions as USFIV playtime, and the Steam overlay and controller support work as for the game. You need an owned Steam copy of Ultra Street Fighter IV and nothing else.

## 1. Run the game once

Install Ultra Street Fighter IV from Steam and start it once with Proton (Properties → Compatibility → force Proton Experimental, or whatever Proton you normally use). This creates the prefix at `steamapps/compatdata/45760/pfx`. Quit the game.

## 2. Put Ember in the prefix

### With the script

```sh
curl -fLO https://raw.githubusercontent.com/Confetti3/SF4-Ember-Netplay/release/scripts/install-linux.sh
bash install-linux.sh
```

The script finds your Steam library (native, Flatpak or Snap), downloads the latest release zip, checks it against the published SHA-256, extracts it to `compatdata/45760/pfx/drive_c/Ember`, verifies every file against the package manifest, and prints the exact Steam launch option for step 3. Pass a version to install a specific release, for example `bash install-linux.sh 1.1.2`. It needs `curl`, `sha256sum` and either `unzip` or `python3`.

### By hand

1. Download the latest `sf4-ember-netplay-<version>.zip` from the [releases page](https://github.com/Confetti3/SF4-Ember-Netplay/releases). Not the `-setup.exe`, not the source code.
2. Find the prefix. Usually `~/.steam/steam/steamapps/compatdata/45760/pfx`, or the same path under the library the game is installed in. If you cannot find it, open Protontricks, pick USFIV, choose "Select the default wineprefix", then "Browse files".
3. Inside `pfx/drive_c`, make a folder named `Ember` (no spaces) and extract the zip into it. The zip holds one top-level folder; move its contents up so `Launcher.exe` sits directly in `Ember`.
4. Check the download if you like: `sha256sum -c sf4-ember-netplay-<version>.zip.sha256`.

## 3. Add Ember to Steam

1. Steam → **Add a Non-Steam Game** → Browse → `.../compatdata/45760/pfx/drive_c/Ember/Launcher.exe`.
2. Right-click the new entry → **Properties**. Rename it, then set **Launch options** to the compatdata folder of the game (everything before `/pfx`):

   ```
   STEAM_COMPAT_DATA_PATH="/path/to/steamapps/compatdata/45760" %command%
   ```

   Other wrappers go between the path and `%command%`, for example `mangohud` or `game-performance`.
3. **Compatibility** → force the same Proton as the game. Without this the entry does nothing.

Press Play. Ember starts, finds the game in the same prefix and launches it. If the recovery screen asks for the game folder, choose `Z:` → your Steam library → `steamapps/common/Super Street Fighter IV - Arcade Edition`. Ember remembers it.

Alternative without a Steam entry: `protontricks-launch --appid 45760 /path/to/pfx/drive_c/Ember/Launcher.exe`. Playtime is not tracked that way.

## Updating

Run `install-linux.sh` again, or extract the new zip over the `Ember` folder. **The in-app updater does not work under Proton**: the launcher extracts updates with Windows' `tar.exe`, which Wine and Proton do not ship, so "Download and install" fails after the download. All players in a room need the same release, so update when Windows players do.

## Differences from Windows

- `preflight.cmd` passes without checking anything: Wine's `powershell.exe` is a stub. The script verifies `MANIFEST.txt` instead; by hand, run `sed 's#\\#/#g; s/\r$//' MANIFEST.txt | sha256sum -c --quiet` inside the `Ember` folder.
- The Visual C++ runtime check accepts Wine's own runtime. Do not install the Microsoft redistributable with winetricks; it is not needed.
- Logs live in `pfx/drive_c/users/steamuser/AppData/Roaming/sf4e/logs`. See [saving logs](SAVING_LOGS.md).
- Updater log, when an in-app update was attempted: `pfx/drive_c/users/steamuser/Temp/sf4-netplay-update.log`.

See [troubleshooting](TROUBLESHOOTING.md) for everything that is not Linux-specific.

## Credits

The Proton-in-the-game-prefix method comes from Discord user **@gyro bart**, who worked it out and wrote it up for the community. Thanks also to a second Discord tester who prefers to stay hidden, like Gouken on the vanilla SF4 select screen: everyone knows they are there, nobody gets to see them.
