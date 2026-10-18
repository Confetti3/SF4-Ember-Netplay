# SF4 Ember Netplay on Linux

Ember runs under Proton inside the game's own Steam prefix. Steam then counts Ember sessions as USFIV playtime, and the Steam overlay and controller support work as for the game. You need an owned Steam copy of Ultra Street Fighter IV and nothing else.

On a Steam Deck, use the [Steam Deck guide](STEAM_DECK.md) instead: its installer does the steps below for you and adds a library tile. This guide is for other Linux systems, where those steps are done by hand.

## 1. Run the game once

Install Ultra Street Fighter IV from Steam and start it once with Proton (Properties → Compatibility → force Proton Experimental, or whatever Proton you normally use). This creates the prefix at `steamapps/compatdata/45760/pfx`. Quit the game.

## 2. Put Ember in the prefix

1. Download the latest `sf4-ember-netplay-<version>.zip` from the [releases page](https://github.com/Confetti3/SF4-Ember-Netplay/releases). Not the `-setup.exe`, not the source code.
2. Find the prefix. Usually `~/.steam/steam/steamapps/compatdata/45760/pfx`, or the same path under the library the game is installed in. If you cannot find it, open Protontricks, pick USFIV, choose "Select the default wineprefix", then "Browse files".
3. Inside `pfx/drive_c`, make a folder named `Ember` (no spaces) and extract the zip into it. The zip holds one top-level folder; move its contents up so `Launcher.exe` sits directly in `Ember`.
4. Check the download if you like: `sha256sum -c sf4-ember-netplay-<version>.zip.sha256`.

Keep Ember inside `drive_c`. On SteamOS, Wine reports the home folder as a link, and Ember's updater refuses to replace files in a folder reached through one; other systems may do the same.

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

### Variant: the USFIV entry starts Ember

No separate entry at all: set the game's own launch options so Play on Ultra Street Fighter IV runs Ember, which then starts the game. Steam's artwork, playtime, overlay and controller settings for the game all apply. Properties → Launch options of USFIV:

```
bash -c 'exec "${@/*SSFIV.exe//path/to/compatdata/45760/pfx/drive_c/Ember/Launcher.exe}"' -- %command%
```

Clear the launch options to get the plain game back.

## 4. Ember ID passphrase

Open **Ember ID** and create a passphrase. Wine and Proton cannot use Windows' key protection, so the passphrase protects your key instead. Ember asks for it once each time the game starts. Rooms and everything else work after that.

## Updating

Ember's in-app updater needs a release that unpacks updates itself. 1.3.0 and older use Windows' `tar.exe`, which Proton does not have, so their update downloads and then fails at "Could not extract update package". Update those by extracting the new zip over the `Ember` folder with the game closed. All players in a room need the same release, so update when Windows players do.

## Differences from Windows

- `preflight.cmd` passes without checking anything: Wine's `powershell.exe` is a stub. To verify the files, run `sed 's#\\#/#g; s/\r$//' MANIFEST.txt | sha256sum -c --quiet` inside the `Ember` folder.
- The Visual C++ runtime check accepts Wine's own runtime. Do not install the Microsoft redistributable with winetricks; it is not needed.
- Logs live in `pfx/drive_c/users/steamuser/AppData/Roaming/sf4e/logs`. See [saving logs](SAVING_LOGS.md).
- Updater log, when an in-app update was attempted: `pfx/drive_c/users/steamuser/Temp/sf4-netplay-update.log`.

See [troubleshooting](TROUBLESHOOTING.md) for everything that is not Linux-specific.

## Credits

The Proton-in-the-game-prefix method comes from Discord user **@gyro bart**, who worked it out and wrote it up for the community. Thanks also to a second Discord tester who prefers to stay hidden, like Gouken on the vanilla SF4 select screen: everyone knows they are there, nobody gets to see them.
