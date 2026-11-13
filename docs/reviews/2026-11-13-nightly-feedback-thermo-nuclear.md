# Nightly feedback merge: thermo-nuclear code quality review, 2026-11-13

Scope: `ea0e13dee..767d8d404` on the local `nightly` branch of sf4-nightly (the commits merged since the published `v1.6.0-nightly20261112.2`): menu feedback, milestone awards, friends fixes, friends' requests and invites in Notifications, the Archive tab, and the card's bridge package. 125 files, +10,413 / -951. `docs/design/PROFILE.md` was read for intent only.

Method: each merge was read as its own stream (`git diff <merge>^1 <merge>`), then the merged tree at `767d8d404` was read around every change (the owning panels, the shell, the runtime, the helper's op table, the bridge routes and the protocol types). Review standard: `thermo-nuclear-code-quality-review` as written. Nothing was built, run or tested. Every correctness item in section 4 is something reading proves; nothing there is a guess.

## 1. Verdict

**Not approved.** The range fails the approval bar on four presumptive blockers:

- clear structural regressions: a second, third and fourth model of "something waiting in Notifications", kept in step by hand across the shell and the Friends panel (finding 1); the Archive tab grafted onto `ReplaysPanel` by forwarding branches when `ScreenPanel` is the canonical owner (finding 2); the award counters spread over four shapes in three naming schemes, the C++ catalog's thresholds copied into the bridge, and two parallel channels for what a player wears, neither of which is drawn (finding 3);
- a file pushed past 1,000 lines: `server/ember/protocol/src/levels.rs` doubles from 811 to 1,614 (finding 9);
- avoidable duplication of canonical helpers and of whole decode layers (findings 4, 12).

Independently of code quality, the range does not compile (`FriendRequests.hxx` declares `Busy` twice) and fails `LocalizationTest` (175 keys only in `en.po`). See section 4.

Findings by severity: **4 blockers, 5 should fix, 3 worth doing** (12 in all).

## 2. Findings

Ordered by the skill's priority list: structural regressions, missed code-judo, spaghetti, boundaries, file size, modularity, legibility.

### 1. Notifications has four models of "waiting", synchronised by hand (blocker)

Files:
- `src/ui/ApplicationShell.hxx:723` (`Toast` gains `answered` and `key`), `:728` (`NoticeExtra::key`)
- `src/ui/ApplicationShell.cxx:220-225` (Home: News row counts waiting items, Profile row badges the same requests), `:662-663` (`noticeSeen_.clear()` before friends' words), `:665-667` (per-frame loop closing friend toasts no longer in `WaitingOf`)
- `src/ui/ApplicationShell__Updates.cxx:139-148` (two copies of the "waiting toast" predicate `!t.actions.empty()&&!t.answered&&t.key.empty()`)
- `src/ui/ApplicationShell__Toasts.cxx:182-205` (`AnswerToast` calls `friends_.ToastAnswered(t->text)` and routes Accept through `friends_.AnswerToast`), `:247-262` (rows skip toasts whose key `WaitingOf` holds)
- `src/ui/FriendsPanel.hxx:122-131` (`Waiting`), `:150-155` (`Told`), `:171` (`PromptScreen = "post-match"`, a copy of the shell's screen id), `:280-286` (`toasted_`, `spoken_`, `again_`)
- `src/ui/FriendsPanel.cxx:219-235`, `:279-290`, `:477-524` (`toastOf`), `:526-529` (`ToastAnswered` matches toasts **by their display text**), `:566-574`
- `src/ui/PlayerCard.cxx:110-116` (`RequestsWaiting`, `ProfileBadge`)

What is wrong: four structures now answer "what is waiting for the player": the shell's toast deque, `friends_panel::WaitingOf` (requests and invites), the Friends panel's own re-say ledger (`toasted_`, `spoken_`, `Told`), and `player_card::RequestsWaiting`. They are kept consistent by a reconciliation loop in `ApplicationShell::Draw`, by a text-equality back-channel from the shell to the panel, by key filters in two places, and by clearing the shell's notice de-duplication before friends' words. Home now counts the same friend request on two rows (Profile's badge and News' "Waiting: n") that open two different screens. "Say it again if Ember hid it before it could be read" is a property of any toast, yet it lives only in the Friends panel.

Restructure (code judo: one model, sources publish into it):
- Add `src/ui/Notifications.hxx`, pure and testable: `struct Item { std::string key; ToastKind kind; std::string text; std::vector<ToastAction> actions; double at, until; enum class State { Waiting, Answered, Gone } state; bool seen; }` and a class with `Say(Item)` (one-shot notices, key `toast-<n>`), `Publish(source, std::vector<Item>)` (upsert by key; a key new to the model becomes a toast; a key the source no longer publishes becomes `Gone`), `Answer(key, index)`, `Conceal(now)` (marks items whose card had not lapsed as unseen), and `TakeToast(bool free)` (the next new or unseen item).
- `FriendsPanel` publishes `WaitingOf` each frame as keyed items carrying the actions it builds today in `toastOf`; it stops owning `toasted_`, `spoken_`, `again_`, `Told`, `ToastAnswered`, and the `AnswerToast` special path (an answered keyed item dispatches to its source by key prefix, which calls the same `Send`).
- The shell keeps the model instead of `toasts_`; `NotificationsWaiting`, `NewsDetail` and `NotificationRows` become queries on it. The `Draw` loop at `:665-667`, `Toast::answered`, `Toast::key`, `NoticeExtra::key` and the `noticeSeen_.clear()` calls go away.
- Choose one Home row for friend requests. Notifications lists them, so the Profile badge either goes or reads the same model's count; delete one of `RequestsWaiting`/`WaitingOf().requests`.

### 2. The Archive tab is forwarded through `ReplaysPanel` instead of owning its screens (blocker)

Files:
- `src/ui/ReplaysPanel.hxx:35` (`Owns` includes `ArchiveTab::Owns`), `:57-73` (`UpdateArchive`, `OpenArchiveFor`, `Archive()`, `Shortcut` pass-throughs), `:137-138`
- `src/ui/ReplaysPanel.cxx:97`, `:174`, `:259-260`, `:368`, `:386`, `:390`, `:420`, `:442-445`, `:487`, `:508`, `:549`, `:592`: twelve `if (ArchiveTab::Owns/IsRow(...)) return archive_.X(...)` prefixes or cross-clears
- `src/ui/ApplicationShell__Screens.cxx:276-286`, `:306`, `:316-334` (fighter-page card lambda, grid and Dress special cases for `ArchiveTab::FighterScreen`)
- `src/ui/ApplicationShell.cxx:649` (a second per-frame Replays update just for the archive)

What is wrong: `ScreenPanel` (`src/ui/ScreenPanel.hxx:37-69`) is how this shell gives a screen an owner, and the 2026-10-26 review's F-UI-1 exists to stop exactly this kind of `Owns`-forwarding. `ArchiveTab` has its own screens, rows, activation, adjust, choose, status, card, detail, shortcuts and hints, yet it is a member of `ReplaysPanel`, and every `ReplaysPanel` verb now starts with an archive branch. There are also two "watch this set a game at a time" queues (`ReplaysPanel::setQueue_` and `ArchiveTab::queue_`). They share one `set-next` row and clear each other at four sites.

Restructure:
- `class ArchiveScreens final : public ScreenPanel`, registered in `ApplicationShell::ScreenOf`, owning `replay-archive`, `replay-archive-filter` and `replay-archive-fighter` and holding the `ArchiveTab` state. `ReplaysPanel::Tabs()` still lists the Archive tab (the strip is navigation; ownership is the panel's). Every forwarding line above disappears, and so do `UpdateArchive`, `Archive()` and the archive branch of `ReplayScreens::Dress`/`Shortcut`.
- One `WatchQueue` (a few lines in `ReplaysPanel` or its own header) whose items are `std::variant<std::filesystem::path, netplay::archive::Game>`. `Next()` starts a local file directly and an archive game through its download command; `ArchiveTab::StartWatch` and `QueueSet` both fill it. All mutual `ClearQueue()`/`setQueue_.clear()` calls and the `if(setQueue_.empty()&&archive_.Queued())` branch go.
- The fighter page reuses the replay filter's card lambda, parameterised by a `Ticked(int)` predicate.

### 3. Award counters in four shapes; worn awards in two channels, neither drawn (blocker)

Files:
- Protocol: `server/ember/protocol/src/levels.rs:160-186` (`OwnAccount.best_streak/peak_class/rival/opponents`), `:380-424` (`AwardMarks.games/wins/streak/class/rival/opponents`), `:58` (`MAX_FAMILY_ID = 32`), `:426-466` (`is_family_id`, `Worn`), `:550-593` (`PlayerCard.worn`, `pins`)
- Bridge: `server/ember/bridge/src/skill/levels.rs:39-56` (`RIVAL_MARKS`, `OPPONENT_MARKS`: "the awards' own thresholds (`src/netplay/Awards.hxx`)"), `:234-282`
- Game: `src/netplay/Levels.hxx:45-53`, `:70-80` (`Own` extra fields, `CardMarks`); `src/session/TournamentAnswers.cxx:541-551`, `:581-587`; `src/netplay/Awards.hxx:141-145` (`IdShaped`, 24 bytes), `:212-249` (two `CountersOf`), `:271` (`WornOnCard`)
- Worn ids through the room: `src/session/RoomTypes.hxx:220-223`, `:278-280`; `src/session/RoomModel.cxx:267-268`; `src/session/RoomModelJson.cxx:7`, `:383-384`, `:423-425`; `src/session/sf4e__SessionProtocol.hxx:17`, `:263`, `:276-277`, `:285-286`, `:491-492`, `:517-522`; `src/session/sf4e__SessionClient.hxx:118`; `src/sf4e/sf4e__NetplayRuntime.cxx:224-226`; `src/ui/ApplicationShell__Awards.cxx:85-93`

What is wrong:
- The six counters behind the awards exist as `OwnAccount` fields and `AwardMarks` (protocol), `Own` fields and `CardMarks` (C++), and `awards::Counters`, named `best_streak`/`streak`/`bestStreak` and `peak_class`/`class`/`peakClass`. Each layer has its own decode and its own `CountersOf`.
- The bridge hard-codes the C++ catalog's rival and opponent thresholds to bucket a card's counts. Change `RivalLevels` in `Awards.hxx` and other players' cards silently level wrong, with no test spanning the two.
- A worn award id is checked in seven places by three rules: shape in the protocol (32 bytes) and in `IdShaped` (24 bytes); catalog membership when the runtime sends it, in the session protocol's `from_json`, in the room model's `from_json`, in `WornOf` and in `DropStaleAwards`. The session layer and the replicated room model now include the awards catalog, a UI-facing table.
- The card stream added `PlayerCard.worn` (stored by the bridge, written by `card_save`), the same fact the awards stream added to the room member profile. **Neither is drawn:** `WornOnCard` has no caller, and nothing in `src/ui` reads `Member::wornTitle`/`wornIcon` or `card.worn`.

Restructure:
- One protocol type, `AwardCounts { games, wins, best_streak, peak_class, rival, opponents }` with `#[serde(default)]` members and one `check()`. Carry it as `OwnAccount.awards` and `PlayerCard.awards`. In C++, one `DecodeAwardCounts(json)` returns `awards::Counters`; `Own` and `Card` each hold `awards::Counters awards`; `Levels.hxx` loses the four `Own` fields and `CardMarks`; `CountersOf(card)` reduces to the cast and frame from the fighters plus `card.awards`.
- Decide the bucketing once. The card already shows exact games, wins and streak, so the simplest consistent rule is exact rival and opponents too. If the milestones must stay, the thresholds move to the protocol crate (`ember_protocol::levels::RIVAL_MARKS`), and a C++ test pins `Awards.hxx` to the same numbers.
- Keep one channel for what a player wears: the bridge card, which is attested and already carries `worn` and `pins`. Delete `MemberProfile`/`Member` `wornTitle`/`wornIcon`, their wire keys, `SetProfileAwards`, and the `Awards.hxx` includes in `RoomModelJson.cxx` and `sf4e__SessionProtocol.hxx`. Viewers level `card.worn` with `card.awards` through `WornOf`. Use one shape rule (32 bytes, `a-z0-9-`, one shared constant) and check catalog membership once, when drawing.

### 4. The archive listing has three hand-written decoders, and the helper finds the archive by probing (should fix)

Files:
- `server/ember/bridge/src/routes/archive.rs:169-201` (the answer built with `json!`, ordered by `sort`), `:207-220`
- `rust/sf4-net/src/tournament/archive.rs:31-48` (limits, `ROUNDS`), `:100-106` and `:375-385` (cursor alphabet twice), `:161-213` (`bases`, `known()`, `ordered`), `:215-405` (`Game`, `SetInfo`, `Page`, `clip`, `text_of`, `pair_of`, `count_of`, `texts_of`, `decode_game`, `decode_page`), `:408-448`
- `src/session/TournamentAnswers.cxx:632-733` (`MaxArchive*`, `ArchiveText`, `Pair`, `Whole`, the cursor alphabet a third time)
- `src/netplay/Archive.hxx:26-28`; `src/ui/ArchiveTab.cxx:26`

What is wrong:
- The bridge writes the listing ad hoc. The helper reads it as `serde_json::Value` with bespoke clip and pair readers, then serialises its own `Page` and renames `sort` to `order`. The game decodes it a third time.
- The limits (400 games, 100 events, 300 countries, 96-byte text, 512-byte URL, 200-byte cursor), the cursor alphabet and the round list are each copied two or three times.
- The helper finds the archive by trying `/archive/v1/archive`, then `/v1/archive`, then an `archive.` sibling host, and remembers the winner in a process-global `known()` map outside `Shared`. The bridge already knows its archive's public base (`config.archive.public_base`), and the helper already has a capability mechanism (`play::Features`) that the levels and profile ops use.
- The range now tolerates the wire in three different ways: serde types plus strict `check()` on both sides (levels, cards); `Value` plus clipping on both sides (archive); catalog filters inside `from_json` (worn ids).

Restructure:
- Put `ArchivePage`, `ArchiveGame` and `ArchiveSet` in `ember_protocol::replays`. They deserialise with defaults and carry a `check()` for the real invariants (64-hex digest, non-empty URL, cursor alphabet). The bridge serialises them.
- The helper does `client::request_as::<ArchivePage>`, then `check()`, then `to_json`, as `levels::search` does. About 150 lines of helper decoding go.
- The bridge lists an `archive` capability carrying its public base. The helper asks one URL, and `bases`, `ordered`, `known` and the 404 fall-through go; `game_digest` checks fetch URLs against that base.
- The C++ decoder remains the single defensive reader at the pipe. Its limits and the round list come from one header that mirrors the protocol's constants.

### 5. The request and paging state machines are duplicated across layers (should fix)

Files:
- `src/ui/ArchivePaging.hxx:24-74` (`Memory`, `NextAsk`) against `src/netplay/ArchiveRequests.hxx:40-80` (`Begin`, `Cursor`, `BeginNext`, `kind_`, `ticket_`)
- `src/netplay/CardRequests.hxx:115-131`, `:138-152`, `:193` (`overtaken_` as a `std::set<std::string>`)
- `src/ui/FriendsPanel.hxx:138-142` (`Outcome`, `GiveUpSeconds = 75`), `src/ui/FriendsPanel.cxx:243-247`, `:413-431`, against `src/netplay/BridgeFlights.hxx` (`GiveUpMs = 60000`) and `src/netplay/FriendRequests.hxx:55-57`, `:77-97` (`Dropped`, `Busy`, `Scope`)

What is wrong:
- Whether the archive's first or next page has been asked for is decided twice: by the UI's `archivepaging::Memory`, because a command takes a tick to arrive, and by the runtime's `Requests`.
- Stale answers are suppressed three ways: `FriendRequests` by flight token, `ArchiveRequests` by ticket number, `CardRequests` by a set of overtaken token strings.
- The friends stream made the runtime guarantee that every action ends in an answer or a refusal, yet the panel keeps its own 75-second deadline and `Outcome::GivenUp` as a second timer for the same invariant.

Restructure:
- Make the archive command declarative. The tab submits `ArchiveWant{bridge, filter, nearEnd, retry}` only when one of those changes, and `Requests` compares it with what it holds and calls `Begin` or `BeginNext`. Delete `ArchivePaging.hxx` and move `WhenThePagesAreAsked` onto `Requests`.
- In `CardRequests`, replace `overtaken_` with a `std::uint64_t generation_` that `Refresh()` bumps and `Sent()` captures. `Finish` treats `sentGeneration != generation_` exactly as it treats an overtaken token today.
- Drop the panel's `GiveUpSeconds` and `Outcome::GivenUp`. A command the queue refuses is already reported by `Send`'s return value.

### 6. `tournament::Command` grows another op-specific block (should fix)

Files: `src/netplay/TournamentStatus.hxx:58-100`; `src/sf4e/sf4e__NetplayRuntime__Tournament.cxx:514-517`; `src/sf4e/sf4e__NetplayRuntime__Archive.cxx:81-85`.

What is wrong: the archive adds six fields (`archiveFilter`, `archiveNext`, `archiveAgain`, `archiveUrl`, `archiveDigest`, `archiveWatch`) to the shared command struct. `Valid()` and `Bytes()` gain 14 terms, including `archiveWatch.path.empty()`. The friends commands already show the canonical shape: typed alternatives of a `std::variant`, each with its own `Valid()` and `Bytes()` (`src/netplay/FriendCommands.hxx:153-154`).

Restructure: `commands::ArchiveList{bridge, filter, next, again}` and `commands::ArchiveWatch{bridge, url, digest, watch}` as `RuntimeRequest` alternatives, with `DispatchArchive` overloaded by type as `DispatchFriends` is. `Command` loses the block, along with `Op::ArchiveList`/`Op::ArchiveWatch` and the case in `DispatchTournament`.

### 7. Special cases bolted into the shell and the replay flow (should fix)

Files:
- `src/ui/ApplicationShell.cxx:200` (`selectionAvailable_` set as a side effect of building rows), `:208-209` (rewrites `enabled` on any row whose id is `"selection"`, for every `ScreenPanel` owner)
- `src/ui/ApplicationShell.hxx:383-384`
- `src/ui/ApplicationShell__Screens.cxx:189-197` (Profile's X shortcut reads the previous frame's flag and writes `selectionFresh_`/`selectionOpenOn_` itself, duplicating `ApplicationShell.cxx:415`)
- `src/ui/ApplicationShell__Toasts.cxx:202` (`NewsFamily` Replace-or-Push branch)
- `src/sf4e/sf4e__NetplayRuntime__Replays.cxx:42-44` (function-local `static bool cleared` in a tick)
- `src/platform/ReplayFiles.cxx:580-596` (`ArchiveWatchPath`, a path query, deletes files)

What is wrong: `FighterSelectRow` is `"selection"`, the same id the room board's Change fighter row uses (`RoomScreens` is a `ScreenPanel`). The Profile fix therefore patches the room's row too, from inside the shell's generic dispatch. One-time initialisation hides in a tick, and a getter has side effects.

Restructure:
- Add `bool selectionAvailable` to `ScreenFrame` (`src/ui/ScreenPanel.hxx:15-21`). `ProfilePanel::Rows` sets its own row's `enabled`, and the shell keeps no member and patches no rows.
- Give the shell one `OpenSelection(screen)`, called by both `HandleActivate`'s `"selection"` branch and the Profile shortcut.
- Clear the watching folder once in `StartHelper`.
- Split `ArchiveWatchPath` into a pure path function and `PruneArchiveWatching(keep)`, called by `DispatchWatch`.

### 8. "Is a Discord account linked" is derived in six readers from four answers (should fix)

Files:
- `src/ui/PublicRoomsPanel.cxx:416-430` (`UnrankedPlay`: the Ember ID panel's account read, else the ranking standing, else the points answer, with "any read that finds an account wins")
- `src/ui/IdentityPanel.cxx:157-163` (`DiscordLinked`)
- `src/ui/IdentityConnect.cxx:31-46` (`DiscordAccountRow` lets `ShowAccount` set a value, then rewrites it: user text becomes "Linked · name", any other non-empty value becomes "Not linked")
- `src/ui/ApplicationShell__Create.cxx:42` and `src/ui/PublicRoomsPanel.cxx:491-496` (the same notice under two ids, `ranked-note` and `pr-ranked`, drawn by two widgets)
- also `ApplicationShell__Awards.cxx:29-37` (`CardLevels::NoAccount`), `FriendsPanel` `Mode::NoAccount`, `PlayerCard` `CardLevels::NoAccount`

What is wrong: each reader infers account presence from answers of different freshness, and the precedence rule lives inside one panel's static helper. The next feature will derive it again.

Restructure:
- One `netplay::DiscordLink { enum class State { Unknown, Linked, None } state; std::string user; }`, computed once per frame by a single tested function that holds the precedence rule, and passed through `ShellView` or `ScreenFrame`.
- `UnrankedPlay` becomes `!idReady || link.state == None`, and `DiscordAccountRow` words the link directly instead of rewriting another function's output.
- One builder for the ranked notice row, used by Create and Public rooms.

### 9. File size: one file doubles past 1,000 lines; two more cross (blocker for `protocol/src/levels.rs`)

Files: see the table in section 3.

What is wrong:
- `server/ember/protocol/src/levels.rs` grows from 811 to 1,614 lines: about 480 lines of card, profile and search types (`:330-807`) plus a test module of about 800 lines (`:809-1614`).
- `server/ember/bridge/tests/profiles.rs` is new at 1,038 lines.
- `src/tests/tournament_fuzz_test.cxx` crosses from 992 to 1,017.
- Two files that were already over keep growing: `rust/sf4-net/src/tournament/mod.rs` (1,585 to 1,634) and `server/ember/bridge/tests/archive.rs` (1,794 to 1,845).
- Within reach of the line: `ApplicationShell.hxx` (838, +34, mostly nine static award helpers that touch no shell state), `ApplicationShell.cxx` (914), `FriendsPanel.cxx` (671 to 856, +185), the new helper `tournament/archive.rs` (820, half of it tests).

Restructure:
- Split the protocol into `levels.rs` (own points, frames, look-up) and `cards.rs` (`PlayerCard`, the award counts of finding 3, `Worn`, `Style`, `Versus`, `CardSave`, `CardSearch`, `SearchHit`, `SearchAnswer`), with `levels/tests.rs` and `cards/tests.rs`.
- Split `bridge/tests/profiles.rs` by route (save, search, look-up) around a shared `common::profiles::World`.
- Move the levels and cards cases out of `tournament_fuzz_test.cxx` into a `levels_answers_test.cxx`.
- In the helper, move the per-op `bridge_request` arms next to the op modules (or into `tournament/dispatch.rs`); `requests!` already names every op.
- Move the award helpers off `ApplicationShell` (finding 11).
- Findings 1 and 2 delete most of the `FriendsPanel.cxx` growth and the `ReplaysPanel` forwarding.

### 10. The friends runtime mixes logging, effects and guessing at where a refusal came from (worth doing)

Files: `src/sf4e/sf4e__NetplayRuntime__Social.cxx:111-136`, `:173-205`, `:224-227`, `:364-389`; `src/ui/RoomActions.cxx:296`.

What is wrong:
- `ActionName` is a name array that parallels the variant's index: the count is asserted, the order is not.
- `TakeAnswer` decides whether a refusal was "never sent" or "timed out" from `requestId == 0` plus the code, and interleaves that logging with the cards' `Refresh` and `Relate`.
- `ChangeOf` re-infers "an Add friend to someone who asked first is a friendship" from the card's relation, which `RoomActions.cxx:296` already worked out for the words.
- `NoteScope` wraps seven `Scope` calls only to log.

Restructure:
- A `static constexpr const char* LogName` on each friend command type, read with `std::visit`.
- `BridgeFlights` marks its own refusals with a typed `local` flag instead of a zero request id.
- The `FriendRequest` command carries the relation the UI already computed.
- One runtime `ScopeFriends(bridge, id)` that logs, used by all seven call sites.

### 11. The bridge assembles the card in its route, and other small duplications (worth doing)

Files:
- `server/ember/bridge/src/routes/levels.rs:126-169`: `levels::card`, then `levels::card_marks` (which reads `account_points` a second time), then `cards::badge`, `cards::stored` and `describe`, `store::record` turned into `Versus`, and `store::friendship_id`
- `server/ember/bridge/src/routes/profiles.rs:85-104` and `routes/levels.rs:60-63` build `Classes` identically
- `server/ember/protocol/src/levels.rs:531-545` (`Versus`) duplicates `friends::PairRecord` (`server/ember/protocol/src/friends.rs:551`) field for field
- `src/ui/ApplicationShell.hxx:416-436`: nine stateless award helpers declared as static members of `ApplicationShell`

Restructure:
- `cards::card_for(tx, viewer, target, &classes, friends_offered, now)` in `cards.rs`; the route only resolves selectors. `levels::card` returns its `Counted`, so the counts are read once.
- `PlayerCard.versus: Option<PairRecord>`, and a `Classes::of(&state)` constructor.
- The award helpers move into an `awards_view` namespace in `ApplicationShell__Awards.cxx`, or an `AwardsPanel` holding `awardWatch_`, `awardRevision_` and `awardsFocus_`.

### 12. Canonical helpers re-implemented, and rules applied halfway (worth doing)

Files:
- `src/ui/ArchiveTab.cxx:46-52` (`When`) re-implements `src/common/LocalTime.hxx:17`, with the same default format
- `src/ui/ArchiveTab.cxx:81-84` (`NeedsMenu`) copies `src/ui/ReplaysPanel.cxx:39`
- `FighterName` now exists at `ArchiveTab.cxx:30` and `TrainingPanel.cxx:213`
- The invite "named room" rule is written at `FriendsPanel.cxx:281` and `:494`
- `Hairline()` (`src/ui/Theme.cxx:135`) is used for 20 strokes, while 28 draw calls in `src/ui` still pass `s` as the line thickness, including code this range wrote (`ApplicationShell__Toasts.cxx:133`, `:166`; `ApplicationShell__Awards.cxx:258`, `:289`; `ReplaysPanel.cxx:599`)
- `MinThemeScale = .6f` (`src/ui/Theme.hxx:87`) contradicts its own comment, which derives it from `MinInterfaceScale` (0.70) on a 100% display
- `Awards.hxx:120-129` stores raw indices into `v2::Icon`'s table from a netplay header
- `sf4e__SessionProtocol.hxx:519` picks its target by `key[4] == 'T'`

Restructure:
- Reuse `LocalTime`, `NeedsMenu` and one `FighterName`.
- Add `v2::Outline(draw, box, colour, rounding)`, so a caller cannot pass its own thickness.
- Name the icons with an `enum class AwardIcon`, mapped in `V2.cxx`.
- Two plain assignments in place of the `key[4]` loop (gone anyway with finding 3).

## 3. File sizes

Every file the range touched, by line count at `ea0e13dee` and at `767d8d404`. Not listed: 46 touched source, test and build files that stay under 300 lines; the docs (`docs/design/PROFILE.md` grew from 519 to 911 lines but is not code); and `locales/en.po` (8,597 to 9,014, a catalog).

Crossed 1,000 in this range: `server/ember/protocol/src/levels.rs`, `server/ember/bridge/tests/profiles.rs` (new), `src/tests/tournament_fuzz_test.cxx`. Over before and grew: `rust/sf4-net/src/tournament/mod.rs`, `server/ember/bridge/tests/archive.rs`.

| File | Before | After | Change | Status |
|---|---:|---:|---:|---|
| `server/ember/bridge/tests/archive.rs` | 1794 | 1845 | +51 | over before, grew |
| `rust/sf4-net/src/tournament/mod.rs` | 1585 | 1634 | +49 | over before, grew |
| `server/ember/protocol/src/levels.rs` | 811 | 1614 | +803 | **crossed 1,000** |
| `server/ember/bridge/tests/profiles.rs` | 0 | 1038 | +1038 | **crossed 1,000** |
| `src/tests/tournament_fuzz_test.cxx` | 992 | 1017 | +25 | **crossed 1,000** |
| `src/sf4e/sf4e__Overlay.cxx` | 994 | 995 | +1 | under, within 200 |
| `src/sf4e/sf4e__NetplayRuntime.cxx` | 942 | 947 | +5 | under, within 200 |
| `src/ui/ApplicationShell.cxx` | 903 | 914 | +11 | under, within 200 |
| `src/sf4e/sf4e__NetplayRuntime__Status.cxx` | 905 | 906 | +1 | under, within 200 |
| `src/tests/public_rooms_journey_test.cxx` | 870 | 905 | +35 | under, within 200 |
| `server/ember/bridge/tests/levels.rs` | 749 | 899 | +150 | under, within 200 |
| `src/tests/settings_store_test.cxx` | 873 | 892 | +19 | under, within 200 |
| `server/ember/bridge/src/archive/listing_tests.rs` | 870 | 875 | +5 | under, within 200 |
| `src/ui/FriendsPanel.cxx` | 671 | 856 | +185 | under, within 200 |
| `src/ui/lab/UiLab.cxx` | 790 | 852 | +62 | under, within 200 |
| `src/ui/ApplicationShell.hxx` | 804 | 838 | +34 | under, within 200 |
| `rust/sf4-net/src/tournament/archive.rs` | 0 | 820 | +820 | under, within 200 |
| `src/platform/ReplayFiles.cxx` | 787 | 811 | +24 | under, within 200 |
| `server/ember/bridge/src/friends/store.rs` | 775 | 799 | +24 | under |
| `src/ui/IdentityPanel.cxx` | 774 | 782 | +8 | under |
| `src/ui/ApplicationShell__Settings.cxx` | 751 | 756 | +5 | under |
| `rust/sf4-net/src/tournament/play.rs` | 748 | 755 | +7 | under |
| `src/ui/GameMenuDraw.cxx` | 735 | 735 | +0 | under |
| `src/session/TournamentAnswers.cxx` | 606 | 735 | +129 | under |
| `src/session/RoomModelJson.cxx` | 694 | 701 | +7 | under |
| `src/tests/ui_render_training.hxx` | 697 | 697 | +0 | under |
| `src/ui/V2.cxx` | 706 | 690 | -16 | under |
| `src/session/RoomModel.cxx` | 685 | 687 | +2 | under |
| `src/sf4e/sf4e__NetplayRuntime.hxx` | 680 | 687 | +7 | under |
| `src/ui/ReplaysPanel.cxx` | 619 | 643 | +24 | under |
| `server/ember/bridge/src/archive/listing.rs` | 619 | 629 | +10 | under |
| `rust/sf4-net/src/tournament/levels.rs` | 231 | 623 | +392 | under |
| `src/session/RoomTypes.hxx` | 614 | 621 | +7 | under |
| `src/sf4e/sf4e__NetplayRuntime__Tournament.cxx` | 603 | 607 | +4 | under |
| `src/session/sf4e__SessionProtocol.hxx` | 562 | 579 | +17 | under |
| `src/ui/PublicRoomsPanel.cxx` | 547 | 568 | +21 | under |
| `server/ember/bridge/src/skill/mod.rs` | 527 | 530 | +3 | under |
| `src/ui/ArchiveTab.cxx` | 0 | 527 | +527 | under |
| `src/ui/Theme.cxx` | 519 | 520 | +1 | under |
| `server/ember/bridge/src/skill/levels.rs` | 371 | 518 | +147 | under |
| `rust/sf4-net/src/tournament/play/tests.rs` | 508 | 512 | +4 | under |
| `src/ui/PublicRoomsCards.cxx` | 501 | 511 | +10 | under |
| `src/session/RoomQueries.hxx` | 480 | 497 | +17 | under |
| `CMakeLists.txt` | 493 | 494 | +1 | under |
| `src/sf4e/sf4e__NetplayFacade.hxx` | 477 | 480 | +3 | under |
| `src/sf4e/sf4e__NetplayRuntime__Social.cxx` | 367 | 467 | +100 | under |
| `cmake/sf4e_test_targets.cmake` | 433 | 437 | +4 | under |
| `server/ember/bridge/tests/levels_migrations.rs` | 267 | 435 | +168 | under |
| `server/ember/bridge/src/db/migrations_ledger_tests.rs` | 431 | 433 | +2 | under |
| `src/ui/PlayerCard.cxx` | 421 | 425 | +4 | under |
| `src/tests/identity_journey_test.cxx` | 418 | 418 | +0 | under |
| `src/ui/IdentityPanel.hxx` | 405 | 409 | +4 | under |
| `src/ui/RoomActions.cxx` | 401 | 403 | +2 | under |
| `src/tests/profile_journey_test.cxx` | 340 | 403 | +63 | under |
| `src/tests/room_friends_journey_test.cxx` | 235 | 399 | +164 | under |
| `server/ember/bridge/src/db/migrations.rs` | 390 | 392 | +2 | under |
| `src/tests/shell_feedback_journey_test.cxx` | 373 | 373 | +0 | under |
| `src/tests/friends_panel_test.cxx` | 252 | 369 | +117 | under |
| `src/tests/shell_replay_journey_test.cxx` | 356 | 356 | +0 | under |
| `server/ember/bridge/src/cards.rs` | 120 | 356 | +236 | under |
| `src/ui/ApplicationShell__Screens.cxx` | 324 | 352 | +28 | under |
| `src/ui/FriendsBoard.cxx` | 350 | 351 | +1 | under |
| `src/tests/shell_archive_journey_test.cxx` | 0 | 351 | +351 | under |
| `server/ember/bridge/src/routes/mod.rs` | 346 | 349 | +3 | under |
| `src/tests/friend_requests_test.cxx` | 337 | 348 | +11 | under |
| `src/tests/shell_update_journey_test.cxx` | 259 | 336 | +77 | under |
| `src/netplay/PlayerPreferences.hxx` | 315 | 319 | +4 | under |
| `src/netplay/Awards.hxx` | 0 | 310 | +310 | under |
| `src/ui/ApplicationShell__Awards.cxx` | 167 | 309 | +142 | under |
| `src/tests/archive_requests_test.cxx` | 0 | 307 | +307 | under |

## 4. Correctness (not code quality): will not compile, will fail its tests, or contradicts itself after the merge

### Will not compile

**C1. `Requests` declares `Busy` twice.**
- Files: `src/netplay/FriendRequests.hxx:57` (`static constexpr const char* Busy = "friends_busy";`) and `:68` (`bool Busy() const`).
- A class may not have a data member and a member function with the same name (MSVC C2365; GCC "conflicts with a previous declaration"). Every translation unit that includes the header fails, which takes in the runtime (`sf4e__NetplayRuntime.hxx:49`) and `friend_requests_test.cxx`.
- Introduced by `10bba5883`. Its uses are `sf4e__NetplayRuntime__Social.cxx:106`, `:384` and `:385` (the code) and `:452`, plus `friend_requests_test.cxx` (the function).
- Fix: rename the constant, for example `BusyCode`, next to `Dropped`.

### Will fail its tests

**C2. `LocalizationTest` fails: new keys only in English.**
- Files: `src/tests/localization_test.cxx:181-188`.
- The test requires every locale catalog to hold exactly `en.po`'s keys. At `767d8d404`, `en.po` has 3,125 msgids and every other locale still has 2,986. 175 keys exist only in English: the award families, the archive, Notifications, Discord account, ranked notice, friends' words and Interface size. 36 keys exist only in the other locales: the removed `award.entrant.*` through `award.cup-champion.*` sets, `discord.tournament_account*` and `awards.detail_locked`.
- The test exits at the first locale it finds lacking a key (`localization_test.cxx:185-186`).

### Contradictions between streams after the merge (they compile and pass their tests, but the merged code disagrees with itself)

**C3. "Find this player's sets" never uses the exact-name filter added for it.**
- The card stream added `name` (an exact side name) to the bridge listing (`server/ember/bridge/src/routes/archive.rs:113-115`, `:152-158`; `archive/listing.rs:40-44`, `:235-237`, `:296`), and its docs say the helper uses it (`server/ember/protocol/src/levels.rs:45-48`, `PROFILES_FEATURE` "and the archive's `name`"; `rust/sf4-net/src/tournament/play.rs:71-73`, `Features::profiles` "the archive's `name`").
- The helper's `archive::Query` has no `name` member and is `deny_unknown_fields` (`rust/sf4-net/src/tournament/archive.rs:59-78`). The game's "Find this player's sets" sends the substring filter `q` (`src/ui/ReplaysPanel.hxx:65-70`, `src/ui/ArchiveTab.cxx:401-409`, `src/sf4e/sf4e__NetplayRuntime__Archive.cxx:22-31`).
- So "Ann" also lists "Anna", "Joanne" and any event whose title contains "Ann". The bridge's exact filter is unreachable from the game, and adding `name` to a game request today would be refused as `invalid_request` by every released helper.

**C4. The card's `marks` are exact counts now; the C++ contract still says milestones.**
- `d34a05d56` made the bridge send exact games, wins and best streak (`server/ember/bridge/src/skill/levels.rs:262-282`; `bridge/tests/levels.rs:799-805` asserts `{"games": 6, "wins": 5, "streak": 3}`).
- The game's contract text still says otherwise: `src/netplay/Levels.hxx:70-73` ("the largest mark the bridge offers that it has reached, never the count itself"), `src/session/TournamentAnswers.cxx:541-542` ("marks, never counts"), and `src/netplay/Awards.hxx:20-22` and `:230-233`.
- The C++ code works for either reading, because it feeds the values to `LevelOf`, but the type is now misnamed and its documented guarantee is false. The rival and opponent buckets still depend on thresholds copied from `Awards.hxx` (finding 3).

**C5. The worn-award plumbing has no consumer, and the two id rules disagree.**
- `23a00b817` carries `wornTitle`/`wornIcon` through the join profile, the room snapshot and the session protocol (finding 3 lists the lines), and `ac42d165f` adds `PlayerCard.worn`. Nothing draws either: `netplay::awards::WornOnCard` (`src/netplay/Awards.hxx:271`) has no caller, and no file under `src/ui` reads `Member::wornTitle`, `Member::wornIcon` or a card's `worn`.
- The bridge accepts family ids up to 32 bytes (`protocol/src/levels.rs:58`, `:426-433`); the game drops anything over 24 (`src/netplay/Awards.hxx:141-145`). The current catalog's longest id is 9 bytes, so nothing breaks yet.

### Checked and consistent

- The `Toast` aggregate (9 members) and its one constructor (`ApplicationShell__Toasts.cxx:72`).
- `NoticeExtra` and `Say` (`ApplicationShell.hxx:150-154`).
- Home's rows: no test still selects the removed `selection` or `notifications` Home rows; `NewsAndNotifications` asserts the new shape.
- `ShellView`: `earnedAwards` was removed with no remaining reader; `archiveBrowse` is filled at `sf4e__Overlay.cxx:462`.
- The helper's op table: 49 entries in `requests!` matching the test's count after `767d8d404`. `IsTournamentPlayOp` (`TournamentAnswers.cxx:205-212`) lists `archive_list` and `archive_fetch`; `card_save` and `cards_search` are not sent by the game yet, so their absence there is not a failure today.
- `friends::FriendAction`'s 14 alternatives match `ActionName`'s `static_assert`.
- Every `loc::T`/`loc::Tf` literal the range added, and every award key the `SF4E_LEVEL`/`SF4E_FAMILY` macros build, exists in `en.po`.
- The new test targets are registered (`cmake/sf4e_test_targets.cmake:84-88`, `:175`), `ArchiveTab.cxx` and `sf4e__NetplayRuntime__Archive.cxx` are in `CMakeLists.txt`, and `ArchiveJourneys`, `NewsAndNotifications` and `SelectorFromProfile` are declared and called.

## 5. Not reviewed

- `docs/design/PROFILE.md`, `REPLAY_ARCHIVE.md`, `FRIENDS.md`, `FIGHTER_LEVELS.md` and the guides: read for intent only.
- `en.po` wording, and the 14 other locales beyond the key-set check in C2.
- The bodies of the Rust bridge tests (`tests/profiles.rs`, `tests/levels.rs`, `tests/levels_migrations.rs`, `tests/archive.rs`): read for structure and helper names, not run through in full. Whether every bridge test passes (for example, whether a ticket issued by `token()` leaves a class in `marks` that `{"games":6,"wins":5,"streak":3}` would then not equal) was not established.
- Migrations 063 and 064: read for shape and backfill intent only; not run against a production-shaped database.
- The drawing math in `ArchiveTab.cxx`, `ApplicationShell__Awards.cxx`, `PublicRoomsCards.cxx` and `PlayerCard.cxx`, and `src/ui/lab/UiLab.cxx`.
- Behaviour in the running game, the helper or the bridge: nothing was built, installed, launched or tested.
