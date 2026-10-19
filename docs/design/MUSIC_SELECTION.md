# Local music selection

Settings and the room menu expose separate menu/lobby and match music choices.
The catalog uses the game's 28 Versus stage banks and 44 character stereo
banks. No music files are distributed by this change. Choices are persisted
with the existing player preferences and affect only the local presentation.
The existing `lobbyMusic` preference key is retained so saved choices carry
over; its scope now includes the title screen and menus before joining a room.

The UI sends a local settings command. The game thread rechecks that it is
at a safe menu before accepting it, preserving all unrelated preferences.
Menu playback uses the native background music manager's load, ready, play
and release operations. A failed bank falls back once to the default; a new
selection allows retrying. Once the event system is ready, a saved theme is
applied at Title and the known UI-only foreground events. Room entry/exit
does not change the selection or restart an already loaded bank. A native
release invalidates the bank so returning from a fight loads it again.
Boot logos, movies, unknown/transitional events and modes containing battles
are excluded. A pending menu load is never started while a match is busy.

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

## Upstream integration

Music saves are local runtime requests, like controller setup and replay
commands. They never enter SessionController or mutate the room. Layouts
and music share one local-presentation save rule. The installed-file layer
order is shared with portraits and Training command lookup; its per-fighter
1.11/1.10 regulation choice is preserved. Stage IDs are validated against
the stock catalog, not a second list of numeric ranges.

A custom stage with an installed music bank keeps both its bank and native
cue mode. A custom stage without music uses the selected local override.

This module should own the menu music path when integrating #76. That PR's
parallel MenuMusic patch and lobby-music row must not be installed alongside
this one: it changes the input path this loader currently recognizes. The
future Random menu choice belongs in this catalog and playback owner, with
one resolved track retained until a match ends. The existing allowRandom
choice helpers support that extension. Menu playback now starts at Title
using the saved fixed choice, following Flavio's requested scope extension.

All 14 catalogs contain translations. The CJK subsets were regenerated;
native speakers should review the new wording. Automated tests cover bank
cue policy, custom-stage precedence, file-layer ordering, persistence and
menu navigation, startup/menu playback policy and battle/transition exclusion;
they do not exercise the native audio engine. Repeat the music checks on this
nightly revision before merging, including a saved theme after relaunch and
Title, MainMenu, Option, room and battle transitions.

Development was AI-assisted with Codex. Flavio directed the feature and
performed the predecessor's manual gameplay tests.
