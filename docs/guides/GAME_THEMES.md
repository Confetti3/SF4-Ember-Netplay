# SF4 Ember Netplay: In-game themes

An in-game theme replaces how parts of the game look and sound: its menus first, and later its battle HUD and custom in-game overlays such as scoreboards. Themes are folders you put next to the game. Ember never changes the game's own files, so taking a theme away always gives you the game back as it shipped.

You choose themes in **Settings > Interface > In-game themes**. Each part has its own row, so you can take the menus from one theme and, once more parts are supported, the battle HUD from another.

| Part | Row | Folder in a theme | State |
|---|---|---|---|
| Menus | Menus | `ui\` | Works |
| Battle HUD | Battle HUD | `battle\hud\` | Planned |
| In-game overlays (scoreboards, player cards) | Overlay | `overlay\` | Planned |

## Where themes go

Every theme is one folder in `ember\themes` inside the game folder. The folder's name is the theme's name in Settings.

```
Super Street Fighter IV - Arcade Edition\
  SSFIV.exe
  resource\          the game's own files (never changed)
  patch_ae2_tu3\     the game's own updates (never changed)
  ember\
    themes\
      Juri\          a theme
        theme.json
        ui\
          main_menu\
            title.emz
            vfx\main_menu.emz
      Neon\          another theme
        ui\...
    theme\           made by Ember: the parts you picked (do not edit)
```

Inside a theme, each part's folder is laid out exactly like the game's own folder of the same name. A file at `resource\ui\main_menu\title.emz` in the game is replaced by `ui\main_menu\title.emz` in the theme. Leave out every file you do not change: the game loads its own file for anything the theme does not have.

`ember\theme` is where Ember copies the parts you picked. The game is told to look there before its own folders. Ember rebuilds it whenever you pick something else, so put your work in `ember\themes\<name>`, never in `ember\theme`.

## theme.json

Optional. Ember does not read it yet; later versions will show it next to the theme's name. Include it so your theme is ready:

```json
{
  "name": "Juri",
  "author": "Your name",
  "version": "1.0",
  "description": "Juri's purple main menu and title screen."
}
```

## Menus: the ui folder

The menus part is the game's `ui` folder: every menu and screen's pictures, layouts and menu sounds. The game's own copy is in `resource\ui`, with newer versions of some files in `patch`, `patch_ae2`, `patch_ae2_tu1`, `patch_ae2_tu2` and `patch_ae2_tu3`. Start from the newest copy of a file: if `patch_ae2_tu3\ui\main_menu\title.emz` exists, that is the one the game uses, not the one in `resource`.

| Folder | What it is |
|---|---|
| `main_menu` | The main menu: background (`bg.emz`), title screen (`title.emz`), the menu itself (`vfx\main_menu.emz`), menu items, player status, page titles per language |
| `chara_select` | Character select, with each fighter's picture in `chara\sel_<fighter>.tex.emz` |
| `stage_select` | Stage select |
| `versus` | The versus screen before a match |
| `pre_battle` | The screen between versus and the fight |
| `pause_menu` | The pause menu in a match |
| `vs_result`, `vs_result_menu` | Results after a Versus match and its menu |
| `option` | The game's Options screens |
| `network_mode`, `net_result`, `net_icon` | The game's online menus and results |
| `trial_mode`, `trial_result_menu` | Trial mode and its results |
| `arcade_status`, `gameover`, `bonus_stage_result_menu` | Arcade mode screens |
| `player_data`, `user_title`, `user_icon`, `user_flag`, `user_text` | Player data, titles, icons and flags |
| `command_list` | The move list |
| `intrusion` | "Here comes a new challenger", per language |
| `msg_box` | The game's dialog boxes |
| `now_loading` | Loading screens |
| `common` | Parts several screens share |
| `fonts` | The game's menu fonts |
| `sound` | Menu music (`bgm\BGM_MAIN.csb` and others) and menu sounds (`se`) |
| `advertise`, `ticker`, `marketplace`, `benchmark` | Logos at start-up, the news ticker, the store, the benchmark |

A theme's ui folder only changes what the menus and screens show and play. Nothing in it is read by the fight itself, so it is safe online: you and your opponent can use different themes, or none.

## The files

Menu files are `.emz` files: a compressed bundle of a menu's pictures (`.dds`) and its Scaleform movie (`.gfx`, which holds the layout and animations). The game also loads a bundle that is not compressed, so a modding tool that writes plain bundles works too.

What you can change without breaking a menu:

- Pictures: any picture in a bundle, at the same size.
- Layout and animation in the `.gfx` movie, as long as the names the game calls in it stay the same (buttons, frames and the actions the game starts).

A menu whose bundle the game cannot read stays empty or stops responding. Take the theme away (pick **The game's own**) to get it back.

## Picking a theme

1. Put the theme folder in `ember\themes`, then start the game. Ember looks for themes when the game starts, so a theme added while it runs shows after a restart.
2. On the main menu, open **Settings > Interface > In-game themes**. **Menus** lists **The game's own** and every theme with a `ui` folder. Left and Right step through them; Select opens the list. Settings can change it only on the main menu, outside rooms and matches.
3. The new menus show at once: the main menu fades and opens again.

Ember remembers your pick. If the theme is gone the next time the game starts, Ember goes back to the game's own menus.

## Making a theme

1. Make a folder `ember\themes\<your theme>\ui`.
2. Copy the game's file you want to change into it, at the same path under `ui`, taking the newest copy (see above).
3. Change the copy with your tools.
4. Start the game, pick your theme under **Menus**, and go to the screen you changed.
5. After changing the file again, pick **The game's own** and then your theme again so Ember copies the new file, or restart the game.

## When it does not work

Ember writes what it does with themes to its log, `%APPDATA%\sf4e\logs\sf4e.log`:

| Log line | Meaning |
|---|---|
| `In-game themes: 2 with menus` | Themes Ember found at start-up |
| `In-game themes: the game searches ember\theme first` | The game was told to look in `ember\theme` |
| `In-game themes: menus Juri` | The theme's menus were copied in |
| `In-game themes: main menu opened again` | The main menu reloaded to show them |
| `In-game themes: menus Juri could not be used: ...` | Why not. A missing theme goes back to the game's own menus; a file the game had open keeps the theme you had |

**Settings says no themes were found:** the theme folder needs a `ui` folder directly inside it (`ember\themes\Juri\ui`, not `ember\themes\Juri\Juri\ui`), and the game needs a restart after adding it.

**A screen is empty or stuck:** the replaced file for it does not work in the game. Pick **The game's own**, then fix or remove that file.
