Review by Opus (Sol had reached its usage limit), against r5-prompt.md.

Reviewed nightly HEAD `f0e83cbc` plus the uncommitted working tree (`git diff`, `git status`, the untracked archive files), excluding `rust/sf4-net/tests/archive_local.rs` and `docs/reviews/`. I checked both round-4 items against the code. I then read the round-5 edits for regressions: `ReplaySender.hxx`, `ReplaySets.hxx`, `sf4e__NetplayRuntime__Tournament.cxx`, `routes/replays.rs` (`hold`, `free_game`, `unconfirmed`), `tournament/replays.rs` (`SET_REFUSALS`), `adjudicate.rs`, `play/mod.rs` (`issue`), `reports.rs`, the design doc, and the new and changed tests in `replay_files_test.cxx`, `tests/play.rs`, `tests/archive.rs` and `tests/common/play.rs`.

**Both round-4 findings are closed. I found nothing at P1 or P2.** Three P3 nits remain. One is a latent timing dependency in a bridge test helper. One is a test file that has just crossed 1,000 lines. One is two design-doc sentences and a code comment that the round-4 changes made out of date.

## Round-4 items

### R4-01 (closed): a replay of a game held for review no longer blocks the queue

- `ReplaySender::Take` answers `Unconfirmed` only for a set request (`set_`) refused with `replay_unconfirmed` (`ReplaySender.hxx:68-72`).
  - It records the path in `aside_` until `now + kUnconfirmedMs` (10 minutes).
  - It drops expired entries whenever it adds one, so the map stays small.
  - It schedules the next look after `kNextMs`, not after `kAgainMs`.
- `NextToSend` skips any path that `Aside` reports for `nowMs` (`ReplaySets.hxx:26-30`). `NextReplay` passes the sender and the time through (`:46-49`).
  - The newer set game, and after it the shared online games, go out while the held game waits.
  - Once the 10 minutes pass, `Aside` is false and the held game is the oldest again, so it is offered first.
- The put-aside state lives only in memory, so a restart offers the game again at once. That is harmless: the bridge just answers 503 again.
- `SendSetReplays` no longer re-derives the sharing cutoff. `TakeReplaySent` handles the new answer by logging only. Nothing is marked sent, so the game is never settled by mistake (`sf4e__NetplayRuntime__Tournament.cxx:252-269`).
- On the archive route, `set_` is false, so `replay_unconfirmed` falls through to `Again`. That matches the scope, and the helper's `REFUSALS` for that route would not word it anyway.
- Bridge-side semantics are unchanged: only confirmed games are bound.
- `TestUnconfirmedReplayWaits` covers the order (held game, then the later set game, then the online game), the expiry boundary at exactly `kUnconfirmedMs`, a plain retryable failure keeping the same file first, and the archive-route case.

I also checked whether the new send order could bind a replay to the wrong game. A held game's set cannot gain a later game while it waits:

- `issue` creates a permit only once `prepare` passes.
- `disagreement_holds_the_match_for_an_organizer` shows `prepare` answers 409 while the match is `needs_review`.
- `adjudicate.rs:164-169` assumes a single open attempt.

So the replays that overtake the held one belong to other sets or are online games. Once the held game is decided, a later game's replay is nearest its own window, at distance 0. Nothing new arises.

### R4-02 (closed): a game nobody reported, decided in place, takes its replay

- `free_game` now left-joins the report subquery and `adjudications` on `a.adjudication_id`, and ends the window at `COALESCE(r.ended, d.created_at)` (`routes/replays.rs:193-205`).
- I checked every path that writes `state = 'accepted'`. Each attempt it produces has either reports (`reports.rs:308`) or an adjudication (`adjudicate.rs:177` in place, `:202` a new organizer attempt). So no accepted attempt is left with a NULL window end.
- The legacy-count subquery never joined reports, so it stays consistent with the wider candidate set.

An in-memory SQLite probe with the exact statement, in a scratch directory that I deleted afterwards, gave these results:

| Case | `free_game` returns |
|---|---|
| Attempt in `review`, no reports | none (so `unconfirmed` answers 503) |
| Same attempt after the in-place decision two hours later | that attempt |
| A reported game, replay inside its window | that game |
| A reported game, replay outside its window | none |

`a_game_decided_without_reports_takes_its_replay` drives the real flow:

1. A permit is issued and two hours pass.
2. `maintain` holds the attempt for review, and the replay gets 503.
3. The organizer decides the attempt in place.
4. The replay is sent again and gets 201.

`pack_stored` advances the bridge clock by an hour per look, up to 200 looks. In both retention tests that use it, the set is already past its retention when the loop starts. What they assert after the loop (the replays are pruned, the set is closed, the pack is stored, and in the first test a late replay gets `set_closed`) does not depend on how many hours the loop took.

Nothing from rounds 1 to 3 is open beyond the accepted residual risks: a participant's own save time, and online names attributed by `sent_by`.

## Findings

### R5-01 — P3: `replay(n)` is pinned to the first call in the test process, so a test that runs late finds its replays outside the game window

Location: `server/ember/bridge/tests/common/play.rs:316-326`. It is used by `tests/archive.rs` and `tests/play.rs` (for example `a_sets_replays_are_kept_once_and_packed_by_save_time`, `retained`, `a_public_events_finished_set_is_downloaded_by_anyone`, `a_sets_games_are_sided_by_its_roster_and_packed_whole`, `a_voided_games_replay_leaves_its_replacement_free`).

The helper saves game `n` at `FIRST + n`. `FIRST` is a process-wide `OnceLock` set to the wall time of the first `replay()` call in that test binary. Each test's bridge has its own clock (`util::Clock`). The `play.rs` and `archive.rs` fixtures never freeze it; only `common/sites.rs:82` does. So each clock follows the wall clock, plus that test's own `advance` calls, and a test's permits are stamped at the time it actually plays. The base is per process, but the clock is per fixture, and that mismatch is the root cause. `free_game` accepts a replay only within `GAME_SLACK` (120 s) of its game's permit.

Failure scenario: a test in the same binary plays its games more than about 120 s after the binary's first `replay()` call. This happens with `--test-threads=1`, or on a loaded laptop or runner, where a long serial run includes `pack_stored`'s polling of up to about 10 s per call. In that case:

- `saved_at = FIRST + n` is earlier than `permit - 120`;
- `free_game` finds no game, and with every attempt accepted, `unconfirmed` is false;
- the send gets 400 "saved in no game", and the test fails on its `CREATED` assertion.

Whether a test fails depends on its position in the run, not on the code under test. The earlier fixed value (`1_800_000_000`) had no window to fit, so this dependency is new with the window check.

Fix: tie the save time to the fixture, not to the process. Record the bridge clock's `now()` when a `Fixture` is built, and make the helper `replay(f, n)` (or `f.replay(n)`), saving at that time plus `n`. A test that needs the same bytes twice, such as `listed_game(&list, &replay(n))`, still gets them, because the base is fixed per fixture.

### R5-02 — P3: `tests/play.rs` crosses 1,000 lines

Location: `server/ember/bridge/tests/play.rs` goes from 970 lines at HEAD to 1,014. The crossing comes from `a_game_decided_without_reports_takes_its_replay` (`:352-388`) and the second-room setup in `a_sets_replays_are_kept_once_and_packed_by_save_time`.

The file's replay tests form one cohesive group:

- `a_sets_replays_are_kept_once_and_packed_by_save_time`
- `held`, `retained`
- `replays_are_kept_forever_by_default`
- `old_sets_are_backed_up_then_pruned`
- `a_failed_backup_keeps_the_replays`
- `a_game_decided_without_reports_takes_its_replay`
- `a_public_events_finished_set_is_downloaded_by_anyone`

Fix: move that group into its own `tests/set_replays.rs`, using the shared `common::play` helpers. `tests/play.rs` then goes back to covering reports, review, deadlines and claims, and both files stay well under 1,000 lines. `tests/archive.rs` (963 lines) is not the place for them, because it would cross the limit too. This is a test file 14 lines over, so I am not blocking on it.

### R5-03 — P3: two design-doc sentences and the sender's comment are out of date after R4-01 and R4-02

Locations: `docs/design/REPLAY_ARCHIVE.md:37-38`, `docs/design/REPLAY_ARCHIVE.md:75-76` and `:80-81`, and `src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:200-207`.

- `REPLAY_ARCHIVE.md:37-38` says "It sends all of a set's games before any other game." Since R4-01, a set game its bridge has not confirmed is put aside for 10 minutes while later set games and online games go.
- `REPLAY_ARCHIVE.md:75-76` defines a game's time as "from its permit to the last report on it". Since R4-02, a game nobody reported, decided by the organizer, ends at the decision.
- `REPLAY_ARCHIVE.md:80-81` says only "which the game sends again later", without the 10-minute wait.
- The block comment on `SendSetReplays` still says "any other failure is tried again a minute after it is answered", with no mention of `Unconfirmed`. Its lines are also broken mid-sentence ("as any / other game would; any / other failure").

A reader taking the doc or the comment at its word would expect the head-of-line behaviour that R4-01 removed.

Fix:

- Doc, `:37-38`: "It sends a set's games before any other game, except that one its bridge has not confirmed yet (`replay_unconfirmed`) waits 10 minutes while the others go."
- Doc, `:75-76`: "from its permit to the last report on it, or to the organizer's decision for a game nobody reported".
- Comment: add the `Unconfirmed` case and rewrap the lines.

## Structure and size

The R4-01 change has the right shape:

- The put-aside state sits in `ReplaySender`, which already owns the one-request-at-a-time state machine.
- `NextToSend` gains one predicate instead of a special case in `SendSetReplays`.
- `TakeReplaySent` stays a flat dispatch over `Answer`.

The R4-02 change is a two-join edit to one query, with no new branches.

No source file crosses from below 1,000 lines to above. `rust/sf4-net/src/tournament/mod.rs` (1,487 to 1,635), `client.rs` (1,049) and `sf4e__NetplayRuntime__Room.cxx` (1,018 to 1,016) were already over the limit, as round 4 noted. `tests/play.rs` is R5-02. `git diff --check` reports nothing beyond CRLF conversion warnings.

## Validation

- I read the scoped diff and the code paths named above.
- I ran the throwaway SQLite probe described under R4-02 and deleted it.
- I traced the accepted, review and permit paths through `reports.rs`, `adjudicate.rs` and `play/mod.rs`.
- I edited nothing except this file. I ran no builds, no tests and no game. Deployment and gameplay acceptance remain unverified.

VERDICT: APPROVE
