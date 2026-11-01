Reviewed pinned HEAD `f0e83cbc7ab3d7e2f5a1b8631f18859d7b23083f`, the design, the archive introduction commits, and subsequent scoped changes. **The archive is not ready for public deployment.** The largest structural problem is that replay identity, uploader attribution, and durable set publication are represented by fields on a game row without enforcing the invariants those fields imply.

1. **F01 — P1: Set packs need their own durable publication state.**  
   Locations: [delivery/archive.rs:96](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/delivery/archive.rs:96), [routes/archive.rs:104](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:104), [routes/replays.rs:349](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:349).

   Pack publication is an incidental side effect of delivering an individual game. This creates three permanent failure paths:
   - Promoting an already-stored online game into a set updates its metadata but leaves it `stored`, with no pending work to create the pack. The listing nevertheless advertises the pack URL.
   - Concurrent delivery jobs for one set can upload different pack snapshots to the same key. If an older snapshot finishes last, it replaces the newer pack; both jobs can then settle successfully.
   - If storage remains unavailable past replay retention, maintenance deletes `match_replays`. A later retry rebuilds the pack from the now-empty table and can publish an empty pack successfully.

   **Fix:** Separate immutable game delivery from one durable, versioned publication job per set. Adding or attaching a game must mark that set dirty transactionally. Preserve the manifest and required bytes until that version is durable, and prevent an older publication from replacing a newer one. This removes the repeated per-game pack orchestration and gives retention a clear durability boundary.

2. **F02 — P1: CRC-32 is being used as a unique content identity.**  
   Locations: [049_archive_games.sql:8](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/migrations/049_archive_games.sql:8), [routes/archive.rs:77](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:77), [routes/archive.rs:94](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:94).

   Different replay bodies with the same CRC are treated as the same game without comparing their contents. The later upload receives success, so its client marks it sent even though its replay was discarded. Deliberate collisions also let an uploader reserve a target CRC before the legitimate game arrives. The repository already explicitly tests distinct, equal-size, equal-CRC replay bodies in [replay_archive_index_test.cxx:114](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/tests/replay_archive_index_test.cxx:114).

   **Fix:** Use a collision-resistant body digest as the archive identity and object key. Retain CRC as native-format metadata or a lookup bucket, and never acknowledge different content as an existing copy merely because its CRC matches.

3. **F03 — P1: A play session does not authorize the uploader’s claimed side or replay attribution.**  
   Locations: [routes/archive.rs:75](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:75), [routes/archive.rs:95](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:95), [routes/replays.rs:61](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:61).

   Any holder of a play session can download a public replay and resubmit it with either `side`, permanently filling an unset country for someone else. `sender` is not checked against the claimed side. A participant in an eligible tournament match can also submit somebody else’s downloaded replay to that match: the route checks membership and available replay count, but does not bind the replay or supplied revision to a particular confirmed game. If the public game has no set yet, this can relabel it as belonging to the uploader’s match.

   **Fix:** Establish a verified participant/game boundary before accepting canonical side updates or set attachment. Derive tournament sides from the roster and validate the confirmed game and binding revision. Keep unverifiable online names and side claims explicitly attributed to their submitter rather than allowing them to mutate authoritative fields.

4. **F04 — P1: “Whole replay” validation accepts arbitrary CRC-correct blobs.**  
   Location: [protocol/src/replays.rs:37](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/protocol/src/replays.rs:37).

   `saved_at` checks outer magic, size bounds, and CRC, but not the record’s declared payload length, used flag, native replay header, or consistency between record and body fighters. An outer wrapper containing one arbitrary payload byte passes. Such uploads become publicly listed downloads and can make set packs unusable. The existing tests reinforce this weak contract by wrapping arbitrary text or repeated bytes as valid games.

   **Fix:** Introduce one protocol parser returning a validated replay model containing identity, save time, and fighters. Apply the structural invariants already enforced by the native `ReplaySlots` validation, and use that parser in both helper and bridge. Add real-format fixtures and negative cases for inconsistent record lengths and headers.

5. **F05 — P1: Unknown provenance is treated as permission to upload a played game.**  
   Locations: [ReplaySets.hxx:24](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/ReplaySets.hxx:24), [ReplayFiles.cxx:62](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/platform/ReplayFiles.cxx:62), [ReplayProvenance.hxx:86](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/common/ReplayProvenance.hxx:86).

   Missing, unreadable, invalid, or not-yet-published name metadata returns default `ReplayNames`, whose `spectated` value is `false`. `NextToArchive` interprets that as “played here.” Consequently, a recently watched game whose provenance could not be bound—or whose archive listing races ahead of provenance publication—is eligible for public upload. Empty names do not prevent sending.

   **Fix:** Model provenance explicitly as unknown, played, or watched. Require positive, body-bound evidence of local play before automatic sharing. Unknown must remain ineligible; another boolean default cannot safely express this boundary.

6. **F06 — P1: Sharing consent transitions are reconciled by the upload timer instead of the preference update.**  
   Locations: [NetplayRuntime__Tournament.cxx:208](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:208), [NetplayRuntime__Tournament.cxx:216](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:216), [NetplayRuntime__Room.cxx:433](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Room.cxx:433).

   Preference saves preserve the previous `shareReplaysSince`; only a later eligible send poll clears or initializes it. Turning sharing off and back on between polls therefore retains the earlier cutoff. Unsent files predating the latest opt-in can then be uploaded, contrary to the documented guarantee. Initialization is also delayed until the helper and a listing are available, potentially excluding games saved after sharing was already on.

   **Fix:** Change the boolean and timestamp atomically where preferences are accepted: clear the timestamp on disabling and establish the new cutoff on enabling. Initialize the default-on state during preference loading. Remove consent mutation from the sending loop.

7. **F07 — P1: The supplied public listing proxy does not enforce the required request budget.**  
   Locations: [default.conf.template:10](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/default.conf.template:10), [routes/archive.rs:288](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:288), [db.rs:177](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/db.rs:177).

   The archive nginx location forwards unauthenticated requests without rate limiting or caching. Every response recomputes event and country choices across stored rows; selective searches can also traverse the stored archive. These reads hold the same connection mutex used by tournament writes. Public traffic can therefore delay gameplay-related bridge operations as the archive grows. The design explicitly requires a limit in the page’s proxy, but this template omits it.

   **Fix:** Ship an archive-specific request budget and bounded concurrency/cache policy. Maintain or cache filter facets separately, and give filtered lookup a suitable query/index strategy. Avoid making every page request perform archive-wide work while holding the bridge’s sole database connection.

8. **F08 — P2: The advertised archive admission limits apply only to the online upload route.**  
   Locations: [routes/archive.rs:174](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:174), [routes/archive.rs:55](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:55), [routes/replays.rs:92](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:92).

   The 3000-per-day and 2 GiB pending checks live in `archive::put`. Tournament uploads call `keep` directly and bypass both. For example, a bridge already holding 2 GiB of pending online games still accepts additional tournament games into the archive. The separate `match_replays` cap measures a different store and does not enforce the archive’s stated pending-byte limit.

   **Fix:** Put archive admission accounting in the canonical archive insertion operation, in the same transaction as insertion. Both routes must use it. If tournament uploads intentionally have a different daily policy, express that policy explicitly while retaining the global pending-storage bound and transient refusal behavior.

9. **F09 — P2: First enablement omits existing set replays, and duplicate submissions can misclassify them as online games.**  
   Locations: [049_archive_games.sql:7](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/migrations/049_archive_games.sql:7), [routes/replays.rs:67](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/replays.rs:67).

   Migration 049 creates an empty archive without enqueueing existing `match_replays`. Clients that already marked those files sent will not upload them again. If another client does submit an existing set replay, the `kept` branch calls `archive::keep(..., None)`, creating an online archive entry without its event, set, or pack.

   **Fix:** Preserve set context in the duplicate branch by passing the match ID. Add a bounded, resumable enqueue/backfill operation for existing tournament replays when an archive is first enabled, rather than depending on client resubmission.

10. **F10 — P2: The game abandons outstanding replay requests without cancelling them.**  
    Locations: [NetplayRuntime__Tournament.cxx:208](C:/Users/Kate/Desktop/sf4/sf4-nightly/src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:208), [tournament/replays.rs:116](C:/Users/Kate/Desktop/sf4/sf4-nightly/rust/sf4-net/src/tournament/replays.rs:116).

    After 60 seconds, the game clears `sending.request` and sends again even if the helper is still trying bridges. Several sequential 15-second bridge failures can exceed that interval. The original request can subsequently succeed, but its answer is discarded because its ID has been replaced. With a consistently slow fallback path, every success can arrive after replacement, leaving the same replay perpetually unsent locally and creating overlapping helper work.

    **Fix:** Give the sender an explicit in-flight state. Retain ownership until completion or acknowledged cancellation, and agree on an overall helper deadline before retrying. A retry timer must not silently replace a live operation.

11. **F11 — P2: Author CSS overrides the page’s `hidden` states.**  
    Locations: [archive.css:19](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.css:19), [archive.css:23](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.css:23), [archive.js:180](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.js:180).

    `main > section { display:flex }` overrides the browser’s ordinary `[hidden]` display rule, so toggling `list-view.hidden` and `set-view.hidden` does not reliably hide either section. Likewise, `.btn { display:inline-flex }` exposes the hidden “Older replays” button even when there is no next page.

    **Fix:** Explicitly enforce hidden visibility, for example with a scoped `[hidden] { display:none !important }` rule, or make display selectors exclude hidden elements. Verify initial load, opening/back navigation, and pagination visibility in a browser.

12. **F12 — P2: Timestamp-only pagination permanently skips games at page boundaries.**  
    Locations: [routes/archive.rs:249](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/routes/archive.rs:249), [archive.js:49](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/site/assets/archive.js:49).

    Ordering uses only `saved_at`, and the next page requires `saved_at < before`. If two games sharing a timestamp straddle the 400-row boundary, the later page excludes the unreturned game. An in-memory check using the SQL from HEAD returned 400 of 401 equal-timestamp rows, reported `more=true`, then returned an empty next page.

    **Fix:** Use deterministic compound ordering and a matching cursor, such as `(saved_at, replay_id)`. Return the next cursor from the listing API rather than having the page reconstruct pagination from the last timestamp.

13. **F13 — P2: S3 signing and sending use different interpretations of the endpoint.**  
    Locations: [s3.rs:29](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/s3.rs:29), [delivery/archive.rs:71](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/delivery/archive.rs:71), [config.rs:524](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/bridge/src/config.rs:524).

    The signer extracts the host by splitting the raw endpoint string, while reqwest parses and normalizes the request URL. An accepted endpoint such as `https://store.example:443` signs `host:store.example:443`, while URL normalization removes the default port from the outgoing authority. Configuration also accepts an endpoint containing a query because it checks only scheme and path; appending the object path then puts it inside that query. Both cases can leave every upload retrying indefinitely.

    **Fix:** Parse and validate the endpoint once as a typed origin, rejecting credentials, query, and fragment. Construct one canonical object URL and derive both the signing authority/path and outgoing request from it. Extend signing tests to default ports, non-default ports, and IPv6.

The checks that held up were the play-session requirement, bounded request bodies, parameterized listing SQL, text-based rendering of player names, restricted CRC path syntax, server-generated set IDs, secret loading, and retry settlement retaining game bytes on storage failure. Migration 049 is additive and runs transactionally; I found no destructive migration operation. None of the supplied archive introduction commits itself crossed the 1000-line threshold.

The privacy switch controls this player’s uploads. It does not prevent an opponent’s upload from publishing their name, and it does not withdraw existing public objects. That is consistent with the current design, but it should be stated explicitly to players. The supplied deployment files also contain no public bucket proxy or read/no-list policy, so the claimed bucket method restrictions and prohibition on listing remain unverified.

No files were edited and no builds or repository test executables were run. Validation comprised source/history review and in-memory SQL probes. Existing archive tests cover basic upload, deduplication/filtering, set delivery, and the disabled archive; they do not establish the failure, concurrency, consent, quota, or migration scenarios above. The S3 integration stand-in checks the payload hash and authorization prefix, so it does not establish acceptance by the pinned RustFS deployment.

VERDICT: CHANGES REQUIRED