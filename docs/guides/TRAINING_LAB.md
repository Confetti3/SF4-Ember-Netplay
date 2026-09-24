# Training lab

Ember adds an offline training overlay. Its frame meter shows native fighter states, action IDs, animation frames, observed action durations, startup and signed recovery advantage. Separate startup/active/recovery coloring remains unavailable, so the complete attack stays orange. New timing behavior still needs gameplay correlation.

## Open the lab

Use the game's main menu to enter **Training** and select both fighters and a stage. The meter appears at the bottom of the screen once the fight is ready. The viewer and its F6 controls exist only inside native offline Training. There is no Training page in the main Ember overlay.

| Control | Action |
| --- | --- |
| F5 | Show/hide the passive meter |
| F6 | Open/close training controls |
| Start | SF4's native pause only; no Ember binding |
| Arrow keys / Enter | Navigate / select within training controls |
| Escape | Cancel confirmation, return one menu level, or close at the root |
| F7 | Start/stop recording P1's controls onto P2. If the selected slot already holds a recording, F7 opens Dummy Recording so you can confirm the overwrite |
| F8 | Start/stop playback of the selected P2 slot |

The passive meter does not capture gameplay input. Training controls use keyboard and mouse only: F6 opens/closes, arrows navigate, Enter selects, and Escape goes back. There is no controller opening shortcut or controller navigation in the flyout. Start retains native pause behavior while Ember is closed. While the flyout is open, all gameplay input is captured, including controllers; closing inputs must release before returning to gameplay. F6 does not request native pause. Recording and playback suspend while the controls are open, and stop if the game loses focus. F5 through F8 are reserved while in training.

The passive HUD uses two slim meter rows, a 42% opaque background and translucent meter cells. Its prompt shows F6 for controls and F5 to hide the HUD. At 720p it is 620 pixels wide, bounded by 75% of viewport width; its size follows viewport height instead of the main menu's DPI scale.

The controls are a fixed, centered flyout targeting 820 by 600 logical pixels, bounded by 80% of each viewport dimension. The game stays visible around it: there is no full-screen artwork or dimming. Narrow layouts place details above a scrolling list; headings, command feedback and the button legend remain fixed. Confirmations stay inside the panel with Cancel selected first. Outside clicks neither close the flyout nor pass through into gameplay. Successful Record and Play commands close Ember; close native pause separately if it remains open. Rejected commands remain visible with an explanation.

## Read the frame meter

Each passive bar holds up to 120 simulated frames for one fighter. Thin grid lines separate frames and stronger marks appear every ten frames. White lines identify changes in native action ID. Read startup and frame advantage beside the bars during practice; there is no separate frame-inspection page. The flyout contains Dummy Recording, Input History and Close training controls. Save/restore position and its F9 shortcut have been removed following a runtime failure report.

| Color | Native state group |
| --- | --- |
| Gray | Standing/crouching neutral |
| Cyan | Movement, jumping, dashing and posture transitions |
| Orange | Attack (`AS_SKILL`), all phases combined |
| Blue | Guard posture or guard damage |
| Red | Damage, blowback or stun |
| Purple | Bound, down or rise |
| Ivory | Missing or unclassified state |

Guard posture alone is not proof of blockstun. The native animation frame can pause or change rate while simulation frames continue; the passive bar is not a move-table recovery or total-duration lookup.

By default, the timeline holds the last exchange after 30 neutral frames and resumes on activity. Clear input history also clears the meter. Native time discontinuities clear the timeline. No states are inferred for missing frames or fighters.

### Signed frame advantage

Both rows display reciprocal numbers after a supported grounded hit/block exchange: P1 `+5 f` / P2 `-5 f` means P1 recovered five simulated frames earlier. Positive is green, negative is red, and equal recovery reads `+0 f`. Let both fighters recover before starting another attack.

Measurement starts from an observed attack and a native damage/guard-damage contact. It follows action changes through target combos, special cancels, airborne phases and landing until grounded recovery, requiring positive unit time scale and no native basic-action inhibit. Each further contact resets the defender's recovery boundary. The attacker's recovery timestamp survives delayed projectile contact. After knockdowns, the HUD says `wakeup` because the comparison includes the defender getting up. This is a measured exchange recovery comparison, not a move-table lookup or a universal cancel/actionability predicate.

`--` means no completed supported exchange. Whiffs, guard posture without contact, trades/interruption, missing actors and timeline gaps do not produce numbers. Slow/frozen phases retain the exchange; values count accepted simulation steps. A new attack after a completed exchange clears the previous result; an action change within an unfinished chain continues measurement. Pending exchanges expire after 600 frames without a new action/contact. Reset and battle exit clear all values. Attribution assumes a two-fighter exchange; reflected projectiles and unusual scripted interactions are not independently verified.

Point the mouse at `Start --` on the passive meter to see why startup is unavailable, or at the `FRAME ADVANTAGE` line below the bars to see why advantage is unavailable. Startup and advantage are independent: a whiff can retain authored startup while correctly leaving advantage unavailable.

### Startup

Each player row shows `Start N f`. It counts observed advancing frames from the action's start to its native BAC script's first attack boundary. This handles animation speed changes; repeated frozen animation frames do not increase startup. It does not wait for the opponent to be hit, so projectile travel distance does not inflate it. Target-combo follow-ups update startup; recovery-only actions retain the preceding value. A multi-phase move without an initial attack boundary keeps counting into the next phase. `Start --` means the script has no usable boundary or observation began mid-move. This uses authored attack timing, not live collision-box activation, and needs native off-by-one and character-specific validation. The last startup remains visible after recovery.

### Opt-in frame-meter capture

Maintainers can set `SF4E_TRAINING_CAPTURE=1` before launching Ember. Offline Training then buffers accepted observations and writes `training-samples-<process>.csv` under `%APPDATA%\sf4e\logs` on a background thread. Rows include the simulation frame, fighter side, status, action ID/frame, posture, time scale, action inhibit, BAC attack start/end with provenance, startup/result state and unavailable reasons. Queue saturation drops capture rows rather than waiting inside simulation. Record the selected fighter editions and the `SSFIV.exe` SHA-256 alongside the CSV; the current native sample does not expose that selection metadata safely.

For Waldo's report, capture both sides and each light/medium/heavy/EX Ryu or Ken DP and Guile Flash Kick on whiff, hit and block, then permitted forward/back FADC. Repeat representative Poison and Adon specials as comparisons. Native player acceptance remains required even when the synthetic `FrameMeter::Observe` regressions pass.

## Record a dummy sequence

Set the native training dummy to **Player/controller control** first; CPU and native playback can supersede controller input. Select one of eight slots, close the controls, and press F7. P1's controller operates P2 while P1 stays neutral. Press F7 again to stop, then F8 to replay. Each slot holds at most 1,800 accepted simulation frames (30 seconds at 60 fps). Playback can loop. Directions are absolute, so switching sides does not mirror a recording. Recordings are local to the current battle.

Input history shows controller inputs, newest first, with how many simulated frames each input was held. It does not identify CPU-generated moves or measure startup.

## Implementation notes

How the overlay is built and validated, including the render checks and earlier test builds, is recorded in [Training Lab implementation history](../validation/TRAINING_LAB_HISTORY.md).
