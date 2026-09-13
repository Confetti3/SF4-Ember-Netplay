# Training lab

This local candidate adds an offline training overlay to SF4 Ember Netplay. Its frame meter shows native fighter states, action IDs, animation frames, observed action durations, startup and signed recovery advantage. Separate startup/active/recovery coloring remains unavailable, so the complete attack stays orange. Test3 fixes action-chain tracking for specials and target combos after the user reported missing advantage values in test2. New timing behavior still needs gameplay correlation.

## Open the lab

Use the game's main menu to enter **Training** and select both fighters and a stage. The meter appears at the bottom of the screen once the fight is ready. The viewer and its F6 controls exist only inside native offline Training. There is no Training page in the main Ember overlay.

| Control | Action |
| --- | --- |
| F5 | Show/hide the passive meter |
| F6 | Open/close training controls |
| Start | SF4's native pause only; no Ember binding |
| Arrow keys / Enter | Navigate / select within training controls |
| Escape | Cancel confirmation, return one menu level, or close at the root |
| F7 | Start/stop recording P1's controls onto P2 |
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

### Startup

Each player row shows `Start N f`. It counts observed advancing frames from the action's start to its native BAC script's first attack boundary. This handles animation speed changes; repeated frozen animation frames do not increase startup. It does not wait for the opponent to be hit, so projectile travel distance does not inflate it. Target-combo follow-ups update startup; recovery-only actions retain the preceding value. A multi-phase move without an initial attack boundary keeps counting into the next phase. `Start --` means the script has no usable boundary or observation began mid-move. This uses authored attack timing, not live collision-box activation, and needs native off-by-one and character-specific validation. The last startup remains visible after recovery.

## Record a dummy sequence

Set the native training dummy to **Player/controller control** first; CPU and native playback can supersede controller input. Select one of eight slots, close the controls, and press F7. P1's controller operates P2 while P1 stays neutral. Press F7 again to stop, then F8 to replay. Each slot holds at most 1,800 accepted simulation frames (30 seconds at 60 fps). Playback can loop. Directions are absolute, so switching sides does not mirror a recording. Recordings are local to the current battle.

Input history shows controller inputs, newest first, with how many simulated frames each input was held. It does not identify CPU-generated moves or measure startup.

## Current flyout implementation

The designated integration target is `sf4-current`; build and package it through `scripts/build-current.ps1` and `scripts/package-team.ps1`. `DrawTrainingFlyout` owns the same fixed window used by the live overlay and the render harness, drops controller menu input, and uses keyboard legends. The native publication filter captures all gameplay input only while Ember is open or closing inputs are draining; training simulation and meter calculations are unchanged.

The render harness covers ten viewport/DPI configurations, including narrow 150% and 720p 200% layouts, long command errors, pending/rejected commands, disabled actions, safe overwrite confirmation and keyboard HUD prompts regardless of connected controller type. It checks centered bounds, contained children, a non-scrolling outer panel, passive input ownership and unchanged background pixels outside training screenshots. Keyboard journeys exercise the actual flyout and shared navigation model while unwanted controller input is discarded. Native Start pause behavior and gameplay input isolation still require user runtime acceptance.

## Historical implementation and validation

Worktree: `C:/Users/Kate/Desktop/sf4/sf4-training`, branch `feat/sf6-style-training`, based on `ef29cc4` (combined Ember/custom rooms). The existing checkouts were preserved. No game installation, game launch, push or PR was performed.

The x86 RelWithDebInfo `Sidecar.dll` builds with VS 18 Build Tools. The game-independent training tests cover inactive/stale generations, slot bounds, recording limits, repeatable frame reads, playback order/looping, teardown/reset isolation, action changes, held exchanges and missing samples. The DX9 harness exercises the actual panel and passive HUD across seven viewport/DPI configurations. These checks do not prove native training behavior.

Build output: `build/training/Sidecar.dll`. Native reader evidence and remaining phase-data work are recorded in `2026-09-08_reverse-USF4-training-report.md`.

Test1 validation, 8 September 2026: TrainingSession, NativeMatchResult, GgpoGate, SaveStateOwnership, OverlayPresentation and UiRender passed (6/6). UiRender covered 2,100 DX9 frames across seven viewport/DPI configurations with device resets. The user subsequently reported that the training viewer works and requested a smaller translucent layout with signed frame advantage.

Test2 validation: the x86 RelWithDebInfo build and TrainingSession, NativeMatchResult, GgpoGate, SaveStateOwnership, OverlayPresentation and PackageInstaller passed. UiRender passed 2,100 DX9 frames across seven viewport/DPI configurations, including HUD bounds and signed-value fixtures. The 720p and narrow 150% DPI captures were visually inspected. Package preflight verified manifest hashes and 1,794 artwork images; the actual package passed native updater inventory validation. Test2 Sidecar SHA-256: `c48a78a29a548dbef394059d6048c801e7c518fc15eec731680f21d7a46f65ae`. Package: `dist/sf4-netplay-launcher-training-20260908-test2.zip`. New native advantage timing remains for user testing.

Test3 validation: the failing action-chain regression now passes along with airborne/slow special phases, delayed projectile contacts, knockdown recovery, startup speed/freeze handling, follow-up startup, missing boundaries and reset isolation. TrainingSession, NativeMatchResult, GgpoGate, SaveStateOwnership, OverlayPresentation and PackageInstaller passed on the final build. The UI update passed 2,100 DX9 frames across seven viewport/DPI configurations; the 720p capture was visually inspected with advantage and startup beside each player. `git diff --check` passed. Final Sidecar SHA-256: `5565ce62ac3ec808af3945799c77c26cba55668e581e3ba307294b8d1240274e`. New native timing acceptance remains pending.

Before native acceptance, check meter progression in standing, crouching, walking, attacking, hit and guard states; pause/resume; a cancel; reset; character change; focus loss; and a normal online match with no training controls or overrides. Test dummy playback using the native Player setting. Full SF6 phase parity additionally needs verified active-hitbox, actionable/recovery and hitstop readers, plus gameplay correlation for normals, cancels, throws and projectiles.

The 2026-09-10 long-session repair separates recovery timestamps from the native
16-bit simulation counter. Previously, the signed half of that counter made
every recovery timestamp look unset, leaving advantage at `--` until training
was reloaded or the counter wrapped again. Continuity is now checked modulo
16 bits; recovery uses an independent elapsed-frame clock. Focused tests cover
both boundaries and 131,100 observations without reloading. Native long-session
acceptance remains pending; see [repair evidence](COSTUME_MENU_TRAINING_FIX.md).
