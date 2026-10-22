# Native reset and the retained seed

The supported target is Steam USF4 1.05, `SSFIV.exe` SHA-256
`5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb`,
image base `0x400000`. The addresses below are image-relative unless marked
as object offsets. The adapter verifies executable instruction signatures
before advertising support.

On 20 October 2026 the installed executable was checked read-only with
MSVC dumpbin. REA's doctor reported no configured analysis engine and a
missing Codex registration, so this is static disassembly evidence, not
an REA analysis or a new in-game run. The earlier
[RNG analysis](../reports/2026-10-10_reverse-USF4-stage-quality-report.md)
independently describes the same reset and System memento.

## What the true argument does

`Restart` at RVA `0x1db480` takes an optional replacement request and a
boolean. The adapter supplies `nullptr, true`: it keeps the loaded request,
fighters and stage. At `0x1db540` the native code tests that boolean. If true:

1. At `0x1db546` it reads the word at `System+0xcd4`.
2. At `0x1db54c..0x1db556` it passes that word to virtual slot `+4` of the
   EmRandom object at `System+0x50`, reseeding its evolving value at `+0x54`.
3. If the unit at `System+0x84` exists, `0x1db558..0x1db568` passes the same
   word to its virtual slot `+0x38`.
4. The function then queues the native restart through `0x1da220`.

The true flag therefore does reseed native objects. It does not preserve
the current evolving EmRandom value. Calling this path with false would
leave that evolving, potentially different value in place.

`+0xcd4` lies inside the retained menu block
`[System+0x6d8, System+0xd10)`, at offset `0x5fc` in that block. The adapter
copies that block at bind and compares it before a fighter emits Commit.
This includes the seed word; it does not read a fresh local clock or choose
a new random seed. The original room request supplied the agreed match data.
Static initialisation code at `0x1dae98..0x1dae9f` also seeds EmRandom from
the incoming settings word at offset `0x5fc`.

## Saved versus invariant state

The System memento at `0x1d7b80` receives the System's secondary base at
`System+4`. Its first copies cover System offsets `+0x10..+0x4f`, then
continue at `+0xdd0`. Restore at `0x1d7d80` writes the same ranges. Neither
the retained seed at `+0xcd4` nor the evolving EmRandom value at `+0x54`
is saved by those copies. Ember's additional SaveState fields do not add
either value.

The retained word is an immutable match-configuration precondition checked
by the adapter, not a new rollback field. The evolving value is presentation
state already excluded by [DESYNC_V2.md](DESYNC_V2.md). These facts are not
proof that replaying the whole native restart across arbitrary rollback is
safe: it also changes native replay, sound and memento owners.

The contribution instead holds the decision past GGPO's prediction window
and retains identical Commit inputs until the reset epoch is confirmed.
A monotonic native reset boundary also refuses loads from earlier frames
before any native restore, covering GGPO's terminal-disconnect adjustment
which can replace a disconnected fighter's retained input with zeros.
The complete input argument and its limits are in
[CONTINUOUS_REMATCH.md](CONTINUOUS_REMATCH.md). Synthetic vote/latch tests
cannot certify native ownership or presentation; current-revision multi-PC
testing with delay and confirmed hashes remains required before integration.
