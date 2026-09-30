# Saved replay corruption (ledger F-007)

Analysis date: 30 September 2026. Tools: IDA / Hex-Rays through the local idalib MCP worker on the installed `SSFIV.exe` (same build as the 2026-09-28 reports), and a read of `sf4-current` at cbe4df5. Nothing here has been tested in a running game. The IDB was not modified.

## Summary

The game records a replay one input record per simulated battle frame, from inside the battle update. During netplay, Ember's rollback re-runs the native battle update for every resimulated frame, and the replay recorder is not part of Ember's save states. So each rollback of N frames leaves N extra records in the recording: the mispredicted frames stay, and the corrected frames are appended after them. On playback the game reads one record per frame, so from the first rollback on, every input is late by the accumulated rollback depth and the match plays out differently. That is the "damaged" replay.

This explains replays saved after Ember netplay matches. Ember runs those as native Versus battles (`GoToVersusMode` with `NBT_PVP`), so the game treats them as local replays. A plain offline Versus match with Ember loaded runs one battle update per frame and should record cleanly. No offline cause was found.

## Native recorder

`Dimps::Game::Battle::ReplaySystem` is a static singleton at `0xAA7228`, returned by `0x5D6700`. It lives outside the battle `System` object, so no memento (native or Ember's) covers it.

| Address | Vtable offset | Role |
| --- | --- | --- |
| `0x5D61E0` | +0x04 | Allocates 7 round streams, 15,360 entries each |
| `0x5D5AC0` | +0x08 | Frees the 7 streams |
| `0x5D5BD0` | +0x3C | Round start: records or restores the round start state and seed |
| `0x5D5CA0` | +0x40 | Start playback of the current round |
| `0x5D65A0` | +0x44 | Start recording the current round (resets its stream, mode 2) |
| `0x5D5CE0` | +0x48 | Stop: flushes the stream, mode 0 |
| `0x5D5D20` | +0x4C | Per frame: record or play one input pair, via the function pointer at +0xADC (`0x5D6000`) |

`0x5D6000` packs each side's input into 11 bits (low 8 bits plus `0x400`, `0x800`, `0x2000`) and appends one 22-bit value per frame.

The per-frame call is in `0x59AA20`, the Command unit's "CMD POST" task registered by `0x59AC20`. It reads both sides' inputs, calls `ReplaySystem+0x4C`, and in playback writes the recorded inputs back. That task runs on every `BattleUpdate`.

Each stream wraps an RLE codec (type 4, vtable `0x9D784C`). Records are 3 bytes: a 22-bit value, or a repeat count flagged with `0x400000`. Append is `0x7831C0`, flush is `0x783180`. The codec never rewrites bytes it has written, and it drops frames silently once the 61,440-byte buffer is full.

## Ember side

`fSystem::ggpo_advance_frame_callback` (`sf4e__Game__Battle__System__Ggpo.cxx`) calls the undetoured native `BattleUpdate` for each resimulated frame, so each one reaches CMD POST and appends a record. `SaveState::Save` and `Load` cover memento keys, sound state and battle flow globals, but nothing in the recorder. Nothing in `src/` references the recorder.

## Fix

Each save state keeps a copy of the recorder, and loading a state puts it back (`src/common/ReplayRecorder.hxx`):

- The recorder object, `0xAE0` bytes, by value. Apart from the seven stream wrappers it is plain data: the header, round records, mode, round, cursor and the per-frame function pointer.
- Each stream's codec, `0x20` bytes. Its fields are frames, bytes, base, write cursor, capacity, last value and pending repeat count.

Stream bytes are append-only, so restoring the cursors truncates each stream exactly, and the buffers are never copied. `SaveState::Save` captures the recorder next to the `GameManager` copy. `SaveState::Load` restores it first, before any other live state, so the legacy round-trip release (which uses `CopyIntoPlace` only to release mementos) leaves the recorder alone. The engine frees and reallocates the streams between sessions (`+0x08`) and a save state never outlives its battle, so a stream whose codec or buffer is not at the saved address means the state is from another session: `Load` then changes nothing and fails, and its callers already handle a failed load (GGPO ends the match). `ReplayRecorderTest` replays the engine's RLE append at the real offsets and checks that a rolled-back recording matches a clean one byte for byte. It runs in 32-bit builds only, since the layout is the 32-bit engine's.

## Verification

On one PC: enable Versus replay recording, play a round with `SF4E_ROLLBACK_STRESS` set, save the replay and play it back. Before the fix it should diverge from the first forced rollback. After the fix it should match. An offline round without stress should play back correctly in both builds. The ledger's stock against Ember offline split is still worth one run to close the offline half.

## Residuals

- If a match ends while its last frames are still predicted, those frames are never corrected, so the tail of the recording can be slightly wrong.
- The round buffer holds 61,440 bytes and the codec stops recording when it is full. Before the fix, duplicate records filled it faster.
