#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "../common/InputDelay.hxx"
#include "../common/NetworkLink.hxx"
#include "../common/NetworkNat.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/MatchResult.hxx"
#include "../common/RoomRules.hxx"

// The room model is deliberately independent from the game and transport
// headers.  It is the value boundary between the session owner and the UI.
// RoomAuthority is game-thread owned; callers exchange copies of Snapshot and
// Action and never retain references into the authority.
namespace sf4e { namespace room {

using MemberId = std::uint64_t;
using ActionId = std::uint64_t;

// 2: tables end first-to-N sets and rotate their queue.
constexpr std::uint32_t ProtocolVersion = 2;
constexpr std::size_t MaximumMembers = MaxMembers;
constexpr std::size_t MaximumRoomNameBytes = 64;
constexpr std::size_t MaximumChatMessages = 100;
constexpr std::size_t MaximumChatBytes = 256;
// How long two ready fighters wait for a locked-in spectator who is still
// leaving the previous game. The catalogs quote it as {0} seconds in
// room.waiting_spectators, room.lock_spectating.detail and
// room.unlock_spectating.detail; the UI passes SpectatorStartHoldMs / 1000.
constexpr std::uint64_t SpectatorStartHoldMs = 10000;

enum class MemberStatus : std::uint8_t {
	Idle = 0,
	Queued,
	Seated,
	Ready,
	Playing,
	Watching,
	WatchingNext,
};

enum class TablePhase : std::uint8_t {
	Idle = 0,
	Waiting,
	Ready,
	Playing,
	Paused,
	Closed,
};

enum class ActionKind : std::uint8_t {
	Join = 0,
	Leave,
	Queue,
	Unqueue,
	Watch,
	Unwatch,
	Ready,
	Unready,
	SetRules,
	SetCapacity,
	Lock,
	Kick,
	Close,
	Chat,
	Rename,
	TransferHost,
	RecordResult,
	MatchFinished,
	CancelResult,
	AbortMatch,
	AcknowledgeTerminal,
	// A spectator's lock-in, on (Action::locked) or off. Appended so older
	// authorities reject it as an unknown kind.
	LockSpectating,
};

enum class RejectReason : std::uint8_t {
	None = 0,
	Closed,
	RoomFull,
	AdmissionLocked,
	NameTaken,
	UnknownMember,
	UnknownTable,
	NotHost,
	NotSeated,
	AlreadySeated,
	AlreadyQueued,
	NotQueued,
	NotWatching,
	InvalidSeat,
	InvalidRules,
	InvalidCapacity,
	NotReady,
	StaleRoom,
	StaleTable,
	WrongPhase,
	WrongGeneration,
	Unauthorized,
	DuplicateResult,
	InvalidChat,
	MemberKicked,
	TerminalLedgerFull,
};

// What a member brings to the room when it joins.
struct MemberProfile {
	int mainFighter = -1;
	NetworkLink link = NetworkLink::Unknown;
	NatClass nat = NatClass::Unknown;
};

struct ConnectionRef {
	std::string host;
	std::string user;
	bool operator==(const ConnectionRef& rhs) const {
		return host == rhs.host && user == rhs.user;
	}
	bool operator<(const ConnectionRef& rhs) const {
		return host < rhs.host || (host == rhs.host && user < rhs.user);
	}
};

struct Member {
	MemberId id = 0;
	std::string name;
	ConnectionRef connection;
	bool host = false;
	MemberStatus status = MemberStatus::Idle;
	std::int8_t table = -1;
	std::int8_t seat = -1;
	std::uint64_t joinOrder = 0;
	// Authenticated endpoint incarnation captured in native match receipts.
	// Older snapshots omit it and restore the protocol default of one.
	std::uint64_t incarnation = 1;
    // Last validated fighter shared through the existing pre-battle message.
    int fighter = -1;
    // Profile identity, independent from the fighter chosen for this table.
    int mainFighter = -1;
    NetworkLink link = NetworkLink::Unknown;
    // How this member's network treats a direct connection, as its own helper
    // reported it when it joined. Unknown while that check was still running.
    NatClass nat = NatClass::Unknown;
    // The selected delay belongs to this fighter until Ready captures it.
    // Values are deliberately bounded by Action deserialization (0..10).
    std::uint8_t selectedDelay = 2;
    std::uint8_t frozenDelay = 2;
    bool delayLocked = false;
    // A spectator who locked in: the next start at its table waits a bounded
    // time for it to finish retiring the previous game. Cleared when it stops
    // watching that table.
    bool spectatorLocked = false;
};

// The set a table finished last: who sat where for its deciding game, and the
// final score by seat. The score itself resets for the next set.
struct SetRecord {
	std::uint64_t generation = 0;
	MemberId p1 = 0;
	MemberId p2 = 0;
	std::uint32_t score[2] = { 0, 0 };
	std::uint8_t winnerSeat = 0;
	MemberId Winner() const { return winnerSeat == 0 ? p1 : p2; }
	MemberId Loser() const { return winnerSeat == 0 ? p2 : p1; }
};

struct Table {
	std::uint8_t id = 0;
	Rules rules;
	TablePhase phase = TablePhase::Idle;
	std::uint64_t revision = 0;
	std::uint64_t matchGeneration = 0;
	MemberId p1 = 0;
	MemberId p2 = 0;
	std::vector<MemberId> queue;
	std::vector<MemberId> spectators;
	std::vector<MemberId> watchingNext;
	// Queued members who left the queue during the live game. They watch it only
	// because the queue listed them as spectators, so their place ends with it
	// unless they choose to watch. Always a subset of spectators.
	std::vector<MemberId> endingWatchers;
	bool ready[2] = { false, false };
	// Immutable per-fighter values captured at the Ready quorum boundary.
	std::uint8_t inputDelay[2] = { 2, 2 };
	std::uint32_t score[2] = { 0, 0 };
	// First-to-N tables: the last finished set, and how many sets in a row
	// streakHolder has won here. The streak ends when its holder loses a set
	// or stops fighting at this table.
	SetRecord lastSet;
	MemberId streakHolder = 0;
	std::uint32_t streak = 0;
	bool resultPending = false;
	// Both fighters are ready and the start is waiting, for at most
	// SpectatorStartHoldMs, on a locked-in spectator still retiring the
	// previous game.
	bool spectatorHold = false;
};

// A seated fighter may still change fighter and delay: no game is being
// prepared, played or resolved at the table, and they have not readied. An
// empty opposite seat (Idle) locks nothing.
inline bool SeatEditable(const Table& table, int seat) {
	return (table.phase == TablePhase::Idle || table.phase == TablePhase::Waiting) && !table.ready[seat];
}

// A fighter who readied can still take it back: while the other seat is not
// ready, or while the start is held for a locked-in spectator.
inline bool ReadyCancellable(const Table& table, int seat) {
	return table.ready[seat] && (table.phase == TablePhase::Waiting ||
		(table.phase == TablePhase::Ready && table.spectatorHold));
}

// Watching this table by choice. A queued member is also listed as a
// spectator of the game it waits out, and stays listed until that game ends
// after it leaves the queue, but it did not choose to watch.
inline bool WatchesByChoice(const Table& table, MemberId member) {
	const auto listed = [member](const std::vector<MemberId>& list) {
		return std::find(list.begin(), list.end(), member) != list.end();
	};
	return (listed(table.spectators) || listed(table.watchingNext)) && !listed(table.queue) &&
		!listed(table.endingWatchers);
}

// A named seat (0 or 1) that a Queue for exactly that seat would take now: the
// table is between games, nobody is queued ahead and the seat is empty.
inline bool SeatOpenNow(const Table& table, int seat) {
	return (seat == 0 || seat == 1) && table.phase != TablePhase::Playing && table.phase != TablePhase::Paused &&
		table.queue.empty() && !(seat == 0 ? table.p1 : table.p2);
}

// What holds a fighter to the table while a game is live or being resolved.
// UnreadyFirst: the fighter's own Ready is the only thing in the way, and taking
// it back frees them.
enum class SeatHold : std::uint8_t { None, UnreadyFirst, Unresolved, AwaitingResult, InProgress };

// The hold a live table puts on its seats: an unresolved result, a finished
// game waiting for its result, or a game under way.
inline SeatHold LiveGameHold(const Table& table, bool localPostMatch) {
	if (table.phase == TablePhase::Paused) return SeatHold::Unresolved;
	if (table.phase == TablePhase::Playing && (table.resultPending || localPostMatch)) return SeatHold::AwaitingResult;
	return SeatHold::InProgress;
}

// Why a fighter may not change fighter or appearance now. The table is
// active while it is Ready, Playing or Paused; a readied fighter has to take
// Ready back whatever the phase.
inline SeatHold ChangingFighterHold(const Table& table, int seat, bool localPostMatch) {
	if (ReadyCancellable(table, seat)) return SeatHold::UnreadyFirst;
	const bool active = table.phase == TablePhase::Ready || table.phase == TablePhase::Playing || table.phase == TablePhase::Paused;
	return active ? LiveGameHold(table, localPostMatch) : SeatHold::None;
}

// Why a fighter may not leave the seat now. A seat is held while the table is
// active, or while this client's own game (localGameLive: preparing or playing)
// is still under way after the table went back to Waiting.
inline SeatHold LeavingSeatHold(const Table& table, int seat, bool localGameLive, bool localPostMatch) {
	const bool active = table.phase == TablePhase::Ready || table.phase == TablePhase::Playing || table.phase == TablePhase::Paused;
	if (!active && !localGameLive) return SeatHold::None;
	if (ReadyCancellable(table, seat)) return SeatHold::UnreadyFirst;
	return LiveGameHold(table, localPostMatch);
}

// What a fighter gives up by leaving the seat: the set score, and the seat
// itself to the next player queued.
struct LeavingCost {
	bool score = false;
	bool handsOver = false;
	explicit operator bool() const { return score || handsOver; }
};
inline LeavingCost CostOfLeavingSeat(const Table& table) {
	return {table.score[0] != 0 || table.score[1] != 0, !table.queue.empty()};
}

// A live game with no result for this long is offered to the host as stuck. The
// room has no clock the clients share, so each client times it from when it
// first saw that game.
constexpr unsigned StaleGameSeconds = 600;

// Both fighters play at the higher of their Ready delays. A fighter's delay
// decides how much rollback the other side sees, so separate values gave the
// lower-delay fighter an advantage. inputDelay keeps each fighter's own choice.
inline int MatchDelay(int first, int second) { return first > second ? first : second; }
inline std::uint8_t MatchDelay(const Table& table) {
	return static_cast<std::uint8_t>(MatchDelay(table.inputDelay[0], table.inputDelay[1]));
}

struct ChatMessage {
	std::uint64_t sequence = 0;
	MemberId sender = 0;
	std::string text;
};

struct Snapshot {
	std::uint32_t protocolVersion = ProtocolVersion;
	std::uint64_t roomEpoch = 0;
	std::uint64_t revision = 0;
	std::string name;
	std::uint8_t capacity = static_cast<std::uint8_t>(MaximumMembers);
	bool locked = false;
	bool closed = false;
	MemberId host = 0;
	MemberId localMember = 0;
	std::vector<Member> members;
	std::array<Table, TableCount> tables;
	// Derived from the durable terminal receipt ledger for this wire recipient.
	// A table remains pending until every frozen native recipient has explicitly
	// acknowledged outcome persistence and teardown; localTerminalPending gates
	// this member's Queue/Watch/Ready mutations across tables.
	std::array<bool, TableCount> terminalPending = {};
	bool localTerminalPending = false;
	// Exact unacknowledged generation for this wire recipient and table. Zero
	// means no local obligation and prevents an older table receipt from being
	// used as confirmation for a newer terminal event.
	std::array<std::uint64_t, TableCount> localTerminalGenerations = {};
	std::vector<ChatMessage> chat;
};

inline const Member* FindMember(const Snapshot& snapshot, MemberId id) {
	const auto found = std::find_if(snapshot.members.begin(), snapshot.members.end(),
		[id](const Member& member) { return member.id == id; });
	return found == snapshot.members.end() ? nullptr : &*found;
}

// Where a member holds a place in the room: a seat at one table, a place in one
// table's queue, or neither. Watching is not a place, because Queue and Watch
// both end it.
struct Place {
	enum class Kind : std::uint8_t { None, Seat, Queue } kind = Kind::None;
	int table = -1;
	int seat = -1;
};
inline Place PlaceOf(const Snapshot& snapshot, MemberId id) {
	Place place;
	const auto* member = FindMember(snapshot, id);
	if (!member) return place;
	if (member->seat >= 0 && member->seat < 2 && member->table >= 0 && member->table < static_cast<int>(snapshot.tables.size())) {
		place.kind = Place::Kind::Seat; place.table = member->table; place.seat = member->seat;
		return place;
	}
	for (const auto& table : snapshot.tables)
		if (std::find(table.queue.begin(), table.queue.end(), id) != table.queue.end()) {
			place.kind = Place::Kind::Queue; place.table = table.id;
			return place;
		}
	return place;
}

struct Action {
	ActionKind kind = ActionKind::Queue;
	std::uint32_t protocolVersion = ProtocolVersion;
	std::uint64_t roomEpoch = 0;
	std::uint64_t revision = 0;
	std::uint64_t tableRevision = 0;
	ActionId actionId = 0;
	std::uint8_t table = 0;
	std::int8_t seat = -1;
	MemberId target = 0;
	Rules rules;
	std::uint8_t capacity = 0;
	bool locked = false;
	MatchResult result = MatchResult::Abort;
	std::uint64_t matchGeneration = 0;
	std::uint8_t inputDelay = 2;
	std::string text;
	// Set on the generation-scoped Unwatch a spectator's runtime sends when its
	// own stream or setup failed: leave this game only. The watch stays, the
	// lock-in does not. A player's own Stop watching never sets it.
	bool keepWatching = false;
};

struct Event {
	enum class Kind : std::uint8_t {
	SnapshotChanged = 0,
	MatchReady,
	MatchStarted,
	MatchEnded,
	MemberRemoved,
	RoomClosed,
	ResultDisputed,
	ChatMessage,
	};
	Kind kind = Kind::SnapshotChanged;
	std::uint8_t table = 0;
	std::uint64_t matchGeneration = 0;
	MemberId member = 0;
	MatchResult result = MatchResult::Abort;
	bool terminalReplay = false;
};

struct Result {
	bool accepted = false;
	RejectReason reason = RejectReason::None;
	// A retry can be accepted from the durable terminal receipt after the
	// mutable table has already rotated to Waiting. The marker lets the session
	// layer replay native teardown without rescoring the generation.
	bool terminalReplay = false;
	Snapshot snapshot;
	std::vector<Event> events;
};

class RoomAuthority {
public:
	RoomAuthority(std::string name, std::uint8_t capacity = static_cast<std::uint8_t>(MaximumMembers),
		std::uint64_t roomEpoch = 1, Rules defaults = Rules());

	const Snapshot& SnapshotView() const { return snapshot_; }
	Snapshot SnapshotCopy() const { return snapshot_; }
	Snapshot SnapshotFor(MemberId member) const;
	std::uint64_t NextMemberId() const { return nextMemberId_; }

	Result Join(const std::string& name, const ConnectionRef& connection, bool host = false, MemberProfile profile = {});
	Result Leave(MemberId member);
	Result Apply(MemberId member, const Action& action);
    bool SetMemberFighter(MemberId member,int fighter);

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
	Result EndMatch(std::uint8_t table, std::uint64_t generation, MatchResult result);
	// True only when AdvanceTime(nowMs) can emit a timer-driven room event.
	// Pending results which have not reached their deadline, and unresolved
	// results already paused for host review, are not timer work.
	bool HasDueTimerTransition(std::uint64_t nowMs) const;
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
	Result ApplyRecordResult(MemberId member, const Action& action, Table* table);
	Result ApplyMatchFinished(MemberId member, const Action& action, Table* table);
	Result ApplyCancelResult(MemberId member, const Action& action, Table* table);
	Result ApplyAbortMatch(MemberId member, const Action& action, Table* table);
	Result ApplyLockSpectating(MemberId member, const Action& action, Table* table, Member* item);
	Result ApplyAction(MemberId member, const Action& action);
	// A locked-in spectator of this table has not yet acknowledged an earlier
	// generation, so a start now would leave it out.
	bool LockedSpectatorReturning(const Table& table) const;
	// A table deadline (ResultDisputeTimeoutMs, SpectatorStartHoldMs) that
	// started at `since` has passed.
	bool TimerDue(std::uint64_t since, std::uint64_t timeout, std::uint64_t nowMs) const;
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
	// Ready flags and the delay locks that go with them.
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
	bool ValidRules(const Rules& rules) const;
	bool ValidTable(std::uint8_t table) const;

	Snapshot snapshot_;
	std::uint64_t nextMemberId_ = 1;
	std::uint64_t nextJoinOrder_ = 1;
	std::uint64_t nextMatchGeneration_ = 1;
	std::uint64_t nextChatSequence_ = 1;
	std::array<MemberId, TableCount> resultReporter_ = {};
	std::array<MatchResult, TableCount> pendingResult_ = {};
	std::array<std::uint64_t, TableCount> resultPendingSince_ = {};
	std::array<std::uint64_t, TableCount> startHeldSince_ = {};
	// Every running table deadline with its timeout, on the owner's monotonic
	// clock. Recovery turns them into ages and back through this one list.
	template <typename Visit> void ForEachTableTimer(Visit&& visit);
	std::array<std::vector<TerminalRecipient>, TableCount> activeMatchRecipients_;
	static constexpr std::size_t MaximumTerminalReceipts = 64;
	static constexpr std::size_t MaximumTerminalAckTombstones = 256;
	std::deque<TerminalReceipt> terminalReceipts_;
	std::deque<TerminalAckTombstone> terminalAckTombstones_;
	std::map<MemberId, std::uint64_t> lastChatMs_;
	std::map<MemberId, ActionId> lastAcceptedActions_;
	std::set<ConnectionRef> kicked_;
	std::uint64_t nowMs_ = 0;
	bool recoveryPaused_ = false;
	MemberId activeActionMember_ = 0;
	ActionId activeActionId_ = 0;
};

void to_json(nlohmann::json& json, const Rules& value);
void from_json(const nlohmann::json& json, Rules& value);
void to_json(nlohmann::json& json, const ConnectionRef& value);
void from_json(const nlohmann::json& json, ConnectionRef& value);
void to_json(nlohmann::json& json, const Member& value);
void from_json(const nlohmann::json& json, Member& value);
void to_json(nlohmann::json& json, const Table& value);
void from_json(const nlohmann::json& json, Table& value);
void to_json(nlohmann::json& json, const ChatMessage& value);
void from_json(const nlohmann::json& json, ChatMessage& value);
void to_json(nlohmann::json& json, const Snapshot& value);
void from_json(const nlohmann::json& json, Snapshot& value);
void to_json(nlohmann::json& json, const Action& value);
void from_json(const nlohmann::json& json, Action& value);
void to_json(nlohmann::json& json, const Event& value);
void from_json(const nlohmann::json& json, Event& value);
void to_json(nlohmann::json& json, const Result& value);
void from_json(const nlohmann::json& json, Result& value);

} }
