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
#include "TournamentBinding.hxx"

// The room model is deliberately independent from the game and transport
// headers.  It is the value boundary between the session owner and the UI.
// RoomAuthority is game-thread owned; callers exchange copies of Snapshot and
// Action and never retain references into the authority.
namespace sf4e { namespace room {

using MemberId = std::uint64_t;
using ActionId = std::uint64_t;

// 2: tables end first-to-N sets and rotate their queue.
// 3: a room can be bound to a tournament match; its table 0 then seats only
// the bound fighters and waits for the bridge's permit before each game.
constexpr std::uint32_t ProtocolVersion = 3;
constexpr std::size_t MaximumMembers = MaxMembers;
constexpr std::size_t MaximumRoomNameBytes = 64;
constexpr std::size_t MaximumChatMessages = 100;
constexpr std::size_t MaximumChatBytes = 256;
// A server-owned room bans at most this many accounts in its lifetime. Bans are
// never evicted: the kick that would exceed the cap closes the room. The helper,
// the supervisor and the bridge use the same number.
constexpr std::size_t MaximumKickedAccounts = 512;
// How long two ready fighters wait for a locked-in spectator who is still
// leaving the previous game. The catalogs quote it as {0} seconds in
// room.waiting_spectators, room.lock_spectating.detail and
// room.unlock_spectating.detail; the UI passes SpectatorStartHoldMs / 1000.
// A held table's snapshot carries what is left of it (Table::holdRemainingMs).
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
	// A bound table's fighter has the bridge's permit for the reserved game
	// (Action::matchGeneration), named by Action::text.
	PermitReady,
	// A member's own word that they are in the game's Training mode, on
	// (Action::locked) or off. It changes what the others are shown and
	// nothing else. Appended so older authorities reject it as an unknown
	// kind.
	SetTraining,
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
	// The player's Ember ID. Only server-owned rooms use it: it is the identity
	// a kick and a duplicate join are keyed on. Never part of a snapshot.
	std::string account;
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

// The longest idle time a snapshot reports (a week); longer reads the same.
constexpr std::uint32_t MaximumIdleSeconds = 7u * 24 * 60 * 60;

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
    // The member said they are in the game's Training mode (SetTraining). It
    // ends by their word, or when a game of theirs starts. Optional on the
    // wire and written only when set, so a state without it reads as before.
    bool training = false;
    // Seconds since the member last did anything in the room (joined, sent a
    // room action, showed a fighter), as of the moment the snapshot was sent;
    // a member in a game counts as active. Stamped per recipient when sent,
    // so the room's own state and checkpoints keep zero, and zero is not
    // serialized.
    std::uint32_t idleSeconds = 0;
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
	// While spectatorHold, the milliseconds left before the start goes ahead
	// anyway, as of the moment the snapshot was sent (at least 1). Stamped per
	// recipient like Member::idleSeconds, so the room's own state and its
	// checkpoints keep zero, and zero is not serialized; a client counts on
	// from when it received the snapshot.
	std::uint32_t holdRemainingMs = 0;
	// A bound table's ready fighters wait, for at most PermitHoldMs, for the
	// bridge's permit for this reserved generation. Each seat's permit ID is
	// filled when that fighter's helper has it; the game starts when both name
	// the same permit. Zero when no game is waiting for one.
	std::uint64_t permitGeneration = 0;
	std::array<std::string, 2> permits;
	// Each seat's permit's start window in milliseconds, as the bridge signed
	// it. The bridge issues a permit only after the room reserved its
	// generation, so the game must start within the shorter window from the
	// reservation; zero where a seat has no permit.
	std::array<std::uint64_t, 2> permitWindows = {};
};

// A seated fighter may still change fighter and delay: no game is being
// prepared, played or resolved at the table, and they have not readied. An
// empty opposite seat (Idle) locks nothing.
inline bool SeatEditable(const Table& table, int seat) {
	return (table.phase == TablePhase::Idle || table.phase == TablePhase::Waiting) && !table.ready[seat];
}

// The bridge's permit for a bound table's reserved game has not reached both
// fighters yet.
inline bool PermitPending(const Table& table) {
	return table.permitGeneration != 0 &&
		(table.permits[0].empty() || table.permits[0] != table.permits[1]);
}

// A fighter who readied can still take it back: while the other seat is not
// ready, or while the start is held for a locked-in spectator or a permit.
inline bool ReadyCancellable(const Table& table, int seat) {
	return table.ready[seat] && (table.phase == TablePhase::Waiting ||
		(table.phase == TablePhase::Ready && (table.spectatorHold || PermitPending(table))));
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
// A match never plays at 0, whatever an older client readied with.
inline int MatchDelay(int first, int second) { return PlayableInputDelay(first > second ? first : second); }
inline std::uint8_t MatchDelay(const Table& table) {
	return static_cast<std::uint8_t>(MatchDelay(table.inputDelay[0], table.inputDelay[1]));
}
// A connection check names the seated pair at the table revision this PC's
// room view shows. The helper reserves it only against its committed copy of
// the room, so the check waits until that copy seats the same pair at the same
// revision (`committed` is the local server's view of the same table).
inline bool ProbePairCommitted(const Table& seen, const Table& committed) {
	return seen.p1 && seen.p2 && seen.revision == committed.revision &&
		seen.p1 == committed.p1 && seen.p2 == committed.p2;
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
	// No player's game runs this room's authority: the room host process does. It
	// is serialized only when true, so a private room's wire form is unchanged.
	bool serverOwned = false;
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
	// The tournament match this room plays at table 0, when it is bound to one.
	TournamentBinding tournament;
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

// The member in the other seat of `table` from `local`'s point of view, or
// nullptr when `local` is not seated there or the seat is empty.
inline const Member* SeatOpponent(const Snapshot& snapshot, std::uint8_t table, MemberId local) {
	if (table >= snapshot.tables.size() || !local) return nullptr;
	const Table& at = snapshot.tables[table];
	if (at.p1 != local && at.p2 != local) return nullptr;
	const MemberId other = at.p1 == local ? at.p2 : at.p1;
	return other ? FindMember(snapshot, other) : nullptr;
}

// True when a custom room's snapshot gives this client no seat: it stood up,
// waits in a queue, or is no longer a member. A legacy lobby (no epoch) has no
// seats to lose.
inline bool LocalLeftSeat(const Snapshot& snapshot) {
	return snapshot.roomEpoch && PlaceOf(snapshot, snapshot.localMember).kind != Place::Kind::Seat;
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
	// PermitReady: the permit's start window in milliseconds (its start_by
	// less its issued_at, both on the bridge's clock).
	std::uint64_t startWindowMs = 0;
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

	// Marks a fresh room server-owned (see RoomModelServerOwned.cxx). False, and
	// no change, once anyone has joined or the room has closed.
	bool SetServerOwned();
	bool ServerOwned() const { return snapshot_.serverOwned; }
	// Accounts a kick removed for the room's life; empty unless server-owned.
	std::vector<std::string> KickedAccounts() const;
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
	Result ApplyPermitReady(MemberId member, const Action& action, Table* table);
	// Table 0 of a room bound to a tournament match.
	bool BoundTable(const Table& table) const;
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
	// A table deadline (ResultDisputeTimeoutMs, SpectatorStartHoldMs) that
	// started at `since` has passed.
	bool TimerDue(std::uint64_t since, std::uint64_t timeout, std::uint64_t nowMs) const;
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
	// Server-owned rules, in RoomModelServerOwned.cxx. Join's account checks,
	// the departure of the last member, and the checkpoint's account fields.
	RejectReason CheckServerOwnedJoin(const MemberProfile& profile) const;
	void RememberAccount(MemberId member, std::string account);
	void BanAccount(const std::string& account);
	bool BanWouldExceedCap(const std::string& account) const;
	void ForgetAccount(MemberId member);
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
	// Every running table deadline with its timeout, on the owner's monotonic
	// clock. Recovery turns them into ages and back through this one list.
	// Permit timers are not among them.
	template <typename Visit> void ForEachTableTimer(Visit&& visit);
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
	// like lastChatMs_). Presentation only: not checkpointed; a restored
	// room counts from its next tick.
	std::map<MemberId, std::uint64_t> lastActiveMs_;
	void NoteActive(MemberId member);
	std::uint32_t IdleSeconds(MemberId member) const;
	std::map<MemberId, ActionId> lastAcceptedActions_;
	std::set<ConnectionRef> kicked_;
	// Server-owned rooms only. Held here rather than on Member so an account
	// never reaches the snapshot other members see.
	std::map<MemberId, std::string> memberAccounts_;
	std::set<std::string> kickedAccounts_;
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
