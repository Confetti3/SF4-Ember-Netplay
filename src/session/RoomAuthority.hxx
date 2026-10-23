#pragma once
// The room authority: the game-thread owner of one room's state.
#include "RoomQueries.hxx"

namespace sf4e { namespace room {

class RoomAuthority {
public:
	// practice and delayPolicy are the room's for its life (Snapshot::practice,
	// Snapshot::delayPolicy); nothing changes them later.
	RoomAuthority(std::string name, std::uint8_t capacity = static_cast<std::uint8_t>(MaximumMembers),
		std::uint64_t roomEpoch = 1, Rules defaults = Rules(), bool practice = false,
		DelayPolicy delayPolicy = DelayPolicy::Shared);

	const Snapshot& SnapshotView() const { return snapshot_; }
	Snapshot SnapshotCopy() const { return snapshot_; }
	Snapshot SnapshotFor(MemberId member) const;
	// As SnapshotFor, but with the chat left empty, and not copied, when withChat is false.
	Snapshot SnapshotFor(MemberId member, bool withChat) const;
	std::uint64_t NextMemberId() const { return nextMemberId_; }

	// Marks a fresh room server-owned (see RoomModelServerOwned.cxx). False, and
	// no change, once anyone has joined or the room has closed, or for a
	// Practice room, which only a player creates.
	bool SetServerOwned();
	bool ServerOwned() const { return snapshot_.serverOwned; }
	// Accounts a kick removed for the room's life; empty unless server-owned.
	std::vector<std::string> KickedAccounts() const;
	// The newest MaximumWonGames games won between two accounts, oldest first;
	// empty unless server-owned.
	std::vector<WonGame> WonGames() const { return {wonGames_.begin(), wonGames_.end()}; }
	// The room host closes a server-owned room; no member's action is involved.
	Result CloseServerOwned();

	Result Join(const std::string& name, const ConnectionRef& connection, bool host = false, MemberProfile profile = {});
	Result Leave(MemberId member);
	Result Apply(MemberId member, const Action& action);
    // The fighter a member last showed. When a seated member who has not
    // readied changes a fighter it already showed, the table revision moves,
    // so no Ready given against the old matchup is accepted later, and the
    // other seat's Ready is taken back as an Unready would
    // (*withdrewOpponentReady). False when nothing changed.
    bool SetMemberFighter(MemberId member,int fighter,bool* withdrewOpponentReady=nullptr);
    // Members whose last activity is tracked; never more than the roster.
    std::size_t TrackedActivity() const { return lastActiveMs_.size(); }

    // Authority recovery is deliberately distinct from the public UI snapshot.
    // Import validates into a temporary owner and leaves this owner unchanged
    // on failure. No transport handles or native game pointers are serialized.
    static constexpr std::size_t MaximumCheckpointBytes = 1024 * 1024;
    nlohmann::json Checkpoint() const;
    bool RestoreCheckpoint(const nlohmann::json& checkpoint);
    Result TransferHost(MemberId actor, MemberId successor);

	// Used by the server's legacy-ready bridge after both table fighters are
	// ready. These operations are intentionally separate from client actions:
	// only the session owner can grant a match generation or acknowledge native
	// game teardown.
	Result BeginMatch(std::uint8_t table, MemberId p1, MemberId p2);
	// The session owner applies the tournament binding its own helper checked.
	// It never comes from another member. Members whose endpoint the binding
	// does not name leave, and the bound fighters are seated by slot at table
	// 0 when no game is under way there. A binding for another match, or an
	// older one for this match, is refused.
	Result BindTournament(const TournamentBinding& binding);
	Result EndMatch(std::uint8_t table, std::uint64_t generation, MatchResult result);
	// True only when AdvanceTime(nowMs) can emit a timer-driven room event.
	// Pending results which have not reached their deadline, and unresolved
	// results already paused for host review, are not timer work.
	bool HasDueTimerTransition(std::uint64_t nowMs) const;
	// Milliseconds from nowMs until HasDueTimerTransition first turns true if
	// nothing else changes: zero when one is due now, the largest value when no
	// timer is running. A caller that sleeps between ticks wakes by then. A
	// permit's age counts on from the sample AgePermitHolds took, so a caller
	// that stops ticking for long should not rely on a permit's figure.
	std::uint64_t NextDueMs(std::uint64_t nowMs) const;
	std::vector<Event> AdvanceTime(std::uint64_t nowMs);
	// Recovery rebases result deadlines against a fresh monotonic clock. The
	// first owner tick resumes the paused timer; no wall-clock deadline elapses
	// while a passive follower is being restored.
	bool RecoveryPaused() const { return recoveryPaused_; }
	// Return every frozen native recipient for a completed generation. These
	// identities remain available after generic effect-journal compaction so a
	// late replay can regenerate game_end while fighter and spectator links are
	// still draining.
	std::vector<MemberId> TerminalMembers(std::uint8_t table, std::uint64_t generation) const;
	// The native roster frozen when the table's current generation began: both
	// fighters, then its spectators. A spectator still acknowledging an earlier
	// generation is left out, because it may still hold that generation's
	// GGPO session. Empty when no match is active.
	std::vector<MemberId> MatchRoster(std::uint8_t table) const;
	// The roster the live generation's grant carries: MatchRoster restricted to
	// a generation this table's fighters still own. Empty when no match is in
	// play, or when the frozen roster belongs to a pair the table has replaced.
	// A native projection must then fall back to the table's own spectator list
	// rather than show a roster that never entered a grant.
	std::vector<MemberId> LiveMatchRoster(std::uint8_t table) const;
	struct TerminalReplay {
		std::uint8_t table = 0;
		std::uint64_t generation = 0;
		MatchResult result = MatchResult::Abort;
	};
	// Rebind/admission replay for a recipient whose original MatchEnded event
	// was compacted or delivered while it was disconnected. Only receipts that
	// still await this exact member's incarnation are returned.
	std::vector<TerminalReplay> PendingTerminalEvents(MemberId member) const;
	// Convert monotonic timestamps to relative ages before handing a
	// checkpoint to another owner. ResumeRecovery rebases those ages against
	// the receiving process's clock; it accepts clocks lower than the sender's.
	void PauseForRecovery();
	// A healthy passive replica keeps relative timer ages current without
	// executing timer effects. Actual control recovery does not call this, so
	// result deadlines remain suspended until quorum and control return.
	void AdvancePausedTimers(std::uint64_t elapsedMs);
	// A permit's start window is the bridge's, so unlike the room's own
	// deadlines it keeps running while coordination is lost, and pausing and
	// resuming leave it alone. Each table holding a permit has one timer: the
	// generation it reserved, and its age as this process counts it, from
	// sampleMs on its monotonic clock. An age a restored commit measured on
	// another clock has no such time here: it counts from the next
	// AgePermitHolds, as an unsampled age, or as heldMs where this process
	// already counts the same reservation. The age at any time is the larger
	// of the two, so both exports read it the same way. The timer outlives
	// BeginMatch until the game natively starts: windowMs is then the shorter
	// window of the two permits, and zero while the table still holds them.
	struct PermitTimer {
		std::uint64_t generation = 0;  // zero when the table holds no permit
		std::uint64_t ageMs = 0;
		std::uint64_t sampleMs = 0;
		bool sampled = false;
		std::uint64_t heldMs = 0;
		std::uint64_t windowMs = 0;
	};
	struct PermitTimers {
		std::array<PermitTimer, TableCount> tables = {};
		// The last time AgePermitHolds was given, where a new reservation's
		// timer starts.
		std::uint64_t clockMs = 0;
		bool clockKnown = false;
	};
	// Brings every permit's age to nowMs on this process's monotonic clock.
	// AdvanceTime and ResumeRecovery call it; a paused replica's owner calls it
	// every tick, its coordination healthy or not.
	void AgePermitHolds(std::uint64_t nowMs);
	// The timers as they stand; a checkpoint carries their ages.
	const PermitTimers& PermitAges() const { return permits_; }
	// Called on a paused room just restored, with the timers of the room it
	// replaced on this process; a live room throws std::logic_error. A held
	// age therefore folds in before a room runs live again, so a live
	// checkpoint, whose ages count from its permit clock, never carries one. The same reservation keeps this process's count and
	// holds the restored age, so a commit made before an outage takes back no
	// time this process saw, a newer commit's age is not counted twice, and
	// restoring again and again, aged or not, loses nothing.
	void KeepPermitAges(const PermitTimers& kept);
	// A bound game begun but not natively started yet, whose permit window
	// has run out since its generation was reserved. The start gate kept
	// PermitStartMarginMs of the window for this preparation, so the native
	// start may use all of it.
	bool NativeStartExpired(std::uint8_t table, std::uint64_t generation) const;
	// The game natively started: its permit timer has no more use.
	void NativeStarted(std::uint8_t table, std::uint64_t generation);
	void ResumeRecovery(std::uint64_t nowMs);
	void SetMemberIncarnation(MemberId member, std::uint64_t incarnation);

private:
	// One rule per action kind, called from Apply() after its shared checks.
	Result ApplyClose(MemberId member);
	Result ApplySetCapacity(MemberId member, const Action& action);
	Result ApplyLock(MemberId member, const Action& action);
	Result ApplyKick(MemberId member, const Action& action);
	Result ApplyChat(MemberId member, const Action& action);
	Result ApplyRename(MemberId member, const Action& action);
	Result ApplyAcknowledgeTerminal(MemberId member, const Action& action);
	Result ApplySetRules(MemberId member, const Action& action, Table* table);
	Result ApplyQueue(MemberId member, const Action& action, Table* table);
	Result ApplyUnqueue(MemberId member, Table* table);
	Result ApplyWatch(MemberId member, Table* table);
	Result ApplyUnwatch(MemberId member, Table* table);
	Result ApplyLeaveGame(MemberId member, Table* table);
	Result ApplyReadiness(MemberId member, const Action& action, Table* table, Member* item);
	// Unready's change to one seat, and the phase, holds, permits and revision
	// that follow any readiness change.
	void ReleaseReady(Table& table, int seat, Member& item);
	Result SettleReadiness(Table& table, MemberId member);
	Result ApplyRecordResult(MemberId member, const Action& action, Table* table);
	Result ApplyMatchFinished(MemberId member, const Action& action, Table* table);
	Result ApplyCancelResult(MemberId member, const Action& action, Table* table);
	Result ApplyAbortMatch(MemberId member, const Action& action, Table* table);
	Result ApplyLockSpectating(MemberId member, const Action& action, Table* table, Member* item);
	Result ApplySetTraining(MemberId member, const Action& action);
	Result ApplyTyping(MemberId member, const Action& action);
	Result ApplyPermitReady(MemberId member, const Action& action, Table* table);
	// Table 0 of a room bound to a tournament match.
	bool BoundTable(const Table& table) const;
	// What a bound room makes of a join (RoomModelTournament.cxx): Unbound is an
	// ordinary room, Named a fighter or watcher the binding names, Stranger
	// anyone else in a bound room, and Refuse an observer the room does not name.
	enum class BoundAdmission { Unbound, Named, Stranger, Refuse };
	BoundAdmission BoundJoin(const ConnectionRef& connection, const MemberProfile& profile) const;
	// Who takes the host field when its holder leaves: the oldest member, a
	// bound room's fighters before its watchers.
	const Member* HostSuccessor(MemberId leaving) const;
	// Everything a bound room adds to a join once the member is in: fighters
	// seated, a fighter taking the host field back from a watcher, a watcher
	// placed as a spectator of table 0.
	void SettleBoundJoin(MemberId joined);
	// Places `member` as a spectator of table 0 (watching next during a game):
	// the room's revision moves, table 0's does not.
	void PlaceWatcher(MemberId member);
	// Puts every named watcher of the binding that is not already a spectator
	// of table 0 there, off any seat or queue place it took.
	void NormalizeBoundWatchers();
	// A change confined to a table's spectator lists: at a bound table only the
	// room's revision moves, so a fighter's Ready is never made stale by one.
	void TouchSpectators(Table& table);
	// What a bound watcher may do, and nothing else: leave, acknowledge a game's
	// end, and say it is in Training. ApplyAction asks it before any kind is
	// dispatched, so a kind added later is refused to a watcher until listed here.
	static bool WatcherMay(ActionKind kind);
	// Seats each bound fighter in its slot at table 0, unless a game is being
	// started, played or resolved there.
	void SeatBoundFighters();
	// Reserves the next generation for a bound table's ready fighters to get
	// a permit for.
	void ReservePermit(Table& table);
	void ClearPermit(Table& table);
	Result ApplyAction(MemberId member, const Action& action);
	// A locked-in spectator of this table has not yet acknowledged an earlier
	// generation, so a start now would leave it out.
	bool LockedSpectatorReturning(const Table& table) const;
	// A table deadline (result dispute, spectator hold or Ready timeout) that
	// started at `since` has passed.
	bool TimerDue(std::uint64_t since, std::uint64_t timeout, std::uint64_t nowMs) const;
	MemberId WaitingForReady(const Table& table) const;
	void RefreshReadyTimers();
	// The table's permit age at nowMs, without moving its timer.
	std::uint64_t PermitAgeAt(std::size_t table, std::uint64_t nowMs) const;
	// Its age at the last time AgePermitHolds was given.
	std::uint64_t PermitAge(const Table& table) const { return PermitAgeAt(table.id, permits_.clockMs); }
	// The shorter window of the permits the table holds; zero with none.
	static std::uint64_t PermitWindow(const Table& table);
	// The permit's start window, less PermitStartMarginMs, has run out by the
	// time the table's generation is ageMs old. A permit without a known
	// window counts as run out.
	bool PermitStartPassed(const Table& table, std::uint64_t ageMs) const;
	// The table's begun game still waits for its native start under a permit,
	// in play or paused over a result reported before that start.
	bool NativeStartPending(std::size_t table) const;
	// The start a bound table holds for its permit is called off: the permit
	// never reached both fighters within PermitHoldMs, or its window passed.
	bool PermitCalledOff(const Table& table, std::uint64_t ageMs) const;
	// Ends every start hold whose spectators are back or whose deadline has
	// passed, appending the MatchReady the hold deferred. Returns true when a
	// table changed.
	bool ReleaseHeldStarts(std::vector<Event>& events);
	Member* Find(MemberId member);
	const Member* Find(MemberId member) const;
	Table* FindTable(std::uint8_t table);
	const Table* FindTable(std::uint8_t table) const;
	Result Reject(RejectReason reason);
	Result Accept(std::vector<Event> events = {});
	void Touch(Table& table);
	void TouchRoom();
	void NormalizeMemberStatus(MemberId member);
	void NormalizeTableMembers(const Table& table);
	// Takes the member off every table's watch lists and clears its lock-in.
	void StopWatching(MemberId member);
	void SeatQueued(Table& table);
	void FillVacancy(Table& table, int seat);
	// A first-to-N table whose score just reached N: records the set, moves the
	// streak, and sends the rotated fighters to the back of the queue.
	void CompleteSet(Table& table, int winnerSeat);
	// Ends a streak whose holder no longer fights at the table.
	void KeepStreak(Table& table);
	void RemoveFromTable(MemberId member, bool preserveSpectator = false);
	// A seat's Ready, what it captured for the match, and the delay lock of
	// whoever sits there. Every readiness reset goes through it.
	void ClearSeatReadiness(Table& table, int seat);
	// Both seats' readiness.
	void ClearReadiness(Table& table);
	// The bookkeeping that ends a live generation: watchers who asked for the next
	// game join the spectators, members whose place ended with this game leave
	// them, the frozen roster is dropped and the fighters' delays unlock.
	void CloseLiveGeneration(Table& table);
	void ResetTable(Table& table, bool clearScore);
	bool IsHost(MemberId member) const;
	bool CanEditRules(MemberId member) const;
	bool IsTableMember(const Table& table, MemberId member) const;
	bool IsParticipant(const Table& table, MemberId member) const;
	struct TerminalRecipient {
		MemberId member = 0;
		ConnectionRef endpoint;
		std::uint64_t incarnation = 1;
		bool acknowledged = false;
	};
	struct TerminalReceipt {
		std::uint8_t table = 0;
		std::uint64_t generation = 0;
		MatchResult result = MatchResult::Abort;
		std::array<MemberId, 2> fighters = {};
		std::vector<TerminalRecipient> recipients;
		bool acknowledged = false;
	};
	// Acknowledged terminal receipts may leave the bounded receipt journal, but
	// an in-flight client ACK can still be retried after its action reply was
	// lost. Keep the exact recipient endpoint/incarnation as a bounded,
	// authenticated idempotency tombstone; unknown generations never match.
	struct TerminalAckTombstone {
		std::uint8_t table = 0;
		std::uint64_t generation = 0;
		MemberId member = 0;
		ConnectionRef endpoint;
		std::uint64_t incarnation = 0;
	};
	TerminalReceipt* FindTerminalReceipt(std::uint8_t table, std::uint64_t generation, MatchResult result, MemberId member);
	const TerminalReceipt* FindTerminalReceipt(std::uint8_t table, std::uint64_t generation, MatchResult result, MemberId member) const;
	bool StoreTerminalReceipt(std::uint8_t table, const Table& value, MatchResult result);
	void RememberTerminalAck(const TerminalReceipt& receipt, const TerminalRecipient& recipient);
	void RetireTerminalReceipts(MemberId member);
	// A table is held only while one of a receipt's fighters has not
	// acknowledged it. Snapshot::terminalPending reports the same rule.
	static bool FightersOutstanding(const TerminalReceipt& receipt);
	bool HasOutstandingTerminalReceipt(std::uint8_t table) const;
	bool HasOutstandingTerminalReceiptForMember(MemberId member) const;
	bool ValidTable(std::uint8_t table) const;
	// Server-owned rules, in RoomModelServerOwned.cxx. Join's account checks,
	// the departure of the last member, and the checkpoint's account fields.
	RejectReason CheckServerOwnedJoin(const MemberProfile& profile) const;
	void RememberAccount(MemberId member, std::string account);
	void BanAccount(const std::string& account);
	bool BanWouldExceedCap(const std::string& account) const;
	void ForgetAccount(MemberId member);
	void RecordWonGame(std::uint8_t tableId, const Table& table, MatchResult result);
	void ReopenEmptyRoom();
	void SaveServerOwned(nlohmann::json& state) const;
	bool LoadServerOwned(const nlohmann::json& state);

	Snapshot snapshot_;
	std::uint64_t nextMemberId_ = 1;
	std::uint64_t nextJoinOrder_ = 1;
	std::uint64_t nextMatchGeneration_ = 1;
	std::uint64_t nextChatSequence_ = 1;
	std::array<MemberId, TableCount> resultReporter_ = {};
	std::array<MatchResult, TableCount> pendingResult_ = {};
	std::array<std::uint64_t, TableCount> resultPendingSince_ = {};
	std::array<std::uint64_t, TableCount> startHeldSince_ = {};
	std::array<std::uint64_t, TableCount> readyWaitingSince_ = {};
	std::array<MemberId, TableCount> readyWaitingFor_ = {};
	// The table deadlines, chat times and activity times each hold one of two
	// things. While the authority runs, a timer holds the instant it started
	// on nowMs_. Paused for recovery it holds the timer's age instead: frozen,
	// except that AdvancePausedTimers moves a passive replica's ages on, each
	// capped. Permit timers keep their own clock and are none of these.
	// Every such timer with its cap: a table deadline's timeout, a second for
	// chat, the idle limit for activity. Recovery turns them all into ages and
	// back through this one list.
	template <typename Visit> void ForEachTimer(Visit&& visit);
	// What a timer starting now holds: nowMs_, or a zero age while paused.
	std::uint64_t TimerStart() const { return recoveryPaused_ ? 0 : nowMs_; }
	// A timer's age on nowMs_; a start after nowMs_ reads as zero.
	std::uint64_t TimerAge(std::uint64_t held) const {
		return recoveryPaused_ ? held : nowMs_ >= held ? nowMs_ - held : 0;
	}
	// A timer's age on the later of nowMs and nowMs_, as TimerDue reads it.
	std::uint64_t TimerAgeAt(std::uint64_t held, std::uint64_t nowMs) const {
		return recoveryPaused_ ? held : (std::max)(nowMs, nowMs_) - held;
	}
	// What a timer of this age holds.
	std::uint64_t TimerFromAge(std::uint64_t age) const { return recoveryPaused_ ? age : nowMs_ - age; }
	// A new room's clock starts at zero, like nowMs_; a restored one's is
	// the live checkpoint's permit clock, or unknown.
	PermitTimers permits_ = {{}, 0, true};
	std::array<std::vector<TerminalRecipient>, TableCount> activeMatchRecipients_;
	static constexpr std::size_t MaximumTerminalReceipts = 64;
	static constexpr std::size_t MaximumTerminalAckTombstones = 256;
	std::deque<TerminalReceipt> terminalReceipts_;
	std::deque<TerminalAckTombstone> terminalAckTombstones_;
	std::map<MemberId, std::uint64_t> lastChatMs_;
	// When each member last did anything (an age while recovery is paused,
	// like lastChatMs_). Not checkpointed; a restored room counts from its
	// next tick. Ready timeout ages separately retain the activity deadline.
	std::map<MemberId, std::uint64_t> lastActiveMs_;
	void NoteActive(MemberId member);
	std::uint32_t IdleSeconds(MemberId member) const;
	// Who is typing in chat: when they last said so and whether they still are.
	// Not checkpointed. TypingLeftMs is how long that word still stands.
	std::map<MemberId, std::pair<std::uint64_t, bool>> typing_;
	std::uint32_t TypingLeftMs(MemberId member) const;
	std::map<MemberId, ActionId> lastAcceptedActions_;
	std::set<ConnectionRef> kicked_;
	// Server-owned rooms only. Held here rather than on Member so an account
	// never reaches the snapshot other members see.
	std::map<MemberId, std::string> memberAccounts_;
	std::set<std::string> kickedAccounts_;
	std::deque<WonGame> wonGames_;
	std::uint64_t nowMs_ = 0;
	bool recoveryPaused_ = false;
	MemberId activeActionMember_ = 0;
	ActionId activeActionId_ = 0;
};

} }
