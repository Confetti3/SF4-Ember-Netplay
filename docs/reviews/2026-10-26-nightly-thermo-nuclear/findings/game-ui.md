# Findings: game UI (src/ui), launcher/updater (src/launcher), Discord companion (src/discord)

Reviewed at nightly head b9ce0e31, read-only. Vendored ImGui backends skipped. No build or run, so every
overhead figure below is a count from the code (allocations, copies, calls), not a measurement.
"Per drawn frame" means each overlay frame on which Ember's menu is shown (about 60 Hz on a 60 Hz
display); "every frame" means every overlay frame, including match frames with the menu hidden.

### F-UI-1: Screen dispatch is 26 string `Owns(screen)` checks over four panels with four different shapes
- File: src/ui/ApplicationShell.cxx:256  (also :258 :260 :329 :450-452 :476 :493-502 :545 :550 :844-861 :877 :903 :925-946)
- File: src/ui/IdentityPanel.hxx:103  src/ui/PublicRoomsPanel.hxx:57  src/ui/RankingsPanel.hxx:43  src/ui/ReplaysPanel.hxx:39
- Category: judo
- Severity: P1
- Claim: Every phase of a frame asks each panel in turn "is this your screen?": BuildRows (:256-260), UpdateStatus
  (:450-452, :476), HandleActivate (:493-502), HandleAdjust (:545, :550), and Draw's card/preview/shortcut/Chosen
  chains (:844-946). That is 26 `Owns(screen)` string checks in ApplicationShell.cxx alone. The four panels
  implement the same verbs (Update, Rows, Status, Activate, Accept/Choose, Shortcut, Refreshable, TakeSaid,
  Conceal) with nine different signatures: `IdentityPanel::Status(status,tone,now)`,
  `PublicRoomsPanel::Status(view,screen,status,tone,now)`, `RankingsPanel::Status(view,screen,status,tone)`;
  Activate returns void in one panel and bool in three, takes `submit`/`error` in one only, and so on.
  The 2026-09 review already called for "per-screen row builders, one dispatcher"
  (docs/reviews/2026-09-code-quality-review.md:47). Draw was split into named parts; the dispatcher was not built.
  Each new screen adds one more `Owns` branch to every phase.
- Proposed change: Add `struct ScreenContext { const ShellView& view; const std::string& screen; MenuNavigation& nav;
  const Submit& submit; double now; ShellSink& sink; }` (the sink takes notices and errors, replacing TakeSaid and the
  `std::string& error` out-params) and `class ScreenPanel` with virtual `Owns`, `Update`, `Rows`, `Status`, `Activate`,
  `Adjust`, `Choose`, `Shortcut`, `Card`, `Detail`, `Conceal` (defaults do nothing). Make PublicRoomsPanel's richer
  shapes the canonical ones (Status with view+screen+now, bool-returning Activate). ApplicationShell keeps a
  `std::array<ScreenPanel*,N>`, resolves `owner = FindOwner(screen)` once per frame, and each phase calls `owner->X(ctx)`.
  The shell's own screens (home, settings, interface, about, profile, ...) become one more `ScreenPanel`
  (`SettingsScreens`), moving about 150 lines of BuildRows/HandleActivate/HandleAdjust out of ApplicationShell.cxx.
  Cross-panel needs (`identity_.UsableBridge(v)`, `identity_.Waiting()`, `identity_.PublicSetup()`) become context fields.
- Behavior preserved: same rows, same status precedence (the final override order in UpdateStatus stays in the shell,
  after `owner->Status`), same action routing.
- Behavior at risk: the special cases that cross panels (the Rankings screen forwarding DiscordConnectScreen to
  identity_, public rooms "pr-quick"/"pr-setup" handled by the shell) must move into the owning panel or stay as an
  explicit shell pre-dispatch; the order `identity_.Update` before `publicRooms_.Update` must be kept.
- Estimated saving: removes 26 dispatch branches and roughly 120 lines from ApplicationShell.cxx; one concept
  ("a screen has an owner") replaces four ad-hoc call shapes.
- Confidence: high

### F-UI-2: The on/off preference rows and their Left/Right handler re-enumerate `netplay::BoolPreferences` by hand
- File: src/ui/ApplicationShell.cxx:565  (HandleAdjust bool branches :565-585)
- File: src/ui/ApplicationShell.cxx:298  (13 `x?loc::T("common.on"):loc::T("common.off")` Value rows in BuildRows)
- File: src/netplay/BoolPreferences.hxx:15
- Category: judo
- Severity: P2
- Claim: `netplay::BoolPreferences` already lists every on/off preference as `{key, &PlayerPreferences::member}`, and
  MenuRows.hxx:8 uses it for SamePreferences. HandleAdjust still has about 20 hand-written branches of the form
  `else if(a.id=="ready-sound")preferences_.readySound=a.delta>0;`, and BuildRows has 13 copies of the on/off value
  expression. Adding a preference means touching three lists that can drift (one is already inverted:
  "game-reads-pads" sets `controllerLayer=a.delta<0`).
- Proposed change: Give the UI its own table beside the netplay one: `struct BoolRow { const char* rowId; bool
  PlayerPreferences::* member; bool inverted; }` (or add `rowId` to BoolPreferenceMember). HandleAdjust's bool branches
  become one loop: `for (row : BoolRows) if (a.id == row.rowId) preferences_.*row.member = (a.delta > 0) != row.inverted;`.
  BuildRows uses `OnOffRow(id, label, detail, enabled)`, which reads the same table for the value text.
- Behavior preserved: identical row ids, labels, details and enablement; same validate-and-save tail.
- Behavior at risk: none identified (ids are only matched in-process).
- Estimated saving: about 35 lines, and one list of on/off settings instead of three.
- Confidence: high

### F-UI-3: Row ids are a string wire format that draw code parses back with `stoi`/`stoull` (27 sites)
- File: src/ui/RoomPanel.cxx:466  (:493 :549 :634 :773 :783 :805)
- File: src/ui/ApplicationShell.cxx:514  (:865 :873; "pad-"+id compare at :869)
- File: src/ui/FighterSelector.cxx:361  (:372-376 :392 :414 :454-470)  src/ui/TrainingPanel.cxx:593  src/ui/RankingsCards.cxx:124
- Category: boundary
- Severity: P2
- Claim: BuildRows/RoomEntries encode model keys into `MenuEntry::id` ("table-"+N, "member-"+ID, "main-"+N,
  "fighter-"+N, "costume-"+N, "pad-"+N, "sent-"+N), and the board, the grids and the action handlers decode them with
  `compare(0,6,"table-")` plus `std::stoi(id.substr(6))`, then find the model object again (`room::FindMember`, a linear
  search, per card). Every row and every card on every frame pays the string build and the parse. A malformed id throws
  (`stoi`) inside a draw callback.
- Proposed change: Add `std::int64_t key = -1;` (or `enum class RowKind kind; std::uint64_t key;`) to `MenuEntry`.
  Builders set it (`Row(...).Keyed(RowKind::Table, table.id)`); draw code and handlers read `e.key`. Keep the string id
  for focus memory and test probes. DrawRoomBoard's `tableCard`/`memberCard` then index `v.room.tables[e.key]` and use a
  member index built once per frame instead of `FindMember` per card.
- Behavior preserved: ids stay the same strings, so navigation focus, probes and tests are unchanged.
- Behavior at risk: none identified.
- Estimated saving: 27 parse sites deleted; per drawn room-board frame about 20 substr allocations and 20 linear member
  searches (16 members, 4 tables). Transport n/a (in-process).
- Confidence: high

### F-UI-4: `ShellView` deep-copies the published runtime snapshot on every drawn frame
- File: src/sf4e/sf4e__Overlay.cxx:277  (DrawApplicationHome, field-by-field copy to about :420; cross-area)
- File: src/ui/ApplicationShell.hxx:45
- Category: overhead
- Severity: P2
- Claim: The frame already holds `GetPresentationSnapshotShared()` (a shared, immutable snapshot), yet
  DrawApplicationHome copies about 100 fields into a fresh `ShellView` per drawn frame, including `room::Snapshot`
  (16 members with names, 4 tables with queue and spectator vectors, and up to 100 chat messages), `members`,
  `preferences` (controller mappings and button layouts), `services` (the sent-reports vector), `pads`, `publicRooms`
  (up to about 45 rooms, each with 4 strings and a fighters vector), `tournament`, `identity` and `memberLatency`.
  The hidden path (ConcealApplicationHome, :259) already avoids copying the room "since this runs every match frame",
  so the cost is known; the drawn path still pays it. The one real reason for the copy is that the overlay patches
  `view.room` in place (member idleSeconds, table holdRemainingMs/readyRemainingMs from roomReceivedMs).
- Proposed change: `ShellView` holds const references backed by the frame's `shared_ptr` (or a `const ShellModel&`
  sub-struct that RuntimeSnapshot contains by value, defined in a UI-free netplay header) plus the dozen
  overlay-derived fields (controller navigation flags, watchedPicture, canReady). Replace the in-place room patch with
  `room::ElapsedSince(snapshot.roomReceivedMs, now)` passed to the readers (IdleText in RoomPanel.cxx:42,
  room_controls::HoldStatus and DescribeReadyDeadline).
- Behavior preserved: every value the shell reads is the same; the idle and hold arithmetic moves to the reader unchanged.
- Behavior at risk: the elapsed-time adjustment must be applied at every reader of those three fields; tests that build
  ShellView by hand need a small builder.
- Estimated saving: payload in-process; roughly 20-60 KB copied and 200-500 heap allocations per drawn frame in a full
  room (the 100 chat strings alone are 100 allocations), about 12k-30k allocations/s at 60 Hz while the menu is open.
  Fan-out x1. Transport n/a.
- Confidence: medium (counts from struct shapes, not profiled)

### F-UI-5: `ShellAction` and `RuntimeCommand` are two bags of the same 25 optional fields, copied field by field
- File: src/ui/ApplicationShell.hxx:186
- File: src/sf4e/sf4e__Overlay.cxx:424  (mirror copy :424-449; cross-area)
- Category: boundary
- Severity: P2
- Claim: ShellAction carries `command`, `service`, `servicePath`, `updateOffer`, `steamProfile`, `reportSubmission`,
  `inputAction`, `padAction`, `discordAction`, `preferences`, `roomAction`, `selectedDelay`, `previewSoundVolume`,
  `shortInvitation`, `training`, `leaveWatchedGame`, `identity`, `tournament`, `publicTicket`, `createdRules`,
  `practiceRoom`, `delayPolicy`, `replay` and `selectionStep`. Comments say several are "sent alone", but nothing in
  the type says so; the receiver checks each field for a non-default value. The overlay then copies them one by one
  into `RuntimeCommand`; forgetting one line when a field is added silently drops the request. Every `Send` also copies
  the whole `PlayerPreferences` (ApplicationShell.cxx:107).
- Proposed change: Make the shell fill the runtime's request type directly: move `RuntimeCommand`'s UI-visible part to
  a UI-free header (for example `netplay/RuntimeRequest.hxx`) and let `ShellAction` be that type plus `selectionStep`,
  so the overlay adds only character, stage and randomPool. Later, a `std::variant` of the "sent alone" requests
  (Training, LeaveWatchedGame, PreviewSound, ShortInvitation, Service, Replay) makes the exclusivity a type fact.
- Behavior preserved: the same requests reach the runtime.
- Behavior at risk: none identified for the type move; the variant step changes the runtime's dispatch.
- Estimated saving: about 25 lines of mirror copy deleted; one struct definition instead of two.
- Confidence: medium

### F-UI-6: `loc::T` allocates a `std::string` for every lookup, hundreds of times per drawn frame (cross-area)
- File: src/common/Localization.cxx:181  (Lookup: `catalog.find(id ? id : "")` on `unordered_map<std::string,std::string>`)
- File: src/common/Localization.cxx:235
- Category: overhead
- Severity: P2
- Claim: `Lookup` builds a temporary `std::string` from the `const char*` key to call `find`. With MSVC's 15-byte small
  string buffer nearly every key ("room.member_status_host_muted", "settings.match_hud_layout_detail") goes to the
  heap. A missing translation looks up twice. src/ui has 1014 `loc::T(` and 221 `loc::Tf(` call sites, and BuildRows,
  RoomEntries, UpdateStatus, DrawRoomBoard and the HUDs run them again every drawn frame (a room board frame is roughly
  150-300 lookups). Almost all keys are literals or literal-selecting ternaries (190 non-literal call sites in src/ui,
  still static `const char*`).
- Proposed change: C++17-safe (the target is cxx_std_17, so heterogeneous unordered_map lookup is not available):
  after `EnsureCatalogs`, build per catalog an `std::unordered_map<std::string_view, const char*>` whose keys view the
  catalog's own key strings (skipping empty translations, so English still falls through as today), and look up with `std::string_view(id)`. No pointer-identity cache, so computed keys stay correct.
- Behavior preserved: same strings returned, same English fallback, same echo of the id for a missing key.
- Behavior at risk: none identified (catalogs are immutable after `call_once`; SetPseudoActive must build its index
  when it builds its catalog).
- Estimated saving: one heap allocation per lookup; about 10k-20k allocations/s at 60 Hz with the menu open, plus a
  smaller share on HUD frames (MatchHud, training HUD). Transport n/a.
- Confidence: high

### F-UI-7: Per-frame bookkeeping recomputes unchanged state (chat transcript, visual feedback, theme glyphs, probes)
- File: src/ui/ChatTranscript.cxx:24  (Update, called every frame from ObserveChat, drawn or hidden)
- File: src/ui/MenuFeedback.hxx:31  (MenuVisualFeedback::Update)
- File: src/ui/Theme.cxx:79  (UpdateWantedUserGlyphs via ApplyTheme, called every overlay frame from sf4e__Overlay.cxx:490)
- File: src/ui/RoomPanel.cxx:439  (roomDetails_ key)  src/ui/RoomPanel.cxx:565  src/ui/MatchHud.cxx:179  src/ui/MatchHud.cxx:259  src/ui/GameMenu.cxx:271
- Category: overhead
- Severity: P2
- Claim: Four mechanisms do full work every frame although their inputs change rarely:
  1. ChatTranscript::Update runs every overlay frame, including every match frame while Ember is hidden
     (ApplicationShell::Background). It reassigns every member name into `names_`, builds a fresh
     `std::set<MemberId> now` in Diff, clears and refills `present_`, and rescans chat and notices. With 16 members that
     is about 32 set-node allocations and frees per frame. `Unread()` walks up to 200 lines and is called 2-3 times per
     drawn room frame (RoomEntries, DrawRoomBoard, UnreadChat).
  2. MenuVisualFeedback::Update builds, per row per drawn frame, a `signature` string (label+value+flags) and inserts
     the row id into a fresh `std::set<std::string> live`: about 2-3 allocations per row, 60-90 per frame on a 30-row screen.
  3. ApplyTheme runs every overlay frame. It calls UpdateWantedUserGlyphs, which walks the whole `userGlyphs` map,
     builds a ranked vector and sorts `wantedUserGlyphs`, then runs `std::includes`, and the caller queries the DPI,
     even when no glyph was added. Cheap with ASCII names; tens of microseconds with a room of CJK names.
  4. Test probes are computed in production: `ReportMenuText(("board-"+e.id).c_str(), ..., CalcTextSizeA(...))`
     (RoomPanel.cxx:565), `("hud-telemetry/"+std::to_string(i))` and `TextWidth(...)` on every match HUD frame
     (MatchHud.cxx:179, :259), `probe+"-value"` per detail column (GameMenu.cxx:271-272), and more in MenuDialogs.cxx:55,
     :230 and RoomCards.cxx:42. Outside tests the probe is an empty `std::function`, so the string and the text measure
     are thrown away. RoomPanel.cxx:439 likewise builds `screen+"/"+table+"/"+id` keys into `roomDetails_` for nine
     rows per frame.
- Proposed change: (1) In ChatTranscript::Update, return early when `(roomEpoch, revision, chat.empty()?0:chat.back().sequence,
  notices.empty()?0:notices.back().sequence)` equals the last key; this is correct whether or not chat moves `revision`.
  Cache the unread count and recompute it on Update, MarkRead and mute changes. (2) Store a `std::size_t` hash of the
  signature instead of the string, and mark states with a frame stamp instead of building `live`. (3) Keep a
  `userGlyphsDirty` flag, set by NoteUserText when it adds a glyph, plus the earliest retention deadline;
  UpdateWantedUserGlyphs runs only when either fires. (4) Add `bool MenuProbesActive()` and guard the call sites, or make
  ReportMenuText take a lambda that builds the id and width only when a probe is installed. Key `roomDetails_` by
  `(table, row kind)` once F-UI-3 lands.
- Behavior preserved: same transcript lines, same smoothing, same atlas rebuild decisions, same probe output in tests.
- Behavior at risk: (1) a room change that alters members or tables without moving revision, the last chat sequence or
  the notice sequence would be missed; add `members.size()` and each table's `revision` to the key if in doubt.
  (3) retention expiry must still drop glyphs, hence the deadline.
- Estimated saving: in-process, transport n/a. Hidden match frames: about 30-40 allocations per frame (about 2k/s)
  removed from the game's render thread during matches. Drawn frames: a further 100-150 allocations per frame
  (6k-9k/s) and the probe-only text measures (one CalcTextSizeA per board button and per HUD telemetry cell).
- Confidence: medium-high (counts from code)

### F-UI-8: The chat log formats and wraps all 200 lines every drawn frame
- File: src/ui/ChatPanel.cxx:139  (DrawChatLog)
- Category: overhead
- Severity: P3
- Claim: DrawChatLog walks every kept line (up to ChatTranscript::MaximumLines = 200) each frame, on the board's Recent
  chat and on the Chat screen. Event lines call `EventText` (a `loc::Tf`), measure with wrapping, then draw; message
  lines run `FitLabel` on the sender, measure the tag and draw wrapped text. There is no clipping and no cache of
  formatted event text, so off-screen lines cost the same as visible ones.
- Proposed change: Format event text once in `ChatTranscript::Append` (store the locale it was formatted in and format
  again after a locale change), cache each line's wrapped height for the current width and font scale, and skip lines
  outside the scroll window by accumulated height (lines vary in height, so a manual clipper).
- Behavior preserved: same text, same layout, same follow-to-bottom rule.
- Behavior at risk: NoteUserText must still be called for visible lines only (as today), so the glyph atlas behavior holds.
- Estimated saving: with a full transcript, about 200 formats and measures per drawn frame drop to the 10-20 visible
  lines. Transport n/a.
- Confidence: medium

### F-UI-9: The text-elide helper exists (`FitLabel`) but is re-implemented seven times
- File: src/ui/MenuDialogs.hxx:9  (canonical `std::string FitLabel(const std::string& text,float width)`, MenuDialogs.cxx:13)
- File: src/ui/GameMenu.cxx:63  src/ui/GameMenu.cxx:262  src/ui/GameMenu.cxx:431  src/ui/MatchHud.cxx:82  src/ui/PublicRoomsCards.cxx:31  src/ui/RoomCards.cxx:64  src/ui/TrainingMeter.cxx:570
- Category: spaghetti
- Severity: P3
- Claim: Each copy measures "...", calls CalcTextSizeA with a width limit to find the cut, and appends "...". They
  differ only in font and size (FitLabel uses the current font size; the copies pass an explicit size or
  DiagnosticFont) and in small edge cases (MatchHud returns empty when even the dots do not fit; GameMenu.cxx:262
  subtracts one pixel).
- Proposed change: Generalize the canonical helper to `std::string FitLabel(const std::string& text, float width,
  ImFont* font = nullptr, float size = 0, bool emptyIfNoRoom = false)` and replace the seven copies.
- Behavior preserved: same cut points for each caller once its font, size and edge flag are passed.
- Behavior at risk: one-pixel differences between copies; the UiRender probe tests would show any change.
- Estimated saving: about 30 lines; one place to fix eliding for CJK and combining characters later.
- Confidence: high

### F-UI-10: ApplicationShell, RoomPanel and TrainingMeter: named decomposition seams
- File: src/ui/ApplicationShell.hxx:244  (class body to :597, about 150 data members)
- File: src/ui/RoomPanel.cxx:109  src/ui/TrainingMeter.cxx:109  src/ui/TrainingMeter.cxx:451
- Category: size
- Severity: P2
- Claim: ApplicationShell.cxx is 957 lines, but the class spans eight .cxx files (ApplicationShell, __Create, __Links,
  __Updates, __PadSetup, __ButtonLayout, RoomPanel, ChatPanel) that share one state bag of about 150 members, so the
  file split did not split the state: any part can touch any field. The members already cluster:
  (a) preference draft and save debounce: preferences_, lobby_, savingPreferences_, saveAt_, saveQueued_, retrySave_,
  saveFailed_, preferencesDirty_, profileSavePending_, languageSaveAt_, languageDirty_, languageSaveError_
  (UpdatePreferenceSave, SetLanguage);
  (b) status, notice and error: notice_, noticeTone_, noticeUntil_, error_, lastError_, errorScreen_, errorBlocked_,
  errorBlockedText_, errorSince_, plus the tournament/watch/link notice block in Draw (ApplicationShell.cxx:735-792);
  (c) chat: transcript_, pendingChat_, queuedChat_, chatGate_*, chatBoxText_, chatShown_, chat_, muted_, mutedEpoch_ (ChatPanel.cxx);
  (d) pad setup and button layout: setupPad_ through setupPreviousMode_, layoutPad_ through layoutFlashUntil_
  (about 20 fields; __PadSetup, __ButtonLayout);
  (e) room: selectedTable_, selectedMember_, tableRules_, rulesDirty_, rulesRevision_, roomDetails_, leaveAsk_,
  liveGames_, lockedInTable_, roomUpdate*_, roomName_, roomCapacity_ (RoomPanel.cxx);
  (f) links: joinLink*_, roomLink*_, shortCopy*_, pendingConnect*_, matchLinkSequence_ (__Links);
  (g) update question: updatePrompt_, launchCheck*_, skipFailuresSeen_ (__Updates).
  RoomPanel.cxx (897 lines) holds three things: SendRoom's authority guards (:109-161), six per-screen row builders
  inside one RoomEntries (:164-451, branching on "room", "room-table", "room-members", "room-member", "room-chat" and
  "room-admin"), and DrawRoomBoard (:453-660). TrainingMeter.cxx (851 lines) holds the flat and angled bars, the
  classic meter and five HUD windows; AngledBar (:109-145) and ClassicMeter's bar loop (:451-467) are the same
  run-detection, split-edge and run-length-label loop twice; `HeldWindow` keeps its scroll offset in a function-local
  `static` (:179) shared by the match meter and the training HUD.
- Proposed change: Turn clusters (a)-(g) into member objects with small classes in the existing `__*.cxx` files:
  `PreferenceDraft`, `ShellStatus` (one `Say(text, tone, seconds)` replacing about 25 three-assignment notice blocks),
  `ChatController`, `PadSetupFlow`, `RoomScreens`, `LinkInbox`, `UpdateQuestion`; ApplicationShell keeps the
  orchestration. Split RoomEntries into one builder per room screen (these become the room `ScreenPanel` of F-UI-1).
  In TrainingMeter, extract `DrawRunBar(draw, meter, side, origin, geometry, colorOf, onRun)` used by AngledBar and
  ClassicMeter, move the HUD windows to a TrainingHud.cxx, and make HeldWindow's offset a member of a `MeterScroll`
  owned by the caller.
- Behavior preserved: same behavior; ownership moves only.
- Behavior at risk: the order of per-frame updates in Draw (listed at ApplicationShell.hxx:317) must be kept.
- Estimated saving: about 40 lines in TrainingMeter (duplicate loop) and about 50 lines of notice boilerplate; mostly
  fewer concepts in scope: each cluster's invariants become local to a class.
- Confidence: high on the seams, medium on the line counts

### F-UI-11: Training and match HUD redo layout and formatting each frame
- File: src/ui/TrainingMeter.cxx:196  (column widths from `loc::Tf("training.start_frames",999)` and others, every frame)
- File: src/ui/TrainingMeter.cxx:363  (ClassicMeter: a `Readings()` vector of strings per side, each measured twice)
- File: src/ui/TrainingMeter.cxx:124  (AngledBar: `std::vector<Run>` and `std::to_string` per run per frame)
- Category: overhead
- Severity: P3
- Claim: These draw over live matches (the match frame meter in private rooms, replays) and in training. Meter()
  formats and measures four template strings per frame only to size columns that depend on locale and font scale.
  ClassicMeter builds `std::vector<Reading>` (label and value strings) per side per frame and measures each label and
  value twice (once to choose labels or ticks, once to draw). TrainingMeterOptions() returns the options struct by
  value and is called four times per HUD frame.
- Proposed change: Cache the column widths keyed by (active locale, font size); measure each reading once into a small
  fixed array; reuse a member `std::vector<Run>` (or label runs inline, as ClassicMeter already does); read
  TrainingMeterOptions() once per frame and pass it down.
- Behavior preserved: identical drawing.
- Behavior at risk: none identified.
- Estimated saving: roughly 20-40 allocations and 10-15 text measures per HUD frame; small, but on the match render
  path. Transport n/a.
- Confidence: medium

### F-UI-12: Image cache keys are built as strings on every request, every frame
- File: src/ui/SelectionArt.cxx:409  (Portrait: `std::string(fighter->code)+"/portrait"`, then `+ "-thumb"` or `"-large"`)
- File: src/ui/SelectionArt.cxx:269  (Request: `std::map<std::string,Entry>::find(key)`)
- Category: overhead
- Severity: P3
- Claim: "screens request every visible image on every frame" (SelectionArt.cxx:267), by design, for the LRU use
  stamps. Each request builds a key with 2-3 allocations and does a string-keyed map lookup: 44 roster portraits on
  fighter select or the main-character grid, plus every member card portrait and class emblem on the board.
- Proposed change: Key entries by a POD `ImageKey{kind, fighter, costume, color, ultra, side}` in an
  `std::unordered_map` (or a flat array for portraits, FighterCount x 2); keep the string for logging and the decode job.
- Behavior preserved: same textures, same LRU eviction, same retry policy.
- Behavior at risk: none identified.
- Estimated saving: about 100-130 allocations and string compares per drawn grid frame. Transport n/a.
- Confidence: medium

### F-UI-13: Public rooms list: sorted twice per frame, and fetched in full every 15 s while the screen is open
- File: src/ui/PublicRoomsPanel.hxx:40  (AutoRefreshSeconds = 15, ErrorBackoffSeconds = 30)
- File: src/ui/PublicRoomsPanel.cxx:244  (auto refresh condition)  src/ui/PublicRoomsPanel.cxx:35  (Sorted)  :342  :428
- File: src/sf4e/sf4e__NetplayRuntime__PublicRooms.cxx  src/session/TournamentAnswers.cxx  (request and answer; cross-area)
- Category: streaming
- Severity: P3
- Claim: This is the only 15 s cadence in this area and a likely match for the owner's "sent every 15 seconds", but it
  is not a background sender: the panel asks for `RoomList` only while the Public rooms screen is open, the player is
  Idle, no request or link is pending, and the previous answer is in. Each answer is the whole list: per room the id,
  name, region, hostName, member/capacity/playing counts, createdAt, up to 16 fighter ids, and lock, ownDelay and set
  rules; roughly 150-250 B of JSON per room, so about 8-10 KB at the 45-room cap, every 15 s (about 0.6 KB/s per
  viewer), through the helper to the bridge over HTTPS. On the client the list is copied into ShellView (F-UI-4) and
  `Sorted()` stable-sorts it twice per drawn frame (Rows at :428 and the status count at :342).
- Proposed change: (UI) cache the sorted listing keyed by `(list.listed, region, filter)`. (Wire, cross-area) send the
  bridge `since=<listedAt or digest>` with the request; a bridge that knows the key answers "unchanged" (or 304) when the
  list digest matches, else the full list. Old bridges ignore the unknown parameter and answer in full; old clients never
  send it and get the full list, so both directions keep working.
- Behavior preserved: same list content and order on screen; same refresh timing.
- Behavior at risk: the "Updated N s ago" text must treat an "unchanged" answer as a fresh listing (listedAt moves).
- Estimated saving: in a quiet hour most refreshes carry about 100 B instead of 8-10 KB; two sorts of up to 45 rooms
  per drawn frame become one per answer.
- Confidence: medium (payload size estimated from the Room struct, not measured on the wire)

### F-UI-14: The launcher handles five link kinds in four parallel cascades
- File: src/launcher/launcher.cxx:605  (parse cascade :605-614, log :615-620, deliver :644-649, env vars :694-706)
- Category: spaghetti
- Severity: P2
- Claim: `--join-link` is parsed into five mutually exclusive variables (joinCode, matchLink, publicRoomLink,
  connectBridge, replayLink), each guarded by a growing `joinUri.empty() || !joinCode.empty() || matchLink.Valid() ||
  ...` chain. The same five-way choice is then written again to log it, again to hand it to a running game
  (DeliverJoinLink, DeliverMatchLink and so on), and again as five SetEnvironmentVariableW calls. Adding a sixth link
  kind means editing all four chains and the guard of every later parse.
- Proposed change: `using LaunchLink = std::variant<std::monostate, RoomCode, MatchLink, RoomLink, ConnectLink,
  ReplayLink>; LaunchLink ParseLaunchLink(const std::string& uri);`, which tries the parsers in order, plus a per-kind
  table or visitor giving `{logLine, Deliver(), envName, envValue()}`. wWinMain then logs the link, calls
  `Deliver(link)` when `!instance.Acquire()`, and calls `ExportLink(link)` (which clears all five variables and sets
  the one present).
- Behavior preserved: same parser precedence, same log text, same environment variables.
- Behavior at risk: none identified (the game reads the same variables).
- Estimated saving: about 30 lines and three of the four cascades.
- Confidence: high

### F-UI-15: Discord companion: a 100 Hz loop re-serializes and re-parses its own state; the game resends full presence each second
- File: src/discord/main.cxx:45  (loop, `Sleep(10)` at :105)  src/discord/main.cxx:70  src/discord/main.cxx:75
- File: src/discord/Presence.hxx:9  (PresenceInput copies netplay::Snapshot and room::Snapshot)
- File: src/sf4e/sf4e__NetplayRuntime__Status.cxx:206  (publishDue every 1000 ms; cross-area)
- Category: overhead
- Severity: P3
- Claim: The companion wakes every 10 ms. On each iteration with a presence it calls `latest.dump()` (about 250 B of
  JSON) and, whenever presence is hidden or has no secret, `Json::parse(lastSent)`: about 100 serializations and up to
  100 parses per second of an unchanged value. The game resends the presence at least once per second (`publishDue`),
  building a `PresenceInput` by copying the full `room::Snapshot` (members, tables, chat) to compute six fields. That
  1 Hz resend is load-bearing: the companion exits when nothing arrives for 15 s (main.cxx:67). Bytes are trivial
  (about 250 B/s over a local pipe); the waste is CPU and allocation churn in two processes.
- Proposed change: Companion: keep `serialized` and a `dirty` flag, updated only on receive or when `expires` passes,
  and keep the `show`/`secret` of what was last sent instead of parsing `lastSent` again. Game: make `PresenceInput`
  take `const room::Snapshot&` and `const netplay::Snapshot&`, and either lengthen the keepalive (for example 5 s
  against the 15 s timeout) or send a `{"type":"ping"}` heartbeat when unchanged. An old companion treats an unknown
  type as fatal (`running=false`), so a ping is safe only because the companion ships in the same package; otherwise
  keep full resends at a longer interval.
- Behavior preserved: same Rich Presence updates, same clear-on-hide and coalescing.
- Behavior at risk: the companion exits on silence if the interval reaches 15 s; a mixed-version companion if a ping is used.
- Estimated saving: about 100 dumps and up to 100 parses per second in the companion; one room-snapshot copy per
  second in the game. Payload about 250 B, interval 1 s, fan-out x1, transport local named pipe.
- Confidence: high

### F-UI-16: Hidden global state couples the menu, embedded screens and the training panel
- File: src/ui/GameMenu.cxx:15  (file-scope `input`, `forwarded`, `menuArt`, glyphs, `playerCard`, six probes)
- File: src/ui/GameMenu.hxx:45  (SetMenuInput, SetMenuPlayerCard, ForwardMenuAction/TakeForwardedMenuAction, SetEmbeddedReturn)
- File: src/ui/TrainingPanel.cxx:52  (`lab` singleton; TrainingMeterOptions() reads it)
- Category: structural
- Severity: P3
- Claim: The shell talks to the fighter selector and the training flyout through process globals: it calls
  `TakeForwardedMenuAction()` once to clear, draws the selector, then calls it again to read what was forwarded
  (ApplicationShell.cxx:814-816); it publishes the player card each frame through `SetMenuPlayerCard`. The order of
  these calls is the contract. TrainingPanel keeps its whole model in an anonymous-namespace `lab` and loads a JSON
  file lazily from the first draw call that reads the options (TrainingPanel.cxx:80).
- Proposed change: Pass an `EmbeddedScreenContext{exitName, hints, fresh, openOn}` into the selector and flyout draw
  calls and return the forwarded action; make the player card a parameter or member of GameMenu; give the training lab
  an owning object created by the overlay with an explicit Load().
- Behavior preserved: same behavior.
- Behavior at risk: tests that rely on the globals (probes) need a setter on the owning object.
- Estimated saving: removes the clear-draw-take call ordering and about 8 global setters; makes the menu re-entrant.
- Confidence: medium

### F-UI-17: The installer's confirm pass hashes every file again, including those it just proved unchanged
- File: src/launcher/update/PackageInstaller.cxx:708  (Comparing: hash all installed files)
- File: src/launcher/update/PackageInstaller.cxx:763  (Confirming: hash all of `target` again)
- Category: overhead
- Severity: P3
- Claim: An update hashes the staged package (:637), then every installed file (:708), then backs up and hashes each
  changed file twice, then hashes every file in `target` again (:763). Between releases most of the roughly 390 MB
  package (artwork) is unchanged, so the last pass reads and hashes again about 390 MB that the Comparing pass hashed
  seconds earlier under the same install lock. The comment at :700-703 says unchanged files stay in `target` "so the
  check after the last write still covers it": this is deliberate integrity coverage.
- Proposed change: Confirm only `written` files by hash, plus a cheap stat check (size and last-write time) of the
  unchanged files against values recorded in the Comparing pass; hash in full if any stat differs.
- Behavior preserved: every replaced file is still verified by hash; unchanged files are still checked for change.
- Behavior at risk: a same-size, same-mtime modification by another process during the install would no longer be
  caught; that is the guarantee the comment names.
- Estimated saving: about 390 MB of disk reads and SHA-256 per update (seconds on SSD, more on HDD). Once per update; no polling.
- Confidence: low (deliberate trade-off; the owner's call)

### F-UI-18: The updater does not poll; one small waste in the release parse
- File: src/launcher/update/github_release_client.cxx:425  (CheckForUpdate)  src/launcher/update/github_release_client.cxx:518
- Category: overhead
- Severity: P3
- Claim: For the record, so it can be crossed off: the update check is one HTTPS GET of `/releases?per_page=20` at
  launch (behind the services' ShouldCheck policy) or on demand, with a 15 s timeout, into a 1 MB buffer. Nothing
  polls. The only waste is that `ReadRelease` runs for every accepted release, including the Markdown-to-plain
  conversion of up to 2000 bytes of notes (`updates::PlainReleaseNotes`) for up to 20 releases, while only the best is kept.
- Proposed change: Choose the best release by tag, kind and asset first, then run ReadRelease (and the notes
  conversion) once. Optionally send `If-None-Match` with the last ETag (GitHub does not count 304 answers against the
  rate limit).
- Behavior preserved: same chosen release and notes.
- Behavior at risk: none identified.
- Estimated saving: about 19 notes conversions per check; the 100-300 KB response avoided on a 304. Payload about
  100-300 KB JSON, interval launch or manual, fan-out x1, transport HTTPS.
- Confidence: high

## Area summary
- Health: the UI is careful and well commented, and earlier work (Draw split into parts, ConcealApplicationHome not
  copying the room, lazy image paths, the shared rankings board) shows overhead awareness. The structural debt sits in
  ApplicationShell: one state bag of about 150 members shared by eight files, and a string-keyed screen dispatch
  repeated in every phase.
- Biggest judo move: F-UI-1, one `ScreenPanel` interface and an owner lookup per frame, deleting 26 `Owns(screen)`
  branches. F-UI-2 and F-UI-3 are cheap follow-ons that delete the hand-enumerated preference branches and 27 id parses.
- Biggest overhead item: per drawn frame, the full `ShellView` copy of the runtime snapshot (F-UI-4) together with
  allocating `loc::T` lookups (F-UI-6); on match frames, the every-frame ChatTranscript, theme and probe work (F-UI-7).
- The "every 15 s" item in this area is the Public rooms list refresh (F-UI-13), and it runs only while that screen is
  open; any always-on 15 s sender lives outside src/ui, src/launcher and src/discord.
- Not assessed: real frame-time impact (no build or profiler; all numbers are counts from code). IdentityPanel (770
  lines) and its split files were read only for polling cadence (assignments every 60 s, every 5 s while a watch or
  link waits). RecoverySurface renders at vsync while open (the launcher's error and update window only) and was not
  pursued. The bridge side of the room-list request was not read.
