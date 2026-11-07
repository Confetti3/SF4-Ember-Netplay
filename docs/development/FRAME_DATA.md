# Frame data

`assets/framedata/usf4.json` is Ember's own frame data for every fighter: 44 fighters, 2566 moves. The phone remote's Frames tab
reads it (`src/training/RemoteServer.cxx`, `FramesIndex` and `FramesOf`, from `assets/framedata/` beside `Sidecar.dll`). The
in-game frame meter and Data panel read the running game, not this file.

The numbers are measured, not copied. A simulator plays every move from the game's own files and counts frames; the numbers it
gives are then checked against moves played in the game. The SuperCombo wiki is kept only as a second opinion, listed per move
where it differs, and none of its text or numbers is in the file.

## What is in the file

```
{"cols": [...], "meta": {"generated": [...], "sim": [...], "moves": 2566}, "fighters": {"Ryu": [[...row...], ...], ...}}
```

One row per move, one cell per column, every cell text. Columns, in order:

| Column | Meaning |
|---|---|
| `section` | Normals, Command normals, Throws, Specials, Super, Ultra 1, Ultra 2, Other |
| `move` | The name shown |
| `input` | The notation (`cl.LP`, `236+LP`, `236236+LP`) |
| `strength` | `LP` to `HK`, `EX` for a two-button special, empty otherwise |
| `startup` | The first frame the move hits on. A super or ultra is `flash+rest` |
| `active` | The frames a hitbox is live, `3`, or `2(6)2` for two hits with a gap. `-` for a throw or a move with none |
| `recovery` | Frames from the last active frame to the first free one |
| `total` | Whole move, first frame to first free frame |
| `block_adv`, `hit_adv` | Advantage in frames, against a standing opponent, signed. `KD` when the hit knocks down: the advantage then depends on the wake-up and is not shown |
| `hitstun`, `blockstun` | The opponent's stun, with hitstop left out (the wiki's count) |
| `inv` | Invincibility, as `1-9 strike` or `1-5 full, 6-10 projectile` |
| `extra` | Notes the simulator made, for example "projectile: recovery counted from the throw" |
| `measured` | Which numbers the game also showed, with the same value, comma separated (`startup_meter` is the training meter's mark) |
| `script` | The move's script number in the fighter's `.bac`. It names the exact move when two share an input |

How a move is counted (these rules come from the simulator and are the reason a number can differ from the wiki):
- Every count is in the mover's own frames. Its hitstop and a super's freeze are left out.
- Startup is the first live box frame plus one, because the hit registers the frame after.
- Advantage is the opponent's first free frame minus the mover's, with the opponent 0.6 away (far normals just past their range) and a dummy that stands, crouches or blocks as the move needs.
- A move that whiffs a crouching opponent says so in `extra`, and its block advantage is then against standing guard.
- Air normals have no `total` or advantage: they end on landing, which the simulator does not model. Moves that end in the air have a `landing` figure in the simulator's own file, not in this one.

## Where the data comes from

The tools below are **not in this repository**. They live in the maintainer's lab folder (`ember-build\lab\`) because they need the
game's files and a long run. This repository holds the result, a converter, a checker and this page.

1. **The simulator** (`lab/sim/framedata.py`). It reads each fighter's `.bac` (moves) and `.bcm` (commands), plays every listed move against a dummy, and writes `lab/framedata/ember/<CODE>.json` per fighter, plus `ember-frame-data.json` (all of them) and `disagreements.tsv`. `python3 framedata.py [CODE ...]` runs the codes given, or every fighter that has a movebook atlas; all 44 take about four minutes.
2. **In-game calibration** (`lab/framedata/run-fd-pass.sh <n...>`, the probes in `selftest-trials/calibrate-fd`). A real game plays a set of moves and the frame meter's own numbers are read back. A number the game showed and the simulator reproduced exactly is listed in the move's `measured` cell; where they differ, the file keeps the simulator's number and says what the game showed in `extra` ("game showed on_hit 5"), so the difference is visible and can be looked into.
3. **The wiki double-check** (`lab/framedata/usf4/frame-data.json`, scraped from SuperCombo with `usf4-extract.js`). Each per-fighter file keeps what the wiki says beside Ember's number and a `status` (`agree`, `wiki_differs`, ...). A difference is settled by the simulator and a probe in the game, never by taking the wiki's number. `disagreements.tsv` lists them all.
4. **This repository's converter** (`scripts/framedata/build.py`) turns the per-fighter files into `usf4.json` and applies `overrides.json`.

How it got here: the phone's Frames tab first read a scrape of the wiki (3105 rows, 2026-10-20), which was replaced by Ember's own
table on 2026-11-07 so the file is ours and its sources are named. The simulator's files in this snapshot are from 2026-10-21 (`meta` has the date and the simulator's commit hash).

## Validate

```
python3 scripts/framedata/validate.py            # assets/framedata/usf4.json
python3 scripts/framedata/validate.py other.json
```

It checks the shape (the columns, a cell for every column, all text), that each count is a number or `-`, that an advantage is
`+n`, `-n`, `KD` or `-`, that `total` is not shorter than `startup`, that `measured` names real numbers, and that no move is listed twice. It
runs as the CTest `FrameData` (with Python 3). It finds a broken edit, not a wrong frame.

To check a number against the game: open Training, play the move on the dummy and read the frame meter and the Data panel (F6, the
HUD page), or run the calibration probes above. The meter's startup mark and the file's `startup` can differ by one on a move
whose hit registers a frame late; `measured` only says they were compared.

## Edit one number

Do not edit `usf4.json` by hand if the number should survive a rebuild. Put the correction in `assets/framedata/overrides.json`:

```json
{
  "Ryu": [
    {"input": "cl.LP", "set": {"startup": "4", "extra": "checked in game 2026-11-07"}, "why": "meter shows 4"}
  ]
}
```

- The key is the fighter's name as in `usf4.json`. `input` plus the optional `"script": 256` pick the move, and exactly one move must match or the build stops.
- `set` holds the cells to replace, by column name, as text. `why` is for the reader of the diff; the build ignores it.
- Run `scripts/framedata/build.py` (below), then `validate.py`, and commit `overrides.json` and `usf4.json` together.
- Keys starting with `_` are ignored, so a note can sit in the file.

## Update after the simulator changes

```
cd <lab>/sim && python3 framedata.py            # or framedata.py RYU KEN for some
cd <repo> && python3 scripts/framedata/build.py <lab>/framedata/ember
python3 scripts/framedata/validate.py
```

Read the diff of `usf4.json` before committing: it is one line per fighter in the file, so use
`git diff --word-diff-regex='[^,]+'` or compare with a JSON tool. A rebuild with no simulator change must change nothing.

## Add a fighter or a move

- **A fighter:** the simulator needs the fighter's movebook atlas (`selftest-trials/atlas`, built from the `.bac` and `.bcm`; see `lab/sim/DATA-ONLY-PLAN.md`). With it, `framedata.py <CODE>` writes the file and the build picks it up. The fighter's name comes from the file's `fighter` field.
- **A move:** the simulator lists a move when the movebook has a row for it (one per input and script, no rapid-fire copies). A move that is missing is a movebook or listing problem, fixed in the simulator, not an override. An override cannot add a row, only change one.
- **A column:** add it to `COLS` in `build.py` and `validate.py`, fill it in `row()`, and show it in the phone page (`FC` for the grid, `FX` for the details a tap opens, in `src/training/RemotePage.hxx`). The server passes the columns through, so nothing else changes.

## Known gaps

In this snapshot 1205 of the 2566 moves have a number the game also showed, and 90 carry a known difference (`game showed ...` in `extra`). At the 2026-10-21 snapshot (`meta` has the date): airborne specials' block advantage (Shoryuken, tatsumaki, Rising Jaguar), the stun
count of multi-hit moves (the wiki counts Shinryuken block -94, the simulator -70), charge dashes' distance, and a few far normals that
hit in the simulator where the game whiffs. Fighters added after the base roster (GKX, JRI, HUG, PSN, RLN) agree least with the game. A move
is only as good as its `measured` cell: an empty one has not been compared with the game, and `game showed ...` in `extra` is a known difference.

## Files

| Path | What |
|---|---|
| `assets/framedata/usf4.json` | The table, shipped beside `Sidecar.dll` (listed in `src/common/PackageInventory.inc`) |
| `assets/framedata/overrides.json` | Hand corrections that survive a rebuild |
| `scripts/framedata/build.py` | Per-fighter simulator files to `usf4.json`, then overrides |
| `scripts/framedata/validate.py` | The shape and number checks, also CTest `FrameData` |
