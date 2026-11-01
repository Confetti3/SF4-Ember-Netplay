Review by Opus (Sol had reached its usage limit), against r4-prompt.md.

Reviewed nightly HEAD `f0e83cbc` plus the uncommitted working tree (`git diff`, `git status`, the untracked archive files), excluding `rust/sf4-net/tests/archive_local.rs` and `docs/reviews/`. I checked each round-3 item against the code, then read the round-4 edits for regressions: `routes/replays.rs`, `archive/sets.rs`, `delivery/packs.rs`, `tournament/replays.rs` and `client.rs`, `ReplaySender.hxx`, `ReplaySets.hxx`, `ReplayFiles.*`, `NetplayRuntime__Tournament.cxx`, `archive-site/deploy/setup.sh` and `archive.conf.in`, the design doc, the README, and the new tests.

**Two findings remain: one P2 and one P3.** Both come from the R3-02 change. The P2 is new in this round. Before R3-02, a replay of an undecided game got a final 400. Now the bridge tells the game to send it again for as long as the game stays undecided, and a game held for review can stay undecided indefinitely. The game always sends the oldest replay first, so that one replay then blocks every later send from that player.

## Findings

### R4-01 — P2: A replay of a game held for review blocks all of that player's replay sending, possibly forever

Locations: `server/ember/bridge/src/routes/replays.rs:143` and `:211-217` (`unconfirmed`), `src/platform/ReplaySets.hxx:25-28` (`NextToSend`, oldest first) and `:43-46` (`NextReplay`, set games first), `src/platform/ReplaySender.hxx:63-65` (`Take` answers `Again`), `src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:226`.

Failure scenario, traced through the code:

1. Player A finishes game N of set S. A's report arrives; B's never does (B drops out or crashes). After `PARTNER_SECS`, `reports::expire` calls `hold()` (`reports.rs:351-367`). The attempt becomes `review` and the match becomes `needs_review`.
2. A's game sends the replay for game N. `free_game` finds nothing, because only accepted attempts count. `unconfirmed` is true: the attempt is in `review`, and the window's upper bound is `now + 120`, so the window never closes. The bridge answers 503 `replay_unconfirmed`.
3. `ReplaySender::Take` answers `Again`, because the reason is not in `SendSettled`. `nextMs_` is set to now + 60 s.
4. A minute later, `NextReplay` calls `NextToSend` again. It still returns the oldest unsent set replay, which is the same file, so steps 2 and 3 repeat.

While that attempt stays in `review`, A's game sends nothing else:

- no replay of any later set (for example, A's games in another event),
- no game for the public archive, even with Share my replays on,
- the whole file is uploaded again every minute.

Nothing ends the `review` state except an organizer:

- `expire_stale` skips `needs_review` matches (`expiry.rs:65`), and `ends_by` does not apply to them either, because the state filter applies first.
- Only adjudication (accept or void) or a cancellation (`lifecycle.rs:39` aborts the attempt) releases it.

On a desk event whose organizer never resolves the set, the player's sharing stops for good. A lone report followed by a vanished opponent is an ordinary tournament outcome, not an exotic one.

The bridge test `a_replay_of_a_game_not_confirmed_yet_is_sent_again_later` covers only a `permitted` attempt that is confirmed seconds later. `TestReplaySender` covers one `Again` answer, not a queue stuck behind one.

Fix (either is enough):

- **(a) Client side, recommended.** Keep the bridge contract and stop the head-of-line block. When a set replay is answered `replay_unconfirmed`, `ReplaySender` records that path with a time before which it is not retried (in memory; 10 minutes, for example). `NextToSend` and `NextReplay` skip deferred paths, so the next set game or shared game goes out meanwhile. The "only confirmed games" rule stays as it is. This is a few lines in `ReplaySender`/`ReplaySets` plus a test that a deferred replay does not block a newer one.
- **(b) Bridge side.** Let `free_game` also bind to a `review` attempt, so that only `permitted` gets the 503. `permitted` lasts at most `SILENT_SECS` + `PARTNER_SECS`, so that wait is bounded. A later void is already harmless (R3-03). The cost is that a bound replay of a review game that ends `aborted` (on cancellation) would stay in the set and its pack as a set game. The design currently admits only confirmed games, so the design doc would have to say otherwise.

I would take (a): it changes no server semantics. (b) is the smaller diff if you accept that change to the design.

### R4-02 — P3: A game nobody reported, then decided in place by the organizer, can never take its replay

Locations: `server/ember/bridge/src/routes/replays.rs:192-194` (`free_game`'s inner `JOIN` on `game_reports`), `server/ember/bridge/src/routes/reports.rs:385-389` (`expire` holds a `permitted` attempt with no reports, reason `no_reports`), `server/ember/bridge/src/routes/matches/adjudicate.rs:174-180` (in-place decision sets the open attempt `accepted`).

Failure scenario:

1. No report for game N ever reaches the bridge. After `SILENT_SECS`, `expire` puts the attempt in `review` with no `game_reports` rows.
2. Meanwhile the replay gets 503 `replay_unconfirmed` (R3-02) and is retried.
3. The organizer decides the game in place, so the attempt becomes `accepted`, still with no reports.
4. On the next retry, `free_game`'s inner join drops the attempt because there is no `r.ended`, and `unconfirmed` is now false. The bridge answers 400 "saved in no game of this set that has none yet". `SendSettled` treats that as final and `MarkSent` records the replay as sent.

The confirmed game's replay is then lost for good, without any notice. That is the same outcome R3-02 was raised for, just rarer, because it needs every report for the game to be lost.

An in-memory SQLite probe using the exact `free_game` and `unconfirmed` statements gave these results:

| State of the game | `unconfirmed` | `free_game` |
|---|---|---|
| In `review`, no reports | 1 | none |
| After the in-place decision | 0 | none |

The same probe confirmed the R3-03 voided-replacement case selects the replacement.

Fix: make the window's end not depend on a report row:

- use `LEFT JOIN` for the report subquery, and
- end the window at `COALESCE(r.ended, d.created_at)`, where `d` is `adjudications` joined on `a.adjudication_id`.

For organizer-created attempts with no reports, the window then becomes roughly the adjudication time, which does no harm. Add a test with a review attempt that has no reports and is decided in place. If you choose R4-01 (b), this is the same query change.

## Round-3 items

All six are closed, and I am not re-raising any of them.

- **R3-01 (closed).** `prune` always applies `NOT PACK_DUE`. It adds `NOT NOT_ARCHIVED` only while an archive is configured (`replays.rs:454-466`; `sets.rs:28-35`). A due pack therefore keeps its set's replays whatever the configuration says. Covered by `an_unstored_pack_keeps_its_sets_replays_while_the_archive_is_off`: the archive is turned off and on again, and the set's replays are kept until the pack is stored, then pruned.
- **R3-02 (closed except as above).** The set route answers 503 `replay_unconfirmed` for the attempt states named in the scope. The helper's `SET_REFUSALS` passes both `set_closed` and `replay_unconfirmed` through (`a_sets_refusals_are_worded_apart`). The game retries (`TestReplaySender`), and the bridge test covers the `permitted` case. R4-01 and R4-02 are the remaining interactions.
- **R3-03 (closed).** Occupancy of modern rows is the bound `attempt_id`, plus the unique index. Legacy null rows are compared against the set's free accepted games, so a voided game's replay no longer occupies its replacement. Covered by `a_voided_games_replay_leaves_its_replacement_free`; my probe agrees.
- **R3-04 (closed).**
  - `closed.txt` is kept separately from `sent.txt` (`ArchivedReplay::setClosed`).
  - `ForItsSet` decides between the set route and the archive.
  - `NextToArchive` takes a closed set's game only while sharing covers it.
  - Any other final set refusal still marks the replay sent, so it stays set-only (`TestClosedSetGoesToTheArchive`).
  - The design doc says this.
- **R3-05 (closed).** `put_back` returns a result. `cleanup` deletes `$backup` only when no rollback was needed or the rollback succeeded. Otherwise it keeps the backup, prints the mirrored path and the recovery commands, and exits non-zero (`setup.sh:222-239`, `259-290`).
- **R3-06 (closed).**
  - Hostname ownership is read per file from `nginx -T`, with comments stripped and `server_name` matched up to its `;`, across line breaks. The script's own site, enabled link and zone file are excluded (`setup.sh:151-175`).
  - A per-install random marker is served as `install.txt` and must come back, together with the page title, over `curl --resolve` after the reload (`setup.sh:205-209`, `333-353`). It goes through `location /` with `try_files`, and the rollback removes it, since it has no predecessor.

From earlier rounds, I found nothing still open beyond the accepted residual risks: a participant's own save time, and online names attributed by `sent_by`. I have no cheap, complete fix for either. The other round-4 changes held up:

- The `put_to`/`first_keeping` extraction in `tournament/replays.rs`: `refusals` is now `pub(super)` on `BridgeCall`, and that is its only use.
- The `ReplaySender` state machine, including its late-answer handling.
- `AcceptedPreferences`, the sharing cutoff set where preferences load.
- The `Provenance` change from a `spectated` bool to a tri-state.

## Structure and size

No changed file crosses from below 1,000 lines to above:

| File | Lines before | Lines after |
|---|---|---|
| `rust/sf4-net/src/tournament/mod.rs` | 1,487 | 1,635 |
| `rust/sf4-net/src/tournament/client.rs` | 1,049 | 1,049 |
| `src/sf4e/sf4e__NetplayRuntime__Room.cxx` | 1,018 | 1,016 |

All three were already over the limit. The growth in `mod.rs` is mostly the two deadline tests. Moving `ReplaySender` out of the `Runtime` struct into its own header, with `SendSettled` beside it, is a clear improvement: `SendSetReplays` and `TakeReplaySent` are now thin. `git diff --check` reports nothing beyond CRLF conversion warnings.

## Validation

- I read the source and the scoped diff.
- I ran the in-memory SQLite probe above in a scratch directory outside the repository and have since deleted it.
- I traced the `review`, `needs_review` and expiry paths through `reports.rs`, `expiry.rs`, `lifecycle.rs` and `adjudicate.rs`.
- `s3.rs`, `protocol/src/replays.rs`, the listing part of `routes/archive.rs` and `archive.js` were last modified before round 3. For them I relied on r2 and r3 having found them sound, and checked only that nothing round 4 touched depends on them differently.
- I edited nothing except this file. I ran no builds, no tests and no game. Deployment and gameplay acceptance remain unverified.

VERDICT: CHANGES REQUIRED
