# Nightly repository merge for 1.5.0 testing (2026-10-28)

The Nightly repository's `nightly` (FRaccie's line: tournament desk, bracket
site, S3 replay archive, Tournament Mode by invitation, hitbox layers,
migrations 031 to 049) had drifted 260 commits behind the main repository's
`nightly` since the 1.4.1 README bump (cf928ff5). This merge brings the main
line onto it in two merge commits, FRaccie's branch as first parent:

- `b3a6833a` merges main `nightly` 9cd5ea02: the 2026-10-26 review's T1, T2
  and T3 work (`docs/reviews/2026-10-26-nightly-thermo-nuclear/HANDOFF.md`),
  version 1.5.0, match links, rankings refresh.
- `70920aae` merges `integrate/150` 0bb8f644: pull requests #98, #156, #160
  to #163 and #76 as the PR review session integrated them.

The replay channel (`feat/replay-channel`) is not in. It collides with the
replay archive on three file names, its migration 031 is taken, and it has
not run in the real game; see the session notes for the product question.

## Ported by hand

Where the review had moved code the desk line then edited:

- `routes/matches.rs` (deleted by the review) into `routes/matches/read.rs`
  (facts on accepted attempts through `reports::agreed_facts`) and
  `routes/matches/adjudicate.rs` (`AdjudicationCommand` pub(crate),
  `check_command`, `adjudicate_for`, `actor` in place of `service`).
- The game-link close-reason test into `service/tests/game_links.rs`.
- FRaccie's eight desk ops and three replay ops into the exhaustive
  `requests!` routing (Bridge lane) and the typed `BridgeCall` reads;
  `desk.rs`, `replays.rs` and `sign_in.rs` rewritten onto that API.
  `Client::exchange` carries his content type and byte limit; `Body::Raw`
  and `.limit(n)` on `BridgeCall`.
- `state.committed()` calls (removed by T2-12) dropped from his routes; the
  desk's delivery wake-ups go through `db::queued()` in `delivery/calls.rs`
  and `delivery/dms.rs`.
- His tests take the shared loopback harness (`ember_bridge::testing`,
  `tests/common/challonge.rs`); `loopback_config` gained `desk`,
  `replay_retention`, `archive`, `trusted_proxies`.
- The room settings save as a `commands::RoomSettings` family (classified
  after Pad, before Input); `updateChannel` and `roomSettings` in
  `RuntimeRequest`; `updateChannel` on `commands::Service`.
- The failed-allocation memento guard into
  `sf4e__Game__Battle__System__Memento.cxx`.
- Hit-advantage marks and stop lines beside the shared run walk in
  `TrainingMeter.cxx`; the replay-share on/off row through `OnOffRows`
  (the `watchedPicture` preference is gone on his side, it is a board
  toggle now); the More… row helpers into `RoomBoardRows`.
- `ReplayFiles` test registered in `sf4e_test_targets.cmake`, after
  nlohmann_json is found, with his explicit link.
- Report version 2 with facts keeps its version rule beside `is_hex`.

Deliberate: his OAuth methods other than `user` still build a client per
call; his notifier feed and award loops keep their own sleeps; `approve`
maps the bridge reason `number_wrong` to `desk_number_wrong` on any status;
`OpenUpdater` is gone (his removal). sf4-net is not rustfmt-formatted on
either side and was left so; the server workspace is.

## Verified

Desktop build on D: (BelowNormal, 8 jobs): the whole tree builds; the full
compile took about 10 minutes. Server workspace `cargo check` clean;
notifier, brackets and bridge test suites 0 failed. sf4-net `cargo check`
clean; tournament unit tests 65/65, game_links 6/6, tournament_bridge 7/7
(replay_send, replay_pack, replay_archive steps included). Targeted ctest
39/39 (ShellJourney, RoomPanelNavigation, UiRender, PackageInstaller,
BattleStep, ReplayFiles, SettingsStore, ControllerNavigation, the room and
session server sets, BridgeSdk, Localization). The whole sf4-net suite:
470 tests, 0 failed (cargo test --no-fail-fast, 8 threads, BelowNormal). No full ctest, by the Nightly rule.

The planned resolution review by a second agent was stopped before it
reported; the spot checks above stand in for it.

## Not done here

- Main repository `nightly` still lacks `integrate/150` (the PR review
  session pushes it); this branch already contains both.
- No Nightly was published from this branch. The 1.5.0 Nightly build comes
  from the main repository's checkout.
- The VPS bridge deploy of migrations 031 to 049 and the desk's trusted
  host default (`SF4E_EMBER_SERVICE`) are owner decisions from the 10-21
  desk review, unchanged by this merge.
- In-game checks of FRaccie's merged features and of the review's
  behavior changes (HANDOFF.md section 2).
