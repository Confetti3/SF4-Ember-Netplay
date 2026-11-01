Reviewed HEAD `f0e83cbc` plus the scoped working tree and untracked files. **Nine findings remain.** The largest issue is still the durability boundary between archived set membership, pack publication, and replay retention.

1. **R2-01 — P1: A late replay can replace a complete pack with an incomplete one. F01 remains open.**  
   Locations: [delivery/packs.rs:76](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/delivery/packs.rs:76), [routes/replays.rs:297](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:297), [routes/replays.rs:386](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:386).

   Once the latest pack is stored and backfill has passed, retention deletes the set’s `match_replays`. A player who reconnects later can submit a previously missing game whose original save time passes the match window. This increments `archive_sets.version`, but `packed()` rebuilds exclusively from the remaining `match_replays`—now just the late game—and overwrites the existing pack. The version then settles successfully despite earlier games disappearing from the public pack.

   An in-memory probe confirmed two archived members with only the late member in the rebuild source. **Fix:** make a durable membership manifest and durable game objects the source of each complete pack version. Alternatively, retain every constituent replay while late additions remain admissible. A publication version cannot derive its contents solely from a table retention is allowed to empty.

2. **R2-02 — P1: The match window does not establish that a replay belongs to the match. F03 remains open.**  
   Locations: [routes/replays.rs:75](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:75), [routes/replays.rs:94](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:94), [protocol/replays.rs:102](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/protocol/src/replays.rs:102).

   A participant can submit an unrelated downloaded replay and change the outer record’s save timestamp to fall inside the window. That timestamp is outside both the checked body CRC and the new body digest; parsing still succeeds. `revision` is stored without validating a corresponding binding or confirmed game, and the capacity check counts all attempts rather than identifying the submitted game. The unrelated replay can therefore occupy a legitimate game’s slot and be published under the roster’s names. Even an unchanged unrelated replay from the same time window passes.

   **Fix:** require a match-bound, confirmed-game receipt and validate its binding revision and replay identity. A caller-controlled timestamp is a plausibility check, not an attribution boundary. Where identity cannot be verified, preserve that uncertainty instead of publishing authoritative set attribution.

3. **R2-03 — P1: The public bridge hostname bypasses the archive’s listing budget. F07 remains partially open.**  
   Locations: [archive nginx configuration:47](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/nginx/archive.conf.in:47), [bridge nginx configuration:106](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/deploy/nginx/bridge.conf.in:106).

   The new archive hostname enforces its cache, request rate, and concurrency ceiling. However, anyone can request the same unauthenticated `/v1/archive` through the bridge hostname. Its catch-all permits 20 requests per second per address, with no archive cache or shared concurrency ceiling. Those reads still contend for the database connection used by tournament writes. Bounding each scan does not bound how many scans this second entrance admits.

   **Fix:** close the public bridge entrance for this route and use the archive proxy, or give every public entrance the same archive-specific controls. The expensive operation needs one enforced admission boundary.

4. **R2-04 — P1: A failed RustFS upgrade destroys the installation’s rollback path.**  
   Locations: [deploy/setup.sh:175](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:175), [deploy/setup.sh:193](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:193), [deploy/setup.sh:232](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:232).

   The script overwrites the installed binary and unit before restarting, without backing up either. A binary that answers `--version` but cannot start under the unit’s restrictions leaves the existing archive down; failure merely exits. A subsequent nginx validation failure also leaves those earlier changes applied. Separately, nginx rollback is disarmed before page installation and reload succeed.

   **Fix:** stage the deployment, retain the immediate predecessor, and keep rollback armed through service health checks, page installation, and nginx reload. Restore the previous binary, unit, configuration, and affected site files on failure. The bridge deployment’s existing staged-swap/`put_back` pattern provides a concrete model.

5. **R2-05 — P2: Backfill treats reusable SQLite row IDs as permanent progress identifiers.**  
   Locations: [archive/sets.rs:177](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/archive/sets.rs:177), [archive/sets.rs:195](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/archive/sets.rs:195), [037_match_replays.sql:5](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/migrations/037_match_replays.sql:5).

   `match_replays` has an ordinary implicit `rowid`, which can be reused after retention deletes the highest rows. For example, after archival advances the checkpoint and retention empties the table, temporarily disabling the archive allows new set replays to receive row IDs below that checkpoint. Re-enabling archival permanently skips those games; the retention predicate also considers them already passed.

   An in-memory schema probe reproduced a new row receiving ID `1` while a checkpoint of `2` selected nothing. **Fix:** use an explicit, non-reusable sequence or durable per-replay backfill state, and migrate/reset existing progress consistently.

6. **R2-06 — P2: Set games still consume the online daily allowance. F08’s accounting is incomplete.**  
   Location: [archive/mod.rs:139](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/archive/mod.rs:139).

   Set admission skips enforcing the daily count, but the count itself includes every archive row from that sender, including set games. A player with 2,999 online submissions and one set submission is refused their next online game, despite the documented exemption.

   **Fix:** explicitly record and count admissions charged to the online allowance. Preserve that accounting when an online game subsequently joins a set; mutable presentation metadata should not retroactively change quota usage.

7. **R2-07 — P2: Pagination can combine a new filter with the previous filter’s cursor and games. F12 remains partially open.**  
   Locations: [archive.js:51](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.js:51), [archive.js:196](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.js:196), [archive.js:258](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.js:258).

   While a filter refresh is pending, the previous “Older replays” button remains usable and `state.next` still belongs to the old filter. Clicking it requests the new filter with the old cursor. Its newer request number invalidates the correct first-page request, then appends the response to the old filter’s games.

   Executing the actual load function with controlled responses reproduced the mixed list and missing new first page. **Fix:** invalidate pagination immediately on filter changes and associate games, cursor, and append requests with one filter generation. Reject appends during first-page loading.

8. **R2-08 — P2: The helper’s replay deadline excludes its queue wait, so abandoned requests can still execute. F10 remains partially open.**  
   Locations: [tournament/replays.rs:78](C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/replays.rs:78), [tournament/mod.rs:778](C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/mod.rs:778), [ReplaySender.hxx:35](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/ReplaySender.hxx:35).

   The 45-second budget starts only after the replay job leaves the bridge queue and obtains an HTTP slot. That queue holds 32 jobs behind four workers, and jobs carry no submission deadline. During slow bridge operations, the game can reach its three-minute timeout and enqueue a retry while the original remains queued. Both can subsequently execute; ignoring the original answer does not cancel its work.

   **Fix:** establish the replay deadline at helper submission, include queue/semaphore waiting, and prevent expired jobs from starting network work. The sender’s safety timeout must rest on an enforced end-to-end bound.

9. **R2-09 — P2: `--replace-site` permits a conflicting vhost without replacing it.**  
   Locations: [deploy/setup.sh:148](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:148), [deploy/setup.sh:221](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:221).

   When another enabled nginx file serves the hostname, `--replace-site` only suppresses the refusal. The conflicting file remains enabled. Nginx can accept the configuration with a conflicting-server-name warning and continue routing requests to the earlier vhost. The final page check can accept an unrelated HTTP 200, while a failed listing check still ends with `done`.

   **Fix:** either refuse conflicting vhosts or back up and explicitly disable the conflicting entries as part of the rollback transaction. Validate an archive-specific response after reload.

F02, F04, F05, F06, F11, and F13 appear closed by source inspection. The server-side compound cursor and bounded-scan continuation also hold up. No scoped change pushes a file from below to above 1,000 lines. Migration 050 intentionally discards 049’s rows, including pending rows; that matches the stated predeployment decision.

No files were edited and no builds or repository test executables were run. Validation comprised source and scoped-diff review, `git diff --check`, in-memory SQLite probes, and execution of the page’s load logic with mocked responses. Deployment and live runtime acceptance remain unverified.

VERDICT: CHANGES REQUIRED