# SF4 Ember Netplay — Accessibility Spec

Oct 10, 2026 · FRaccie · tracks upstream issue [#63](https://github.com/Confetti3/SF4-Ember-Netplay/issues/63)

## Relation to #63

Issue #63 asks for a small core: settings, two calls ("play this cue", "say this"), audio cues for lobby events, in-menu voice-over, one Accessibility page in Settings. That is **phase 0** of this spec. Everything after it stacks on that core and ships as separate PRs to nightly. This document stays on the fork under `docs/design/`; the issue links here.

The open question in #63 (system screen reader or own speech) is answered below: Tolk to the user's screen reader, SAPI 5 fallback.

## Source and design principles

Ember should copy Tokon's self-voicing menus, granular audio and per-strength hit sounds, then fix the eight sound-design gaps a blind tester found in it. Source: BlindWarriorSven's open-beta review of Marvel Tokon: Fighting Souls. Platform-specific findings (boot CPU load, PlayStation account) are left out.

Six principles carry through every section:

1. **Hear it when you could see it.** If a sighted player sees a counter on the frame it is pressed, a blind player hears it on that frame. No cue later than its visual, none earlier.
2. **One action, one cue.** Every (move, stance) pair maps to one fixed cue. Layers (strength, stance, counter-hit) are deterministic too. Never random variants.
3. **Every state change has a sound.** Crouch, stand, back dash, jump direction: if it matters to gameplay, it is audible.
4. **Own and opponent always differ.** Every cue has a self and an opponent timbre, in stereo as well as mono. Panning alone fails on cross-ups and side swaps.
5. **Blind players set it up alone.** Speech is on before the first screen.
6. **Never touch game files.** Ember reads game state and filters the game's own sound requests at its existing hook. It does not patch or replace Capcom assets, and ships no Capcom audio.

## What Ember already has

The spec builds on these; none of them is new work.

| Need | Existing piece |
|---|---|
| Hook on every SF4 sound request, silence during resimulation | `src/sf4e/sf4e__Game__Battle.cxx` shadow SoundPlayerManager + `src/common/SoundReconcile.hxx` |
| Menu focus change (one place to speak from) | `src/ui/MenuNavigation.hxx` `Focus()` |
| Button names per device for hints | `src/ui/MenuGlyphs.hxx`, `SetMenuGlyphs` |
| Translated strings | `locales/*.po` (gettext) |
| Lobby, fighter, costume, Ultra, stage select | Ember's own ImGui screens (`src/ui/FighterSelector.*`, `GameMenu.*`); `CostumeLabel`, `UltraLabel`, `StageLabel` |
| Health, meter, revenge, recoverable health | `Actor::GetVitalityPct_FixedPoint` and siblings, `src/Dimps/Dimps__Game__Battle__Chara.hxx` |
| Action ID, status, posture per frame | `Actor::GetActionID`, `GetStatus`, `GetActionPosture` |
| Frame advantage | `src/training/FrameMeter.hxx` |
| Rollback | GGPO (vcpkg); stress harness `sf4e__Game__Battle__System__RollbackStress.cxx` |
| Settings file | JSON via `sf4e__OverlayPrefs` / `UiPreferencesStore` |
| Chat | `src/ui/ChatPanel.cxx` |

Native SF4 menus are only reached offline (arcade, training, options). Everything on the netplay path is Ember UI.

## Phase 0: the #63 core

- `Accessibility` settings struct in the existing JSON prefs: speech on/off, speech rate, speech volume, cue master volume, per-cue on/off and volume.
- Two calls: `PlayCue(CueId, Side)` and `Say(text, interrupt)`.
- Speech backend: Tolk (x86 DLL, SSFIV is 32-bit) → NVDA / JAWS / other screen reader; SAPI 5 via COM when none runs. Windows OneCore voices need WinRT from a 32-bit injected DLL; not worth it, dropped.
- Audio backend: miniaudio (single header, vcpkg, public domain). WASAPI low-latency mode required; shared-mode default is 10–30 ms, more than a frame.
- Lobby cues, merged list from #63 and this spec: your turn in the queue, opponent ready, member joined, member left, match starting, opponent found, match accepted, rematch requested, connection lost, notice appeared. Each with on/off and volume.
- Menu voice-over hooked at `MenuNavigation::Focus()` change: row name, then value, then detail after ~400 ms, then button hint. New notices and incoming chat messages spoken.
- Interrupt-on-navigate and a stop-speech hotkey from day one.
- One Accessibility page in Settings, itself narrated.

## First launch, menus and screen reader

| Feature | Tokon | Ember implementation |
|---|---|---|
| Speech on at first launch | Speaks from the first screen | Speech enabled on first run. Welcome line plus "press F1 / Select to turn speech off". No sighted step anywhere. |
| Menu item narration | Each item name on cursor move | Ember UI: speak on `Focus()` change. Native SF4 menus (offline only): read cursor index and screen ID from memory, map to a string table. Later phase. |
| Item descriptions | One-line explanation after the name | Description after ~400 ms so fast scrolling hears names only. Strings in the `.po` files. |
| Contextual button hints | "cross confirm, circle return" | Built from the player's real bindings via the existing glyph mapping. Toggleable. |
| Shortcut announcements | Hotkeys named | Ember hotkeys (quick training, rematch, replay save) in the hint string. |
| Screen and tab announcements | New screen name on entry | Every transition, including SF4 transitions (loading, VS, result) detected from game state. |
| Confirmation prompts | Question plus choices spoken | Every modal speaks its question, then the focused choice. |
| Language | Setup in own language | Speech strings come from the same `.po` files as the UI. Voice follows the screen reader; SAPI fallback picks a voice matching the UI language. |

## Audio and speech settings

Two sources: SF4's own channels, filtered per cue ID at the existing hook, and Ember's cue layer with its own buses.

| Setting | Ember implementation |
|---|---|
| Master volume | Ember master bus; SF4 master via SF4's setting or the Windows per-app session. |
| Music volume | SF4 BGM setting exposed in Ember's narrated menu. |
| Voice volume | SF4 voice cue IDs filtered at the hook. Consistent-voice mode mutes them entirely. |
| Ambient volume | Stage ambience cue IDs filtered at the hook. The hook sees every request, so no channel is unsplittable. |
| Per-channel mute | Mute flag stored separately from level. |
| Second output | Optional: route a cue category to a second audio device. |
| Mixer | Per cue category: movement, attacks, hits, blocks, meter, announcer, speech. |
| Vibration | Optional haptics per cue, default off. |
| Headphone mode | Wider panning, optional HRTF. |
| Mono | Downmix. Side is still audible through principle 4 (own vs opponent timbre) plus a pitch offset for left/right. |
| Dynamic range | Compressor/limiter on Ember's master bus; "narrow" keeps a super from masking the next footstep. |
| Speech on/off, volume, rate | Own bus, ducks game audio slightly while speaking. Rate follows the screen reader when one is used. |
| Captions | Caption overlay for announcer and Ember spoken events. |
| Revert to defaults | Per page, spoken confirmation. |

## Side, stage and character select

Ember screens: narrated through phase 0 with no extra work beyond strings. Native SF4 screens (offline): memory reads, later.

| Feature | Ember implementation |
|---|---|
| Side | Speak on change, confirm ("You are player 1, left side"). Online: announce side when the match is created. |
| Stage and music | `StageLabel` spoken. Online: announce the stage before the match. |
| Character grid | Fighter name on focus, archetype hint after a delay ("Ryu, shoto"). |
| Ultra and costume | Speak `UltraLabel` and `CostumeLabel`, then "confirm". |
| Costume colour | Improve on Tokon's "color 01": speak a colour name. Needs a colour-name table, 44 fighters × costumes × 10 colours. Community-filled plain file; placeholder is the slot number. Not a blocker. |
| Opponent's pick | Spoken online; it is on screen, so it must be audible. |

## In-match cues that work in Tokon

Every cue fires from game state (action ID, status, posture, position), never from SF4's own sound timing.

| Feature | Ember implementation |
|---|---|
| Panned footsteps | Pan by screen X (camera-relative), updated every frame for loops, re-evaluated on side swap. |
| Forward dash | Cue on the first frame of the dash action. |
| Air movement | Same rule for air specials and teleports. |
| Attack strength on swing | Whiff cue per button (LP/MP/HP/LK/MK/HK) plus a stance layer (stand/crouch/jump). |
| Attack strength on hit | Hit cue by strength; counter-hit layer. |
| Hit vs block | Separate hit, block and chip sets. |
| Throws and landing | Throw start, throw tech (distinct), knockdown landing, wake-up actionable. |
| Counter / parry | Focus Attack start, absorb, crumple; cue on frame 1 of startup. |

## Gaps Sven flagged, and Ember's fix

| Gap | Ember fix |
|---|---|
| Random voice cries per move | Consistent-voice mode: SF4 voice cue IDs muted at the hook, one fixed Ember cue per (fighter, action ID). Original Ember sound set, no Capcom audio. |
| Standing and crouching heavy sound alike | Strength + stance layer; crouching attacks lower-pitched or "low" tagged, overheads a rising tag. |
| Jump directions share one sound | Separate cues by direction, panned. Opponent jump-in matters most. |
| Back dash nearly silent | Distinct back-dash cue as loud as forward dash. |
| Hurt voice on block | Suppress defender voice cue IDs while status is blockstun; play only the block cue. |
| Counter sound late | Cue from the state machine on startup frame 1. |
| Crouch and stand silent | Short cue on enter-crouch, another on stand-up. Separate toggle, it is frequent. |
| Speech can't be interrupted | Stop-speech hotkey, interrupt-on-navigate. Phase 0. |
| Colours read as numbers | Colour-name table above. |

## Ember-specific additions

### Netplay and lobby
- Room list, player names, region and ping read as a normal menu.
- Lobby cue list from phase 0.
- Connection quality on demand: hotkey speaks ping and rollback frames ("62 milliseconds, 2 frames rollback").
- Results spoken: winner, rounds, rematch/leave choice with its countdown.
- SF4's own "Round 1, Fight" is already audible; no extra countdown unless testers ask for the exact frame inputs go live.

### SF4 mechanics
- Distance cue: optional low tone, pitch or volume follows the gap; separate cue near the corner.
- Meter: cue when a Super stock fills and when Revenge reaches usable. Read from the Actor getters.
- Focus Attack: start, each charge level, absorb, crumple.
- Wake-up: actionable cue; quick rise vs normal rise distinct.
- Projectiles: looping panned sound that travels with the fireball until it hits, is blocked or dissipates.
- On-demand status: hotkeys speak health, meter, timer and round count for both players without pausing. Actor getters exist.

### Training mode
- Speak FrameMeter's result after each exchange ("plus 2", "minus 6"). Measurement already exists.
- Speak the last input sequence and whether a combo dropped, and on which hit.
- Narrated dummy settings.

### Motor and other disabilities
- Full remapping per device, keyboard and hitbox layouts, one-handed presets. Builds on the keyboard-assignment work already in progress on the fork.
- Hold-to-toggle and adjustable button timing.
- Macros and input assists offline and in training only, or online when both players opt in and the lobby shows it.
- Captions for spoken events; visual flash option for audio-only cues.

## Architecture and rollback-safe cues

Cues come from Ember's reading of game state inside the GGPO advance callback. Each event carries a stable ID `(frame, player, actionID)` so resimulation cannot play a cue twice or play one that never happened.

Rules:
1. **Play on prediction.** Cues fire on the predicted frame; the screen shows the prediction too, so principle 1 holds.
2. **Deduplicate on resimulation.** An event ID already played is skipped while the resimulating flag is set.
3. **Cancel what never happened.** A played event missing from the corrected timeline fades out within ~30 ms.
4. **Trim late arrivals.** An event for a past frame plays with its first (current frame − event frame) × 16.7 ms cut off.
5. **Silence SF4 during resimulation.** Already built: the shadow SoundPlayerManager and SoundReconcile.

Building blocks:
- Audio: miniaudio, WASAPI low-latency, buses per cue category.
- Speech: Tolk → NVDA / JAWS / SAPI fallback.
- Data: per-fighter plain files mapping action IDs to cue names, and costume slots to colour names, so the community can add and fix entries.
- Latency budget: state change to sound start under one frame (16.7 ms) plus device latency. Device latency must be measured and shown in Settings; a test build logs both.

## Sound set

Principle 6 means every cue is an original Ember asset: hundreds of cues (44 fighters × moves) plus lobby and menu cues. Biggest unowned task in this spec. Order: synthesised placeholder set first (one generator, parameters per strength/stance), authored set later. Owner: open.

## Roadmap

0. **#63 core.** Settings struct, `PlayCue` / `Say`, Tolk + SAPI, miniaudio, lobby cues, menu narration off `Focus()`, interrupt and stop, one Settings page. One PR to nightly.
1. **Self-voicing Ember complete.** Every Ember screen, dialog, notice and chat message narrated; button hints; speech on at first run.
2. **Match framing voiced.** Side, stage, fighter, Ultra, costume; VS and result screens; on-demand status hotkeys; connection quality hotkey.
3. **Core match cues.** Event detector with rollback rules; panning; own/opponent timbre; strength and stance cues; hit, block, chip; jumps by direction; both dashes; crouch and stand. Placeholder sound set.
4. **Consistent-voice mode and mixer.** Cue-ID filtering at the hook, one cue per move, per-category volumes, mono, narrow dynamic range.
5. **SF4 mechanics and training.** Focus, meter, projectiles, wake-up, distance; spoken frame advantage and combo drops.
6. **Native SF4 menus** (offline paths) via memory reads.
7. **Motor options.** Remapping presets, hold-to-toggle, offline-only assists.

## Testing

- [ ] Recruit two or three blind fighting-game players early (Sven first); give them phase 0 before anything else.
- [ ] Acceptance per phase: a blind tester installs, configures and plays an online match with no sighted help.
- [ ] Measure cue latency against the frame where the action starts visually; fail above one frame plus measured device latency.
- [ ] Run every cue under the existing rollback stress harness with induced packet loss; dedupe and cancel rules must hold.
- [ ] Blind A/B move identification by ear; any confused pair is a sound-design bug.
