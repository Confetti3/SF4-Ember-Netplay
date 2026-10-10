# Training lab

Ember adds an offline training overlay. Its frame meter shows native fighter states, action IDs, animation frames, observed action durations, startup and signed recovery advantage. Authored attack boundaries give the startup, active and recovery numbers, and the bars colour an attack's frames by them. New timing behavior still needs gameplay correlation.

## Getting there

**Training** on Ember's Home sends the game straight into Training mode, to the fighter select, without the main menu's Fight Request question. It is offered outside a room and a match, like **Play offline**, and rides on the same command: the game's own Training selection is made for the player with that question switched off for the one call, which is the path the game takes by itself where fight requests cannot be made. Choosing Training in the game's own menu still asks.

## Open the lab

Use the game's main menu to enter **Training** and select both fighters and a stage. The meter appears at the bottom of the screen once the fight is ready. The viewer and its F6 controls exist only inside native offline Training. There is no Training page in the main Ember overlay.

| Control | Action |
| --- | --- |
| F5 | Show/hide the passive meter |
| F6 | Open/close training controls |
| Back held, then Start (pad) | Open/close training controls |
| Start | SF4's native pause, unless Back is already held |
| Arrow keys / Enter | Navigate / select within training controls |
| D-pad / A (pad) | The same on an Xbox pad; LP selects on a DirectInput pad |
| Escape / B (pad) | Cancel confirmation, return one menu level, or close at the root; LK on a DirectInput pad |
| F7 | Start/stop recording P1's controls onto P2. If the selected slot already holds a recording, F7 opens Dummy Recording with the overwrite question on Record, answered Cancel until you choose Record |
| F8 | Start/stop playback of the selected P2 slot |
| F2 | Reset position: back to the saved state |
| F11 | Save position |
| Back (pad) | Tap to reset the position, hold half a second to save it |
| Enter, or View on an Xbox pad | While a room calls you back ("Your match is ready"): go now instead of waiting out the banner |

F2 and F11 are the **Reset position** and **Save position** rows of the Position screen and work with the controls closed or open, except while a text field is being typed in. The two **key** rows at the end of that screen move each to F1 to F4, F9 or F11, or turn it off; one key does one thing. The HUD's chips name both keys and, for a few seconds, what the last one did.

The passive meter does not capture gameplay input. Training controls work from the keyboard, the mouse and the assigned pad. On the keyboard, F6 opens and closes them, arrows navigate, Enter selects, and Escape goes back. On the pad, hold Back (View on an Xbox pad, Select on a DirectInput pad) and press Start within half a second to open or close them. Inside, the pad works as in Ember's other menus: the D-pad moves, A selects and B goes back on an Xbox pad (LP and LK on a DirectInput pad), and Left and Right change values. The chord takes nothing from the pad's other uses: Start pressed first is still the game's pause, Back tapped or held alone still resets or saves the position, and a Back used for the chord does neither. The chord's Start never reaches the game, so it does not pause it. The legend and the HUD's chips show the keys or the pad's buttons, whichever was used last. Typing a recording's name or reply moves needs a keyboard. Start retains native pause behavior while Ember is closed. While the flyout is open, all gameplay input is captured, including controllers; closing inputs must release before returning to gameplay. F6 does not request native pause. Recording suspends while the controls are open; playback keeps running under them. Both stop if the game loses focus. F5 through F8 are reserved while in training.

The passive HUD is a translucent panel with a row for each player: the frame advantage, the startup, then a bar of rectangular cells across the rest of the width. Under the bars, one line gives the meter's state (`FRAME ADVANTAGE | live`, `held`, `measuring` or `wakeup`) and another says why a reading is unavailable. Above the panel, a chip opens the controls (F6) beside the F5 hint to hide the HUD; after a pad press, the chip shows the pad's Back and Start and the hint says what Back does alone. **Recovery on HUD** (Frame data, off at first) adds each fighter's last recovery to its row. At 720p it is 620 pixels wide, bounded by 75% of viewport width; its size follows viewport height (up to one and a half times) instead of the main menu's DPI scale. It sits just above the game's super meters, with its bottom edge at 82% of the viewport height, so it does not cover them.

The controls are a flyout targeting 820 by 600 logical pixels, bounded by 80% of each viewport dimension. It opens at the top centre and can be dragged anywhere inside the viewport, out of the way of a playback. The game stays visible around it: there is no full-screen artwork or dimming. Narrow layouts place details above a scrolling list; headings, command feedback and the button legend remain fixed. Confirmations stay inside the panel with Cancel selected first. Outside clicks neither close the flyout nor pass through into gameplay. Successful Record and Play commands close Ember; close native pause separately if it remains open. Rejected commands remain visible with an explanation.

## Read the frame meter

**Frame data** in the training controls (F6) gives each fighter's last move: **Startup** (the frame it first hits on), **Active** and **Recovery** in frames, hitstop left out, so the move's total is startup - 1 + active + recovery. They are counted as the game's frame data counts them: Startup includes the first active frame, and a move that completes has its Recovery count the frame it is free on, so one that ends with no recovery frames left reads 1. After them comes what the move met and what it was worth: **Hit** or **Block** with the frame advantage, or **Whiff** when it touched nothing, which has no advantage to give. A cancel into another attack counts as a new move. What the thrower still has to play once the thrown fighter is let go counts as its recovery. A multi-hit move's gaps between hits count as active. **Meaty** appears after a knockdown: the number of the attack's active frames that had already passed when the other fighter could first be hit. Each is a frame of advantage gained, so an attack that is +2 on block with Meaty 3 is +5. A signed number means the attack missed that frame: +N left the other fighter N frames to act, -N went active N frames before it and was over by then. The same screen holds the colour key, the meter's style, the frames it shows and the HUD's recovery option.

Each bar holds the last 120 simulated frames for one fighter, the upper one Player 1's, one cell a frame. **Frames shown** on the Frame data page sets how many of them cross the bar: 60 at first, so each cell is wide enough to count by eye, 90, or 120 for the whole history at once. The newest frame is always at the right. While the meter is held, the mouse wheel over the bars scrolls back through the 120 it keeps, ten frames a notch, and the newest show again once it runs. A thin gap parts every cell; every fifth boundary is a faint line and every tenth a brighter one with a tick under the bar. **Meter style** on the same page chooses how the cells are drawn. The meter in matches follows both choices.

**Angled**, the default, leans each cell as the game's gauges lean and colours it by what the fighter is doing that frame: an attack's startup, active and recovery frames apart, and hitstun apart from blockstun (the first table below). Each run of one kind carries its length, centred on it, where the number fits. A new action inside a run (a cancel, the next hit of a string) and each hit of a combo start a count of their own, parted from the one before by a dark edge. Holding back in the air near an attack is a guard status with nothing to guard, so it shows as idle.

**Flat** is Stable's bars: rectangular cells, a cell's colour the fighter's state group (the second table), so an attack is one colour from start to end and its numbers are in Frame data. A bright line marks the cell where the native action ID changed, so a cancel or a follow-up starts a stretch of its own.

The colour key on the Frame data page lists the colours of the style chosen. The bars fill from the left and start again there once both fighters have been idle for half a second. The lines below them give the meter's state, or say what a pending reading is waiting for. The controls hold Dummy recording, Input history, Frame data, Dummy, Dummy reply, Position, and Close training controls. Position save/reset and their configurable keys are available in offline Training.

Angled:

| Color | What the fighter is doing | Colour key (F6) |
| --- | --- | --- |
| Dim gray | Standing/crouching neutral, idle, jumps | (none) |
| Green | An attack's startup, up to its first active frame | Startup |
| Ember orange | An attack's active frames | Active |
| Amber | An attack's recovery; also what a thrower still plays once the thrown fighter is let go | Recovery |
| Muted | An attack whose script names no attack boundary | Attack, not split |
| Red | Hitstun: damage, blowback or stun | Hit |
| Blue | Blockstun: guard posture or guard damage | Block |
| Purple | Bound, down or rising (wakeup) | Knockdown |
| Light blue | Walks, dashes, turns, crouching and standing up | Move |
| Beige | A throw or cinematic sequence holding both fighters | Throw / cinematic |

Flat:

| Color | Native state group | Colour key (F6) |
| --- | --- | --- |
| Dim gray | Standing/crouching neutral, idle | (none) |
| Light blue | Movement: walks, dashes, jumps, turns, crouching and standing up | Move |
| Ember orange | Attack (`AS_SKILL`), from startup to the end of recovery | Attack |
| Blue | Guard posture or guard damage | Block |
| Red | Damage, blowback or stun | Hit |
| Purple | Bound, down or rising (wakeup) | Knockdown |
| Beige | Any other state, chiefly a throw or cinematic sequence holding both fighters | Throw / cinematic |

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

**Settings > Frame meter in matches** (off by default) draws the meter over your own online matches and the ones you watch, in the meter style and frames shown chosen in the Training lab (angled, 60 frames, until changed). Only you see it. It reads the game and changes nothing in it. It shows a frame once the other player's inputs for it have arrived, so it trails the fight by the frames still predicted and never shows one that is later played differently. The controls, recordings and hotkeys of Training are not there. A projectile move without attack frames in its script shows no startup in a match.

### Training with another player

A room's host can turn **Training** on in a table's rules (Table options, with the round count and the round time). At that table both fighters' health, Super and Ultra gauges fill again about a second after they are left alone, and nobody is knocked out. Both players and every spectator get the rule from the table; nothing has to be set on their side.

A game under the rule ends when the round time runs out or a fighter leaves the seat, so turning the rule on sets the round time to 9999; the Round time row points it out if the host shortens it again. There is no shared position save/reset in rollback matches; the match HUD says so. The position keys and pad Back apply only in offline Training; the table rule does not enable them.

There is no dummy and no recording at such a table: the other player is the dummy. Turn **Frame meter in matches** on to see the frames.

### Opt-in frame-meter capture

Maintainers can set `SF4E_TRAINING_CAPTURE=1` before launching Ember. Offline Training then buffers accepted observations and writes `training-samples-<process>.csv` under `%APPDATA%\sf4e\logs` on a background thread. Rows include the simulation frame, fighter side, status, action ID/frame, posture, time scale, action inhibit, BAC attack start/end with provenance, startup/result state and unavailable reasons. Queue saturation drops capture rows rather than waiting inside simulation. Record the selected fighter editions and the `SSFIV.exe` SHA-256 alongside the CSV; the current native sample does not expose that selection metadata safely.

For Waldo's report, capture both sides and each light/medium/heavy/EX Ryu or Ken DP and Guile Flash Kick on whiff, hit and block, then permitted forward/back FADC. Repeat representative Poison and Adon specials as comparisons. Native player acceptance remains required even when the synthetic `FrameMeter::Observe` regressions pass.

## Record a dummy sequence

Set the native training dummy to **Player/controller control** first; CPU and native playback can supersede controller input. Select one of eight slots, close the controls, and press F7. P1's controller operates P2 while P1 stays neutral. Press F7 again to stop, then F8 to replay. Each slot holds at most 7,200 accepted simulation frames (120 seconds at 60 fps). Playback can loop. Directions are absolute, so switching sides does not mirror a recording. Recordings are local to the current battle.

Input history shows controller inputs, newest first, with how many simulated frames each input was held. It does not identify CPU-generated moves or measure startup.

## Position and dummy

Open the training controls (F6) and select **Position** for the position rows and their keys, or **Dummy** for the dummy's settings. These settings and the two hotkeys are kept in `training.json` beside Ember's settings; dummy recordings saved from the Dummy Recording screen go to `recordings\` next to it.

**Save position** keeps both fighters' place, health, meters and dummy state for this battle. **Reset position** restores the saved state. On a pad, a tap of Back (View on an Xbox pad, Select on a DirectInput pad) resets and a hold of half a second saves; Back held while Start is pressed opens or closes the controls instead and does neither.

The dummy rows (dummy action, guard, counter hit, quick stand, super and revenge gauge) show the game's own Training menu settings and change them at once.

### Dummy reply

**Dummy reply** makes the dummy act by itself once it is free again: after being hit (a dropped combo), after blocking, on wake-up, or any of them. None starts while a playback or a recording runs.

**Reply preset** picks a common reply with Left and Right: **Reply slot** (no moves, so the dummy plays the reply slot), `623P`, `44`, `66`, `LP+LK`, `8`, `9`, `2LK`, `214K`, `236P`, or **Custom**, which brings back the last reply typed that is not a preset. Select on it opens Reply moves to type one. A preset is checked and sent exactly as the same text typed into Reply moves would be, and a reply that spells a preset, in any spelling (`lp+lk`, or an old `throw`), shows as that preset.

**Reply moves** is the reply typed in numpad notation (`623HP`, `44`, `2LK > 2LP xx 623HP`), read for the side the dummy faces. Older saved replies using shared names such as `dash`, `throw` or `focus` migrate to notation when loaded. Character-specific move names are not read. Invalid nonempty text shows an error and is not sent to the dummy. While the line is empty the dummy plays the **Reply slot** instead, a dummy recording, once from its first pressed frame.

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

While a reply plays the dummy's Training-menu action is set to Stand, so the pad input is the dummy's, and put back after. The Dummy action row continues to show the chosen setting; editing it during a reply changes what is restored when the reply ends. **Reply chance** (25 to 100%) makes it reply only some of the time. **Vary stance** has the dummy stand or crouch at random each time it recovers, while its action is Stand or Crouch.

Recordings last for the battle, so a reply slot has to be recorded again; reply moves need nothing.

## Implementation notes

How the overlay is built and validated, including the render checks and earlier test builds, is recorded in [Training Lab implementation history](../validation/TRAINING_LAB_HISTORY.md).
