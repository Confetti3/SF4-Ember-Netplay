#!/usr/bin/env python3
# SFV Ember's rows out of SF4 Ember's production database, for the SF4 1.6.0
# deploy (docs/design/SFV_BRIDGE.md, "SF4's database"). Not part of
# upgrade.sh: SF4's side runs it, once, with SF4's bridge stopped, after a
# backup and after SFV's bridge imported its ratings, right before SF4 1.6.0's
# bridge goes in (that bridge refuses to start while any row of another game
# than usf4 is left):
#
#   sudo bash ~/ember-sfv-bridge/upgrade.sh import
#   sudo systemctl stop ember-bridge
#   sudo systemctl start ember-bridge-backup.service
#   sudo -u ember-bridge python3 -I clear-sfv-rows.py /var/lib/ember-bridge/bridge.sqlite3
#   (install SF4 1.6.0's bridge and start it)
#
# The schema stays (057-059 included). It deletes SFV's public rooms with their
# bans and rated games, and SFV's ratings, sittings and ranking choices, in one
# transaction, and commits only when no row of another game than usf4 is left
# in rooms, matches, lobbies, tournaments and the three rating tables, and the
# foreign keys check clean. It deletes nothing when SFV
# owns an organised match, lobby or tournament, or a room a connection made:
# those have child rows without a cascade and need a look first.
import sqlite3, sys
db = sqlite3.connect(sys.argv[1], isolation_level=None)
db.execute("PRAGMA foreign_keys = ON")
db.execute("BEGIN IMMEDIATE")
blockers = {what: db.execute(sql).fetchone()[0] for what, sql in (
    ("matches", "SELECT COUNT(*) FROM matches WHERE game = 'sfv'"),
    ("lobbies", "SELECT COUNT(*) FROM lobbies WHERE game = 'sfv'"),
    ("tournaments", "SELECT COUNT(*) FROM tournaments WHERE game = 'sfv'"),
    ("connection rooms", "SELECT COUNT(*) FROM rooms WHERE game = 'sfv' AND connection_id IS NOT NULL"),
)}
if any(blockers.values()):
    db.execute("ROLLBACK")
    sys.exit(f"stopped, nothing deleted: SFV rows that need hand work {blockers}")
for sql in (
    "DELETE FROM room_bans WHERE room_id IN (SELECT room_id FROM rooms WHERE game = 'sfv')",
    "DELETE FROM rated_games WHERE room_id IN (SELECT room_id FROM rooms WHERE game = 'sfv')",
    "DELETE FROM rooms WHERE game = 'sfv'",
    "DELETE FROM skill_ratings WHERE game = 'sfv'",
    "DELETE FROM rated_pairs WHERE game = 'sfv'",
    "DELETE FROM ranking_choices WHERE game = 'sfv'",
):
    print(f"{db.execute(sql).rowcount:6} {sql}")
# The same tables SF4 1.6.0's bridge checks before it starts.
left = sum(db.execute(f"SELECT COUNT(*) FROM {t} WHERE game <> 'usf4'").fetchone()[0] for t in (
    "rooms", "matches", "lobbies", "tournaments", "skill_ratings", "rated_pairs", "ranking_choices"))
keys = db.execute("PRAGMA foreign_key_check").fetchall()
if left or keys:
    db.execute("ROLLBACK")
    sys.exit(f"stopped, nothing deleted: {left} non-usf4 rows left, foreign keys {keys[:5]}")
db.execute("COMMIT")
print("committed")
