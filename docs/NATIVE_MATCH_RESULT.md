# Native match-result reader

The room score source is the native GameManager result state.  The observer
does not infer a winner from health, HUD text, actor position, or the
developer `Report win` path.

The target is the installed Steam USF4 binary:

- Steam app 45760, buildid 834219
- `G:\SteamLibrary\steamapps\common\Super Street Fighter IV - Arcade Edition\SSFIV.exe`
- SHA-256 `5D724595A8AB3C6C6D6F4959187F756F5BE35BB497E51E5233C4E73B18B0B9EB`
- x86 image base `0x00400000`

The following native addresses are image-relative bindings in
`Dimps::Game::Battle::GameManager::Locate`:

| Vtable slot | RVA | Meaning |
| --- | ---: | --- |
| `GameManager + 0x30` | `0x001D1370` | returns `[GameManager + 0x418]`, the native result selector (`-1` selects draw) |
| `GameManager + 0x4C` | `0x001D1430` | returns `[GameManager + 0x41C]`, the current round-winner index consumed by the result HUD |

The native flow transition at absolute virtual address `0x005DD93A` (RVA
`0x001DD93A` from the x86 image base) calls the `+0x30` reader.  It selects
native `BF_MATCH_RESULT` (`5`) when the value is not
`-1`, otherwise `BF_DRAW_RESULT` (`6`).  The result HUD path at
`0x005DF8E0` calls the separate `+0x4C` reader and compares that index with
the System player iteration index.  The matching actor receives the winner
HUD code; the other actor receives the loser code.  The `+0x4C` reader's
field is populated by the round-result setup, so this proves its `0`/`1`
values are player-array indices without making it the match-score source.
SF4E preserves that ordering through the GGPO player and native conditions
arrays.

The observer uses the native `+0x30` selector/index because it is written by
the match-level round-score finalizer and is the field consumed by the
native match/draw flow.  It does not use the separate `+0x4C` round-winner
HUD field.  This keeps winner identity tied to the engine's native result
object while avoiding any presentation text or developer report path.

Capture and publication are separate (`src/common/NativeMatchResult.hxx`):

- **Capture.** Every GGPO save callback, including saves made while
  re-simulating after a rollback, records the native flow and winner index
  against the GGPO state frame in a 64-entry `Timeline`. A state load rewinds
  the timeline to the restored frame, so outcomes from a discarded prediction
  are dropped and corrected saves replace them.
- **Publication.** `NotifyRuntimeMatchResult` receives the newest captured
  outcome whose inputs GGPO has confirmed: save frame `N` qualifies once
  `ggpo_get_last_confirmed_frame >= N - 1`. The once-emitted latch is per match
  and is not rollback state.

Publication is attempted after every successful outer `ggpo_advance_frame`,
after every application tick's `ggpo_idle`, and once more in `CloseBattle`
before the session is retired. The poll entries matter at the end of a fight:
local simulation stops, but the peer's inputs for the result frames can still
be confirmed by a plain GGPO poll. Before v0.8.6 publication happened only
after an advance, so a result confirmed after the last simulated frame was
never sent. That fighter reported only `MatchFinished`, and the table paused
after 30 seconds. `CloseBattle` logs `outcome_emitted`, the newest candidate
and its frame, and the confirmed input frame, so a missing report can be
traced to capture or to confirmation.

Frame identity is the monotonic GGPO state frame, never the native simulation
counter: `Dimps::Math::FixedPoint::integral` is a signed 16-bit field and
wraps after 32767 frames in a long match.

The index mapping is also present in the native object accessors: System's
`+0xB4` accessor at absolute `0x005D72D0` asks the Chara Unit for an actor,
and the Chara Unit `+0x40` accessor at `0x00565D10` returns
`playerActors[index]` for indices `0` and `1`.  This is why the observer maps
the native `+0x30` match winner index directly to room P1/P2 slots while
preserving the existing SF4E player/conditions ordering.

The producer/write path distinguishes the two fields.  GameManager
initialization at `0x005D2F84-0x005D2FD2` sets both `+0x418` and `+0x41C` to
`-1`.  The native result finalizer at `0x005D3EF0` iterates the player
records with `ebx = 0..count-1` and asks System's `+0xB4` accessor for the
corresponding Chara actor.  It clears `+0x418` at `0x005D4198`, compares
each player's round-win total to the target in the loop beginning at
`0x005D41C0`, and selects that same player index into `+0x418` at
`0x005D41CA`; its final chooser result is stored at `0x005D4211`, while the
draw branch explicitly stores `-1` at `0x005D4223`.  Separately, the round
result setup at `0x005D16E0-0x005D1761` stores its current-round player
parameter in `+0x41C` and uses that same index to fetch an actor.  Together
with the HUD comparison, this proves that `+0x30` exposes the match-level
winner/draw selector and `+0x4C` exposes the last-round winner index; they
are related native fields with distinct roles, not interchangeable aliases.

The confirmation bound follows the pinned GGPO source at
`C:\Users\Kate\Desktop\sf4\vcpkg\buildtrees\ggpo\src\c88b667-e764611353.clean`:

- `src/include/ggponet.h:34` defines `GGPO_MAX_PREDICTION_FRAMES` as `8`.
- `src/lib/ggpo/sync.cpp:58-64` rejects a new local input when
  `framecount - last_confirmed_frame >= max_prediction_frames`.
- `src/lib/ggpo/sync.cpp:47-54` updates the confirmed frame and discards
  older input.
- `src/lib/ggpo/sync.cpp:137-141` increments the GGPO frame only after the
  deterministic frame has run.
- `src/lib/ggpo/backends/p2p.cpp:102-145` computes the minimum remote
  confirmed frame and feeds it to `SetLastConfirmedFrame` during polling.

The confirmed-frame accessor itself is added by
`vcpkg-ports/ggpo/confirmed-frame-accessor.patch`. Because publication waits
for GGPO's own confirmation boundary rather than an age heuristic, a terminal
native state from a discarded timeline cannot become a score.
