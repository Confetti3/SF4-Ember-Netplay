# Training lab

Ember adds an offline training overlay. Its frame meter shows native fighter states, action IDs, animation frames, observed action durations, startup and signed recovery advantage. Authored attack boundaries give the startup, active and recovery numbers, and the bars colour an attack's frames by them. New timing behavior still needs gameplay correlation.

## Getting there

Offline, choose **Play offline** on Ember's Home, then Training in the game's own menu. The lab comes on with the game's Training mode, however it was entered. In a room, **Wait in Training** sends the game straight into Training mode without the main menu's Fight Request question, while the room stays joined (see TRAINING_IN_ROOMS.md in docs/design).

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

The passive HUD is a translucent panel with a row for each player: the frame advantage, the startup, then a bar of rectangular cells across the rest of the width. Under the bars, one line gives the meter's state (`FRAME ADVANTAGE | live`, `held`, `measuring` or `wakeup`) and, after it, why a reading is unavailable. While a slot is recorded or played, a strip over the rows names the slot, how far along it is and the key that stops it. Above the panel, a chip opens the controls (F6) beside the F5 hint to hide the HUD; after a pad press, the chip shows the pad's Back and Start and the hint says what Back does alone. **Recovery on HUD** (Frame data, off at first) adds each fighter's last recovery to its row. The numbers, the bars, the state line, the chip with its hint and the recording strip can each be turned off on the HUD page of the training controls; with all of them off nothing is drawn. **HUD size** on the same page picks small, standard (the default) or large: at 1080p their text is about 17, 22 and 28 pixels tall. Small is 620 pixels wide at 900p; every size is bounded by 75% of viewport width. The size follows the viewport and the player's Interface size, not Windows' DPI scale, and in a smaller or narrower window shrinks down to 65% of small at 900p. It sits just above the game's super meters, with its bottom edge at 82% of the viewport height, so it does not cover them.

The controls are a flyout targeting 820 by 600 logical pixels, bounded by 80% of each viewport dimension. It opens at the top centre and can be dragged anywhere inside the viewport, out of the way of a playback. The game stays visible around it: there is no full-screen artwork or dimming. Narrow layouts place details above a scrolling list; headings, command feedback and the button legend remain fixed. Confirmations stay inside the panel with Cancel selected first. Outside clicks neither close the flyout nor pass through into gameplay. Successful Record and Play commands close Ember; close native pause separately if it remains open. Rejected commands remain visible with an explanation.

## Read the frame meter

**Frame data** in the training controls (F6) gives each fighter's last move: **Startup** (the frame it first hits on), **Active** and **Recovery** in frames, hitstop left out, so the move's total is startup - 1 + active + recovery. They are counted as the game's frame data counts them: Startup includes the first active frame, and a move that completes has its Recovery count the frame it is free on, so one that ends with no recovery frames left reads 1. After them comes what the move met and what it was worth: **Hit** or **Block** with the frame advantage, or **Whiff** when it touched nothing, which has no advantage to give. A cancel into another attack counts as a new move. What the thrower still has to play once the thrown fighter is let go counts as its recovery. A multi-hit move's gaps between hits count as active. **Meaty** appears after a knockdown: the number of the attack's active frames that had already passed when the other fighter could first be hit. Each is a frame of advantage gained, so an attack that is +2 on block with Meaty 3 is +5. A signed number means the attack missed that frame: +N left the other fighter N frames to act, -N went active N frames before it and was over by then. The same screen holds the colour key, the meter's style, the frames it shows, and the HUD's recovery option.

Each bar holds the last 120 simulated frames for one fighter, the upper one Player 1's, one cell a frame. **Frames shown** on the Frame data page sets how many of them cross the bar: 60 at first, so each cell is wide enough to count by eye, 90, or 120 for the whole history at once. The newest frame is always at the right. While the meter is held, the mouse wheel over the bars scrolls back through the 120 it keeps, ten frames a notch, and the newest show again once it runs. A thin gap parts every cell; every fifth boundary is a faint line and every tenth a brighter one with a tick under the bar. **Meter style** on the same page chooses how the cells are drawn. The meter in matches follows both choices.

**Angled**, the default, leans each cell as the game's gauges lean and colours it by what the fighter is doing that frame: an attack's startup, active and recovery frames apart, and hitstun apart from blockstun (the first table below). Each run of one kind carries its length, centred on it, where the number fits. A new action inside a run (a cancel, the next hit of a string) and each hit of a combo start a count of their own, parted from the one before by a dark edge. Holding back in the air near an attack is a guard status with nothing to guard, so it shows as idle.

**Flat** is Stable's bars: rectangular cells, a cell's colour the fighter's state group (the second table), so an attack is one colour from start to end and its numbers are in Frame data. A bright line marks the cell where the native action ID changed, so a cancel or a follow-up starts a stretch of its own.

The colour key on the Frame data page lists the colours of the style chosen. The bars fill from the left. Once both fighters have been idle for half a second they hold that exchange: a whiff, a walk or a jump leaves them as they are, and the next hit, block or throw starts them again from the left with everything since the first move after both fighters were last idle; an attack still waiting for its contact, such as a projectile in flight, keeps its frames through the idle ones until it lands. The frame advantage beside them stays until the next reading replaces it, dimmed while that one is measured. The Frame data page gives each move's own result and, while the next exchange has no reading yet, the last one apart as **Last advantage**. Reset position, Clear input history and the battle's end clear both. The line below them gives the meter's state and says what a pending reading is waiting for. The controls hold Dummy recording, Input history, Frame data, HUD, Dummy, Dummy reply, Position, and Close training controls. Position save/reset and their configurable keys are available in offline Training.

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

To practise with another player, create a private room with **Practice room** on (Online play > Create room). It is off each time that screen opens, and a public room never offers it. Every game in a Practice room is a Practice match: both fighters' health, Super and Ultra gauges fill again about a second after they are left alone, and nobody is knocked out. Both players and every spectator get it from the room; nothing has to be set on their side. The room keeps it for as long as it is open, and its status line says **Practice room**. To play normal matches, close the room and create a new one.

Nobody is knocked out, so every game in a Practice room takes the longest round time, 9999, and does not end on its own while you practise; its tables have no round time to choose. To end the game, either fighter presses Start or F10 during the fight: Ember opens on the room board at **End practice**, and confirming it ends the game for both with no result and the notice "Practice ended". Both stay seated and can ready again. While Ember is open over the fight your fighter stands still; closing Ember (B back to Home and B again, or F10) returns you to the fight. The match HUD names those keys. Leaving the room also ends the game. There is no shared position save/reset in rollback matches; the match HUD says so. The position keys and pad Back apply only in offline Training; a Practice room does not enable them.

There is no dummy and no recording in a Practice room: the other player is the dummy. A Practice match always shows the frame meter, to both players and everyone watching, whether **Frame meter in matches** is on or off; a public room still never shows it.

### Opt-in frame-meter capture

Maintainers can set `SF4E_TRAINING_CAPTURE=1` before launching Ember. Offline Training then buffers accepted observations and writes `training-samples-<process>.csv` under `%APPDATA%\sf4e\logs` on a background thread. Rows include the simulation frame, fighter side, status, action ID/frame, posture, time scale, action inhibit, BAC attack start/end with provenance, startup/result state and unavailable reasons. Queue saturation drops capture rows rather than waiting inside simulation. Record the selected fighter editions and the `SSFIV.exe` SHA-256 alongside the CSV; the current native sample does not expose that selection metadata safely.

For Waldo's report, capture both sides and each light/medium/heavy/EX Ryu or Ken DP and Guile Flash Kick on whiff, hit and block, then permitted forward/back FADC. Repeat representative Poison and Adon specials as comparisons. Native player acceptance remains required even when the synthetic `FrameMeter::Observe` regressions pass.

## Record a dummy sequence

Nothing needs setting in the game first, and Player 2 needs no controller: while Ember records or plays a slot on Player 2, or plays the dummy's reply, the dummy plays from Player 2's input, which Ember supplies, for exactly those frames, and the dummy's Training-menu action is left as the player set it. Outside them the game's own dummy plays, so its action and guard settings apply as usual. Picking another action in the pause menu meanwhile stops the recording or playback, and the new action stands. Select one of eight slots, close the controls, and press F7. P1's controller operates P2 while P1 stays neutral. Press F7 again to stop, then F8 to replay. Each slot holds at most 7,200 accepted simulation frames (120 seconds at 60 fps). Playback can loop: Left and Right on **Play** choose once or looped. A recording follows the side the dummy faces: it is kept as if the dummy faced right and played for the way the dummy faces on each frame, so a dragon punch recorded on the left comes out on the right too, and a side switch inside a sequence plays correctly. Facing is taken from which fighter stands further left, one frame old; the game turns a fighter a few frames later, after a jump lands, so a recording that changes sides in the air may turn early. Input history shows the directions actually pressed or played. Recordings are local to the current battle.

Input history shows controller inputs, newest first, with how many simulated frames each input was held. It does not identify CPU-generated moves or measure startup.

## Position and dummy

Open the training controls (F6) and select **Position** for the position rows and their keys, or **Dummy** for the dummy's settings. These settings and the two hotkeys are kept in `training.json` beside Ember's settings; dummy recordings saved from the Dummy Recording screen go to `recordings\` next to it. Files saved before recordings followed the dummy's facing are read as recorded with the dummy on the right, facing left; **Mirror selected recording** swaps a loaded slot's Left and Right in one press for one that comes out the wrong way round.

**Save position** keeps both fighters' place, health, meters and dummy state for this battle. **Reset position** restores the saved state. On a pad, a tap of Back (View on an Xbox pad, Select on a DirectInput pad) resets and a hold of half a second saves; Back held while Start is pressed opens or closes the controls instead and does neither.

The dummy rows (dummy action, guard, counter hit, quick stand, super and revenge gauge) show the game's own Training menu settings and change them at once. The action row offers Stand, Crouch, Jump and CPU; Human, Record and Playback set in the pause menu are shown by name. Save position leaves a recording, a playback or a reply running. Reset position leaves a recording running, starts a playback again from its first frame, in step with the restored positions and with its waits cued by the restored fight, and ends a reply, whose timing belonged to the stun the reset undid.

### Dummy reply

**Dummy reply** makes the dummy act by itself once it is free again: after being hit (a dropped combo), after blocking, on wake-up, or any of them. None starts while a playback or a recording runs.

#### Set one up

1. Press F6 and open **Dummy reply**.
2. On **Dummy reply**, choose when it replies: **After a dropped combo**, **After blocking**, **On Wake-Up** or **Whenever it recovers**.
3. On **Reply preset**, pick a common reply with Left and Right, or press Select to type your own into **Reply moves**.
4. Close the controls and knock the dummy down, or hit it, as the reply needs.

The first time the dummy sees a given knockdown or stun, it measures it and replies as it ends, often a little late. Every time after that, the reply comes out on the first free frame, as a reversal. **Reply timing** moves it up to five frames earlier or later.

#### Ready-made replies

Directions are written for a dummy facing right; the dummy turns them for the side it faces, so `6` is always forward and `4` always back.

| What you want | Type |
| --- | --- |
| Reversal dragon punch | `623HP` or `dp HP` |
| Throw | `LP+LK` |
| Backdash | `44` |
| Forward dash | `66` |
| Jump back, up, forward | `7`, `8`, `9` |
| Fireball | `236P` or `qcf P` |
| EX fireball | `236PP` |
| Super | `236236P` |
| Ultra | `236236PPP` |
| Spinning piledriver | `360P` |
| Low short | `2LK` or `cr.LK` |
| Focus attack | `MP+MK` |
| Dragon punch, then focus cancel and dash | `623HP xx FADC` |
| Short combo | `2LK > 2LP xx 623HP` |
| Hundred Hand Slap | `HP(mash)` |
| Block low until free, then sweep | `[1] > 2HK` |
| Charge move on wake-up (Flash Kick, Sonic Boom) | `[1] > 8HK`, `[1] > 6HP` |

#### Charge moves

Start the reply with a held direction, then type the charge move **without** its charge: `[1] > 8HK` for a Flash Kick, `[1] > 6HP` for a Sonic Boom, `[1] > 6P` for a Psycho Crusher. `[1]` is down-back, so it charges both back and down. The dummy holds it from the moment it is knocked down or stunned and lets go just in time to press the move on its first free frame.

A charge built this way is only as long as the stun. A knockdown is long enough, so charge moves work **On Wake-Up**. Every charge in the game is longer than a normal blockstun, so after blocking the move comes out uncharged unless the blockstun is long.

Don't write the charge twice: `[1] > [4]6P` holds back for a whole new charge after the dummy gets up. `[4]6P` on its own is right only when nothing comes before it to hold the charge through.

#### Notation cheat sheet

Any case works, and spaces are optional.

| Write | Means | Example |
| --- | --- | --- |
| `1` to `9` | Directions, as on a number pad: `2` down, `4` back, `6` forward, `8` up, `5` neutral | `623HP` |
| `LP MP HP LK MK HK` | Buttons; join several with `+` | `LP+LK` |
| `P` `PP` `PPP`, `K` `KK` `KKK` | Any one, two or three punches or kicks | `236PP` |
| `qcf` `qcb` `dp` `rdp` `hcf` `hcb` | 236, 214, 623, 421, 41236, 63214 | `qcfP`, `dp HP`, `qcb+LK` |
| `360` `720` | A full or double circle | `360P` |
| `cr.` `st.` | Crouching (2) or standing (5) | `cr.MK` |
| `cl.` `far.` | Close or far standing normal | `cl.HP` |
| `j.` | A straight-up jump with the attack in the air | `j.HK` |
| `>` or `,` | Then the next move, once the dummy can act again (a link) | `2LK > 2LP` |
| `xx` | Cancel into the next move when the hit lands | `2MK xx 236HP` |
| `~` | Follow-up pressed about ten frames into the move before | `236P ~ LP` |
| `[4]6` | Charge back, then forward | `[4]6P` |
| `[1]` | Hold a direction until the next move | `[1] > 2HK` |
| `[2]30f` | Hold a direction for 30 frames | `[2]30f > 8K` |
| `10f` | Wait 10 frames, holding nothing | `2LP > 10f > 2LP` |
| `[HP]` / `]HP[` | Hold a button / let it go | `[MP+MK]` |
| `(mash)` | Press five times | `HP(mash)` |
| `FADC` `FADC44` | Focus cancel, dash forward or back | `623HP xx FADC` |
| `RFADC` | Red focus that lands, then dash | `RFADC` |
| `@N` | Move this one N frames later or earlier | `623HP@+2` |
| `#N` | Press this one N frames into the reply | `2LP > 10f > 2LP#30` |

Character-specific move names (`shoryuken`, `sonic boom`) are not read. A line the dummy can't use shows an error and is not sent.

#### Notation reference

**Reply preset** steps through **Reply slot** (no moves, so the dummy plays the reply slot), `623P`, `44`, `66`, `LP+LK`, `8`, `9`, `2LK`, `214K`, `236P`, and **Custom**, which brings back the last reply typed that is not a preset. A preset is checked and sent exactly as the same text typed into Reply moves would be, and a reply that spells a preset, in any spelling (`lp+lk`, or an old `throw`), shows as that preset.

**Reply moves** is the reply typed in numpad notation, read for the side the dummy faces. Older saved replies using shared names such as `dash`, `throw` or `focus` migrate to notation when loaded. While the line is empty the dummy plays the **Reply slot** instead, a dummy recording, once from its first pressed frame. If that slot is empty, the Dummy reply page says so in its status line.

Moves are separated by `>` or `,`; `xx` before a move means it cancels the one before it, `~` that it is a follow-up pressed about ten frames into it with no hit to wait for (`@N` moves it), otherwise it links. The full form of one move:

```
[xx|~] [j.|cr.|st.|cl.|far.] [motion] [buttons] [(mash)] [#N] [@N]
[xx] FADC[66|44] [#N] [@N]
[xx] RFADC[66|44] [#N] [@N]
[d] | [d]Nf | Nf
```

- motion: numpad digits, `[4]6` for charge, `360`, `720`, or a word: `qcf` (236), `qcb` (214), `dp` (623), `rdp` (421), `hcf` (41236), `hcb` (63214), with its button straight after, after a space or after `+` (`qcfP`, `dp HP`, `qcb+LK`). `cr.` is 2; `st.`, `cl.` and `far.` are 5.
- buttons: `LP MP HP LK MK HK` joined by `+`, or `P PP PPP K KK KKK`. `[HP]` holds, `]HP[` releases, `(mash)` presses five times: `P(mash)`, `K(mash)` or several buttons cycle one per frame (a piano, five presses in five frames), a single button goes on and off (five in ten). `(mash HP-MP-LP-HP-MP)` sets the order and the count, one press per frame, up to 10; a digit before a button gives that press its own direction, `5K(mash 1MK-1MK-1MK-1MK)`.
- `@N`: the move's own timing offset, -120 to +120 frames, from what it waits on: the dummy being free for the first move (added to **Reply timing**), the move before for a link, the hit for a cancel. `#N`: the frame of the reply the move takes effect on (its button; with none, its last direction, as for a jump or a dash; for a jumping attack, its jump), 0 to 7200, counted from the reply's first played frame, a later move's `@N` then counted from that frame (the first move's stays the reply's); only frames of a known length can come before it, so it is refused on the first move with nothing before it, after a hold with no count leading the reply, and after any wait on the fight (a link or a cancel). The first move cannot be a cancel, a follow-up or a button let go (`]HP[`, which comes out on the release, a time the reply cannot set). A reply needs a move: holds and waits alone are refused.
- `FADC` is the focus cancel (MP+MK, then the dash at once). `RFADC` is the red focus: LP+MP+MK tapped and let go so the red focus attack comes out and lands, then the dash on that hit.
- A direction held alone, read for the facing like any other: `[1]` holds down-back until the next move begins its own input, held through that move's wait for the dummy to be free or a follow-up's delay (`[1] > 3HP`); `[2]30f` holds down for 30 frames (1 to 7200); `10f` waits 10 frames with nothing held. Before the first move these lead into it: the move plays straight after them, and a reply starts early enough for them and the move's motion to end with its button, or a jump's or a dash's last direction, on the first free frame. A hold with no count needs a move right after it, not another hold or a wait; a reply cannot start with a wait; and moves that would take more than 7200 frames are refused whole.

Directions take a few frames each and a button two; between moves the input waits on the fight itself: a link waits for the dummy to be free again, a cancel for the hit to land. The game decides what comes out.

#### How the reply is timed

The reply is timed to the stun. The game does not say how long a stun lasts, so each one is measured the first time it is seen, from the last hit or change of reaction to the free frame, per attacking move and reaction. The first time, the reply starts as the stun ends; from then on its motion goes in while the dummy is still held and its first attack button lands on the first free frame. **Reply timing** moves that frame by up to five either way; a later frame is waited for past the free frame, the first time too. A counter hit's longer stun replaces the plain hit's until the plain hit is seen again. Hit again before it is free, the dummy drops the reply it had begun.

A reply that starts with a held direction (`[1] > 3HP`) begins with the stun instead: the dummy holds the direction from the stun's first frame and lets go in time for the move after it, its motion included (`[1] > 623P`), to press its button on the first free frame plus the reply timing and the move's own `@N`, as a reversal when the stun was seen before; the first time, it lets go as the stun ends. It waits out any stun, a dizzy too. A hold later in the line (`5LP > [4] > 6P`) waits as a link does, until the dummy is free again or 90 frames at most.

While a reply plays the dummy takes its input from Ember, and before and after it the dummy follows its own Training-menu settings, so it blocks as its guard setting says. It cannot block during the frames its reply's motion is entered, and while it holds a direction its guard setting is set aside: it blocks by what it holds, low with `[1]`, high with `[4]`, and not at all with anything else, so a hit that comes then lands and drops the reply. The hit it is blocking when the reply begins stays blocked. The Dummy action row shows the chosen setting throughout. **Reply chance** (25 to 100%) makes it reply only some of the time. **Vary stance** has the dummy stand or crouch at random each time it recovers, while its action is Stand or Crouch.

Recordings last for the battle, so a reply slot has to be recorded again; reply moves need nothing.

## Implementation notes

How the overlay is built and validated, including the render checks and earlier test builds, is recorded in [Training Lab implementation history](../validation/TRAINING_LAB_HISTORY.md).
