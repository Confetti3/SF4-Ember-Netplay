# Local music selection

Settings and the room menu expose separate lobby and match music choices.
The catalog uses the game's 28 Versus stage banks and 44 character stereo
banks. No music files are distributed by this change. Choices are persisted
with the existing player preferences and affect only the local presentation.

The UI sends a local settings command. The game thread rechecks that it is
at a safe menu before accepting it, preserving all unrelated preferences.
Lobby playback uses the native background music manager's load, ready, play
and release operations. A failed bank falls back once to the default; a new
selection allows retrying.

Match selection replaces only the battle BGM loader's bank. Character banks
have one cue, while stage banks have three; the BGM constructor must select
the corresponding player mode. Otherwise a character theme can go silent
when the native stage music changes phase. `MusicPlaybackTest` checks that
mapping for every character entry.

Random resolves from installed banks when a match is entered from the lobby,
using a local RNG independent of the gameplay seed. The stored preference
remains Random. A continuous rematch retains the loaded bank; choosing a
new random bank for each rematch would require a separate audio reload path.

The native hooks target Steam USF4 1.05 and verify instruction signatures
before attachment. Other layouts leave selection unavailable. The native
audio hooks and loading fallback still need review in the game; unit and
UI tests do not exercise the native sound engine.
