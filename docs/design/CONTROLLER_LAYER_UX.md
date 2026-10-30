# Controller layer: UX spec

Status: design, for implementation on `feat/controller-layer` (off `nightly`).
Audience: the coding agents that implement it and the reviewer. Follow it literally. Where this spec names a component, reuse that component; do not add new widgets unless a section says so.

Engineering decisions are fixed (SDL3 gamepad layer, Ember answers the game's XInput calls for slots 0 to 3, DirectInput copies of SDL-served pads hidden, unknown-pad wizard, opt-out at next launch, identity-based saved controller, Steam virtual pad handling, netplay unaffected). This spec decides what the player sees and does.

Terms used below:

- **Recognised pad**: SDL opens it as a gamepad (built-in mapping, or a mapping the player saved with the wizard). Ember serves it to the game in an XInput slot. An SDL built-in mapping that lacks a button the wizard requires (its non-optional steps, 5.3) does not count: the pad is unmapped until the wizard records the rest, and the mapping the player saves then replaces SDL's.
- **Unmapped pad**: SDL sees it as a joystick with no gamepad mapping. The game keeps reading it through DirectInput, as today.
- **Gameplay controller**: the device in `input::Assignment::Selected()`. It drives Ember's menus and is bound to the match side.
- **Slot**: one of the game's four XInput indices 0 to 3. The player sees them as "game slot 1" to "game slot 4".
- **Brand**: Xbox, PlayStation, Nintendo, Stick (a pad set up with Ember's wizard) or Generic.

---

## 1. Principles

1. A plain Ember launch makes every pad work. No Steam setup, no drivers, no files to copy.
2. The game sees one kind of pad (Xbox). Ember adapts the pictures, never the button positions.
3. Ember's menus confirm and go back on the Xbox A and B positions of every XInput pad, served or not, and on the buttons the game's own menus use on a DirectInput stick the game reads (its LK and MK), unless the player binds other buttons for Ember's menus (11.14).
4. A pad Ember does not know still works as it does today until the player sets it up. Nothing gets worse.
5. Messages appear only when the player can act on them, and never over a fight.
6. The wizard can be finished, undone and cancelled with nothing but the pad being set up.
7. Netplay input is untouched: Ember changes which device the game reads, and with a button layout which attacks this PC's buttons do, before the input is sent. The opponent and the match are unaffected. (see 11)

---

## 2. Glyph policy

### 2.1 Brand detection

Brand comes from `SDL_GetGamepadType` for recognised pads, from the saved wizard mapping, or from the device path for game-read pads.

| Source | Brand | Start/Select art variant |
|---|---|---|
| `SDL_GAMEPAD_TYPE_XBOX360`, `SDL_GAMEPAD_TYPE_XBOXONE` | Xbox | Xbox |
| `SDL_GAMEPAD_TYPE_PS3` | PlayStation | PS3 (Start, Select) |
| `SDL_GAMEPAD_TYPE_PS4` | PlayStation | PS4 (Options, Share) |
| `SDL_GAMEPAD_TYPE_PS5` | PlayStation | PS5 (Options, Create) |
| `SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO`, `..._JOYCON_LEFT`, `..._JOYCON_RIGHT`, `..._JOYCON_PAIR` | Nintendo | Nintendo (Plus, Minus) |
| Pad SDL does not map itself, set up with Ember's wizard (any type) | Stick | Stick |
| `SDL_GAMEPAD_TYPE_STANDARD`, `SDL_GAMEPAD_TYPE_UNKNOWN`, any other value | Generic | drawn as Xbox |
| Steam virtual pad (its SDL GUID encodes 28DE:11FF) | the type SDL reports for it (SDL3 reads Steam's virtual pad info); Generic if none | as its brand |
| Game-read XInput pad (opt-out, SDL failed) | Xbox | Xbox |
| Game-read DirectInput pad (unmapped, opt-out, SDL failed) | none: a generic circle with "LK"/"MK", the game's menu functions (11.14; in the launcher: "1"/"2") | unchanged |

A pad SDL already maps keeps its brand after a wizard setup; only a pad SDL could not map before draws as Stick. Ember's mapping replaces SDL's own for that GUID, so whether SDL maps the pad itself is noted before Ember adds its mapping, at layer start and at save.

Generic draws Xbox art because SDL lays every gamepad out on Xbox positions, and most generic pads (8BitDo in X mode, PowerA, Logitech, PC arcade sticks in SDL's database) print Xbox letters.

### 2.2 Nintendo confirm and back: by position

By default Ember confirms with the **south** button and goes back with the **east** button on every pad it serves, Nintendo included, where the game's default menu table also puts LK and MK (11.14). Moving LK and MK in the game's button screen does not move Ember's; binding Menu confirm and Menu back on the Button layout screen does. On a Switch Pro pad that is the button printed **B** (confirm) and the one printed **A** (back), and the legend shows those printed letters.

Why: SDL3 reports face buttons by position (`SDL_GAMEPAD_BUTTON_SOUTH`, `EAST`, `WEST`, `NORTH`) and the game receives the south button as Xbox A. With the game's default menu table its own menus therefore confirm with the south button too. If Ember confirmed by label (east), the same player would press different buttons to confirm in Ember and in the game a second later. Positional keeps one rule; the picture tells the truth about which button that is. Ember does not follow LK when the player moves it in the game's button screen: that screen moves LK and MK in the menu table along with the fight layout, so Ember's confirm would change with every change of fight buttons.

A forced style does not move buttons either. With Button prompts set to Xbox on a Switch pad, the legend shows Xbox **A** for the same south button.

### 2.3 Glyph ids stay; art and spoken names change

Keep the logical glyph ids exactly as they are today: `"A" "B" "X" "Y" "LB" "RB" "LT" "RT" "View" "Start" "dpad" "horizontal"`, and add `"LS"` and `"RS"` for the stick clicks (see 11). They name Xbox positions. `PhysicalGlyph` keeps returning them for every recognised pad, Stick included. Code that hard-codes ids (replay HUD `ReplayPlaybackHud.cxx:80`, room hints `ApplicationShell.cxx:836`, `GoNowGlyph`) keeps doing so.

The brand is applied at exactly one translation point, a new `BrandGlyph(id, brand)` in `MenuGlyphs.hxx`, used by `PromptAsset` and by every place that prints a glyph's name:

- For Xbox, PlayStation, Nintendo and Generic it only selects the art (2.6). No name is printed.
- For Stick it returns the generic circle plus a fight name, by position (follows the wizard's mapping in 5.3): A = `LK`, B = `MK`, X = `LP`, Y = `MP`, LB = `3P`, RB = `HP`, LT = `3K`, RT = `HK`, View = `Select`, Start = `Start`. These are the defaults; the names actually drawn follow the layout in force for that pad, and an empty one draws no name (see 11.11). Every hint that draws a glyph with a label (`MenuLegend`, the replay HUD hints, the room hints, the challenger banner) prints that name before the label, the way `MenuLegend`'s `prefix()` prints "LP" for DirectInput pads today. The legend reads "LK Select", "MK Back", "LP Fighter"; the replay strip reads "HP Pause".

Two paths compare the literal id and keep working unchanged:

- `GameMenu.cxx:353`: room shortcut hints (X, Y, View) show only when `selectGlyph` is `"A"`. Every recognised pad now has `"A"`, so PlayStation, Nintendo and Stick players get the shortcut hints.
- `MenuGlyphs.hxx` `MenuLegend` `prefix()`: extend its generic circle test to ask `BrandGlyph` for the name, so Stick names print and DirectInput "LK"/"MK" print too.

The brand that applies is chosen by `SetMenuGlyphs`, which gains a brand argument. The menu prompts follow the gameplay controller's brand, resolved through the Button prompts setting (2.4). Keyboard glyph behaviour is unchanged: keyboard prompts still win whenever the keyboard was used last (`NoteMenuDevice`, `ReadMenuInput`).

### 2.4 Button prompts setting

Values, in Left/Right order: **Automatic**, **Xbox**, **PlayStation**, **Nintendo**. Default Automatic.

| Setting | Recognised pad | Stick (wizard) pad | Game-read XInput (opt-out, SDL failed, Steam) | Game-read DirectInput |
|---|---|---|---|---|
| Automatic | its brand (2.1) | Stick ids, generic circles | Xbox | LK/MK (11.14) |
| Xbox / PlayStation / Nintendo | that brand | that brand, Xbox ids | that brand | LK/MK (11.14) |

The PlayStation forced style uses PS4 art for Start/Select. A forced style applies to every place prompts are drawn: the shell legend, room hints, replay HUD, training strip, challenger banner, Go now glyph.

### 2.5 Text names (where Ember prints a button name instead of art)

`TrainingShortcuts.cxx` draws button names as text pills. Use this table (not localised; they are printed button names):

| Brand | View position | Start position |
|---|---|---|
| Xbox, Generic | Back | Start |
| PlayStation PS3 | Select | Start |
| PlayStation PS4 | Share | Options |
| PlayStation PS5 | Create | Options |
| Nintendo | Minus | Plus |
| Stick | Select | Start |
| Game-read DirectInput | Select | Start (unchanged) |

### 2.6 Art files

All from Kenney Input Prompts 1.5A, folder `.../Double/`, 128 px, unmodified, copied into `assets/input/` next to the existing files. Take them from the same archive recorded in `assets/input/README.md` (check the SHA-256) and add each folder to the README's "Selected assets" sentence.

| Glyph id | Xbox (existing unless marked new) | PlayStation Series | Nintendo Switch | Stick |
|---|---|---|---|---|
| A | `xbox_button_color_a.png` | `playstation_button_color_cross.png` | `switch_button_b.png` | generic circle + "LK" |
| B | `xbox_button_color_b.png` | `playstation_button_color_circle.png` | `switch_button_a.png` | generic circle + "MK" |
| X | `xbox_button_color_x.png` | `playstation_button_color_square.png` | `switch_button_y.png` | generic circle + "LP" |
| Y | `xbox_button_color_y.png` | `playstation_button_color_triangle.png` | `switch_button_x.png` | generic circle + "MP" |
| LB | `xbox_lb.png` | `playstation_trigger_l1.png` | `switch_button_l.png` | generic circle + "3P" |
| RB | `xbox_rb.png` | `playstation_trigger_r1.png` | `switch_button_r.png` | generic circle + "HP" |
| LT | `xbox_lt.png` | `playstation_trigger_l2.png` | `switch_button_zl.png` | generic circle + "3K" |
| RT | `xbox_rt.png` | `playstation_trigger_r2.png` | `switch_button_zr.png` | generic circle + "HK" |
| View | `xbox_button_back.png` | PS3 `playstation3_button_select.png`, PS4 `playstation4_button_share.png`, PS5 `playstation5_button_create.png` | `switch_button_minus.png` | generic circle + "Select" |
| Start | `xbox_button_start.png` | PS3 `playstation3_button_start.png`, PS4 `playstation4_button_options.png`, PS5 `playstation5_button_options.png` | `switch_button_plus.png` | generic circle + "Start" |
| LS (new id, see 11) | `xbox_ls.png` (new) | `playstation_button_l3.png` (new) | `switch_stick_l_press.png` (new) | generic circle + its layout name |
| RS (new id, see 11) | `xbox_rs.png` (new) | `playstation_button_r3.png` (new) | `switch_stick_r_press.png` (new) | generic circle + its layout name |
| dpad | `xbox_dpad.png` | `playstation_dpad.png` | `switch_dpad.png` | `generic_joystick.png` (new, Generic) |
| horizontal | `xbox_dpad_horizontal.png` | `playstation_dpad_horizontal.png` | `switch_dpad_horizontal.png` | `generic_joystick_horizontal.png` (new, Generic) |
| up / down / left / right (input test only) | `xbox_dpad_up.png`, `xbox_dpad_down.png`, `xbox_dpad_left.png`, `xbox_dpad_right.png` (new) | `playstation_dpad_up.png`, `_down`, `_left`, `_right` | `switch_dpad_up.png`, `_down`, `_left`, `_right` | `generic_stick_up.png`, `_down`, `_left`, `_right` (new, Generic) |

Controller pictures (capture screen, controller list, wizard header):

| Device | File (folder) |
|---|---|
| Xbox 360 type | `controller_xbox360.png` (Xbox Series) |
| Xbox One / Series type | `controller_xboxseries.png` (Xbox Series) |
| PS3 | `controller_playstation3.png` (PlayStation Series) |
| PS4 | `controller_playstation4.png` (PlayStation Series) |
| PS5 | `controller_playstation5.png` (PlayStation Series) |
| Switch Pro | `controller_switch_pro.png` (Nintendo Switch) |
| Joy-Con pair or single Joy-Con | `controller_switch.png` (Nintendo Switch) |
| Steam virtual pad with no known original type | `controller_steam.png` (Steam Controller) |
| `SDL_JOYSTICK_TYPE_ARCADE_STICK`, or a pad only Ember's wizard maps (2.1) | `generic_joystick_red.png` (Generic) |
| Generic, unmapped, game-read DirectInput | `controller_generic.png` (Flairs) |
| Saved controller not connected | `controller_disconnected.png` (Flairs) |

Total new files: 63, including the six LS and RS files (see 11). Keyboard art is unchanged.

---

## 3. First connect, selection and restore

### 3.1 Slots

1. At layer start the saved gameplay controller, if connected and recognised, takes slot 1. Other recognised pads take slots 2 to 4 in SDL enumeration order.
2. A pad keeps its slot while connected.
3. A pad that disconnects frees its slot and the slot stays reserved for that pad's identity for the rest of the session.
4. A newly connected pad takes, in order: its own reserved slot if free; the lowest free slot reserved for nobody; the lowest free slot reserved for someone else (the reservation is dropped and logged); else it is **Not in use**.
5. When a slot frees and pads are waiting as Not in use, the one that connected first takes it.

Reason for rule 1: the game applies its saved button layouts to XInput slots by position, not by pad, so keeping the player's own pad in slot 1 keeps the layout they set in the game's Options with that pad. A pad with an Ember button layout carries its layout to whatever slot it takes, so for it rule 1 only keeps the slot stable (see 11).

### 3.2 Identity

A pad's identity is `{guid, serial, path, name}` from SDL. Two identities match when both serials are non-empty and equal; otherwise when guid and path are equal. (A DualShock 4 has different GUIDs over USB and Bluetooth; the serial ties them together.)

The saved gameplay controller is stored as that identity plus `brand` in Ember's player preferences, replacing `deviceType`/`deviceIdx` for pads. The keyboard is saved as today. On first launch of this version, an old saved XInput slot (`deviceType` 3, `deviceIdx` 0 to 3) is migrated by adopting whatever recognised pad is in that slot at the first main menu, then saving its identity.

### 3.3 Restore on launch

At the first main menu (`CaptureMenuInput`, which already runs there):

| Situation | Result | Player sees |
|---|---|---|
| Saved controller connected | Adopted and assigned to side 1 | Nothing new |
| Saved controller missing, exactly one pad connected | That pad is adopted for this session only; the saved identity is kept | Home status, 4 s: "{saved} is not connected. Using {pad} for now." |
| Saved controller missing, several pads | Capture starts, as today | ASSIGN CONTROLLER screen |
| Saved controller missing, no pads | Keyboard fallback, provisional, as today | Home row "Choose gameplay controller", as today |
| Saved controller connects later, at the main menu, no room, current device provisional | Saved controller adopted again | Status or passive hint: "{saved} connected. You play with it." |
| Saved is the keyboard | Unchanged | Unchanged |

### 3.4 When a pad connects

Hints are debounced 1 s: a pad that disconnects and an equivalent pad that appears within 1 s (Steam Input toggling, a wireless pad reconnecting) produce no hint.

"Passive hint" means a new centered, no-input window drawn by `DrawOverlayLayers` exactly like the existing "Ember shortcut" hint, at top 76 (scaled), shown 4 s, only when `passive` is true and `matchActive` is false. "Shell status" means the screen's status line (Home: its small-print line) in `Tone::Neutral` for 4 s; it never replaces an error the screen is showing.

| Context | Recognised pad (not the gameplay one) | Gameplay controller reconnects | Unmapped pad | Fifth pad |
|---|---|---|---|---|
| Main menu, Ember hidden | Passive hint `controller.connected` (or `controller.connected_adopted` when adopted per 3.3, or `controller.connected_keyboard` when the keyboard is the chosen device) | Passive hint `controller.reconnected` | Passive hint `controller.connected_unmapped` | Passive hint `controller.connected_full` |
| Main menu, Ember open, no room | Shell status, same keys | Shell status `controller.reconnected` | Shell status `controller.connected_unmapped`; setup prompt (5.1) on its first press | Shell status `controller.connected_full` |
| In a room (any room screen) | Nothing (log only) | Room status line no longer shows `controller.disconnected_room` | Nothing | Nothing |
| Online match, offline battle, Training, replay | Nothing (log only) | Warning disappears (3.5) | Nothing | Nothing |
| Game's own menus (Options, etc.) | Nothing | Nothing | Nothing | Nothing |

A pad plugged in inside a room cannot become the gameplay controller there: `canChangeController` requires the main menu and no room (`sf4e__NetplayRuntime__Status.cxx:466`). This spec keeps that gate.

### 3.5 Disconnects

| Context | Player sees |
|---|---|
| Main menu, Ember hidden | Passive hint `controller.disconnected_named` |
| Ember open, no room | Shell status `controller.disconnected` in `Tone::Error` while disconnected (existing `controllerUnavailable` path, ApplicationShell.cxx:522, new copy) |
| In a room, not fighting | Same path, but the room screens use `controller.disconnected_room` instead |
| Online match (GGPO), gameplay controller | Existing `DrawControllerWarning` with `runtime.match_input_blocked` (new copy). Netplay keeps running with neutral input, unchanged. When it reconnects in its reserved slot the warning disappears and input resumes. |
| Online match, it comes back in another slot (reservation lost per 3.1 rule 4) | Same warning, text `runtime.match_input_moved`, until the match ends. At the next main menu Ember adopts it again by identity. |
| Offline battle, Training | Ember adds nothing; the game reacts as it does to an unplugged Xbox pad. |

### 3.6 Capture screen (ASSIGN CONTROLLER)

Screen and flow unchanged (`input::Assignment`, Idle, ReleaseAll, Press, ReleaseSelected). Changes:

- Device names come from SDL (`"DualSense Wireless Controller"`), not the game's `"Controller N"`. `DeviceName` uses the layer's name for slots 0 to 3; DirectInput and keyboard names are unchanged.
- The row's detail pane gets a `GameMenu::Detail` callback (the same mechanism as `screen=="interface"`, ApplicationShell.cxx:901):
  - **ReleaseAll / Press**: under the existing text, one line per connected pad: 32 px controller picture (2.6), then its name. A pad whose buttons are held draws at full opacity, others at 40%.
  - **ReleaseSelected**: the candidate's 64 px picture and `controller.release_to_use` ("Release the button to play with {0}.").
- The row's detail text, legend and cancel behaviour stay as they are.

---

## 4. Settings > Player & controller

### 4.1 Player screen (`screen=="player"`, title `player.title`)

Rows in order. Existing ids are kept so the journey tests still find them.

| # | id | Component | Label | Value | Detail | Enabled |
|---|---|---|---|---|---|---|
| 1 | `name` | `TextRow` | unchanged | unchanged | unchanged | unchanged |
| 2 | `controllers` (new) | `Row` with `value` set (as `sent-reports`) | `player.controllers` | `player.controllers_value` with the count of connected pads (recognised + unmapped + not in use) | `player.controllers_detail` with the gameplay controller's label | always |
| 3 | `capture` | `Row` | unchanged | none | unchanged key; `{0}` now the SDL name | `canChangeController` |
| 4 | `keyboard` | `ConfirmRow` | unchanged | none | unchanged | `canChangeController` |
| 5 | `button-prompts` (new) | `Value` | `player.button_prompts` | `player.prompts_auto_with` ({0} = resolved brand word) / `controller.brand.xbox` / `controller.brand.playstation` / `controller.brand.nintendo` | `player.button_prompts_detail` | `canEditPreferences`, else detail `settings.leave_room_to_edit` |
| 6 | `controls` (see 11) | `Row` with `value` set | `player.button_layout` (replaces `player.native_menus`) | the gameplay controller's layout name (11.3), or none | per 11.5 (`player.button_layout_detail` and its disabled variants) | per 11.5 |
| 7 | `game-buttons` (new, see 11) | `Row` | `player.game_buttons` | none | `player.game_buttons_detail` (replaces `player.native_menus_detail`) | `idle` |
| 8 | `background-play` | `Value` | unchanged | unchanged | unchanged | unchanged |
| 9 | `lower-input-lag` | `Value` | unchanged | unchanged | unchanged | unchanged |
| 10 | `lower-keyboard-lag` | `Value` | unchanged | unchanged | unchanged | unchanged |
| 11 | `faster-display` | `Value` | unchanged | unchanged | unchanged | unchanged |
| 12 | `game-reads-pads` (new) | `Value` | `player.game_reads` | `common.on` / `common.off` | `player.game_reads_detail`; while the saved value differs from the running mode, append `"\n\n"` + `player.game_reads_pending` | `canEditPreferences`, else detail `settings.leave_room_to_edit` |

Button behaviour on this screen (standard shell rules, listed so nothing is guessed):

| Row | A / Enter | B / Esc | Left / Right | X / Y / View |
|---|---|---|---|---|
| controllers | opens `controllers` screen | back to Settings | nothing | nothing |
| capture | starts capture (unchanged) | back | nothing | nothing |
| keyboard | confirmation (unchanged) | back | nothing | nothing |
| button-prompts | nothing | back | previous / next value, saved by the normal autosave | nothing |
| controls (see 11) | opens `button-layout` for the gameplay controller (11.5); nothing when disabled | back | nothing | nothing |
| game-buttons (see 11) | `StartOffline`, Ember closes (the old `controls` behaviour) | back | nothing | nothing |
| game-reads-pads | nothing | back | Off / On, autosaved | nothing |

The resolved brand word for `player.prompts_auto_with`: the gameplay controller's brand name (`controller.brand.*`); `controller.brand.stick` for Stick; Xbox for Generic and game-read XInput; when the gameplay device is the keyboard or a DirectInput pad, the value is plain `player.prompts_auto`.

`button-prompts` and `game-reads-pads` are stored in `netplay::PlayerPreferences` (add both to `SamePreferences` and the bool/enum preference lists the same way `fasterDisplay` is). Button prompts apply immediately. Let the game read controllers applies at the next launch only.

### 4.2 Controllers screen (`screen=="controllers"`, new, title `controllers.title`)

Pushed from Player. Back returns to Player. A plain list (`GameMenu::Draw`, one column) with a `GameMenu::Detail` callback.

Rows, in this order:

1. **State row, first, only when it applies** (`InfoRow`, no value):
   - SDL failed to start: id `pads-unavailable`, label `controllers.unavailable`, detail `controllers.unavailable_detail`.
   - Opt-out running: id `pads-game`, label `controllers.game_reads`, detail `controllers.game_reads_detail`.
2. **One row per pad**, ordered: slot 1 to 4, then unmapped pads in DirectInput index order, then Not in use pads in connection order, then the saved controller if it is not connected. id `pad-<n>` where n is a stable per-session number. Label: the pad's name, `userText=true` (elided, not translated). Pad names are driver text and may hold any characters: pass every one to `NoteUserText` (as `ApplicationShell.cxx:347` does for the room name), and set `detailText=DetailText::Name` on any row whose detail embeds a pad name (this screen's rows, Player rows 2 and 3, the capture row, the wizard rows). Value and detail per state:

| State | Value | Detail | A / Enter |
|---|---|---|---|
| Recognised, gameplay controller | `controllers.value_yours` | `controllers.detail_yours` ({0} slot) | opens the pad choice (below) |
| Recognised, other | none | `controllers.detail_other` ({0} slot) | opens the pad choice |
| Recognised with an Ember setup | as above | as above + `"\n"` + `controllers.detail_custom` | opens the pad choice |
| Recognised, Stick or Steam, with an Ember button layout (see 11) | as above | as above + `"\n"` + `controllers.detail_layout` ({0} = layout name) | opens the pad choice |
| Steam virtual pad | `controllers.value_steam` | `controllers.detail_steam` | opens the pad choice (setup disabled) |
| Unmapped | `controllers.value_unmapped` | `controllers.detail_unmapped` | starts the wizard (5) if `canEditPreferences`; otherwise disabled with `settings.leave_room_to_edit` |
| Not in use (more than four) | `controllers.value_full` | `controllers.detail_full` | nothing (`info=true`) |
| Saved, not connected | `controllers.value_absent` | `controllers.detail_absent` | nothing (`info=true`) |
| Opt-out or SDL failed: each game device from `ReadDevices` | `controllers.value_yours` for the gameplay one, else none | `controllers.detail_game_read` | nothing (`info=true`) |

3. With no pad rows at all: `InfoRow` id `pads-none`, label `controllers.none`, detail `controllers.none_detail`.

**Pad choice** (the row carries `choices`, so A opens the shell's choice modal; Left/Right move, A picks, B cancels):

| Choice id | Label | Detail | Enabled |
|---|---|---|---|
| `play` | `controllers.choice_play` | `controllers.choice_play_detail` | not already the gameplay controller and `canChangeController`; else detail `controllers.choice_play_current` or `settings.leave_room_to_edit` |
| `layout` (see 11) | `controllers.choice_layout` | `controllers.choice_layout_detail` | Recognised, Stick and Steam pads, in rooms too (the screen opens read-only, 11.6.4). On a GameRead row (layer active) shown disabled with detail `controllers.choice_layout_setup` |
| `setup` | `controllers.choice_setup` | `controllers.choice_setup_detail` | `canEditPreferences` and not a Steam pad; else detail `settings.leave_room_to_edit` or `controllers.choice_setup_steam` |
| `forget` | `controllers.choice_forget` | `controllers.choice_forget_detail` | shown only when an Ember setup is saved for its GUID |

Results: `play` adopts the pad (same path as a finished capture: `Adopt`, `AssignToSide(...,0,false)`, save identity) and shows shell status `controllers.now_playing`. `layout` opens `button-layout` for the pad, focus `test` (see 11.5). `setup` opens the wizard. `forget` removes the saved mapping from preferences and shows shell status `controllers.forgotten`; the pad keeps working with the old setup until the next launch.

**Detail pane drawing** (`GameMenu::Detail`, under the row's detail text, as the Interface screen draws its preview):

- Recognised and Steam pads: 64 px controller picture (2.6), then heading `controllers.test_heading`, then the **input test**:
  - Line 1: four 32 px direction squares (up, down, left, right art for the pad's resolved prompt brand).
  - Line 2: 32 px squares in this order: A, B, X, Y, LB, RB, LT, RT, LS, RS, View, Start, drawn with `DrawPromptGlyph` and the resolved brand, leaving out buttons the pad lacks (11.2). Stick shows generic circles named by the pad's layout in force (11.11; by default LK, MK, LP, MP, 3P, HP, 3K, HK, then Select, Start). (see 11)
  - Each square is drawn at 35% alpha when up and full alpha with a 2 px `palette::Ember` underline when held. A trigger counts as held past 50%. A direction lights from the d-pad or from the left stick past 50%.
  - The test reads the pad through the layer every frame, whatever slot it has and whether or not it is the gameplay controller. On the gameplay controller, A and B still act on the menu as usual.
- Unmapped pads: 64 px `controller_generic.png`, heading `controllers.test_heading`, then one text line. The game holds an unmapped pad for exclusive DirectInput use, so outside the wizard Ember only knows whether the game reads a press on it (its held-button word for the matching DirectInput index, `GetButtonsOn`). The picture is drawn at full strength while it does and dimmed otherwise; the line reads `controllers.test_nothing` at rest and is left empty while pressed. When raw inputs are known (`PadView::raw`), the line is `controllers.test_raw` with a comma-separated list using the input names from 5.6 (`"Button 4, Hat up"`). A pad that cannot be matched to a DirectInput index shows no presses.
- Not in use, absent, game-read rows: the picture only.

### 4.3 How "Fighting buttons" (the game's own button config) is presented

(see 11) The game's own screen moves to the new row 7, `game-buttons`, with the old behaviour (Ember closes and the game's main menu takes over). Row 6, `controls`, is now Ember's Button layout (11.5, 11.6). The `game-buttons` copy says the game's screen is for the keyboard and for controllers the game reads itself, that a controller with an Ember layout uses that layout instead, and that the game only keeps a layout where every action has a button (the native validator silently drops other layouts; see Code notes and 11.13). `about.controls_detail` is rewritten (section 8) so Help no longer says only Xbox pads have A/B.

---

## 5. Unknown-pad wizard

### 5.1 Entry points

1. **Automatic prompt.** When all of these hold: Ember's shell is visible; the screen is not a room screen; no capture, wizard, notice or modal is open; `canEditPreferences`; an unmapped pad that has not been prompted for this session reports a press (the game's own DirectInput state for that pad, which also counts a stick pushed past the game's dead zone; SDL does not open the pad outside the wizard). Ember shows the shell's modal notice (`GameMenu::ShowNotice`) with heading `pad_setup.prompt_heading`, text `pad_setup.prompt_text` ({0} = pad name). The notice's dismiss button reads `pad_setup.not_now` (small extension: `ShowNotice` takes an optional label for its OK button). While the notice is open, Ember keeps reading that pad the same way: once it is at rest, the **next** press on it closes the notice and opens the wizard. Not now, Esc, a click or B on the gameplay controller close it; that pad is not prompted again this session.
2. **Controllers screen**: A on an unmapped row, or `setup` on a recognised pad's choice.
3. No entry from inside a room, a match or Training.

### 5.2 Screen

Screen id `pad-setup`, title `pad_setup.title`. Drawn with `GameMenu::Draw`: the list holds one row per step (label = step name, value = what is recorded), and a `GameMenu::Detail` callback draws the prompt. The wizard has two modes.

- **Capture mode**: list navigation is off (`ControllerNavigation::Update(..., enabled=false)`, as during controller capture); the focused row follows the current step. Ember reads only the pad being set up, raw, through `SDL_Joystick` (for a recognised pad, through `SDL_GetGamepadJoystick`). While the wizard runs, the layer reports that pad's XInput slot as neutral (connected, nothing held), so the game sees nothing. An unmapped or game-read pad is opened by SDL only while its wizard runs: SDL's DirectInput backend and the game both take the pad for exclusive use, so the game's own device lets go of it first and fails `Acquire` until the wizard ends (`DirectInputFilter::LendToSetup`). The game sees that pad as disconnected meanwhile. After a cancel SDL closes it and the game takes it back; after Save Ember serves it in an XInput slot.
- **Summary mode**: normal list navigation; see 5.8.

Detail pane in capture mode, top to bottom:

1. 64 px picture of the pad (2.6) and its name.
2. `pad_setup.step_count` ("Step {0} of {1}").
3. The step name at 1.4x the body font.
4. The instruction: `pad_setup.press_direction` for the direction steps (1 to 4) and the stick steps (15 to 18), `pad_setup.press_button` for the rest. On optional steps, add `pad_setup.optional`, or `pad_setup.optional_stick` on a stick step. On the LK step add `pad_setup.lk_selects`; on the MK step add `pad_setup.mk_backs`.
5. A feedback line, one at a time, priority top first: disconnect countdown (5.10); hold ring with `pad_setup.hold_cancel`; conflict `pad_setup.conflict` (2 s, `Tone::Error`); required `pad_setup.required` (2 s, `Tone::Error`); idle hint `pad_setup.idle_hint` (after 10 s with no input).

Legend (`MenuLegend` with `primary=nullptr` and `extras`; the standard select/back pair is not shown in capture mode):

| Hint | Glyph | Label | Shown |
|---|---|---|---|
| Keyboard cancel | `Esc` | `common.cancel` | always |
| Keyboard back a step | `Bksp` (new key cap; add `"Bksp"` and `"Space"` to `KeyCapGlyph`) | `pad_setup.back_step` | from step 2 |
| Keyboard skip | `Space` | `pad_setup.skip` | optional steps |
| Pad back a step | `generic_stick_left` | `pad_setup.back_step` | from step 5 |
| Pad skip | `generic_stick_right` | `pad_setup.skip` | optional steps from step 5 |
| Pad cancel | generic circle | `pad_setup.hold_to_cancel` | always |

Mouse: no clickable controls in capture mode beyond the shell header's Back button, which acts as Cancel.

### 5.3 Steps and mapping

The SDL target for each attack follows the game's default Xbox fight layout, so a pad set up here fights the way the player pressed it without visiting the game's button config. This default is the layout of a standard Xbox 360 arcade stick (top row X, Y, RB, LB; bottom row A, B, RT, LT). It is **assumed, not verified in this repo**; verify it against the game's default XInput fight table (`0xA5D078`, see Code notes) before implementation and change only the SDL target column if it differs.

| # | Step | Name key | SDL mapping target | Required |
|---|---|---|---|---|
| 1 | Up | `pad_setup.step.up` | `dpup` | yes |
| 2 | Down | `pad_setup.step.down` | `dpdown` | yes |
| 3 | Left | `pad_setup.step.left` | `dpleft` | yes |
| 4 | Right | `pad_setup.step.right` | `dpright` | yes |
| 5 | Light punch | `pad_setup.step.lp` | `x` (west) | yes |
| 6 | Medium punch | `pad_setup.step.mp` | `y` (north) | yes |
| 7 | Heavy punch | `pad_setup.step.hp` | `rightshoulder` | yes |
| 8 | Light kick | `pad_setup.step.lk` | `a` (south: Ember's Select) | yes |
| 9 | Medium kick | `pad_setup.step.mk` | `b` (east: Ember's Back) | yes |
| 10 | Heavy kick | `pad_setup.step.hk` | `righttrigger` | yes |
| 11 | All three punches | `pad_setup.step.3p` | `leftshoulder` | no |
| 12 | All three kicks | `pad_setup.step.3k` | `lefttrigger` | no |
| 13 | Left stick click | `pad_setup.step.ls` | `leftstick` | no |
| 14 | Right stick click | `pad_setup.step.rs` | `rightstick` | no |
| 15 | Left stick right | `pad_setup.step.lx` | `leftx` (stick step) | no |
| 16 | Left stick down | `pad_setup.step.ly` | `lefty` (stick step) | no |
| 17 | Right stick right | `pad_setup.step.rx` | `rightx` (stick step) | no |
| 18 | Right stick down | `pad_setup.step.ry` | `righty` (stick step) | no |
| 19 | Start | `pad_setup.step.start` | `start` | yes |
| 20 | Select | `pad_setup.step.select` | `back` | no |

Order: the game's attack order LP MP HP LK MK HK, then the optional 3P and 3K so a six-button pad simply skips the last two attacks, then the optional stick clicks and sticks, then Start and Select. The steps cover every control the game or Ember reads. There is no Home (Guide) step: the game reads XInput through `XInputGetState`, which has no Guide bit, and nothing in Ember reads Guide. A Guide binding the pad already has is kept (5.9).

Directions go to the d-pad. Each stick is one step for its X axis (push right) and one for its Y axis (push down). A stick step records by the 5.4 rules and maps the full axis: `leftx:a<i>` when the push went to the axis's plus end, `leftx:a<i>~` when it went to the minus end. A stick step that records a button, a hat or a trigger maps the plus half of the axis, `+leftx:b3`.

Consequence, stated on the LK, MK and summary screens: on a set-up pad, the LK button selects and the MK button goes back in Ember's menus and in the game's menus.

### 5.4 Reading inputs

- **Rest.** When the wizard opens, after the triggering press is released, Ember samples every axis and hat; those values are rest. A button already held then is waited on for up to 5 s with `controller.release_buttons`; if it is still held it is ignored for this wizard (logged). Rest is sampled again after a reconnect (5.10).
- **Button**: becomes active on down. Recorded as `b<i>`.
- **Hat**: active when it leaves centred with a single cardinal direction (diagonals are ignored until they become cardinal). Recorded as `h<h>.<v>` with v = 1 up, 2 right, 4 down, 8 left.
- **Axis**: active when `|value - rest| > 16000`.
  - If rest is within 8000 of zero (a stick): half axis, `+a<i>` or `-a<i>` by direction of travel.
  - If rest is beyond 24000 from zero (a trigger at an end): full axis, `a<i>`, with `~` appended when rest is the positive end.
  - Otherwise: half axis as for a stick.
  - Inactive again within 8000 of rest.
- **Several at once**: the first input to become active in a step owns the step; others are ignored until everything is back at rest.
- **Hitboxes and leverless controllers**: directions are buttons and are recorded as buttons. Ember does nothing about SOCD; the device's own cleaning applies, and Ember records what the device reports.
- **Sticks with the lever on a hat**: recorded as hat directions. A stick that reports the lever on both a hat and axes records whichever arrives first.
- Ember never reads the keyboard or other pads as step input.

### 5.5 The tap and hold ladder (capture mode)

One gesture rule for every capture step:

| Gesture on any input of the pad | Result |
|---|---|
| Active, back at rest within 1000 ms | **Tap.** In steps 1 to 4: record it. From step 5: if it is the input recorded for Left, back a step; if it is the input recorded for Right, skip (optional step) or show `pad_setup.required`; any other already recorded input shows `pad_setup.conflict`; otherwise record it. |
| Held 1000 ms | Nothing is recorded. The hold ring starts with `pad_setup.hold_cancel`. |
| Released between 1000 and 3000 ms | Nothing happens; the ring disappears. |
| Held 3000 ms | Cancel (5.9). |

A recorded tap fills the row's value and moves to the next step once everything is at rest. Recording on release (not on press) is what lets a hold mean cancel.

Keyboard, any time in capture mode: Esc cancels (5.9), Backspace backs a step (nothing on step 1), Space skips an optional step (on a required one it shows `pad_setup.required`).

### 5.6 How recorded inputs are named

Used in step values, the summary, the unmapped input test and conflicts.

| Recorded | Text |
|---|---|
| `b<i>` | `pad_setup.input.button` with i + 1 |
| `h<h>.1` / `.2` / `.4` / `.8` | `pad_setup.input.hat_up` / `_right` / `_down` / `_left` (hat number omitted; if the pad has two hats, append " {h+1}") |
| `+a<i>` / `-a<i>` | `pad_setup.input.axis_plus` / `pad_setup.input.axis_minus` with i + 1 |
| `a<i>` / `a<i>~` | `pad_setup.input.trigger` with i + 1 |
| skipped | `pad_setup.skipped` |
| nothing yet | `common.not_set` |

### 5.7 Conflicts

An input already recorded for another step is never recorded twice. In capture mode the player sees `pad_setup.conflict` ("{0} is already {1}. Press a different one.") and the step waits. To move an input to another step, the player redoes the step that holds it from the summary, which clears it first.

A stick step maps both halves of its axis, so a half axis recorded for any other step (a direction on that stick, say) conflicts with it, whichever was recorded first. The tap ladder (5.5) is checked before conflicts, so on a stick step the input recorded for Left still backs a step and the one for Right still skips.

### 5.8 Summary mode

Shown after the last step (20). Same screen; list navigation on.

Rows:

1. The 20 step rows (`Row`, value per 5.6). A on a step row clears that step and runs capture mode for that one step, then returns to the summary with focus on it. Ember never offers to clear a required step without redoing it.
2. `save`: `Row` label `pad_setup.save`, detail `pad_setup.save_detail`. Enabled only when every required step is recorded.
3. `restart`: `Row` label `pad_setup.start_over`, detail `pad_setup.start_over_detail`. Clears all steps and goes to step 1.
4. `cancel`: `ConfirmRow` label `pad_setup.cancel`, detail `pad_setup.cancel_detail`. Confirming cancels (5.9).

Detail pane in summary mode: pad picture and name, then `pad_setup.summary_detail`. While any recorded input is held, its step row's value draws in `palette::Ember` (the test). For step rows, `pad_setup.redo_detail` is the row detail.

Navigation in summary mode, with the pad being set up (its raw inputs read through the just recorded steps, `SetupMenuDraft`): Up and Down move, LK selects, MK is Back, and the legend names those recorded inputs, never the installed menu buttons (11.14). Back on the summary focuses `cancel` and opens its confirmation. The hold ladder's 3000 ms cancel still applies. The gameplay controller (if it is another pad) and the keyboard navigate as on any screen; Esc is Back.

### 5.9 Cancel, save and results

- **Cancel** (3 s hold, Esc in capture mode, `cancel` confirmed, header Back in capture mode): nothing is saved, the pad's slot stops reading neutral, the wizard closes to the screen it came from, and shell status shows `pad_setup.cancelled`.
- **Save**: Ember first takes SDL's snapshot of the pad (`ReadSnapshot`, `sf4e__GamepadBindings.hxx`): the bindings SDL makes for it, the face style its button labels follow and its effective type. The snapshot owns those three; mapping text never supplies them. Ember then builds the mapping string `<guid>,<name>,<pairs>,face:<style>,[type:<type>,]hint:!SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,<metadata>platform:Windows,` (`SetupMapping`, `WriteSdlMapping`), calls `SDL_AddGamepadMapping`, stores the string under the GUID in preferences (`controllerMappings`), and loads all stored strings at every layer start before opening pads. For example, an unmapped stick with LP on button 0, LK on button 3 and Up on the hat saves `<guid>,<name>,dpup:h0.1,x:b0,a:b3,face:abxy,hint:!SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,platform:Windows,`.
  - **Pairs**: the merged bindings (below), recorded steps first in step order with skipped steps omitted. They are positional (`a`, `b`, `x`, `y` are south, east, west, north) in one canonical form (`SdlMapping.hxx`: `+`/`-` for a half axis, `~` for an inverted one).
  - **Positional marker**: the negated GameCube hint is SDL's mark of a positional mapping, without which SDL swaps `b` and `x` on a GameCube pad. It is the only hint written; every hint the old text had is replaced by it.
  - **Face**: SDL's face style (`abxy`, `axby`, `bayx` or `sony`) told apart by the labels SDL gives the open gamepad's south and east buttons. It comes before every other field that binds nothing, because SDL takes the first `face:` and, without one, reads the positional marker as GameCube labels. An unmapped pad takes the face style SDL gives its type.
  - **Type**: SDL's canonical name for the type it gives the open gamepad (`SDL_GetGamepadType`, `SDL_GetGamepadStringForType`), written after the face only when it differs from the type SDL finds for the device itself (`SDL_GetRealGamepadTypeForID`, SDL's fallback without a type field). A type SDL would find anyway is never pinned, so a PS4 pad known by its VID and PID gets no `type:`, a `type:standard` mapping over it keeps `type:standard`, and an unmapped pad gets none. The type is what Ember's brand and prompts come from (`PadType`, `DetectBrand`), so a save never changes them.
  - **A pad SDL already maps** (a recognised pad, or one with an earlier Ember setup): the new mapping merges the recorded steps over the bindings SDL makes for the pad now, read with `SDL_GetGamepadBindings` from its open gamepad, or one opened for the read. Those are SDL's own results, with label hints and the HIDAPI fixup applied, so Ember never reads bindings from mapping text. Every target a step recorded takes the recorded input. An existing binding whose input overlaps one a step now uses for another target is dropped, so one physical input never drives two targets: the same button, hat masks that share a direction (`h0.3` overlaps `h0.1` and `h0.2`), or the same axis where a full axis covers both halves and the two halves stay apart. Every other existing binding is kept, including those of skipped steps: skipping 3P keeps the pad's own LB, and skipping the stick steps keeps its sticks. Recorded bindings come first in step order, then the kept bindings in SDL's order. A target with a half-axis sign (`+leftx`) counts as the same target as `leftx`.
  - **Metadata**: from the text SDL returns for the GUID (`MappingForGuid`), only `crc:`, `sdk>=:` and `sdk<=:` are carried over (`SdlMetadata`), as SDL reads them (the first of each key after the name) and only when the value is plain letters and digits. `face:` and `type:` come from the snapshot instead, since that text can be cut short (SDL stops at a second `platform:`) or spell a value the filter would refuse (`type:+ps4`). Hints are dropped: a label hint would rename the positional pairs, and any other hint can keep SDL from adding the mapping. `platform:Windows,` is written once, last.
  - **A HIDAPI pad** (GUID byte 14 is `h`): SDL takes a mapping for a legacy one when the first binding to a button from `dpup` on reads that same button number, and then moves every button input. The first such pair written is one that reads another input. When every one reads its own number, no order avoids it, so the save fails as a rejected string does.
  - **An unmapped pad** has no existing mapping, so its string holds the recorded pairs only, with its type's face style and no type field.
  - The pad becomes a recognised pad: it takes a slot (3.1), its DirectInput copy is hidden from the game, and if it was the gameplay controller it is adopted again by identity.
  - The wizard closes to the Controllers screen with focus on that pad and shows `GameMenu::ShowNotice` heading `pad_setup.saved_heading`, text `pad_setup.saved`, with the alternative button `player.button_layout` that opens `button-layout` for that pad (see 11.5). When the wizard was started from `button-layout` (11.6.4), it closes back to that screen instead, now editable, with the same notice minus the alternative.
  - If SDL cannot open the pad or its binding read fails (`SDL_GetGamepadBindings` returns null), there is no snapshot and the save stops before any mapping is built: a failed read never stands for a pad that binds nothing. That, a rejected string, or the HIDAPI case above leaving no string: `ShowError(pad_setup.save_failed)`, nothing stored, wizard stays in summary mode.
  - If preferences cannot be written, the existing settings error path applies (`error.settings_not_saved`); the mapping still applies for this session.
- **Reset**: `forget` on the Controllers screen (4.2). Effective at the next launch.

### 5.10 Disconnect and no input

- **Pad disconnects during the wizard**: progress is kept for 15 s. The feedback line shows `pad_setup.reconnect` with the seconds left. If a pad with the same identity reconnects in time, rest is sampled again and the wizard continues at the same step (or summary). Otherwise the wizard closes, nothing is saved, and the shell shows `ShowNotice(pad_setup.stopped_disconnect)`.
- **No input**: after 10 s without any input on a capture step, show `pad_setup.idle_hint`. After 60 s without any input (pad or keyboard), the wizard closes, nothing is saved, and the shell shows `ShowNotice(pad_setup.stopped_idle)`.
- **The game leaves the main menu or the shell is hidden**: the wizard cancels silently (logged); the next time Ember opens, the Controllers screen is where it was.

---

## 6. Messages and edge states

| State | What the player sees | Where |
|---|---|---|
| More than four recognised pads | Extra pads are Not in use. `controller.connected_full` when one connects; row value `controllers.value_full` and detail `controllers.detail_full`. When a slot frees, the first waiting pad takes it with `controller.connected`. | 3.4, 4.2 |
| Steam Input virtual pad present (its SDL GUID encodes 28DE:11FF; SDL reports the physical pad's VID:PID when Steam supplies it, so the GUID decides) | Ember serves the Steam pad and hides its physical twin. One row per physical pad: the original pad's name and brand when SDL knows them, else `controllers.steam_name`; value `controllers.value_steam`; detail `controllers.detail_steam`; setup disabled with `controllers.choice_setup_steam`. No other message; launch options are never mentioned. | 4.2 |
| Steam pairing rule | SDL3 already hides most physical pads Steam has taken. If SDL still reports a physical pad of the same SDL type as a Steam pad, Ember pairs them one to one in enumeration order, serves the Steam pad and does not open the physical one. Why the Steam pad: the player chose Steam by launching through it, Steam applies their Steam layout to it, and Steam can hide the physical pad at any moment. | Log only |
| Opt-out on (Let the game read controllers) | The game reads pads as before this feature. Controllers screen shows `controllers.game_reads` first and lists game devices read-only. Setup, input test and forget are not offered. Button prompts still apply to game XInput pads (useful with Steam Input); DirectInput pads show the generic circle with LP and LK again, and those buttons select and go back. Ember button layouts stay stored but are not applied, and Player `controls` is disabled (see 11). | 4.2 |
| SDL failed to start | Same as opt-out for this launch, with `controllers.unavailable` as the first Controllers row. No notice or popup elsewhere. | 4.2 |
| Pad recognised, gameplay device is the keyboard | `controller.connected_keyboard` (passive hint or shell status). The pad is not adopted. Controllers screen offers `play` on it. | 3.4 |
| Pad disconnects during the wizard | 5.10. | 5 |
| Saved mapping rejected at startup (corrupt prefs) | Nothing for the player; the pad is unmapped and gets the prompt again. Logged. | 7 |
| Unmapped pad is the gameplay controller | Works as today: LP selects, LK goes back, Start opens Ember. Its row says so (`controllers.detail_unmapped`). | 4.2 |

---

## 7. Log lines

All at `spdlog::info` unless marked, each prefixed `Input:`. Names are quoted; GUIDs are 32 hex chars; `id` is a short identity tag (first 8 hex of a hash of serial or path) so logs can follow one pad without printing serials.

| When | Line |
|---|---|
| Layer start | `Input: SDL {version} started, hidapi {on/off}, background events on, {n} saved mappings loaded` |
| Saved mapping rejected | `Input: saved mapping for guid {guid} rejected: {SDL_GetError}` (warn) |
| SDL failed | `Input: SDL failed to start ({SDL_GetError}); the game reads controllers itself` (warn) |
| Opt-out | `Input: the game reads controllers itself (setting)` |
| Pad added, recognised | `Input: pad added '{name}' {VID:PID} type {sdl type} brand {brand} guid {guid} id {id} slot {n}` |
| Pad added, unmapped | `Input: pad added '{name}' {VID:PID} guid {guid} id {id} unmapped, left to DirectInput` |
| Pad added, no slot | `Input: pad added '{name}' id {id} not in use, four slots taken` |
| Steam pad | `Input: Steam virtual pad '{name}' as {sdl type} slot {n}` |
| Steam twin | `Input: physical pad '{name}' id {id} paired with Steam pad slot {n}; not opened` |
| DirectInput copy hidden | `Input: hiding DirectInput copy of '{name}' {VID:PID}` |
| Pad removed | `Input: pad removed '{name}' id {id} slot {n} reserved` |
| Reservation dropped | `Input: slot {n} reserved for id {old} given to '{name}' id {new}` |
| Waiting pad placed | `Input: '{name}' id {id} takes freed slot {n}` |
| Restore | `Input: saved controller '{name}' id {id} restored to slot {n}` / `Input: saved controller id {id} not connected; using '{name}' for now` |
| Gameplay controller set | `Input: gameplay controller '{name}' id {id} slot {n} by {capture/list/restore/only pad/migration}` |
| Brand change | `Input: prompts {brand} ({automatic/forced})`, only when it changes |
| Setup prompt | `Input: setup prompt for '{name}' guid {guid}` / `Input: setup prompt declined` |
| Wizard start | `Input: setup started for '{name}' guid {guid}: {b} buttons, {h} hats, {a} axes; rest {a0=..,a1=..}` |
| Stuck at start | `Input: setup ignores held {input}` |
| Step | `Input: setup step {name} = {b3/h0.1/-a1/a5~}` / `skipped` / `conflict with {step}` / `back` |
| Wizard end | `Input: setup saved for guid {guid}: {mapping string}` / `Input: setup cancelled ({hold/escape/confirm/disconnect/idle/left menu})` |
| Forget | `Input: setup for guid {guid} forgotten; applies next start` |
| Rumble | `Input: rumble unsupported on '{name}'` (once per pad per session) |
| Button layouts | the lines in 11.12 (see 11) |

Existing lines stay: `Input: game pads ... | hid ... | steam overlay ...` (LogInventory) and `Input: P{n} plays with ...`, which now prints the SDL name for slots 0 to 3.

---

## 8. Localization (en.po)

New keys. Other languages fall back to English. Screen titles follow the existing all-caps title convention.

| Key | English |
|---|---|
| `controller.brand.xbox` | Xbox |
| `controller.brand.playstation` | PlayStation |
| `controller.brand.nintendo` | Nintendo |
| `controller.brand.stick` | Fight buttons |
| `controller.connected` | {0} connected. |
| `controller.connected_adopted` | {0} connected. You play with it. |
| `controller.connected_keyboard` | {0} connected. You play with the keyboard; to switch, use Change controller in Settings > Player & controller. |
| `controller.connected_unmapped` | {0} connected. Open Ember to set up its buttons. |
| `controller.connected_full` | {0} connected, but four controllers are already in use. Disconnect one to use it. |
| `controller.disconnected_named` | {0} disconnected. |
| `controller.reconnected` | {0} reconnected. |
| `controller.using_for_now` | {0} is not connected. Using {1} for now. |
| `controller.release_to_use` | Release the button to play with {0}. |
| `player.controllers` | Controllers |
| `player.controllers_value` | {0} connected |
| `player.controllers_detail` | You play with {0}. See every connected controller, test its buttons, and set up ones Ember does not know yet. |
| `player.button_prompts` | Button prompts |
| `player.prompts_auto` | Automatic |
| `player.prompts_auto_with` | Automatic ({0}) |
| `player.button_prompts_detail` | The button pictures Ember shows. Automatic matches the controller you play with. Which button does what stays the same. |
| `player.button_layout` (see 11) | Button layout |
| `player.button_layout_detail` (see 11) | Choose what each button on {0} does in fights. It changes nothing for your opponent. |
| `player.button_layout_setup` (see 11) | Ember does not handle {0} yet. Select to set up its buttons first. |
| `player.button_layout_keyboard` (see 11) | Button layouts are for controllers. For the keyboard, use Game's button settings. |
| `player.button_layout_game_reads` (see 11) | Turn off Let the game read controllers to use button layouts. Until then, use Game's button settings. |
| `player.button_layout_failed` (see 11) | Ember's controller support did not start this time. Use Game's button settings. |
| `player.game_buttons` (see 11) | Game's button settings |
| `player.game_buttons_detail` (see 11) | Ember closes. In the game's menu, choose Options, then the controller settings. Use it for the keyboard and for controllers the game reads itself. A controller with an Ember button layout uses that layout instead. The game keeps a layout only when every action has a button. |
| `player.game_reads` | Let the game read controllers |
| `player.game_reads_detail` | Off: Ember reads every controller and the game sees each one as an Xbox pad. On: the game reads controllers itself, as it does without Ember, and PlayStation and Switch pads then need Steam Input. Changes apply the next time the game starts. |
| `player.game_reads_pending` | Restart the game to apply this. |
| `controllers.title` | CONTROLLERS |
| `controllers.unavailable` | Controller support is off |
| `controllers.unavailable_detail` | Ember could not start its controller support, so the game reads controllers itself this time. Xbox pads work as usual; other pads may need Steam Input. sf4e.log has the details. |
| `controllers.game_reads` | The game reads controllers |
| `controllers.game_reads_detail` | Let the game read controllers is on, so Ember does not set up or test controllers. Turn it off in Player & controller and restart the game. |
| `controllers.none` | No controllers connected |
| `controllers.none_detail` | Connect a controller and it shows here. The keyboard works without one. |
| `controllers.value_yours` | Yours |
| `controllers.value_steam` | Steam Input |
| `controllers.value_unmapped` | Not set up |
| `controllers.value_full` | Not in use |
| `controllers.value_absent` | Not connected |
| `controllers.detail_yours` | You play with this controller. Game slot {0}. Press its buttons to test them. |
| `controllers.detail_other` | Game slot {0}. Press its buttons to test them. Select to play with it or set up its buttons. |
| `controllers.detail_custom` | Uses the button setup you saved. |
| `controllers.detail_steam` | Steam Input handles this controller, so its buttons follow your Steam settings. To let Ember handle it, start Ember outside Steam, or turn off Steam Input for Ultra Street Fighter IV in Steam. |
| `controllers.detail_unmapped` | Ember does not know this controller's buttons yet. Until it is set up, the game reads it directly and Ember's menus use the buttons set as LP and LK. Select to set it up. |
| `controllers.detail_full` | The game has room for four controllers. Disconnect another one to use this one. |
| `controllers.detail_absent` | Your saved controller. Connect it to play with it again. |
| `controllers.detail_game_read` | The game reads this controller itself. |
| `controllers.steam_name` | Steam controller |
| `controllers.test_heading` | Press buttons to test them |
| `controllers.test_raw` | Pressing: {0} |
| `controllers.test_nothing` | Pressing: nothing |
| `controllers.choice_play` | Play with this controller |
| `controllers.choice_play_detail` | Use it for online play and Ember's menus. |
| `controllers.choice_play_current` | You already play with this controller. |
| `controllers.choice_setup` | Set up buttons |
| `controllers.choice_setup_detail` | Press each button in turn to tell Ember what it does. |
| `controllers.choice_setup_steam` | Steam Input sets this controller's buttons. |
| `controllers.choice_forget` | Forget button setup |
| `controllers.choice_forget_detail` | Go back to how this controller worked before you set it up. Applies the next time the game starts. |
| `controllers.choice_layout` (see 11) | Button layout |
| `controllers.choice_layout_detail` (see 11) | Choose what each button does in fights. |
| `controllers.choice_layout_setup` (see 11) | Set up its buttons first. |
| `controllers.detail_layout` (see 11) | Button layout: {0}. |
| `controllers.now_playing` | You play with {0}. |
| `controllers.forgotten` | Button setup forgotten. This applies the next time the game starts. |
| `pad_setup.title` | SET UP CONTROLLER |
| `pad_setup.prompt_heading` | New controller |
| `pad_setup.prompt_text` | Ember does not know the buttons of {0} yet. Press any button on it to set it up now. It takes about a minute. |
| `pad_setup.not_now` | Not now |
| `pad_setup.step_count` | Step {0} of {1} |
| `pad_setup.step.up` | Up |
| `pad_setup.step.down` | Down |
| `pad_setup.step.left` | Left |
| `pad_setup.step.right` | Right |
| `pad_setup.step.lp` | Light punch |
| `pad_setup.step.mp` | Medium punch |
| `pad_setup.step.hp` | Heavy punch |
| `pad_setup.step.lk` | Light kick |
| `pad_setup.step.mk` | Medium kick |
| `pad_setup.step.hk` | Heavy kick |
| `pad_setup.step.3p` | All three punches |
| `pad_setup.step.3k` | All three kicks |
| `pad_setup.step.ls` | Left stick click |
| `pad_setup.step.rs` | Right stick click |
| `pad_setup.step.lx` | Left stick right |
| `pad_setup.step.ly` | Left stick down |
| `pad_setup.step.rx` | Right stick right |
| `pad_setup.step.ry` | Right stick down |
| `pad_setup.step.start` | Start |
| `pad_setup.step.select` | Select |
| `pad_setup.press_direction` | Push the stick or press the button for this direction, then let go. |
| `pad_setup.press_button` | Press the button you want for this, then let go. |
| `pad_setup.optional` | If your controller has no button for this, skip it. |
| `pad_setup.optional_stick` | If your controller has no stick for this, skip it. |
| `pad_setup.lk_selects` | This button will also select in menus. |
| `pad_setup.mk_backs` | This button will also go back in menus. |
| `pad_setup.hold_cancel` | Keep holding to cancel setup. |
| `pad_setup.hold_to_cancel` | Hold any button: cancel |
| `pad_setup.back_step` | Back a step |
| `pad_setup.skip` | Skip |
| `pad_setup.required` | This one is needed. |
| `pad_setup.conflict` | {0} is already {1}. Press a different one. |
| `pad_setup.idle_hint` | Nothing pressed yet. Hold any button for 3 seconds to cancel, or press Esc. |
| `pad_setup.reconnect` | Controller disconnected. Reconnect it within {0} seconds to carry on. |
| `pad_setup.input.button` | Button {0} |
| `pad_setup.input.hat_up` | Hat up |
| `pad_setup.input.hat_right` | Hat right |
| `pad_setup.input.hat_down` | Hat down |
| `pad_setup.input.hat_left` | Hat left |
| `pad_setup.input.axis_plus` | Axis {0} + |
| `pad_setup.input.axis_minus` | Axis {0} - |
| `pad_setup.input.trigger` | Trigger axis {0} |
| `pad_setup.skipped` | Skipped |
| `pad_setup.summary_detail` | Press any button to see which step it is. Here, Light kick selects and Medium kick goes back, as they will in every menu. |
| `pad_setup.redo_detail` | Select to press this one again. |
| `pad_setup.save` | Save |
| `pad_setup.save_detail` | Save this setup and use it now. |
| `pad_setup.start_over` | Start over |
| `pad_setup.start_over_detail` | Clear every step and begin again. |
| `pad_setup.cancel` | Cancel setup |
| `pad_setup.cancel_detail` | Leave without saving. |
| `pad_setup.cancelled` | Setup cancelled. Nothing was saved. |
| `pad_setup.saved_heading` | Controller set up |
| `pad_setup.saved` (see 11) | {0} is set up. To change what its buttons do in fights, choose Button layout. |
| `pad_setup.save_failed` | Ember could not use this setup, so nothing was saved. sf4e.log has the details. |
| `pad_setup.stopped_disconnect` | Setup stopped because {0} disconnected. Nothing was saved. |
| `pad_setup.stopped_idle` | Setup stopped because nothing was pressed for a minute. Nothing was saved. |

New keys for button layouts (see 11). Short fight names (LP, 3P, LP+LK) and printed button names (Cross, L1, ZL) are not localised.

| Key | English |
|---|---|
| `button_layout.title` | BUTTON LAYOUT |
| `button_layout.button_title` | CHANGE BUTTON |
| `button_layout.test` | Try your buttons |
| `button_layout.test_detail` | Press buttons on {0} to see which attacks they do. On this row, only Back acts on the menu. |
| `button_layout.attacks` | Attacks |
| `button_layout.throw` | Throw (LP+LK) |
| `button_layout.focus` | Focus attack (MP+MK) |
| `button_layout.red_focus` | Red Focus attack (LP+MP+MK) |
| `button_layout.row_detail` | Select, then press the button you want for {0}. If another button has it, the two buttons swap. |
| `button_layout.row_missing` | No button does only this, so you cannot press it on its own. |
| `button_layout.no_button` | No button |
| `button_layout.edit_button` | Change one button |
| `button_layout.edit_button_detail` | Select, then press a button to choose exactly which attacks it does. Use it for any mix of attacks, or to turn a button off. |
| `button_layout.preset` | Preset |
| `button_layout.preset_detail` | Select to use a ready-made layout. Default is the game's own layout. |
| `button_layout.custom` | Custom |
| `button_layout.game_layout` | Game's layout |
| `button_layout.preset.default` | Default |
| `button_layout.preset.default_detail` | The game's own layout. It suits most pads and 8-button sticks. |
| `button_layout.preset.triggers` | Triggers for all three |
| `button_layout.preset.triggers_detail` | For six-button pads. Heavy punch on the right shoulder button and heavy kick on the left. All three punches on the left trigger, all three kicks on the right. |
| `button_layout.preset.focus_throw` | Focus and throw |
| `button_layout.preset.focus_throw_detail` | Like Default, but the left shoulder button does Focus attack and the left trigger throws. On an 8-button stick, that is the fourth column. All three punches and all three kicks move to the stick clicks. |
| `button_layout.preset_applied` | Layout set to {0}. |
| `button_layout.remove` | Use the game's layout |
| `button_layout.remove_detail` | Ember stops changing this controller's buttons, and the layout from Game's button settings applies again. |
| `button_layout.removed` | The game's layout applies to this controller again. |
| `button_layout.press_for` | Press the button for {0} |
| `button_layout.press_to_change` | Press the button to change |
| `button_layout.waiting` | Press a button... |
| `button_layout.press_cancel` | Move the stick or press Start to cancel. |
| `button_layout.assigned` | {0} is on {1} now. |
| `button_layout.swapped` | {0} is on {1} now. {2} does {3} instead. |
| `button_layout.swapped_nothing` | {0} is on {1} now. {2} does nothing now. |
| `button_layout.bit_detail` | Left or Right turns {0} on or off for this button. |
| `button_layout.clear` | No attacks |
| `button_layout.clear_detail` | This button does nothing in fights. |
| `button_layout.button_does` | In fights: {0} |
| `button_layout.nothing` | Nothing |
| `button_layout.absent` | Not connected |
| `button_layout.absent_detail` | {0} is not connected. Its layout is saved. Connect it to change it. |
| `button_layout.setup_first` | Set up its buttons first |
| `button_layout.setup_first_detail` | The game reads {0} itself, so Ember cannot change its buttons yet. Set it up once and it works like any other controller here. |
| `button_layout.missing_heading` | Some attacks have no button |
| `button_layout.missing_text` | These attacks have no button of their own: {0}. The layout is saved. |
| `button_layout.keep_editing` | Keep editing |
| `button_layout.leave` | Leave |

Changed keys (new English text; translators see them as changed):

| Key | English |
|---|---|
| `controller.disconnected` | Controller disconnected. Reconnect it, or choose another in Settings > Player & controller. |
| `runtime.match_input_blocked` | {0} disconnected. Your fighter gets no input until you reconnect it. |
| `about.controls_detail` (see 11) | Xbox: A selects and B goes back. PlayStation: Cross selects and Circle goes back. Switch: B selects and A goes back, as in the game's own menus. A controller set up in Ember: the button you chose for Light kick selects and the one you chose for Medium kick goes back, whatever its button layout. Keyboard: arrows, Enter and Escape. The D-pad moves; Left and Right change a value. Going back at Home hides Ember. F10, or Start on a controller, opens it again at the game's main menu.\n\nIn a room, A on a table lets you pick a seat, A on your own seat readies up, and B leaves your seat or queue place (on other controllers, the buttons in the same places). On Xbox, X opens Fighter, Y opens Options and View opens Chat; press it again to return to the room. PlayStation uses Square, Triangle and Share or Create; Switch uses Y, X and Minus.\n\nIn training, F6 opens the controls, F5 hides the HUD, F7 records and F8 plays. On a controller, hold View (Share, Create or Minus) and press Start. |

New keys, replacing nothing:

| Key | English |
|---|---|
| `controller.disconnected_room` | Controller disconnected. Reconnect it, or leave the room to choose another. |
| `runtime.match_input_moved` | {0} is back, but in another game slot. It works again from the next game. |

Retired keys (remove from every .po): `player.native_menus`, `player.native_menus_detail`, and `player.fight_buttons`, `player.fight_buttons_detail` (replaced by `player.button_layout*` and `player.game_buttons*`; see 11).

---

## 9. Out of scope, later

- The launcher's recovery window (`RecoveryController`, `RecoverySurface.cxx:150`) runs before the game exists and keeps its own XInput and DirectInput polling and its "1"/"2" generic glyphs. Unchanged.
- (see 11) Fight button remapping is now in scope for controllers the layer serves. Still out of scope: Ember layouts for the keyboard and for game-read DirectInput pads, writing `.ctrlsav`, and remapping directions, View or Start.
- A rumble on/off setting, rumble strength, gyro, touchpad, LEDs, battery display.
- Per-pad Button prompts choice (the setting is global).
- Left stick deadzone or SOCD settings.
- Shipping extra mappings beyond SDL's built-in database.
- Local versus with two pads beyond what the game already does: Ember only guarantees side 1.
- Players with more than four pads choosing which four are used.

---

## 10. Acceptance checklist

U = unit test of pure logic (no SDL, no game). R = UiRender harness shot. G = in-game check.

Glyphs and prompts

1. U: brand from SDL type follows the 2.1 table for every listed type, including Steam with and without a known original type.
2. U: glyph resolution (brand x Button prompts x device kind) follows 2.4; the glyph id for the south button is `"A"` for every recognised brand; `BrandGlyph("A", Stick)` gives the generic circle and the name `LK`; `BrandGlyph("RB", Stick)` gives `HP`.
3. U: `PromptAsset` returns the 2.6 file for every id and brand; Nintendo A id returns `switch_button_b`.
4. U: every file named in 2.6 exists in `assets/input/` and is listed in the README.
5. R: Player screen legend with each of Xbox, PlayStation (PS4 and PS5), Nintendo, Stick, keyboard; room screen with the X/Y/View shortcut hints shown for PlayStation and Nintendo.
6. R: training strip pill text per 2.5 for each brand.
7. G: with a DualSense, Cross selects and Circle goes back in Ember; Square, Triangle and Create open Fighter, Options and Chat in a room; replay controls work with L1, R1, L2, R2.
8. G: with a Switch Pro pad, the button printed B selects in Ember and in the game's main menu.

Slots, identity, restore

9. U: slot assignment rules 3.1 (start order, sticky slot, reservation, reservation reuse, fifth pad waits, waiting pad takes a freed slot).
10. U: identity match rule 3.2 (serial wins; guid plus path otherwise; USB and Bluetooth DS4 with the same serial match).
11. U: preferences round trip of saved identity, button prompts, game reads controllers, and `controllerMappings`; migration of an old XInput slot save.
12. U: restore table 3.3, every row.
13. G: restart the game with two pads plugged in a different order; the saved one is still the gameplay controller and is game slot 1.
14. G: in an online match, unplug and replug the gameplay pad; the warning shows the new text, then disappears and input resumes in the same match.

Settings and Controllers screens

15. R: Player screen rows in the 4.1 order, with labels, values and details; `controllers` value shows the count.
16. R: Controllers screen with one of each row state from 4.2, plus `pads-none`, `pads-unavailable` and `pads-game` variants.
17. R: Controllers detail pane input test with all squares up, and with A, LB and Up held (lit with underline).
18. R: unmapped row detail with `Pressing: Button 4, Hat up`.
19. U (shell journey): A on a recognised row opens the choice with `play`, `layout`, `setup`, and `forget` only when a mapping is saved, in that order; `play` disabled with the right detail when already in use or in a room; `layout` enabled in a room and disabled with `controllers.choice_layout_setup` on a GameRead row. (see 11)
20. U (shell journey): Button prompts Left/Right cycles Automatic, Xbox, PlayStation, Nintendo and autosaves; Let the game read controllers shows the pending line after a change.
21. R: capture screen in Press with two pads listed (one lit) and in ReleaseSelected with the candidate picture.

Wizard

22. U: mapping string builder: a recorded step list gives exactly `<guid>,<name>,` + pairs in step order + `face:abxy,hint:!SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,platform:Windows,`; skipped steps are absent; hats, half axes, full axes and inverted full axes are written as in 5.4; stick steps write full axes or `+` half outputs (5.3). Over an existing mapping (5.9): recorded targets replace their pairs, a reused input drops its old pair (a composite hat included), a skipped step keeps its binding, `crc:` (also `crc:b003`), `sdk>=:` and `sdk<=:` stay, old hints give way to the one positional marker, `face:` and `type:` appear at most once and come from the snapshot, `platform` appears once, and SDL accepts every result. On virtual pads, every corpus mapping written back keeps SDL's bindings, the labels of all four face buttons, `SDL_GetGamepadType` and Ember's brand, including `type:ps4` behind a second `platform:`, `type:+ps4`, a pad with no type, a GameCube hint and a PS4 pad known by VID and PID. A failed binding read gives no snapshot and no mapping.
23. U: input classifier with rest values: trigger resting at -32768 is not active at start; moving it to +32767 records `a<i>`; resting at +32767 records `a<i>~`; a stick axis records `+a`/`-a`; a diagonal hat waits.
24. U: tap and hold ladder 5.5 with fake time: tap under 1000 ms records; 1000 to 3000 ms release records nothing; 3000 ms cancels; from step 5 the Left input backs a step and the Right input skips only optional steps.
25. U: conflicts (5.7), required step skip refused, back a step clears the previous step, Start over clears all.
26. U: disconnect keeps progress 15 s and resumes on the same identity; 60 s idle stops; nothing is saved in either stop.
27. R: wizard capture mode on step 1, step 8 (LK, with its extra line), step 11 (optional, skip hints), hold ring, conflict line, idle hint, reconnect countdown; summary mode with Save enabled and with one row lit.
28. R: setup prompt notice with Not now.
29. G: with a hitbox or a stick SDL does not know: the prompt appears on its first press at Home; the wizard completes using only that device; after Save it shows as an Xbox pad in the game's controller settings; LP to HK do the right attacks in Training with the game's default layout.
30. G: the wizard can be cancelled with a 3 s hold, and with Esc.

Edge states

31. G: launched through Steam with Steam Input on and a DualShock 4: one controller row, value Steam Input, PlayStation prompts; log shows the Steam and twin lines; no duplicate.
32. G: Let the game read controllers on, restart: the log shows the setting line, the Controllers screen shows `pads-game`, a DirectInput pad selects with LP again.
33. G: five pads: the fifth is Not in use; unplug one and the fifth takes the freed slot with a hint.
34. G: keyboard chosen, then a pad is plugged in at Home: the keyboard hint shows and the keyboard stays the gameplay device.
35. G: with a DualSense that has no Ember button layout, change a button in the game's own controller settings, restart the game; the change is still there (the "layout won't save" report). A pad with an Ember layout uses that layout instead (see 11).
36. G: with Play in the background on and the game window behind another window, the DualSense still moves the fighter.
37. G: every `Input:` line in section 7 appears in sf4e.log for its event.

Button layout (see 11)

38. U: `ButtonLayout.hxx` presets: `default` equals the 0xA5D078 masks for A to RT and 0 for LS and RS; `PresetOf` returns `default`, `triggers` and `focus-throw` for their exact masks and Custom after any single change.
39. U: `FightName`: 0x10 `LP`, 0x430 `3P`, 0x8C0 `3K`, 0x50 `LP+LK`, 0xA0 `MP+MK`, 0xB0 `LP+MP+MK`, 0xC00 `HP+HK`, 0x70 `LP+MP+LK`, 0 empty.
40. U: `Assign` from Default: Light punch on Y gives X = MP, Y = LP; 3P on RB gives LB = HP, RB = 3P; Throw on LT when no button has exactly LP+LK changes only LT; Light punch on X changes nothing; Light punch on Y leaves LB at 3P; with old mask 0 the other button becomes 0.
41. U: `SetMask` accepts any subset of 0xCF0 and the same function on two buttons; `PlayerPreferences::Valid` rejects a mask with any bit outside 0xCF0 and more than 32 layouts.
42. U: `MissingAttacks`: Default gives none; X cleared gives Light punch; LP only on an LP+LK button gives Light punch; a button the pad lacks (LT on a wizard pad that skipped 3K) does not count.
43. U: preferences round trip of `buttonLayouts`; an entry with 9 masks or a mask of 0x1000 is dropped and the rest load; saving a 33rd identity drops the oldest `edited`; `SamePreferences` sees a one-mask change.
44. U: `FindLayout`: serial match; guid plus path match; guid-only fallback when exactly one serial-less entry has that guid and no other connected pad has it; no fallback with two such entries or two such pads.
45. U: `SlotWritePlan`: a pad with a layout placed in a game-owned slot gives copy then layout; a pad without a layout taking an owned slot gives the game copy; a pad without a layout taking a game-owned slot gives nothing; a pad returning in slot 3 from slot 1 gives a write to slot 3 only; a removed layout gives the game copy; the first edit of a pad without a layout, sitting in a game-owned slot, gives copy then layout; an edit writes only its own slot; no plan ever addresses bits 0 to 9 or the menu table. `GameTableSwap`: around a config load the owned slots hold `gameCopy`, and the table the loader leaves becomes `gameCopy` with the layout written back, also when it equals the layout; a later removal, or the button screen opened later, shows that table; unowned slots are left untouched; a bounds failure refuses the whole Open or Close; a load while the button screen holds the tables is refused and the screen's Close takes the result.
46. U: Stick names: the default table gives the 2.3 names; after Light punch on RB the RB circle reads `LP` and X reads `HP`; a 0 mask gives no name and `PromptLabel("A", "Select", Stick)` with that table returns `Select`.
47. U (shell journey): Player `controls` gives the value, detail and enabled of each 11.5 row (layer pad, DirectInput pad, keyboard, game reads, SDL failed); `game-buttons` sends `StartOffline`.
48. U (shell journey): on `button-layout`, A on `fn-lp` opens capture; the held A is ignored until released; the next press of Y assigns, the status shows `button_layout.swapped`, then Saving and Saved; while capture is open its row reads `button_layout.waiting` and the other rows are disabled, and after it the row shows its button again; Start, a direction, Esc and 10 s without a press each cancel with no change; with `canEditPreferences` false every row but `test` is disabled and capture cannot open.
49. U (shell journey): Back after a change that leaves Light punch on no button opens the notice with Keep editing selected; Keep editing stays; Leave returns to Player; Back with no change this visit returns at once; returning from `button-layout-button` never opens it.
50. U (shell journey): the wizard's saved notice offers Button layout; choosing it opens `button-layout` for that pad with focus on `test`; Back returns to Controllers.
51. R: `button-layout` for an Xbox pad, a DualSense (L3, R3 and Cross in the values), a Switch Pro pad and a wizard Stick that skipped the stick clicks (raw input names in the values, layout names on the circles, no LS or RS squares); the `test` row with LB and A held, lighting LP, MP, HP and LK; capture mode for Red Focus with the capture card, and pick capture on `edit-button`; the read-only room state; the disconnected state; the set-up-first state.
52. R: `button-layout-button` for LB with LP and LK on; the missing-attack notice; the Controllers row detail with `Button layout: Custom.`
53. G: in Training with a DualSense, put Light punch on R1 from the layout screen; on the next frame R1 does light punch and Square does what R1 did.
54. G: restart the game; the layout still applies. Connect another pad first so the DualSense lands in game slot 2; its layout follows it, and the pad in slot 1 has the game's layout.
55. G: an online match between two PCs where one uses a custom layout: no desync, the other PC's buttons are unchanged, and the replay plays back correctly.
56. G: local versus with two pads, each with its own layout: each side uses its own.
57. G: with a wizard-set stick, change its layout; the legend's circle names follow, and the same physical button still selects in Ember's menus and the game's menus.
58. G: with the keyboard as the gameplay device, `controls` is disabled and `game-buttons` opens the game's screen.
59. G: unplug the pad mid-edit: the screen shows Not connected; plug it back in: editing resumes with the changes kept.
60. G: every `Input:` line in 11.12 appears in sf4e.log for its event.

---

## 11. Button layout

Kate: "make sure people can actually bind their controller buttons to what they want." This section adds Ember's own fight button layout per physical controller. Where it disagrees with sections 1 to 10, this section wins; the earlier spots it changes are marked "(see 11)".

### 11.1 Why Ember owns layouts

The game keeps one binding record per device (system+0xD4, stride 0x27C). Its fight table is 32 dwords at record+0x17C: entry i is the bitmask of game functions that physical input bit i triggers. The game re-reads the table on every input update (0x511E70), so a value Ember writes works on the next frame. Mapped buttons are read after this mapping and sent through GGPO, so a layout is local: the opponent is unaffected and netplay stays deterministic.

The game's own button screen (now Player row `game-buttons`, 11.5) cannot do this job: it silently drops a layout unless all nine required functions are bound (validator 0x4CBA40), it saves layouts by XInput slot position rather than per pad, and Ember must close to reach it.

### 11.2 Which controllers it covers

- **Covered:** every pad the layer serves to the game as XInput: Recognised pads, Steam virtual pads, and wizard-mapped (Stick) pads. Steam Input's own remapping, if any, applies first; the layout acts on what Steam sends.
- **Not covered:** game-read DirectInput pads (unmapped, and the 14 native-profile sticks of Decisions 2), every pad while Let the game read controllers is on or SDL failed, and the keyboard. For a DirectInput pad the screen offers Set up buttons (the wizard) first (11.6.4). Keyboard players use the game's own screen, row `game-buttons`.
- **Buttons a pad has.** A pad is offered only the bindable buttons it has: `SDL_GamepadHasButton` for A, B, X, Y, LB, RB, LS, RS and `SDL_GamepadHasAxis` for LT, RT. A wizard-mapped pad has LS and RS when their steps were recorded (5.3) or its own mapping had them (5.9), and lacks LB, LT, LS or RS when those steps were skipped on a pad SDL did not map. Hidden buttons keep their stored masks and are still written, but they are not listed, not counted by the missing-attack check (11.9) and not shown in the detail pane.

### 11.3 The layout record

**Bindable inputs**, in this order everywhere (storage, logs, tests). Glyph ids from 2.3; LS and RS are new ids (see 2.3). Bit index into the fight table, then the game's default mask (0xA5D078):

| # | Glyph id | Bit | Default |
|---|---|---|---|
| 0 | A | 18 | 0x40 (LK) |
| 1 | B | 17 | 0x80 (MK) |
| 2 | X | 19 | 0x10 (LP) |
| 3 | Y | 16 | 0x20 (MP) |
| 4 | LB | 20 | 0x430 (3P) |
| 5 | RB | 21 | 0x400 (HP) |
| 6 | LT | 22 | 0x8C0 (3K) |
| 7 | RT | 23 | 0x800 (HK) |
| 8 | LS | 24 | 0 |
| 9 | RS | 25 | 0 |

Bits 0 to 7 (directions), 8 (View, 0x2000) and 9 (Start, 0x1000) are fixed and never written. Ember never writes the menu table at record+0x1FC, so an Ember layout does not move the game's menu buttons or Ember's. The game's own button screen does write it, in step with the fight table (11.14).

**Functions:** LP 0x10, MP 0x20, HP 0x400, LK 0x40, MK 0x80, HK 0x800. A mask is any subset of 0xCF0, including 0 (the button does nothing in fights).

**Short names** (`FightName(mask)`, not localised; fighting notation, like 2.5): 0x430 is `3P`; 0x8C0 is `3K`; any other non-zero mask joins the names of its bits with `+` in the order LP, MP, HP, LK, MK, HK (`LP+LK`, `MP+MK`, `HP+HK`, `LP+MP+LK`); 0 is the empty string.

**Long names** (rows and status): the six attacks use `pad_setup.step.lp` to `pad_setup.step.hk`; 0x430 `pad_setup.step.3p`; 0x8C0 `pad_setup.step.3k`; 0x50 `button_layout.throw`; 0xA0 `button_layout.focus`; 0xB0 (LP+MP+MK, Red Focus Attack) `button_layout.red_focus`; any other mask uses its short name.

**Presets** (masks in the order of the table above):

| Id | Label | A | B | X | Y | LB | RB | LT | RT | LS | RS |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `default` | `button_layout.preset.default` | LK | MK | LP | MP | 3P | HP | 3K | HK | none | none |
| `triggers` | `button_layout.preset.triggers` | LK | MK | LP | MP | HK | HP | 3P | 3K | none | none |
| `focus-throw` | `button_layout.preset.focus_throw` | LK | MK | LP | MP | MP+MK | HP | LP+LK | HK | 3P | 3K |

`default` is the game's table exactly; it suits Xbox-style pads and 8-button sticks (top row X, Y, RB, LB; bottom row A, B, RT, LT). `triggers` is for six-button pads that want 3P and 3K on the triggers. `focus-throw` puts Focus and throw on the fourth column of an 8-button stick and keeps 3P and 3K on the stick clicks. Reset to default is picking `default` (11.6.2); there is no separate reset row.

`PresetOf(layout)` returns the preset whose ten masks all equal the layout's, else Custom (`button_layout.custom`). The **layout name** shown in values is that, or `button_layout.game_layout` when the pad has no Ember layout.

**Storage.** `netplay::PlayerPreferences` gains `buttonLayouts`: a list of `{identity, buttons[10], edited}`. `identity` uses the same fields as the saved gameplay controller (3.2); `buttons` are the ten masks in table order; `edited` is the Unix time of the last change. JSON key `buttonLayouts`, an array of objects `{"guid","serial","path","name","buttons":[10 integers],"edited"}`. Add it to `SamePreferences` and to `PlayerPreferences::Valid` (at most 32 entries; every mask a subset of 0xCF0). On load an entry with a wrong length or a mask outside 0xCF0 is dropped and logged; the rest load. Saving a 33rd identity drops the entry with the oldest `edited` (logged). Pure logic lives in a new `src/common/ButtonLayout.hxx`: the table above, presets, `FightName`, `PresetOf`, `Assign` (11.7), `SetMask`, `MissingAttacks` (11.9), `FindLayout`, `UpsertLayout` and `SlotWritePlan` (11.4).

**Finding a pad's layout** (`FindLayout`): the entry whose identity matches by 3.2 (`Same`). If none matches and the pad has no serial, the single entry with the same guid and an empty serial is used, provided exactly one such entry exists and no other connected pad has that guid; its stored path is then updated to the pad's (logged). So a layout follows its controller to any slot, to either side, and across launches. The 3.2 caveat applies: two identical serial-less pads are told apart by USB port, and swapping their ports swaps their layouts.

**Starting point.** A pad with no stored layout is shown, and edited, from the ten entries in force for it: its slot's fight table as read this frame, or the defaults when it has no slot. Its first change (assign, editor change or preset) creates its `buttonLayouts` entry from that starting point plus the change. After `remove` the screen shows the restored game copy, which is the same starting point again.

The runtime updates a stored path (guid fallback above) in its own copy of the preferences and lets the next save carry it; it never starts a save for that alone.

A layout saved for a controller that is not connected stays stored and is applied when it connects. Nothing lists it; it ages out only by the 32-entry cap. A wizard re-setup of the same pad keeps its layout (layouts sit on positions A to RS, which the wizard does not move). `forget` (4.2) leaves the layout stored; it simply stops applying once the pad is game-read again at the next launch.

### 11.4 Writing layouts into slots

The layer writes on the game thread, in the same pass that answers the game's XInput calls, before the frame's input update, and only once the binding records exist; until then writes wait. It writes only the ten entries of the bindable bits of the slot's XInput record.

Per slot the layer keeps `owner` (the identity whose Ember layout is in the table, or none) and `gameCopy` (the ten entries as the game had them).

| Event | Write |
|---|---|
| A pad with a layout takes slot n (layer start, connect, freed slot, reservation reuse; 3.1) | If `owner[n]` is none, copy the ten entries into `gameCopy[n]` first. Write the layout; `owner[n]` = that identity. |
| A pad without a layout takes slot n and `owner[n]` is set | Write `gameCopy[n]`; `owner[n]` = none. |
| A pad without a layout takes slot n and `owner[n]` is none | Nothing. |
| The layout of the pad in slot n changes (edit, preset), including the first edit of a pad that had no layout | If `owner[n]` is none, copy the ten entries into `gameCopy[n]` first and set `owner[n]` to the pad. Write the new layout. |
| The layout of the pad in slot n is removed (`remove`, 11.6.2) | Write `gameCopy[n]`; `owner[n]` = none. |
| The game loads its button config (the `.ctrlsav` loader `0x51BC30`, System vtable +0xC4, run by the title screen task `0x638130` on each visit to the title screen) | Detoured. Before the loader runs, write `gameCopy[n]` into every owned slot, so it works on the game's own tables. After it returns, copy the ten entries of every owned slot into `gameCopy[n]` whatever they are (the loaded table may equal the owner's layout), then write the layouts again; `owner[n]` is unchanged. Not while the game's button screen holds the slots (Decisions 6); that screen's close takes the tables back. |
| A pad disconnects | Nothing. The table stays until another pad takes the slot. |
| A pad comes back in a different slot (3.1 rule 4), including mid-match | Treated as "takes slot m" above. Its old slot is handled when another pad takes it. |
| Let the game read controllers on, or SDL failed | Never writes. |

`SlotWritePlan(previous owners, new placements, layouts)` returns the list of `{slot, source (layout or gameCopy)}` writes and is the unit under test; the game-thread writer only executes it. `GameTableSwap` does the config load row above and the button screen of Decisions 6: `Open(use)` writes every owned slot's `gameCopy`, and `Close` reads every owned slot back into `gameCopy` and writes the layouts again. It is the only path that changes an owned slot's `gameCopy` after the first copy, so nothing compares tables on each update. The swap saves and restores only the ten fight entries. The menu table needs no swap: Ember never writes it, and what the game's screen writes there (its own LK and MK, in step with `gameCopy`) is meant to stay, since the game's menus and Ember's default (11.14) follow it. In IDA the only writers of the fight tables are `System::Initialize` (`0x517900`, from the boot function `0x403DA0`, which builds the records before any layout is written), the loader, and the button screen (its edit copy back in the destructor, and its reset to defaults `0x512A90`/`0x514640`, reached only from `0x4CA9D0`). A slot whose record fails the bounds check is never written; in the swap, one such owned slot refuses the whole `Open` or `Close`, and a refused `Close` is retried on each update.

**Applying an edit.** The shell does not wait for the autosave. Each change sends `ControllerRequest{Kind::Layout, padId}` with the ten masks (new `Kind::Layout`; `Kind::RemoveLayout` for `remove`), and the runtime writes it on the next frame and logs it. The shell also sets `preferences_.buttonLayouts`, `preferencesDirty_ = true` and `saveAt_ = now + 0.45 s`, as a main fighter pick does, so the normal autosave stores the same data and the status shows Saving, then Saved. If the save fails, the existing `error.settings_not_saved` path applies and the layout stays in force for this session.

### 11.5 Entry points

**Player screen** (4.1, see 4.1 for the full table):

- Row `controls` keeps its id and becomes Ember's Button layout for the gameplay controller. Label `player.button_layout`. A opens screen `button-layout` targeting the gameplay controller (focus `test`), or does nothing where disabled:

| Gameplay device | Value | Detail | Enabled |
|---|---|---|---|
| Recognised, Stick or Steam pad (layer active) | its layout name (11.3) | `player.button_layout_detail` ({0} = pad name, `detailText=Name`) | yes, in rooms too (the screen is read-only there, 11.10) |
| Unmapped or native-profile DirectInput pad (layer active) | none | `player.button_layout_setup` ({0} = pad name) | yes; opens the screen in its set-up-first state (11.6.4) |
| Keyboard (including the provisional fallback) | none | `player.button_layout_keyboard` | no (`quiet`) |
| Any device, Let the game read controllers on | none | `player.button_layout_game_reads` | no (`quiet`) |
| Any device, SDL failed | none | `player.button_layout_failed` | no (`quiet`) |

- New row `game-buttons`, right after `controls`: the game's own button screen, kept for the keyboard and game-read pads. `Row`, label `player.game_buttons`, detail `player.game_buttons_detail`, enabled `idle`. A sends `StartOffline` and Ember closes, exactly as `controls` did before.

**Controllers screen** (4.2): the pad choice gains `layout`, second after `play`. It opens `button-layout` targeting that pad, focus `test`. Enabled for Recognised, Stick and Steam rows, in rooms too. On GameRead rows (layer active) it is shown disabled with detail `controllers.choice_layout_setup`. Unmapped rows keep opening the wizard directly. Rows of pads with an Ember layout append `"\n"` + `controllers.detail_layout` ({0} = layout name) to their detail.

**Wizard end** (5.9): the saved notice becomes `ShowNotice(pad_setup.saved, pad_setup.saved_heading, alternative = player.button_layout, onAlternative = open button-layout for that pad)`. OK keeps its default label and is selected first. The alternative pushes `button-layout` over the Controllers screen, targeting the just saved pad by identity, focus `test`; Back returns to Controllers.

### 11.6 Screen `button-layout`

Title `button_layout.title`. Drawn with `GameMenu::Draw`, one column, with a `GameMenu::Detail` callback. The shell keeps the target as `layoutPad_` (session pad id plus identity, resolved by identity every frame).

#### 11.6.1 Model

**Function list, press to assign, plus a per-button editor.** The list names what the player wants (Light punch, Throw); Select on a row, then a press of the physical button binds it. Reasons:

- It is the wizard's gesture (5.5) and the fighting-game convention (the game's own screen and SF6 list functions).
- The player never needs a button's printed name, which matters on sticks and hitboxes whose buttons have none.
- The press happens in a capture where no button means Select or Back, so every bindable button, A and B included, can be bound with only the pad being edited in hand.

A pure button list would force a stick player to find "the circle named HP" in a list and would collide with A and B as menu buttons. Mixes beyond the named combinations (HP+HK, LP+MP) go through one extra row, `edit-button`, which opens a per-button editor (11.8).

#### 11.6.2 Rows (editable state)

| # | id | Component | Label | Value | Detail | A / Enter |
|---|---|---|---|---|---|---|
| 1 | `test` | `InfoRow` | `button_layout.test` | none | `button_layout.test_detail` ({0} = pad name, `detailText=Name`) | nothing |
| 2 to 7 | `fn-lp`, `fn-mp`, `fn-hp`, `fn-lk`, `fn-mk`, `fn-hk` | `Row` with `value` | `pad_setup.step.lp` to `pad_setup.step.hk` | button names (below) | `button_layout.row_detail` ({0} = label); if this attack is in `MissingAttacks`, append `"\n\n"` + `button_layout.row_missing` | opens capture for this mask (11.7) |
| 8 | `fn-3p` | as above | `pad_setup.step.3p` | as above | `button_layout.row_detail` | capture, mask 0x430 |
| 9 | `fn-3k` | as above | `pad_setup.step.3k` | as above | as above | capture, mask 0x8C0 |
| 10 | `fn-throw` | as above | `button_layout.throw` | as above | as above | capture, mask 0x50 |
| 11 | `fn-focus` | as above | `button_layout.focus` | as above | as above | capture, mask 0xA0 |
| 12 | `fn-red-focus` | as above | `button_layout.red_focus` | as above | as above | capture, mask 0xB0 (LP+MP+MK) |
| 13 | `edit-button` | `Row` | `button_layout.edit_button` | none | `button_layout.edit_button_detail` | opens capture for a button to edit (11.7), then `button-layout-button` |
| 14 | `preset` | `Row` with `value` and `choices`, `chosen` = current preset id (none when Custom or no layout) | `button_layout.preset` | layout name (11.3) | `button_layout.preset_detail` | opens the choices modal: `default`, `triggers`, `focus-throw`, each with its label and `_detail` key; picking one applies it at once (choices beat confirmation, as everywhere in the shell) and shows status `button_layout.preset_applied` ({0} = label) |
| 15 | `remove` | `ConfirmRow`, shown only when the pad has an Ember layout | `button_layout.remove` | none | `button_layout.remove_detail` | confirmation; yes removes the layout (11.4), focus moves to `preset`, status `button_layout.removed` |

**Value of the `fn-` rows:** the names of the listed buttons whose mask equals the row's mask exactly, joined with ", ", in table order; `button_layout.no_button` when none. A button carrying a different mix (LP+MP) appears in no row; the detail pane shows it. Button names by the target pad's resolved prompt brand (`ResolvePrompts(setting, PadXInput, pad.brand)`, 2.4); printed names, not localised:

| Id | Xbox, Generic | PlayStation | Nintendo | Stick |
|---|---|---|---|---|
| A | A | Cross | B | the wizard input recorded for target `a`, named per 5.6 ("Button 3") |
| B | B | Circle | A | as above, target `b` |
| X | X | Square | Y | target `x` |
| Y | Y | Triangle | X | target `y` |
| LB | LB | L1 | L | target `leftshoulder` |
| RB | RB | R1 | R | target `rightshoulder` |
| LT | LT | L2 | ZL | target `lefttrigger` |
| RT | RT | R2 | ZR | target `righttrigger` |
| LS | LS | L3 | LS | target `leftstick` |
| RS | RS | R3 | RS | target `rightstick` |

Stick names come from the bindings SDL makes for a pad with an Ember setup (`PadView::bindings`, read with `SDL_GetGamepadBindings` each frame) and name their raw inputs with `RawInputName`; they name the physical button and do not change with the layout. A disconnected pad keeps the bindings of its last frame; a frame whose binding read fails names no bound inputs, so that frame shows the default A, B, X and Y names.

**What the gameplay pad's buttons do on this screen** (standard shell rules):

| Row | A / Enter | B / Esc | Left / Right |
|---|---|---|---|
| `test` | nothing | back (11.9) | nothing |
| `fn-*`, `edit-button` | capture | back | nothing |
| `preset` | choices modal | back | nothing |
| `remove` | confirmation | back | nothing |

When the target is not the gameplay controller, the target pad also drives this screen and `button-layout-button`: the shell passes its d-pad (or left stick past 50%) as Up/Down/Left/Right and its A and B as Select and Back through `menu_.additionalInput`, as wizard summary mode does. The gameplay controller and the keyboard navigate as on any screen. The mouse clicks rows as usual.

Legend: the standard one. Glyphs follow the gameplay controller's brand as on every screen.

#### 11.6.3 Detail pane

Under the row's detail text, top to bottom:

1. The target's 64 px controller picture (2.6) and its name (`NoteUserText`).
2. **Layout map.** Three lines of 40 px squares in physical order: X, Y, RB, LB; then A, B, RT, LT; then LS, RS. Buttons the pad lacks are left out (a line with none left is omitted). Each square is `DrawPromptGlyph` with the target's resolved brand and its names (11.11), and under it its short name in 12 px Muted text, or `button_layout.nothing` for 0. A held square draws at full alpha with the 2 px `palette::Ember` underline of 4.2; others at 35%.
3. **Attacks line.** `button_layout.attacks`, then six text pills LP, MP, HP, LK, MK, HK (the `TrainingShortcuts.cxx` pill style). A pill lights (full alpha, `palette::Ember` border) while any held listed button's mask contains that function; otherwise 35%. Holding LB on Default lights LP, MP and HP.

Held state comes from the target's `PadView.held` (triggers past 50%), which now includes LS and RS (11.13 item 3). The map and pills work on every row and in every state where the pad is connected. On the `test` row only Back acts on the menu, so it is the safe place to try A and B; on the other rows A also activates the focused row, as on the Controllers screen.

#### 11.6.4 Other states of the screen

Rebuilt every frame from the snapshot, so the screen moves between them live.

| State | Rows | Pane |
|---|---|---|
| Editable: target connected and served by the layer, `canEditPreferences` | 11.6.2 | 11.6.3 |
| Read-only: `canEditPreferences` false (a room, not at the main menu, a GGPO session) | the same rows, values filled; every row but `test` disabled with detail `settings.leave_room_to_edit`; `remove` hidden | 11.6.3 (the test keeps working) |
| Target disconnected | first `InfoRow` `layout-absent`, label `button_layout.absent`, detail `button_layout.absent_detail` ({0} = name); then rows 2 to 14 disabled with the same detail and the saved values; `test` and `remove` hidden. Focus moves to `layout-absent`. When the same identity reconnects, the editable or read-only rows return with focus on `test`. | picture `controller_disconnected.png`, the name, the map without highlights |
| Target game-read (unmapped, or native-profile stick) | `InfoRow` `layout-setup-first`, label `button_layout.setup_first`, detail `button_layout.setup_first_detail` ({0} = name); then `Row` `setup`, label `controllers.choice_setup`, detail `controllers.choice_setup_detail`, enabled `canEditPreferences` (else detail `settings.leave_room_to_edit`). A on `setup` starts the wizard exactly as the Controllers `setup` choice does. After a save the wizard returns to this screen, now editable, focus `test`, and shows the saved notice without its alternative (5.9). A cancelled or stopped wizard also returns here. | the picture and name only |

Any capture open when the state leaves Editable is cancelled silently (11.7).

### 11.7 Capture (press a button)

Opened by A or Enter (or a click) on an `fn-` row (**assign** capture) or on `edit-button` (**pick** capture).

- **Shell:** `menu_.listEnabled = false`, `menu_.ownsBack = true`, `additionalInput = 0`. The legend follows the wizard's capture convention (5.2): no Select or Back pair; extras `{"Start", common.cancel}` drawn in the target's resolved brand, and `{"Esc", common.cancel}`. Header Back acts as cancel.
- **Rows** (`ButtonLayoutRows` with `LayoutRowMarks`): the capture's row (the `fn-` row for assign, `edit-button` for pick) shows the value `button_layout.waiting` in `palette::Ember` (`valueHighlighted`). Every other row is disabled and `quiet`, so it draws dimmed. The shell has no shared pulse helper, so the value does not pulse.
- **Card** (`GameMenu::listCard`): a card over the list, on the panel the flyout's confirmation uses (`BeginDialogCard`). It is centred on the list, or centred in the larger band above or below the capture's row when it would cover that row. Top to bottom: `button_layout.press_for` ({0} = row label) for assign, or `button_layout.press_to_change` for pick, at 1.4x the body font as in 5.2; the target's name in Muted; a 4 px `palette::Ember` bar that empties over the 10 s timeout (`LayoutCapture::Left`); `button_layout.press_cancel`.
- **Pane** (replaces 11.6.3 while open): picture and name, then the layout map (11.6.3 item 2) so the player sees where things are now. The card carries the prompt and the cancel line.
- **Arming:** capture ignores the target until none of its listed buttons, directions, View or Start is held (so the A that opened it does nothing). Opened with Enter or a click while nothing is held, it is armed at once.
- **Taking the press:** the first listed button of the target to go down after arming (trigger past 50%) is the button P, taken on press. Other pads and the keyboard never bind. After a press the screen ignores the target's menu input until all its buttons are released (`NeutralGate`), so a press of A or B never also selects or goes back.
- **Cancel:** Start or any direction on the target (d-pad or left stick past 50%), Esc, header Back, or 10 s with no press. Nothing changes and no message is shown. Also cancelled silently when the screen leaves the Editable state.
- **Assign result** (`Assign(layout, P, mask)`): let `old` be P's mask. If `old == mask`, nothing changes. Otherwise every other listed button whose mask equals `mask` exactly gets `old` (a swap), and P gets `mask`. Buttons whose mask merely contains a function of `mask` are untouched (assigning LP to Y leaves 3P on LB as it is). Nothing is ever lost: the attack P had moves to the button the new one came from. The change is applied and saved (11.4). Status, `Tone::Success`, 4 s:
  - no other button had it: `button_layout.assigned` ({0} = row label, {1} = P's name);
  - a swap, `old` non-zero: `button_layout.swapped` ({0} row label, {1} P's name, {2} the other button's name, {3} `FightName(old)`); with several other buttons, {2} is the first in table order;
  - a swap, `old` zero: `button_layout.swapped_nothing` ({0}, {1}, {2} as above).

  Focus stays on the row. Whenever a press is taken, changed or not, the row's value shows in `palette::Ember` for 600 ms (`LayoutRowMarks::flash`), so the row flashes with its new button.
- **Pick result:** push `button-layout-button` for P, focus `bit-lp`.

Decision on duplicates: assigning by press moves (swaps) so one press can never leave an attack without a button. The same function on several buttons is allowed, but only on purpose, through the per-button editor or a preset (`focus-throw` has MP on Y and in Focus).

### 11.8 Screen `button-layout-button`

Title `button_layout.button_title`. Pushed by pick capture; Back returns to `button-layout` with focus on `edit-button`. Same target, same states (11.6.4): if the target disconnects or loses editing, this screen returns to `button-layout` at once.

| # | id | Component | Label | Value | Detail | Enabled |
|---|---|---|---|---|---|---|
| 1 to 6 | `bit-lp`, `bit-mp`, `bit-hp`, `bit-lk`, `bit-mk`, `bit-hk` | `Value` | `pad_setup.step.lp` to `pad_setup.step.hk` | `common.on` / `common.off` | `button_layout.bit_detail` ({0} = label) | `canEditPreferences` |
| 7 | `clear` | `Row` | `button_layout.clear` | none | `button_layout.clear_detail` | `canEditPreferences` and the mask is not 0 |

Left/Right on a `bit-` row turns that function on or off for P (`SetMask`), applied and saved at once (11.4); A does nothing there, as on every `Value` row. A on `clear` sets P's mask to 0 and moves focus to `bit-lp`. There is no swap on this screen. No status line beyond Saving and Saved.

Pane: P's 64 px glyph (`DrawPromptGlyph`, target brand and names), P's printed name (11.6.2 table), `button_layout.button_does` ({0} = `FightName` of P's mask, or `button_layout.nothing`), then the attacks line of 11.6.3 item 3.

### 11.9 Leaving with an attack on no button

`MissingAttacks(layout, listed buttons)` returns the six single attacks, in order LP, MP, HP, LK, MK, HK, for which no listed button has exactly that one function. Such a layout is allowed and saved like any other.

`button-layout` sets `menu_.ownsBack = true`. On Back (pad B, Esc, header Back): if the layout was changed since the screen was pushed (any assign, editor change or preset; `remove` resets the flag) and `MissingAttacks` is not empty, show `ShowNotice(text = button_layout.missing_text with {0} = the long names joined with ", ", heading = button_layout.missing_heading, alternative = button_layout.leave, onAlternative = return to the parent screen, okLabel = button_layout.keep_editing)`. OK (Keep editing) is selected first and closes the notice, staying on the screen. Otherwise Back returns as usual. Returning from `button-layout-button` never shows the notice; leaving `button-layout` does.

The `fn-` rows of missing attacks also carry `button_layout.row_missing` in their detail while the screen is open.

### 11.10 Rooms, matches and edge states

| Situation | Behaviour |
|---|---|
| In a room, between games | Player `controls` and the `layout` choice still open the screen, read-only (11.6.4). The layout in force is shown and can be tested, not changed. |
| During any fight (online, offline, Training, replay) | Ember is not open over fights. The layout in force when the fight starts applies for its whole length. |
| The pad comes back in another slot mid-match (3.5) | The layer writes its layout into the new slot at once (11.4) and logs it. Nothing is shown. |
| Local versus with two pads | Each slot gets its own pad's layout. |
| A pad with a layout is placed in a slot | Applied silently; logged. |
| The target disconnects while the screen is open | 11.6.4; an open capture is cancelled. Changes already made are saved. |
| The target is the gameplay controller and disconnects | As above, and the shell status shows `controller.disconnected` as today (3.5). |
| A layout is stored for a controller that is not connected | Kept; applied when it connects; logged at layer start in the count line. |
| The wizard saves a mapping for the target while the screen is open | The set-up-first state turns editable on the next frame. |
| Let the game read controllers on, or SDL failed | Layouts stay stored and are never applied. Player `controls` is disabled (11.5); the game's own screen applies. |
| Steam Input remaps a Steam pad | Steam's mapping applies first, the Ember layout second. |

### 11.11 Stick names follow the layout

For the Stick brand, the name drawn on a glyph's generic circle (2.3) is no longer a fixed table. It is `FightName` of the mask in force for that button on that pad:

- a pad in a slot: the ten entries of its slot's fight table as the snapshot read them this frame (so a pad without an Ember layout shows whatever the game has, usually the 2.3 defaults);
- a pad without a slot: its Ember layout, else the defaults.

An empty mask gives no name: the circle is drawn without text and `MenuLegend` prints the label alone. View and Start keep `Select` and `Start`.

API shape: `BrandGlyph(id, brand, const FightNames* names = nullptr)`, where `FightNames` holds the ten names in table order; null keeps the 2.3 defaults. `SetMenuPromptBrand` gains the gameplay controller's names and `MenuPromptNames()` returns them; `MenuLegend`, the room hints, replay HUD hints, challenger banner and training strip use those. The Controllers test (4.2), the layout map and the capture pane pass the drawn pad's own names. Like a prompt brand change, an unsaved edit shows at once, because the edit is already written to the table.

Ember's menus still read physical A and B (unless Menu confirm and Menu back are bound, 11.14), so on a Stick pad whose A now does HP the legend reads "HP Select" and that button selects.

### 11.12 Log lines

At `spdlog::info` unless marked, prefix `Input:`, conventions of section 7. `{layout}` prints the ten masks as `A=LK B=MK X=LP Y=MP LB=3P RB=HP LT=3K RT=HK LS=- RS=-` (short names, `-` for 0) followed by the preset id or `custom`.

| When | Line |
|---|---|
| Layer start | `Input: {n} button layouts loaded, {m} for controllers not connected` |
| Layer start, opt-out or SDL failed | `Input: {n} button layouts kept, not applied (the game reads controllers)` |
| Stored entry rejected | `Input: button layout {index} rejected: {wrong length / mask 0x{hex} outside 0xCF0}` (warn) |
| Layout applied on placement | `Input: layout for '{name}' id {id} applied to slot {n}: {layout}` |
| Game table copied | `Input: slot {n} game table saved before the first Ember layout` |
| Game table restored | `Input: slot {n} back to the game's table ('{name}' id {id} has no layout)` / `(layout removed)` |
| Pad moved slot | `Input: '{name}' id {id} moved from slot {old} to slot {new}; layout written to slot {new}{, during a match}` |
| Assign | `Input: layout for id {id}: {button} = {short} ({row id})` and, on a swap, `Input: layout for id {id}: {button} = {short} (swap)` |
| Editor change | `Input: layout for id {id}: {button} = {short} (by hand)` |
| Preset | `Input: layout for id {id} set to preset {preset id}` |
| Removed | `Input: layout for id {id} removed` |
| Capture | `Input: layout capture for id {id} {assign {row id} / pick} {took {button} / cancelled ({start/direction/escape/back/timeout/state})}` |
| Target lost mid-edit | `Input: layout editor: '{name}' id {id} disconnected; editing paused` |
| Guid fallback match | `Input: layout for guid {guid} matched '{name}' id {id} by guid only; path updated` |
| Cap reached | `Input: layout for id {id} dropped, oldest of 33` |
| Leave warning | `Input: layout for id {id} left with no button for {LP, HK}` |
| Game button config loaded (11.4) | `Input: game button config loaded; Ember layouts reapplied` (once per load) |

### 11.13 Limits and engineering notes

1. **The game's own screen and Ember layouts share memory.** The game's screen edits the same slot tables. Decisions 6 now sets Ember layouts aside while that screen is open. The game's config loader, run on each visit to the title screen, also gets the game's own tables, and what it leaves becomes each owned slot's `gameCopy` before the layouts are written back (11.4); `player.game_buttons_detail` says Ember layouts win. When the game saves its config, the tables it saves include any Ember layouts in the slots, so a pad without a layout can later inherit one by slot position.2. **Record to slot mapping.** The writer addresses the XInput record of slot n as in the 2026-10-08 IDA notes (code notes 11 and 13). Confirm which record index each XInput slot uses before implementing; the spec assumes one fixed record per slot.
3. `HeldGlyphs` (`ControllerView.hxx`) gains `LS` (`Button::LeftStick`) and `RS` (`Button::RightStick`). `PadView` gains `has` (ten bools, 11.2), `table` (the ten masks in force, 11.11), `layout` (the stored masks or none) and `buttonNames` (the ten printed names, 11.6.2).
4. Writes go through the game thread only; the overlay thread only submits requests. A write is ten aligned dword stores and the game re-reads the table on each update, so no lock with the game is needed beyond writing before the update in the same pass.
5. Code notes 13 (layouts by slot position) still describes the game's own layouts; for pads with an Ember layout 3.1 rule 1 is no longer what keeps a layout, though the rule stays for pads without one.

### 11.14 Menu confirm and back

Ember's menus confirm and go back on the south and east buttons of an XInput pad, and on the buttons the game's own menus use on a DirectInput stick, unless the player binds others on the Button layout screen. One resolver answers "which physical buttons confirm and go back for this device" for every Ember menu, its legend and its prompt glyphs: `input::ResolveMenuButtons` (`src/common/MenuButtons.hxx`), asked by `MenuButtonsFor` in the runtime. A DirectInput stick goes through `Dimps::Pad::MenuButtons` on the game thread, with its tables and no binding; an XInput pad passes no tables, and a served one its binding. Ember never writes the game's menu table, so the game's own menus are unchanged.

**What the game does (IDA, `SSFIV.exe` Steam build, read only).**

- Each device's binding record (System+0xD4, stride 0x27C) holds two pad tables of 32 dwords, indexed by physical bit, each entry the game functions that bit triggers: the fight table at +0x17C and the menu table at +0x1FC. The mapper `0x511E70` reads +0x17C when System+0x08 is 0 (fight mapping) and +0x1FC otherwise; the all-device reads the menus use (`0x511640`, `0x5117C0`) always read +0x1FC.
- Defaults come from `System::Initialize` (`0x517900`): XInput fight `0xA5D078` and menu `0xA5D0F8`; 14 known sticks by GUID (fight/menu pairs `0xA5BEB8`..`0xA5CC38`); any other DirectInput pad fight `0xA5D178` and menu `0xA5D1F8`. The `.ctrlsav` loader (`0x51BC30`, applied by `0x516540`) writes both tables.
- The game's button screen writes the menu table in step with the fight table: record method `0x510B00` stores the row's fight function at +0x17C[bit] and its menu function at +0x1FC[bit] (row tables `0x942808`, `0x942858`). Whichever button is LK in fights is LK in its menus, and the same for MK.
- Confirm and cancel are function masks on `Pad::System`: +0xCC = 0x40 (LK) and +0xD0 = 0x80 (MK), set by the constructor `0x517000` and never changed in this build (`0x6503F0` can handle a swap, but nothing sets one). The Scaleform translator `0x49A6B0` sends DECIDE and CANCEL from them; Start and Back are separate START and SELECT tokens. The prompt lookup `0x519D90` finds the physical button whose entry equals the function exactly.
- So with the defaults the game confirms on A and goes back on B on an XInput pad, and on buttons 3 and 2 of a generic stick (bits 18 and 17); every profiled stick confirms on its own LK button and goes back on its MK button.

**Resolution, for each of confirm and back on its own:**

1. The player's binding (`MenuBinding` in the pad's `buttonLayouts` entry, below).
2. The lowest physical bit from 8 up (bits 0 to 7 are directions) whose menu-table entry equals System+0xCC (confirm) or +0xD0 (back). A zero or unreadable mask counts as 0x40 or 0x80.
3. The bit holding the same function in the record's fight table.
4. Bits 18 and 17, when the record fails its bounds check or neither table has the function.

An XInput pad skips steps 2 and 3 (`MenuButtonsFor` passes no tables), so it confirms on A and goes back on B unless bound. The game's own menus still follow their table.

When a binding lands on the other's default button (the player bound back to A, say), the other takes this one's default, so the two never share one. Glyphs: on an XInput pad the physical button's glyph id (A, B, X, ..., View, Start), drawn in the pad's brand; on a DirectInput stick the function's fight notation (LK, MK), as the 2.1 table's generic circle. Every consumer reads this one answer: `ControllerButtons` (Select and Back from the physical bits; X, Y and View stay room shortcuts unless one of them confirms or goes back, and then its hint is dropped from the legend), the overlay's per-frame `SetMenuGlyphs`, the Button layout screen's target pad, and the shell after the wizard closes.

**Binding.** The Button layout screen (11.6) lists `menu-confirm` (`button_layout.menu_confirm`) and `menu-back` (`button_layout.menu_back`) after the attack rows. Their value is the pad's name for the resolved button; Select starts the same capture as an attack row (11.7, card `button_layout.press_for`) and the press binds that button for this controller. Only the ten layout buttons can be bound, so Start, View and the directions keep their jobs. The button the other row uses now is refused with `button_layout.menu_taken` in the error tone and nothing is sent; a success shows `button_layout.assigned`. The binding is stored in the pad's layout entry as `menuConfirm` and `menuBack` (indexes into the 11.3 table, -1 for A and B), sent with `Kind::Layout`, kept by presets and edits, and cleared with the entry by `remove`. A first binding on a pad without a layout creates its entry from the starting point (11.3). On load an out-of-range or equal pair falls back to -1 for both; the layout still loads. Only served pads have layouts, so a game-read DirectInput stick always follows the game.

**The wizard's summary is separate.** In summary mode (5.8) the runtime's snapshot carries `SetupMenuDraft`: the newly recorded Up, Down, LK and MK. The summary's Select and Back come from those inputs, and its legend names them with their 5.6 names; it never asks the resolver. After Save, ordinary menus use the resolved buttons of the installed pad.

**Launcher.** The launcher's recovery window, which runs before the game, keeps buttons 1 and 2 (`RecoveryController.cxx`) and its prompts print those numbers. It has no binding record to read, so it does not follow this section.

---

## Code notes

Where the code differs from the brief, or where an implementer must take care.

1. `sf4e__OverlayPrefs.cxx:111`: `Clamp` discards any saved device that is not the keyboard or an XInput slot 0 to 3, so a DirectInput pad can never be the saved controller, and an XInput save is a slot number, not a pad. The identity fields replace this (3.2), including the migration.
2. `sf4e__NetplayRuntime__Input.cxx:31`: restore matches `config.deviceType`/`deviceIdx` by slot, and `input::Device::Same` (`InputAssignment.hxx:15`) compares type, index and name. With SDL-served pads, `Same` must compare identity for slots 0 to 3, or a pad moving slot is treated as a new device.
3. `sf4e__NetplayRuntime__Status.cxx:466`: `canChangeController` is false in any room, so the controller cannot change in a room. Kept as is; the room copy points to leaving the room.
4. `sf4e__NetplayRuntime__Status.cxx:451` and `en.po` `runtime.match_input_blocked`: the warning exists only for GGPO matches; offline battles and Training show nothing from Ember (kept). Its current text says "If its slot changed, return to the room to reassign it", which slot reservation makes wrong; replaced in section 8.
5. `GameMenu.cxx`: room shortcut hints show for an XInput pad (or the keyboard), less any hint whose button the player made Select or Back (11.14). Keeping glyph ids positional (2.3) makes them appear for PlayStation and Nintendo with no change there.
6. `MenuGlyphs.hxx` `MenuLegend` `prefix()`: prints a glyph's name when its art is the generic circle. Stick ids rely on this.
7. `sf4e__ReplayPlayback.cxx:219`, `TrainingShortcuts.cxx:51`, `sf4e__Pad.cxx:127` (Go now) and `sf4e__Overlay.cxx:541` (`padBehind`) all test `PadXInput`. SDL-served pads pass these tests with no change. `TrainingShortcuts.cxx:53` hard-codes "Back"; it needs the brand name table 2.5. `TrainingInRoom.hxx:51` `GoNowGlyph` returns `"View"`, which the brand art then covers.
8. `sf4e__Overlay.cxx:541`: background play for pads needs XInput answers while the game is unfocused. The layer must keep answering from SDL regardless of focus (set `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` to 1) and leave the existing focus gating to Ember's code as it is today.
9. The brief cites `src/ui/ReplayPlayback.cxx ~219`; the file is `src/sf4e/sf4e__ReplayPlayback.cxx:219`, and the Xbox-only button reads are in `src/common/ReplayTransport.hxx:235`.
10. The brief cites `RecoverySurface.cxx ~150` as an in-game DirectInput case. It is the launcher's recovery window (`RecoveryController.hxx:10`), which runs before the game and is outside the layer (section 9).
11. Native validator (from the 2026-10-08 IDA notes, `0x4CBA40`): when any XInput pad is connected, the game saves its button config only if every XInput record 0 to 3 has all nine functions on physical buttons. Now that every recognised pad is XInput to the game, this applies to all of them; the Game's button settings detail says so. Not verified in this checkout (the IDA server was unreachable). Ember layouts with a missing attack can trip it for every player of the PC (see 11.13).
12. Default XInput fight layout (`0xA5D078`): the 5.3 SDL targets assume X LP, Y MP, RB HP, LB 3P, A LK, B MK, RT HK, LT 3K. Not verified here; confirm before implementing.
13. The game applies saved XInput layouts to slots by position (`.ctrlsav`, loader `0x51BC30`), which is why 3.1 rule 1 puts the saved controller in slot 1. Pads with an Ember layout carry it to any slot instead (see 11.4).
14. After a wizard save, the game must stop seeing the pad's DirectInput copy and see a new XInput pad without a restart. The game re-enumerates DirectInput on device change (observed when Steam Input toggled). The layer must trigger the same re-enumeration; if it cannot, `pad_setup.saved` must add "Restart the game to use it." and checklist item 29 changes accordingly.
15. Sticks with a native game profile (14 DirectInput GUIDs matched at `0x517900`): when SDL's database knows them they become XInput pads, and the game's native stick profile no longer applies to them. Their buttons follow SDL's mapping instead.
16. `ApplicationShell.cxx:938`: during capture the legend is forced to keyboard glyphs. The wizard sets its own legend (5.2) and must restore the gameplay controller's glyphs when it closes.
17. `ShellView` carries none of the layer's state today. Add these, filled by the runtime snapshot, so the three screens share one shape:
    - `padLayer`: Active, GameReads (opt-out), Failed; plus `padLayerPending` (saved opt-out differs from running).
    - `pads`: per pad `{id, name, brand, artVariant (PS3/PS4/PS5, Joy-Con), picture, slot (-1 none), state (Recognised, Steam, Unmapped, NotInUse, Absent, GameRead), gameplay, customMapping, held (the 2.6 glyph ids held now), raw (held raw inputs for unmapped)}`, plus the button layout fields of 11.13 item 3 (see 11).
    - `promptBrand`: the resolved brand for prompts (2.4).
    - `padSetup`: `{active, padId, step, mode (Capture, Summary), recorded[20], holdMs, feedback (None, Conflict, Required, Idle, Reconnect), reconnectSeconds}`.
    - `padSetupPrompt`: the pad id to prompt for, or none.
    - Pad hints (3.4): `padHint` text and its expiry, so `OverlayLayersView` and the shell status read the same message.

---

## Decisions after review (2026-10-12)

These override anything above that disagrees.

1. **Default fight layout verified.** IDA, table `0xA5D078` (one dword of function bits per physical XInput button): X = 0x10 (LP), Y = 0x20 (MP), RB = 0x400 (HP), LB = 0x430 (3P), A = 0x40 (LK), B = 0x80 (MK), RT = 0x800 (HK), LT = 0x8C0 (3K), View = 0x2000, Start = 0x1000. Section 5.3 is correct as written. Code notes 12 is closed.
2. **Sticks with a native game profile stay with the game** (Kate). The layer never serves these VID:PIDs (Steam's virtual pad is XInput, so it is served whatever VID:PID SDL reports for the pad behind it). They stay DirectInput pads, keep the game's own profile and any layout saved for them, and are listed as game-read. The player can still choose `setup` on one, and once a wizard mapping exists for its GUID, it becomes a recognised pad like any other. The list comes from the guidProduct strings at `0x517900`, written as VID:PID:
   - 0079:0011
   - 05FD:262A
   - 046D:C20C
   - 0E8F:0003
   - 0E8F:3013
   - 0F0D:0032
   - 0F0D:0022
   - 0738:8838 (Madcatz FightStick TE)
   - 1A34:0802 (SpeedLink 360 Style Gamepad)
   - 0E8F:0012
   - 0079:0006
   - 046D:C216
   - 0079:0002
   - 0F0D:0037 (HORI Fighting Edge)

   Code notes 15 is replaced by this rule.
3. **No restart after setup (code notes 14).** The DirectInput wrapper from package P2 also wraps `CreateDevice`. Once SDL serves a device, its DirectInput device answers `GetDeviceState` and `GetDeviceData` with a neutral state, so the game stops reading the copy at once. Hiding it from `EnumDevices` covers later enumerations. The new XInput pad appears in its slot through the XInput answers, which the game polls every frame. `pad_setup.saved` keeps its text unless an in-game test shows the game does not pick up the new XInput slot.
4. **Steam rule (section 6) stands.** Ember serves the Steam virtual pad and does not slot the physical twin.

## Decisions after review, section 11 (2026-10-12)

These override section 11 where they differ.

5. **Button layout is editable in a room** (Kate). This applies between games only, since Ember is never open over a fight. Override `canEditPreferences` for the `button-layout` screens, its pad choice `layout`, and the Player `controls` row. Every other setting stays locked in a room. A layout only changes local input, so netplay is unaffected.
6. **The game's own button screen is detected** (IDA, closes 11.13 item 1). `Dimps::GameEvents::OptionUIController` has its constructor at `0x4CC280` and its destructor at `0x4CC0D0` (vtable `0x9429AC`).
   - The constructor copies every live binding record (system+0xD4) into the edit copy (system+0xE4).
   - The destructor runs the validator `0x4CBA40(0)`. If it passes, it copies the edit copy back to +0xD4 and saves through System vtable +0xC8 (the `.ctrlsav` writer `0x51A1E0`).

   Detour both:
   - Before the original constructor, write `gameCopy` into every slot that holds an Ember layout, so the screen shows and edits the game's own layout.
   - After the original destructor, read those slots back as the new `gameCopy`, then write the Ember layouts again.

   The game therefore never saves an Ember layout under a slot position, and an Ember layout can never make the validator reject a keyboard change. Log `Input: game button screen open; Ember layouts set aside` and `Input: game button screen closed; Ember layouts restored`.
7. **Record per slot confirmed** (closes 11.13 item 2). The binding record for XInput slot n is record n (begin + n * 0x27C). Both `Pad::ReadController` and the mapper `0x511E70` index records by device index, and XInput slots are device indices 0 to 3.
