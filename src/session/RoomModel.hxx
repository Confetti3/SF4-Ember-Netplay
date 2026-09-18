#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

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

constexpr std::uint32_t ProtocolVersion = 1;
constexpr std::size_t MaximumMembers = MaxMembers;
constexpr std::size_t MaximumRoomNameBytes = 64;
constexpr std::size_t MaximumChatMessages = 100;
constexpr std::size_t MaximumChatBytes = 256;

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
    // The selected delay belongs to this fighter until Ready captures it.
    // Values are deliberately bounded by Action deserialization (0..10).
    std::uint8_t selectedDelay = 2;
    std::uint8_t frozenDelay = 2;
    bool delayLocked = false;
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
	bool ready[2] = { false, false };
	// Immutable per-fighter values captured at the Ready quorum boundary.
	std::uint8_t inputDelay[2] = { 2, 2 };
	std::uint32_t score[2] = { 0, 0 };
	bool resultPending = false;
};

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

	Result Join(const std::string& name, const ConnectionRef& connection, bool host = false, int mainFighter = -1);
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
	void SetLocalMember(MemberId member);
	void SetMemberIncarnation(MemberId member, std::uint64_t incarnation);
	void SetRoomEpoch(std::uint64_t epoch);

private:
	Member* Find(MemberId member);
	const Member* Find(MemberId member) const;
	Table* FindTable(std::uint8_t table);
	const Table* FindTable(std::uint8_t table) const;
	Result Reject(RejectReason reason);
	Result Accept(std::vector<Event> events = {});
	void Touch(Table& table);
	void TouchRoom();
	void NormalizeMemberStatus(MemberId member);
	void SeatQueued(Table& table);
	void FillVacancy(Table& table, int seat);
	void RemoveFromTable(MemberId member, bool preserveSpectator = false);
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
