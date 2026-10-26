# Adversary verdicts: game UI, launcher/updater, Discord companion (F-UI-1 to F-UI-18)

Read at sf4-nightly b9ce0e31, read-only. Nothing built or run; all costs are counts from code.

Gating fact used throughout: `DrawApplicationHome` runs only when `presentation.Visible()`; otherwise
`ConcealApplicationHome` runs, which does not copy the room (src/sf4e/sf4e__Overlay.cxx:578-579, :255-273).
The menu can be visible while a game runs on this PC: Practice match, spectating (the watched-game picture,
Overlay.cxx:318-322), replay export. During a player's own fight it is hidden. Between games in a room it is
normally up.

### F-UI-1: PLAUSIBLE  (severity: P2)
- Decisive invariant/evidence: there are exactly 26 `Owns(` checks in ApplicationShell.cxx (:256 to :946). The
  2026-09 review's row reads "13-screen string chain plus 20-id action chain | per-screen row builders, one
  dispatcher | 6" (docs/reviews/2026-09-code-quality-review.md:47). Its second pass only split Draw into named
  phases "called in the original order" (:270). So "the dispatcher was not built" is correct. But the proposal
  says "each phase calls `owner->X(ctx)`" and lists `Update` among the virtuals, and that would change behavior.
  The panel Updates run unconditionally every drawn frame (:723-731), and they act when the panel does NOT own
  the screen: PublicRoomsPanel clears `service_`/`pending_` (PublicRoomsPanel.cxx:201, :235), IdentityPanel
  schedules background retries and assignment polls off-screen (IdentityPanel.cxx:101, IdentityMatches.cxx:140),
  and RankingsPanel resets on entry (RankingsPanel.cxx:102). Navigation: Back, Close and Returned are handled by
  the shell before any panel dispatch (:928-936), and focus memory and controller navigation live in
  MenuNavigation/GameMenu. A ScreenPanel interface therefore leaves navigation alone if the screen-specific focus
  calls keep their place: `TakeFocus` (:331) and the `pr-opening` to `cancel-open` move (:925), both before
  `menu_.Draw`. docs/design/menu-input-scope.md covers native input suppression only and does not constrain this
  dispatch. There is no overhead saving: 26 short string compares per frame cost nothing.
- Adjustment: Update (and IdentityPanel::Hidden in Background, :637) stay fan-out calls to every panel, in today's
  order (replays, identity, publicRooms, UpdatePublicBridge, rankings). Only the owner-routed phases go through
  `owner->`. The shell's precedences must be kept explicitly:
  - room screens first for Shortcut, Chosen and Adjust (:938, :942, :547);
  - `retry-save` excluded from Identity (:493);
  - PublicRooms Status also runs on "create" (:451);
  - a panel Activate that returns false falls through to the shell's generic id chain (:503-529). Rankings relies
    on this.

  Leave the "SettingsScreens" move of the shell's own screens for later: it is about 150 lines of the riskiest
  surface for a P2 code-quality gain. Verify the change the way the 2026-09 pass did: hash the draw lists across
  ControllerNavigationTest, RoomPanelNavigationTest and NativeSelectionAvailabilityTest (review :304-307).
  Severity drops from P1 to P2 because nothing gets faster and the owner's goal is overhead.
- Duplicate of: no (canonical for the dispatch; F-UI-10's RoomEntries split folds into it)

### F-UI-2: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: HandleAdjust has 19 hand-written bool branches (ApplicationShell.cxx:565-585),
  including the inverted `game-reads-pads` (`controllerLayer=a.delta<0`). There are 13 copies of the
  `?loc::T("common.on"):loc::T("common.off")` expression. `netplay::BoolPreferences`
  (src/netplay/BoolPreferences.hxx:15) is already used by SamePreferences (MenuRows.hxx:8). Row ids are matched
  in-process only. The loop can sit in the same else-if position because no id overlaps another branch.
- Adjustment: `hud-spacing` shows `spacing.raised`/`spacing.normal`, not On/Off, so OnOffRow covers only the 13
  on/off rows. The adjust table still needs the row (or a value-label field). `visibility` and `delay-policy` are
  not plain bools and stay hand-written.
- Duplicate of: no

### F-UI-3: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: there are 28 `stoi`/`stoul`/`stoull` sites in src/ui, not 27. The overhead figure
  is overstated. `substr(6)` of "table-N" is one character and fits MSVC's 15-char small-string buffer, so the 4
  table parses allocate nothing. Only "member-<u64>" ids (MemberId is uint64, RoomModel.hxx:32) of 16 or more
  digits spill to the heap, and `FindMember` over at most 16 members is trivial. Ids are built by the same code
  that parses them, so the "malformed id throws" risk is theoretical.
- Adjustment: present it as code quality: a typed key on MenuEntry replaces parsing. The realistic saving is at
  most about 16 small allocations per room-board frame, not 20 allocations plus 20 searches.
- Duplicate of: no

### F-UI-4: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the copy is real and happens only on drawn frames (Overlay.cxx:274-409; gated at
  :578). It does not happen on hidden match frames, where the room is passed by reference (:270). The struct
  shapes give an upper bound per full frame:
  - members: 16 × name, host and user strings (RoomModel.hxx:163-178);
  - chat: up to 100 × ≤256 B messages (RoomModel.hxx:41-42);
  - tables: 4 × 4 vectors plus 2 permit strings;
  - public rooms: up to 45 × id (32 hex), name, hostName and fighters (PublicRooms.hxx:26-37);
  - MemberView and preferences.

  That supports the finding's 200-500 allocations and 20-60 KB only for a full room with a full chat and a
  retained public list. A typical 4-8 member room with 20-30 chat lines is about 50-120 allocations. The readers of
  the in-place patch are exactly three: RoomControls.cxx:183-207 (hold and ready deadline) and RoomPanel.cxx:46
  (idle).
- Adjustment: state the typical number (about 3k-7k allocations/s with the menu open, not 12k-30k). Smaller,
  lower-risk form: pass `const room::Snapshot&` into Draw as Background already does (ApplicationShell.hxx:293),
  plus an elapsed-ms value for the three readers. Do not turn all of ShellView into references in one step. It
  matters most while spectating with the menu open, because then the copy runs on the render thread of a live game.
- Duplicate of: no (F-UI-13's "copied into ShellView" is this one)

### F-UI-5: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: the mirror copy is at Overlay.cxx:424-449. RuntimeCommand
  (sf4e__NetplayFacade.hxx:101-146) carries game types (`ConfirmedCharaConditions`), which is why it cannot be
  included from the UI. The type move keeps every request identical. The `Send` copy of PlayerPreferences happens
  per action, not per frame, so it is negligible.
- Adjustment: do the header move only. The variant step changes the runtime's dispatch and needs its own review.
  This also resolves the layering item the 2026-09 review left open ("sf4e__NetplayFacade.hxx includes a UI
  header", review :322).
- Duplicate of: no

### F-UI-6: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: `Lookup` calls `catalog.find(id)` on `unordered_map<std::string,std::string>`
  (Localization.cxx:181; type at Localization.hxx:15), building a temporary std::string per call. A non-English
  miss looks up twice (:238-239). sf4e_localization is cxx_std_17 (CMakeLists.txt:247). The counts are off:
  there are 1656 `loc::T(` occurrences on 1158 lines and 263 `loc::Tf(` in src/ui, not 1014 and 221; immaterial.
  The menu part runs only on drawn frames. MatchHud (27 sites) and TrainingMeter (43 sites) run on match frames.
- Adjustment: the string_view index adds about 2200 keys × 14 catalogs, roughly 31k nodes (about 1-1.5 MB), to a
  32-bit game process. Simpler and memory-free: reuse a `thread_local std::string` scratch key (`key.assign(id)`,
  no allocation after warm-up) with the same `find`. Or build the index only for English and the active
  catalog. Behavior is identical either way.
- Duplicate of: no

### F-UI-7: PLAUSIBLE  (severity: P3; part (1) is the only UI item on hidden match frames)
- Decisive invariant/evidence:
  - (1) ChatTranscript::Update runs from Background on every hidden frame while in a room (ApplicationShell.cxx:636).
    Diff builds a `std::set` of ids and `present_` is cleared and refilled (ChatTranscript.cxx:52-53, :69-70), about
    32 node allocations per frame with 16 members. The early-return key relies on `room.revision`, which moves only
    where the authority calls TouchRoom/Touch explicitly (RoomModel.cxx:388-395). I could not verify that every
    member, host and score change touches it, including on server-owned room hosts. A missed Joined, Left or
    GameWon line would be a visible behavior change.
  - (2) The signature string plus the `live` set is about 2-3 allocations per row (MenuFeedback.hxx:34-40).
  - (3) With ASCII-only text `userGlyphs` is empty and UpdateWantedUserGlyphs costs almost nothing (Theme.cxx:79-99).
  - (4) The probe-only measures are real (RoomPanel.cxx:565, MatchHud.cxx:179, :259).
- Adjustment:
  - (1) Make Diff allocation-free instead of keyed: reuse a member-id buffer and keep `present_` as a sorted vector
    (at most 16). That is the same output with no dependence on revision semantics.
  - (2) Compare label, value and flags field by field against the stored state, not a hash: a hash collision would
    skip a reset.
  - (3) Low value; at most skip the walk while `userGlyphs` is empty.
  - (4) As proposed: a `MenuProbesActive()` guard, so tests see identical probe output.
- Duplicate of: no

### F-UI-8: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: DrawChatLog formats and measures every kept line (ChatPanel.cxx:139-196). But
  "off-screen lines cost the same as visible ones" is overstated. ImGui's ItemAdd skips vertex generation for
  clipped items, and NoteUserText is already gated on `IsItemVisible`. What off-screen lines still pay is
  `EventText` formatting and the wrap measure. Drawn frames only.
- Adjustment: keep the clipper with cached heights invalidated on width, font scale and locale. Claim the saving
  as formatting and measuring of off-screen lines, not their drawing.
- Duplicate of: no

### F-UI-9: REJECTED  (severity: P3)
- Decisive invariant/evidence: the 2026-09 review already considered and rejected this: "One text elision helper:
  `FitLabel` measures with `CalcTextSize`, which rounds, and the other copies use `CalcTextSizeA`, so merging them
  changes pixels" (review, "Considered and rejected"). The finding does not answer that. The copies also differ
  in more than font and size. MatchHud's `FitText` (MatchHud.cxx:82-90) trims one UTF-8 character at a time in a
  loop instead of a width-limited CalcTextSizeA cut. FitLabel subtracts a pixel for rounding (MenuDialogs.cxx:12).
  The saving is about 30 lines.
- Adjustment: none; leave the copies.
- Duplicate of: no

### F-UI-10: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the sizes are confirmed: ApplicationShell.cxx 957 lines, .hxx 597, RoomPanel.cxx
  897, TrainingMeter.cxx 851, with seven shell .cxx files plus RoomPanel/ChatPanel sharing one class. Two
  "behavior preserved" claims are wrong:
  - HeldWindow's function-local `static back` (TrainingMeter.cxx:179) is shared by both callers (:207, :403).
    Today an unfrozen caller resets the other's scroll, so per-caller state changes behavior whenever both meters
    draw in one frame.
  - AngledBar and ClassicMeter are not the same loop. AngledBar collects runs and labels them after CountMarks so
    the numbers sit over the marks (:122, :141-143). ClassicMeter labels inline, dims missing frames instead of
    skipping them, and has meaty colors. A shared DrawRunBar needs a deferred-label mode or pixels change.
- Adjustment: name both differences as risks. Split RoomEntries only as part of F-UI-1. Verify with the
  draw-list hash, not by inspection.
- Duplicate of: F-UI-1 for the RoomEntries split (F-UI-1 canonical); otherwise no

### F-UI-11: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: these costs are real on match and training HUD frames:
  - Meter formats `loc::Tf(...,999)` templates to size columns every frame (TrainingMeter.cxx:196-198);
  - ClassicMeter measures each reading twice (:367-381);
  - `TrainingMeterOptions()` returns a copy (TrainingPanel.cxx:285).

  The alternative "label runs inline, as ClassicMeter already does" would draw AngledBar's run lengths under the
  counting marks. The code says "The run lengths are drawn last, over the counting marks" (:122). That is a
  pixel change.
- Adjustment: use only the reused member `std::vector<Run>`, not inline labels. Key the width cache by (active
  locale, font pointer, font size).
- Duplicate of: no

### F-UI-12: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the key strings are built per request (SelectionArt.cxx:409, :426) into
  `std::map<std::string,Entry>` (:224). The allocation count is overstated. "RYU/portrait" (12 chars) fits the
  small-string buffer, so a portrait request makes one heap allocation, for the "-thumb"/"-large" concatenation.
  Appearance keys make 1-2. That is about 45-60 per grid frame, not 100-130.
- Adjustment: eviction picks the minimum `lastUse` in map iteration order (:400-404), so ties break by key order.
  Keep an ordered map with a POD key, or define the tie-break, so the eviction choice does not change.
- Duplicate of: no

### F-UI-13: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the refresh runs only while the Public rooms screen is shown. `publicRooms_.Update`
  is called only from the drawn Draw path (ApplicationShell.cxx:727). Hidden frames call Conceal. The refresh
  also requires `Owns(screen)`, a bridge, not Busy, no link, the list not loading, and session Idle
  (PublicRoomsPanel.cxx:243-244, :254-259).

  The payload is underestimated. The bridge's RoomSummary (server/ember/protocol/src/rooms.rs:202-239) also
  carries `build_id` (up to 128 chars; the sidecar hash), `room_id` is 32 hex (bridge create.rs:64, helper
  check_room_id), and the client asks `detail=1` (rust/sf4-net/src/tournament/rooms.rs:97). That is about
  280-380 B per room, so about 13-17 KB at 45 rooms, every 15 s, about 1 KB/s per viewer. The bridge allows up
  to 100 rooms per listing (LIST_LIMIT, bridge rooms/mod.rs:71).

  Wire compatibility of `since=` holds:
  - the bridge's ListQuery has no `deny_unknown_fields` (rooms/mod.rs:411-417), so old bridges ignore it;
  - the helper parses `RoomList` with `deny_unknown_fields` and requires 200 (tournament/rooms.rs:127), so an
    "unchanged" answer must be a distinct status or shape, sent only when `since` was given.

  Two stable sorts of at most 45 pointers per drawn frame (:342, :428) take microseconds.
- Adjustment: correct the per-room size. The UI sort cache key must include `list.listed`. The wire change saves
  about 1 KB/s per viewer, only while the screen is open. It is not worth a protocol change now.
- Duplicate of: no

### F-UI-14: CONFIRMED  (severity: P3)
- Decisive invariant/evidence: there are four parallel five-way chains (launcher.cxx:605-620, :644-649, :694-706).
  This is link parsing and handoff only. It touches no updater, hash, receipt or installer code, so integrity is
  unaffected.
- Adjustment: keep these properties of the variant:
  - the room code and public room IDs are never logged;
  - `WipeText(joinUri)` still runs after parsing;
  - a replay link whose `ResolveReplayFile` is empty falls to the "Ignored a link" log;
  - ExportLink sets the four absent variables to null, as today.
- Duplicate of: no

### F-UI-15: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the companion loop sleeps 10 ms. Each iteration runs `latest.dump()` and, when
  hidden or with no secret, `Json::parse(lastSent)` (src/discord/main.cxx:70-75, :105). The game resends at least
  every 1000 ms (sf4e__NetplayRuntime__Status.cxx:207). The companion exits after 15 s of silence (main.cxx:67),
  and any unknown message type is fatal (:52). Describe is synchronous, so a `const&` PresenceInput is safe.
- Adjustment: take the companion-side dirty flag (re-serialize on receive or at `expires`) and the const-ref
  input. Reject the keepalive lengthening and the ping:
  - the saving is about 250 B/s on a local pipe;
  - the 1 Hz resend bounds how fast presence comes back after a companion restart;
  - a ping relies on same-package versions for a contract that is fatal on mismatch.
- Duplicate of: no

### F-UI-16: PLAUSIBLE  (severity: P3)
- Decisive invariant/evidence: the clear-draw-take ordering is real (ApplicationShell.cxx:814-816), as are the file
  globals (GameMenu.cxx:15-24) and the `lab` singleton (TrainingPanel.cxx:52). LoadPractice is lazy by design:
  "Not until the directory is known, so a lookup before it is set (the match meter, a test) does not stop the
  file being read once it is" (TrainingPanel.cxx:76-80). There is no overhead saving.
- Adjustment: an explicit Load() must keep the gate "read once, only after the directory is set". The overlay's
  `selection` callback is a `std::function<void()>` (Overlay.cxx:452-455), so the context struct crosses into
  src/sf4e.
- Duplicate of: no

### F-UI-17: REJECTED  (severity: P3)
- Decisive invariant/evidence: the proposal weakens the installer's integrity guarantee. The Confirming pass
  re-hashes every file in `target` after the last write (PackageInstaller.cxx:756-762). The comment says
  unchanged files stay in `target` "so the check after the last write still covers it" (:700-703).
  docs/development/BUILDING.md:45 states the installer verifies the "reconstructed target". A size and mtime stat
  would miss a same-size modification with a preserved mtime; Copy-Item preserves mtime. The InstallLock does not
  stop other processes, such as antivirus or the player's tools. It also runs once per update, not per frame.
- Adjustment: none; keep the full confirm hash.
- Duplicate of: no

### F-UI-18: REJECTED  (severity: P3)
- Decisive invariant/evidence: the factual part is correct. There is one GET of `/releases?per_page=20` per launch
  or on demand (github_release_client.cxx:425-446), and nothing polls. The change saves at most 19 Markdown
  conversions of 2000 bytes or less, once per launch: negligible. A release counts only once ReadRelease finds a
  zip asset (`candidate.ok`, :518-521), so "pick first" must still check the asset. ETag/If-None-Match adds cached
  state to an update decision. The 2026-09 review calls this path "security sensitive" with no end-to-end test
  (review, "Not done").
- Adjustment: none; cross it off as "does not poll".
- Duplicate of: no

## Batch verdict
- Counts: CONFIRMED 3 (F-UI-2, 5, 14); PLAUSIBLE 12 (F-UI-1, 3, 4, 6, 7, 8, 10, 11, 12, 13, 15, 16); REJECTED 3
  (F-UI-9 prior-art rejection, F-UI-17 weakens integrity, F-UI-18 negligible). Severity changes: F-UI-1 P1 to
  P2; F-UI-2, 4, 5, 6, 7, 10, 14 P2 to P3.
- Do first:
  1. F-UI-7 (1) and (4): the allocation-free ChatTranscript::Diff and the probe guard. These are the only UI costs
     on hidden match frames, on the game's render thread, and they change no output.
  2. F-UI-6 with a thread_local scratch key: a few lines, identical strings, and it covers menu and HUD frames,
     including MatchHud and TrainingMeter during play.
  3. F-UI-4 in its smaller form: pass the room by reference plus elapsed ms. It is the largest per-drawn-frame
     cost and matters while spectating with the menu open.
- Could not verify by reading:
  - real frame times;
  - whether every room mutation, on peer-hosted and server-owned rooms, moves `room.revision`, which is why the
    F-UI-7 revision key is replaced;
  - the exact sidecar hash length (it sets the `build_id` share of F-UI-13's payload);
  - whether the match meter and the training HUD's ClassicMeter can draw in the same frame (it decides whether
    F-UI-10's HeldWindow change is visible).
