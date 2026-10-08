# Training lab

Ember adds an offline training overlay. Its frame meter shows native fighter states, action IDs, animation frames, observed action durations, startup and signed recovery advantage. Separate startup/active/recovery coloring remains unavailable, so the complete attack stays orange. New timing behavior still needs gameplay correlation.

## Getting there

**Training** on Ember's Home sends the game straight into Training mode, to the fighter select, without the main menu's Fight Request question. It is offered outside a room and a match, like **Play offline**, and rides on the same command: the game's own Training selection is made for the player with that question switched off for the one call, which is the path the game takes by itself where fight requests cannot be made. Choosing Training in the game's own menu still asks.

## Open the lab

Use the game's main menu to enter **Training** and select both fighters and a stage. The meter appears at the bottom of the screen once the fight is ready. The viewer and its F6 controls exist only inside native offline Training. There is no Training page in the main Ember overlay.

| Control | Action |
| --- | --- |
| F5 | Show/hide the passive meter |
| F6 | Open/close training controls |
| Start | SF4's native pause only; no Ember binding |
| Arrow keys / Enter | Navigate / select within training controls |
| Escape | Cancel confirmation, return one menu level, or close at the root |
| F7 | Start/stop recording P1's controls onto P2. If the selected slot already holds a recording, F7 opens Dummy Recording with the overwrite question on Record, answered Cancel until you choose Record |
| F8 | Start/stop playback of the selected P2 slot |
| F2 | Reset position: back to the saved state |
| F11 | Save position |
| Select (pad) | Tap to reset the position, hold half a second to save it |

F2 and F11 are the **Reset position** and **Save position** rows of the Position and dummy screen and work with the controls closed or open, except while a text field is being typed in. The two **key** rows at the end of that screen move each to F1 to F4, F9 or F11, or turn it off; one key does one thing. The HUD's chips name both keys and, for a few seconds, what the last one did.

The passive meter does not capture gameplay input. Training controls use keyboard and mouse only: F6 opens/closes, arrows navigate, Enter selects, and Escape goes back. There is no controller opening shortcut or controller navigation in the flyout. Start retains native pause behavior while Ember is closed. While the flyout is open, all gameplay input is captured, including controllers; closing inputs must release before returning to gameplay. F6 does not request native pause. Recording suspends while the controls are open; playback keeps running under them. Both stop if the game loses focus. F5 through F8 are reserved while in training.

The passive HUD is modelled on Street Fighter 6's frame meter and dressed like this game's own panels: a data line over two slanted meter bars that span its width, under an orange trim. Its prompt shows F6 for controls and F5 to hide the HUD. At 720p it is 900 pixels wide, bounded by 75% of viewport width; its size follows viewport height instead of the main menu's DPI scale. It sits just above the game's super meters, with its bottom edge at 82% of the viewport height, so it no longer covers them.

The controls are a flyout targeting 820 by 600 logical pixels, bounded by 80% of each viewport dimension. It opens at the top centre and can be dragged anywhere inside the viewport, out of the way of a playback. The game stays visible around it: there is no full-screen artwork or dimming. Narrow layouts place details above a scrolling list; headings, command feedback and the button legend remain fixed. Confirmations stay inside the panel with Cancel selected first. Outside clicks neither close the flyout nor pass through into gameplay. Successful Record and Play commands close Ember; close native pause separately if it remains open. Rejected commands remain visible with an explanation.

## Read the frame meter

The data line gives each fighter's last move: **Startup** (the frame it first hits on), **Active** and **Recovery** in frames, hitstop left out, so the move's total is startup - 1 + active + recovery. After them comes what the move met and what it was worth: **Hit** or **Block** with the frame advantage, green when ahead and red when behind, or **Whiff** when it touched nothing, which has no advantage to give. A cancel into another attack counts as a new move. Where the HUD is too narrow for the words, a bar in each reading's colour stands for its label.

Each bar holds up to 120 simulated frames for one fighter, the upper one Player 1's, one cell a frame, with a tick under every tenth. The colours are Street Fighter 6's: green startup, red active, blue recovery, split at the attack boundary of the move's script, counted the way the game's frame data counts: a 3-frame jab has three green cells and is active from the fourth; orange being hit and yellow blocking; an attack whose script gives no boundary stays ember orange, a knockdown purple while the fighter bounces or lies down and light purple while it gets up, a throw or cinematic that holds both fighters beige, movement grey-blue. What the thrower still has to play once the thrown fighter is let go counts and shows as its recovery. Every run of one state shows its length in frames on the bar when it is wide enough. While a fighter is being hit, each hit of the combo is a stretch of its own, parted from the one before by a dark line and counted by itself. Beside each bar runs a lane of what that player pressed, Player 1's above their bar and Player 2's below theirs: a direction as it changes, a button as it goes down, each at the frame it did. Presses too close together to draw apart are moved to the right, in order. A change of native action ID starts a new count. A multi-hit move's gaps between hits read as active. The bars fill from the left and start again there once both fighters have been idle for half a second. The line below is the legend and the meter's state, or says what a pending reading is waiting for; there is no separate frame-inspection page. The flyout contains Dummy Recording, Input History and Close training controls. Save/restore position and its F9 shortcut have been removed following a runtime failure report.

| Color | Native state group |
| --- | --- |
| Gray | Standing/crouching neutral |
| Cyan | Movement, jumping, dashing and posture transitions |
| Orange | Attack (`AS_SKILL`), all phases combined |
| Blue | Guard posture or guard damage |
| Red | Damage, blowback or stun |
| Purple | Bound or down |
| Light purple | Rise (wakeup) |
| Bright pink | The frame a fighter can first be hit after a knockdown, and on the other's bar the active frames of a meaty attack that had already passed by then. Each of those is a frame of advantage gained: an attack that is +2 on block with 3 pink frames is +5. From the frame the attack meets the fighter on it is red as any active frame; the Meaty reading is the count of pink frames |
| Beige | Throw or cinematic sequence holding both fighters |

Guard posture alone is not proof of blockstun. The native animation frame can pause or change rate while simulation frames continue; the passive bar is not a move-table recovery or total-duration lookup.

By default, the timeline holds the last exchange after 30 neutral frames and resumes on activity. Clear input history also clears the meter. Native time discontinuities clear the timeline. No states are inferred for missing frames or fighters.

### Signed frame advantage

Both rows display reciprocal numbers after a supported grounded hit/block exchange: P1 `+5 f` / P2 `-5 f` means P1 recovered five simulated frames earlier. Positive is green, negative is red, and equal recovery reads `+0 f`. Let both fighters recover before starting another attack.

Measurement starts from an observed attack and a native damage/guard-damage contact. It follows action changes through target combos, special cancels, airborne phases and landing until grounded recovery, requiring positive unit time scale and no native basic-action inhibit. Each further contact resets the defender's recovery boundary. The attacker's recovery timestamp survives delayed projectile contact. After knockdowns, the HUD says `wakeup` because the comparison includes the defender getting up. This is a measured exchange recovery comparison, not a move-table lookup or a universal cancel/actionability predicate.

`--` means no completed supported exchange. Whiffs, guard posture without contact, trades/interruption, missing actors and timeline gaps do not produce numbers. Slow/frozen phases retain the exchange; values count accepted simulation steps. A new attack after a completed exchange clears the previous result; an action change within an unfinished chain continues measurement. Pending exchanges expire after 600 frames without a new action/contact. Reset and battle exit clear all values. Attribution assumes a two-fighter exchange; reflected projectiles and unusual scripted interactions are not independently verified.

Point the mouse at `Start --` on the passive meter to see why startup is unavailable, or at the `FRAME ADVANTAGE` line below the bars to see why advantage is unavailable. Startup and advantage are independent: a whiff can retain authored startup while correctly leaving advantage unavailable.

### Startup

Each player row shows `Start N f`. It counts observed advancing frames from the action's start to its native BAC script's first attack boundary. This handles animation speed changes; repeated frozen animation frames do not increase startup. It does not wait for the opponent to be hit, so projectile travel distance does not inflate it. Target-combo follow-ups update startup; recovery-only actions retain the preceding value. A multi-phase move without an initial attack boundary keeps counting into the next phase. A projectile move whose script names no attack frames (Evil Ryu's light Hadoken) takes them from the fighter's script file instead: the frame the move spawns a projectile that can hit. That frame shows as the one active cell, the rest as recovery; the projectile's own flight is not on the meter, and the count may be a frame late. `Start --` means the script has no usable boundary or observation began mid-move. This uses authored attack timing, not live collision-box activation, and needs native off-by-one and character-specific validation. The last startup remains visible after recovery.

### In an online match

**Settings > Frame meter in matches** (off by default) draws the meter over your own online matches and the ones you watch. Only you see it. It reads the game and changes nothing in it. It shows a frame once the other player's inputs for it have arrived, so it trails the fight by the frames still predicted and never shows one that is later played differently. The controls, recordings and hotkeys of Training are not there. A projectile move without attack frames in its script shows no startup in a match.

### Training with another player

A room's host can turn **Training** on in a table's rules (Table options, with the round count and the round time). At that table both fighters' health, Super and Ultra gauges fill again about a second after they are left alone, and nobody is knocked out. Both players and every spectator get the rule from the table; nothing has to be set on their side.

A game under the rule ends when the round time runs out or a fighter leaves the seat, so set the round time to 9999. Either player can save where both fighters stand and put both back there: the keys chosen in Training for **Save position** and **Reset position** (F11 and F2 unless changed), or the pad's Select, held half a second to save and tapped to reset. It happens for both players and every spectator on the same frame, and both are told who did it. Until somebody saves, a reset goes back to the start of the round. A line under the frame meter names the keys.

There is no dummy and no recording at such a table: the other player is the dummy. Turn **Frame meter in matches** on to see the frames.

### Opt-in frame-meter capture

Maintainers can set `SF4E_TRAINING_CAPTURE=1` before launching Ember. Offline Training then buffers accepted observations and writes `training-samples-<process>.csv` under `%APPDATA%\sf4e\logs` on a background thread. Rows include the simulation frame, fighter side, status, action ID/frame, posture, time scale, action inhibit, BAC attack start/end with provenance, startup/result state and unavailable reasons. Queue saturation drops capture rows rather than waiting inside simulation. Record the selected fighter editions and the `SSFIV.exe` SHA-256 alongside the CSV; the current native sample does not expose that selection metadata safely.

For Waldo's report, capture both sides and each light/medium/heavy/EX Ryu or Ken DP and Guile Flash Kick on whiff, hit and block, then permitted forward/back FADC. Repeat representative Poison and Adon specials as comparisons. Native player acceptance remains required even when the synthetic `FrameMeter::Observe` regressions pass.

## Record a dummy sequence

Set the native training dummy to **Player/controller control** first; CPU and native playback can supersede controller input. Select one of eight slots, close the controls, and press F7. P1's controller operates P2 while P1 stays neutral. Press F7 again to stop, then F8 to replay. Each slot holds at most 7,200 accepted simulation frames (120 seconds at 60 fps). Playback can loop. Directions are absolute, so switching sides does not mirror a recording. Recordings are local to the current battle.

Input history shows controller inputs, newest first, with how many simulated frames each input was held. It does not identify CPU-generated moves or measure startup.

## Position and dummy

Open the training controls (F6) and select **Position and dummy**. Its settings and the two hotkeys are kept in `training.json` beside Ember's settings; dummy recordings saved from the Dummy Recording screen go to `recordings\` next to it.

**Save position** keeps both fighters' place, health, meters and dummy state for this battle. **Reset position** restores the saved state. On a pad, a tap of Select resets and a hold of half a second saves.

The dummy rows (dummy action, guard, counter hit, quick stand, super and revenge gauge) show the game's own Training menu settings and change them at once.

### Dummy reply

**Dummy reply** makes the dummy act by itself once it is free again: after being hit (a dropped combo), after blocking, on wake-up, or any of them. None starts while a playback or a recording runs.

**Reply moves** is the reply typed in numpad notation (`623HP`, `44`, `2LK > 2LP xx 623HP`), read for the side the dummy faces. Move names are not read, since the reply belongs to whoever the dummy is. While the line is empty the dummy plays the **Reply slot** instead, a dummy recording, once from its first pressed frame.

Moves are separated by `>` or `,`; `xx` before a move means it cancels the one before it, `~` that it is a follow-up pressed about ten frames into it with no hit to wait for (`@N` moves it), otherwise it links. Notation is numpad or prefix style, any case:

```
[xx|~] [j.|cr.|st.|cl.|far.] [motion] [buttons] [(mash)] [#N] [@N]
[xx] FADC[66|44] [#N] [@N]
[xx] RFADC[66|44] [#N] [@N]
```

- motion: numpad digits, `[4]6` for charge, `360`, `720`. `cr.` is 2; `st.`, `cl.` and `far.` are 5.
- buttons: `LP MP HP LK MK HK` joined by `+`, or `P PP PPP K KK KKK`. `[HP]` holds, `]HP[` releases, `(mash)` presses five times: `P(mash)`, `K(mash)` or several buttons cycle one per frame (a piano, five presses in five frames), a single button goes on and off (five in ten). `(mash HP-MP-LP-HP-MP)` sets the order and the count, one press per frame, up to 10; a digit before a button gives that press its own direction, `5K(mash 1MK-1MK-1MK-1MK)`.
- `@N`: the move's own timing offset, -120 to +120 frames. `#N`: the frame of the reply the press lands on, 0 to 7200.
- `FADC` is the focus cancel (MP+MK, then the dash at once). `RFADC` is the red focus: LP+MP+MK tapped and let go so the red focus attack comes out and lands, then the dash on that hit.

Directions take a few frames each and a button two; between moves the input waits on the fight itself: a link waits for the dummy to be free again, a cancel for the hit to land. The game decides what comes out.

The reply is timed to the stun. The game does not say how long a stun lasts, so each one is measured the first time it is seen, from the last hit or change of reaction to the free frame, per attacking move and reaction. The first time, the reply starts as the stun ends; from then on its motion goes in while the dummy is still held and its first attack button lands on the first free frame. **Reply timing** moves that frame by up to five either way. A counter hit's longer stun replaces the plain hit's until the plain hit is seen again. Hit again before it is free, the dummy drops the reply it had begun.

While a reply plays the dummy's Training-menu action is set to Stand, so the pad input is the dummy's, and put back after. **Reply chance** (25 to 100%) makes it reply only some of the time. **Vary stance** has the dummy stand or crouch at random each time it recovers, while its action is Stand or Crouch.

Recordings last for the battle, so a reply slot has to be recorded again; reply moves need nothing.

## Implementation notes

How the overlay is built and validated, including the render checks and earlier test builds, is recorded in [Training Lab implementation history](../validation/TRAINING_LAB_HISTORY.md).
