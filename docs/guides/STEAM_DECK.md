# SF4 Ember Netplay: Steam Deck and Linux

Ember runs on a Steam Deck through the game's own Proton. Installing adds an **SF4 Ember Netplay** tile to the Steam library, under Non-Steam, that starts the game with Ember, in Desktop Mode and in Game Mode. Ultra Street Fighter IV's own tile still starts the plain game. This was tested on a Steam Deck with SteamOS 3.8 and Proton 11.0. Online matches from a Deck, games on an SD card and other Linux systems have not been tested yet.

## Before installing

Start Ultra Street Fighter IV once from Steam, wait for the main menu, then quit. The first start asks you to accept the game's license and downloads about 4 GB of shader cache. If it seems stuck, quit and press **Play** again.

## Install

1. Switch to Desktop Mode (Steam button > Power > Switch to Desktop).
2. In the browser, open the [Steam Deck installer release](https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases/tag/steamdeck) and click **install-ember.desktop** under Assets, or download [install-ember.desktop](https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases/download/steamdeck/install-ember.desktop) directly.

   ![The Steam Deck installer release page with install-ember.desktop under Assets](../images/steamdeck-1-download.png)

3. Open the Downloads folder and double-click **install-ember.desktop**.

   ![install-ember.desktop in the Downloads folder](../images/steamdeck-2-downloads.png)

4. KDE asks whether to start the program **Install Ember Netplay**. Choose **Continue**.

   ![KDE's warning: This will start the program Install Ember Netplay, with Continue and Cancel](../images/steamdeck-3-trust.png)

5. A terminal downloads Ember, closes Steam, adds Ember to the library and ends with "Done. Play SF4 Ember Netplay from your Steam library." Steam opens again on top of it; press Enter in the terminal to close it.

   ![The terminal after a finished install](../images/steamdeck-4-done.png)

6. Play **SF4 Ember Netplay** from your Steam library, in Desktop Mode or Game Mode. At the game's title screen, press Start and Ember opens.

   ![SF4 Ember Netplay in the Steam library in Desktop Mode](../images/steamdeck-5-library.png)

   ![SF4 Ember Netplay in Game Mode](../images/steamdeck-6-game-mode.png)

The installer downloads the latest stable release, checks its SHA-256 and extracts it to `~/Ember`, replacing any earlier copy. It adds the SF4 Ember Netplay tile with Ember artwork and sets USF4's launch option to `/home/deck/Ember/steam-launch.sh %command%`. That script starts the plain game unless the Ember tile asked for Ember a moment before. Steam is closed while the installer changes its settings and then started again. Playtime, friends and achievements count for Ultra Street Fighter IV, since that is the game that runs. Ember sets VSync off itself; set Frame Rate to FIXED in the game's Options when Ember asks.

## Updating

Run the installer again. Your Ember settings, replays and logs live in the game's Proton folder and are kept. Ember's own updater has not been tested under Proton.

## Uninstalling

Remove the SF4 Ember Netplay tile (its gear > Manage > Remove non-Steam game), clear **Launch Options** in USF4's **Properties**, then delete the `~/Ember` folder.

## Logs

What Windows keeps in `%APPDATA%\sf4e` is in the game's Proton folder:

```text
~/.local/share/Steam/steamapps/compatdata/45760/pfx/drive_c/users/steamuser/AppData/Roaming/sf4e
```

The `logs` folder there is the one [Saving logs](SAVING_LOGS.md) asks for.

## Troubleshooting

- **First start stops at the license or the shader download:** quit and press **Play** again.
- **"Quit USF4 first.":** close the game, then run the installer again.
- **"Steam did not close":** quit Steam yourself, run the installer again, then start Steam.
- **SF4 Ember Netplay starts the game without Ember:** check that USF4's **Launch Options** reads `/home/deck/Ember/steam-launch.sh %command%`, or run the installer again.

For everything else, see [Troubleshooting](TROUBLESHOOTING.md).
