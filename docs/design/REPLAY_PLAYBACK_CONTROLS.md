# Replay playback controls

Pause, frame step and slower speeds while the game's Battle Log plays a replay, with the frame meter and live input lanes over it. This is version one of the plan in `REPLAY_PLAYBACK_CONTROLS_DESIGN_2026-10-09.md`, kept outside the repository (work packages 1 to 6). Seek and rewind are not part of it. The player's side is in `docs/guides/USER_NETPLAY.md`, under "Replay controls".

Nothing here has been run in the game yet. The mechanism is read from the code; the checks that need the game are listed at the end.

## Mechanism

The game's own half speed is the replay cadence function `0x5D7750`. `SysMain_UpdatePauseState` (`0x5DBAA0`) calls it after the pause menu check. In a fight with the slow-motion flag (`System+0x1464`) set, it sets the freeze bit `0x8` of the simulation flags (`System+0x1454`) on one call and clears it on the next. `0x5DBAA0` then suspends or resumes the simulation tasks when the flags change, as it does for the pause menu, and the replay cursor moves only on frames that run.

Ember detours `0x5D7750` (`sf4e__ReplayPlayback.cxx`). During playback in the fight it sets or clears the freeze bit itself, once a call, from its transport. A call without the bit plays one frame. So 1/2 is release, hold; 1/4 is release, hold, hold, hold; a pause holds every call; and a step releases one call. The frame limiter, `BattleUpdate`, the GGPO gate and the developer gate are untouched.

## When Ember acts

`replaytransport::Decide` (`common/ReplayTransport.hxx`) picks one of three routes on each call:

- **Original:** the game's function, unchanged. Taken with a GGPO session, when the recorder is not playing back (`ReplaySystem+0x708` is not 1), in Training, outside the fight, and while the pause menu is open (bit `0x1`). With the slow-motion flag set the game's function toggles the freeze bit rather than clearing it, so during playback Ember first takes back a half speed it wrote itself (`replaytransport::Relinquish`) and the game's function then clears the bit: the introduction, the knockout, the round's end and the pause menu run at 1x, and the transport keeps the speed the player chose for when its own cadence resumes. A half speed the player set with Select there is the game's and stays.
- **FullSpeed:** a video export (`replaystore::Exporting()` or the capture recording). The slow-motion flag is cleared, then the game's function runs, so an export always plays at 1x.
- **Transport:** everything else, which is a replay in the fight with the pause menu closed.

With netplay, the detour reads only the GGPO pointer before it calls the game's function. Versus, Arcade and Training add one read of the recorder's mode. None of them writes anything.

The guard does not require that Ember started the replay. Replays picked from the game's own list get the controls too. Only the input lanes need Ember's copy of the file.

## Transport

`replaytransport::Transport` holds playing or paused, a speed divisor (1, 2 or 4) and a bounded count of frames to step. Step while playing pauses without playing a frame. A speed change releases on the next call.

The game's Select still toggles its half speed in `0x484170`, which flips the slow-motion flag before the cadence runs. While Ember drives the cadence it writes the flag back as "slower than 1x" every call and remembers what it wrote. A flag found different from that is a Select press: set means 1/2, cleared means 1x. Select keeps its meaning and the game's legend agrees with Ember's speed.

Each round starts playing at 1x: the transport resets, and the flag is cleared, while the battle flow is ROUND_START or READY and whenever the recorder's round changes. The battle start and close reset it too. "Play again" starts a battle without a known close, so the battle start also clears the flag when a replay is playing back.

Each battle's playback is a session. Its start and its close begin a new one, which drops the commands still waiting, the pad's held buttons and repeat, the transport, the meter's gate and the strip's first showing; only the input lanes' visibility is kept. Every command carries the session it was made under (`View::session`), and the cadence takes only those of the current session, so a key pressed over an older snapshot never acts on the next playback.

The round, the cursor and the rounds' frames are observed after each battle update (`replayplayback::AfterUpdate`, `replaytransport::ObserveUpdate`), once the Command unit's CMD POST has moved the cursor, never at the cadence call, which runs before it. The strip, the lanes and an export's progress read that observation.

## Frame meter (P8)

The meter was drawn only with a GGPO session, so "Watch with frame meter" showed nothing offline. It is now drawn during playback when the replay asked for it, and F5 turns it on or off. `FrameMeter::Observe` starts over on any frame that does not follow the last, and a held frame repeats the frame count, so the meter is fed only when the battle's frame counter moved (`replaytransport::AdvanceGate`). This also keeps the meter whole under the game's own half speed.

## Input lanes

When a Watch request plays, `replaystore::PlayingFile()` names the archived file. The replay detail worker reads it off the game thread and builds its lanes with it (`replaylane::Build`, once, into `Detail::lanes`); the playback asks as a requester of its own (`platform::replays::DetailFor::Playback`), so its request and completion never meet the Replays screen's, and only takes the finished lanes. The overlay looks up up to 14 rows a side by the recorder's round (`+0x710`) and the frame just played, `cursor - 1` (`+0x714`).

## Input

- **Keyboard:** F1 to F5 and F9, read with `ImGui::IsKeyPressed` as the training keys are. F2 repeats with `GetKeyPressedAmount` (300 ms, then 10 a second). The window procedure keeps those keys from the game during playback, plain presses only, so Alt+F4 still reaches it. Training's own keys and their swallowing are off during playback. Text input and Alt suppress the keys.
- **Pad:** the game thread reads the player's XInput pad's physical buttons once a tick (`Dimps::Pad::ReadController`) and turns RB, RT, LB and LT edges into commands (`replaytransport::PadControls`). Start and View stay the game's. DirectInput pads are not read.
- Commands go through one bounded queue and act at the next cadence call. Transport commands outside the fight are refused and counted, which shows the "Controls work during the fight" chip. Under the pause menu and during exports, every command is dropped.

## Drawing

`ui::DrawReplayStrip` and `ui::DrawReplayLanes` (`ReplayPlaybackHud.cxx`) are passive windows drawn from `DrawOverlayLayers`. They hide under Ember's windows, the training controls and the game's pause menu, and are not drawn during exports. The strip sits under the frame meter's band, between the super meters, at most 460 by 30 pixels at 720p. The lanes start below the life bars and the name logos.

## Files

- `common/ReplayTransport.hxx`: transport, route, round boundary, meter gate, pad and key mapping, strip fade, clock.
- `common/ReplayInputLane.hxx`: lanes from a parsed replay.
- `common/ReplayRecorder.hxx`: named `mode`, `round` and `cursor` fields.
- `Dimps__Game__Battle__System`: `ReplayCadence` (`0x5D7750`) and `GetSlowMotion` (`+0x1464`).
- `sf4e__ReplayPlayback.cxx`: the detour, the queue, the pad, the published view.
- `ui/ReplayPlaybackHud.cxx`: the strip and the lanes.

Tests: `ReplayTransportTest`, `ReplayInputLaneTest`, `ReplayRecorderTest`, and the `replay-strip-*` and `replay-lanes` UiRender shots.

## Checks that need the game

Section 2.6 of the plan, its open questions in section 7, and these:

- `ReadController` reads the pad cleanly in the middle of a battle.
- The game's "BATTLE CHECK SLOW" task still runs while Ember holds the freeze bit, so Select folding works while paused.
- F5 feeds the meter on a "Watch now" replay.
- The lanes' `cursor - 1` lines up with what the fighters do.
