# SF4 Ember Netplay: In-game themes

An in-game theme replaces how parts of the game look and sound: its menus and battle HUD, and later custom in-game overlays such as scoreboards. Themes are folders you put next to the game. Ember never changes the game's own files, so taking a theme away always gives you the game back as it shipped.

You choose themes in **Settings > Interface > In-game themes**. Each part has its own row, so you can take the main menu from one theme, the title screen from another and the battle HUD from a third.

| Part | Row | Folder in a theme | State |
|---|---|---|---|
| Main menu | Main menu | `ui\main_menu\` | Works |
| Title screen, logos | Title screen | `ui\advertise\` | Works |
| Character select | Character select | `ui\chara_select\` | Works |
| Stage select | Stage select | `ui\stage_select\` | Works |
| Versus screen | Versus screen | `ui\versus\` | Works |
| Pause menu | Pause menu | `ui\pause_menu\` | Works |
| The game's online menus | Network menus | `ui\network_mode\` | Works |
| Edition marks | Edition marks | `ui\edition_mark\` | Works |
| Button prompts and icons | Buttons and icons | `ui\common\` | Works |
| Menu music and sounds | Menu sounds | `ui\sound\` | Works |
| Battle HUD | Battle HUD | `battle\hud\` | Works |
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

The menu parts are folders of the game's `ui` folder, one part each for the folders in the table at the top. A theme's files in other `ui` folders (`option`, `trial_mode` and so on below) are not used yet. The `ui` folder holds every menu and screen's pictures, layouts and menu sounds. The game's own copy is in `resource\ui`, with newer versions of some files in `patch`, `patch_ae2`, `patch_ae2_tu1`, `patch_ae2_tu2` and `patch_ae2_tu3`. Start from the newest copy of a file: if `patch_ae2_tu3\ui\main_menu\title.emz` exists, that is the one the game uses, not the one in `resource`.

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

## Battle HUD: the battle\hud folder

The battle HUD part is the game's `battle\hud` folder: what is drawn over a fight. The game's own copy is in `resource\battle\hud`, with newer files in the patch folders (`patch_ae2_tu2\battle\hud\CMN\cockpit.tex.emz` is the one the game uses) and the later fighters' files in `dlc\03_character_free` and `dlc\04_ae2`. A theme puts all of them in its one `battle\hud` folder, the newest copy of each.

| Folder | What it is |
|---|---|
| `CMN\cockpit.tex.emz` | The life, super and revenge bars, the timer and the round icons (`d_common.dds`, `d_time.dds`, `d_d_text.dds`) |
| `CMN\cockpit.ems.emz` | Where those sit on screen |
| `CMN\announce.tex.emz` | Round calls: Round, Final, Fight, K.O., Perfect, Time over, Win, Lose, Draw |
| `CMN\notice.tex.emz` | Messages shown during a fight |
| `ENG\cockpit_text.tex.emz`, `ENG\cursor_text.tex.emz` | Text on the HUD, per language |
| `ENG\<fighter>\chara.tex.emz` | Each fighter's pictures: by the life bar (`cockpit.dds`), at the round call, on the result screen, and the face |
| `ENG\<fighter>\uc.tex.emz` | Each fighter's two Ultra pictures |

A battle HUD is pictures and their places on screen. Ember's desync check looks at the fighters and the battle flow, not at the HUD. A new pick shows from the next battle.

## The files

Menu files are `.emz` files: a compressed bundle of a menu's pictures (`.dds`) and its Scaleform movie (`.gfx`, which holds the layout and animations). The game also loads a bundle that is not compressed, so a modding tool that writes plain bundles works too.

What you can change without breaking a menu:

- Pictures: any picture in a bundle, at the same size.
- Layout and animation in the `.gfx` movie, as long as the names the game calls in it stay the same (buttons, frames and the actions the game starts).

A menu whose bundle the game cannot read stays empty or stops responding. Take the theme away (pick **The game's own**) to get it back.

## Picking a theme

1. Put the theme folder in `ember\themes`, then start the game. Ember looks for themes when the game starts, so a theme added while it runs shows after a restart.
2. On the main menu, open **Settings > Interface > In-game themes**. Each row lists **The game's own** and every theme with that part's folder: **Main menu** the themes with `ui\main_menu`, **Battle HUD** those with `battle\hud`, and so on. Left and Right step through them; Select opens the list. Settings can change it only on the main menu, outside rooms and matches.
3. A new main menu shows at once: the main menu fades and opens again. Other screens show their pick the next time they open.

Ember remembers your pick. If the theme is gone the next time the game starts, Ember goes back to the game's own menus.

## Making a theme

1. Make a folder `ember\themes\<your theme>\ui`.
2. Copy the game's file you want to change into it, at the same path under `ui`, taking the newest copy (see above).
3. Change the copy with your tools.
4. Start the game, pick your theme in the row of the screen you changed, and go to that screen.
5. After changing the file again, pick **The game's own** and then your theme again so Ember copies the new file, or restart the game.

## When it does not work

Ember writes what it does with themes to its log, `%APPDATA%\sf4e\logs\sf4e.log`:

| Log line | Meaning |
|---|---|
| `In-game themes: 2 with ui\main_menu`, `In-game themes: 1 with battle\hud` | Themes Ember found at start-up, per part |
| `In-game themes: the game searches ember\theme first` | The game was told to look in `ember\theme` |
| `In-game themes: ui\main_menu Juri` | The theme's main menu was copied in (`battle\hud CvS2` for the battle HUD) |
| `In-game themes: main menu opened again` | The main menu reloaded to show them |
| `In-game themes: ui\main_menu Juri could not be used: ...` | Why not. A missing theme goes back to the game's own menus; a file the game had open keeps the theme you had |

**Settings says no themes were found:** the theme folder needs its `ui` folder directly inside it (`ember\themes\Juri\ui\main_menu`, not `ember\themes\Juri\Juri\ui\main_menu`), and the game needs a restart after adding it.

**A screen is empty or stuck:** the replaced file for it does not work in the game. Pick **The game's own**, then fix or remove that file.
