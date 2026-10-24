# Local music selection

Settings and the room menu expose separate menu/lobby and online-match music choices.
Offline Versus, Arcade and Training keep their native music. Spectating an
online room match uses the local online-match choice.
The catalog describes the game's 28 Versus stage banks and 44 character stereo
banks; the ordered chooser includes only banks installed on this PC. The
immutable list is built once and shared by display and directional stepping.
No music files are distributed by this change. Choices are persisted
with the existing player preferences and affect only the local presentation.
The existing `lobbyMusic` preference key is retained so saved choices carry
over; its scope now includes the title screen and menus before joining a room.

The UI sends one local presentation request for music and controller layouts. The game thread rechecks that it is
at a safe menu before accepting it, preserving all unrelated preferences.
Menu playback uses the native background music manager's load, ready, play
and release operations. A failed bank falls back once to the default; a new
selection allows retrying. Once the event system is ready, a saved theme is
applied at Title and the known UI-only foreground events. Room entry/exit
does not change the selection or restart an already loaded bank. A native
release invalidates the bank so returning from a fight loads it again. Random
menu music resolves once at startup and again after that native release; it
does not roll on each menu frame. The default preference remains unchanged.
Boot logos, movies, unknown/transitional events and modes containing battles
are excluded. A pending menu load is never started while a match is busy.

Match selection replaces only the battle BGM loader's bank. Character banks
have one cue, while stage banks have three; the BGM constructor must select
the corresponding player mode. Otherwise a character theme can go silent
when the native stage music changes phase. `MusicPlaybackTest` checks that
mapping for every character entry.

Random resolves from installed banks only after GoToVersus accepts the room
match entry, using a local RNG independent of the gameplay seed. A deferred
entry does not roll or arm anything. The resolved local track travels in
NextRequest and is consumed once when the native battle request is prepared.
The native BgmPlayer destructor clears its audio choice; returning to a safe
menu also retires an abandoned load that never constructed that player. There
is no separate online-match flag or room-lifecycle reset list. The stored preference
remains Random. The optional `Music::ObserveMatch` integration accepts a
monotonically increasing, confirmed game token on the outer game tick.
At a confirmed result it preloads another installed bank, excluding the
current track when alternatives exist. Once the next game is confirmed,
it replaces the BGM cue storage using the native constructor and cleanup.
Loading never controls network readiness or pauses the simulation. A bank
or sound-pool failure keeps the previous track.

The native BgmPlayer address stays unchanged. Its queued fade functors hold
that owner and a volume axis (0/1), not a cue handle, bank handle or pointer
to its cue-entry array; their callbacks read the current array and count.
Stage crossfading likewise loops over the current array. The adapter waits
until no noncancelled BGM functor currently owns the player, and does not
cancel or modify tasks. Cue storage is allocated and checked before the
old storage is released. The original scene bank remains owned by the game;
the adapter owns at most its active and pending replacement banks. Native
player teardown releases cue players before these owned banks are freed.

This API is independent of the continuous-rematch implementation. A combined
build must provide its confirmed boundary explicitly; predicted reset
callbacks must never call it. Without that integration, the normal
one-bank-per-scene behavior remains. Native verification of successive random
rematches, both character/stage cue-mode transitions, rollback, cancellation
and cleanup is still required before publishing this addition.

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

This module owns menu music, including Random, when integrating #76. That
PR's parallel MenuMusic patch must not be installed alongside it: it changes
the original path this loader recognizes. MusicPanel owns its distinct
music-menu/music-online row ids, so it does not intercept another panel's
lobby-music row. The native byte and readable/executable page checks live in
platform/NativeSignature.hxx, shared with the rematch contribution.

All 14 catalogs contain translations. The CJK subsets were regenerated;
native speakers should review the new wording. Automated tests cover bank
cue policy, custom-stage precedence, file-layer ordering, persistence and
menu navigation, startup/menu playback policy and battle/transition exclusion;
they do not exercise the native audio engine. Repeat the music checks on this
nightly revision before merging, including a saved theme after relaunch and
Title, MainMenu, Option, room and battle transitions.

Development was AI-assisted with Codex. Flavio directed the feature and
performed the predecessor's manual gameplay tests.
