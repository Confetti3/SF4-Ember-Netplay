# Upgrading production to the 1.6.0 bridge

What this upgrade does to the production database, in what order to do it,
and how to step back. The bridge runs migrations 060 to 065 (fighter levels,
friends, account points, award counters, player cards, the class bonus) as it
starts, each in its own `IMMEDIATE` transaction, and refuses to start on a
database it does not recognise without changing anything.

Read this through before starting; do the steps in order.

## Facts the order rests on

- **`setup.sh` does not back up the database.** It keeps the previous binary,
  scripts, `bridge.json`, units, nginx site and zone, and puts them back if the
  new bridge does not start or answer. The database, secrets and bridge ID it
  leaves where they are. The daily backup (`ember-bridge-backup.timer`) may be
  up to a day old. Take a backup yourself immediately before the upgrade.
- **The only safe rollback target is the bridge build production runs
  today** (the shared library's bridge). It records migrations by number only
  and ignores the 060 to 065 records it does not know, and its
  `INSERT INTO rated_games (room_id, table_id, generation)` still fits 060's
  table (`game` defaults to 0). Nothing it does needs a restore.
- **Not the 1.5.7 bridge of the release branch.** It starts, but its rating
  upsert says `ON CONFLICT (discord_user_id)`, which no longer matches 058's
  key `(game, discord_user_id)`: every rated game fails, and it fails inside
  the one transaction that polls every room, so the room list freezes.
- **Both room supervisors first.** A supervisor before this range drops a
  room's whole games array over 6,144 bytes; a busy 1.6.0 room can report up
  to 8,192 (32 games with fighters). Its games would then be neither rated nor
  counted, and nothing logs it. The VPS supervisor and server1's must both run
  this range's `ember-rooms` before any 1.6.0 room host build is installed on
  either. Keep the VPS supervisor's SFV hosting working while you do, or move
  SFV off it first.
- **Other games' rows are cleared with the bridge stopped.** This bridge is
  USF4's alone and refuses a database holding rows of another game in
  `rooms`, `matches`, `lobbies`, `tournaments`, `skill_ratings`,
  `rated_pairs` or `ranking_choices` (the SFV rows the library's 057 to 059
  allowed). `clear-sfv-rows.py` (the shared library's `server/sfv-bridge`)
  removes them; run it only while no bridge has the database open.
- **Refusals change nothing.** Every refusal at start (another file recorded
  under a number, an unnamed record above 059, other games' rows) is checked
  before any migration runs, in a transaction that is rolled back. Its message
  says what to do: start the previous bridge build again, or restore the
  backup taken before the upgrade, or clear other games' rows and start again.

## Order

1. **Rehearse** on a copy of the production database (take one with
   `backup.sh`, copy it to the runner): import the SFV rows into the SFV
   bridge, run `clear-sfv-rows.py`, start the 1.6.0 bridge on the copy and
   check it starts and answers. `clear-sfv-rows.py` commits only when a whole
   database `PRAGMA foreign_key_check` is clean, and orphan `rated_games` rows
   from deletions made with keys off (sqlite3 work such as the stuck-match kill
   script) make it roll back: find and remove those on the copy first, and
   write down what you removed to do the same on production.
2. **Build** the 1.6.0 bridge binary on the runner (`cargo build --release
   --locked -p ember-bridge`) and copy it to `~/ember-bridge/bin/` on the VPS.
   Keep a copy of the binary production runs today:
   `cp /usr/local/lib/ember-bridge/ember-bridge ~/ember-bridge-rollback/`.
3. **Upgrade both room supervisors** (VPS and server1) to this range's
   `ember-rooms` with `sudo bash ~/ember-rooms/setup.sh`, staging no 1.6.0
   room host build yet. Check each answers `GET /limits`.
4. **Move SFV's rows** to the SFV bridge if they are not there yet (the
   library's `docs/design/SFV_BRIDGE.md`): its import reads them from this
   database, so it comes before the clean-up.
5. **Stop the bridge**: `sudo systemctl stop ember-bridge`.
6. **Back up the database**: `sudo systemctl start ember-bridge-backup.service`
   (runs `backup.sh` as the bridge's account, with an integrity check). Note
   the stamp of the newest `bridge-*.sqlite3` in `/var/backups/ember-bridge/`.
7. **Clear other games' rows**, the bridge still stopped: run
   `clear-sfv-rows.py` on `/var/lib/ember-bridge/bridge.sqlite3` as the bridge's
   account. If it rolls back, start the old bridge again
   (`sudo systemctl start ember-bridge`) and stop here: nothing changed.
8. **Install the 1.6.0 bridge**: `sudo bash ~/ember-bridge/setup.sh`. It starts
   the bridge, which runs 060 to 065. If it refuses to start, read the message
   in `journalctl -u ember-bridge`; `setup.sh` has already put the previous
   binary back and started it.
9. **Check** `GET /v1/capabilities` lists `points`, `points_class_bonus`,
   `friends` and `profiles` where the configuration turns them on, the room
   list moves, and `journalctl -u ember-bridge` shows no `database error`.
10. **Only now** install the 1.6.0 room host builds on the supervisors
    (`builds/<build_id>/` staged, `setup.sh` again) and publish the game.

## Stepping back

- **Bridge only**: copy `~/ember-bridge-rollback/ember-bridge` to
  `~/ember-bridge/bin/ember-bridge` and run `sudo bash ~/ember-bridge/setup.sh`.
  The database stays as 1.6.0 left it; the old bridge ignores the new tables
  and records. Do not use a 1.5.7 release-branch build (see above).
- **Room hosts**: the supervisors keep running rooms of every installed
  build; a 1.6.0 room host build can be pruned (`setup.sh --prune` without it
  staged) once its rooms end. Supervisors from this range stay: they accept
  older hosts.
- **Restore** the backup of step 6 (`sudo bash
  /usr/local/lib/ember-bridge/restore.sh <stamp>`) only if the database itself
  is damaged: everything written since the backup (games, points, friends,
  cards) is lost. A restored database has the SFV rows again, so restore only
  with the old bridge in place, and clear them again before the next attempt.
