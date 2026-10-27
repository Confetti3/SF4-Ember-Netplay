#pragma once
// The room model's value types and their JSON form (RoomModel.hxx has all of
// the room model).

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
#include "../common/SteamId.hxx"
#include "../common/Country.hxx"
#include "Admission.hxx"
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
// A waiting fighter's inactivity limit. room.ready_timeout.you and
// room.ready_timeout.other quote its remaining seconds; the last 30 s are shown.
constexpr std::uint64_t ReadyTimeoutMs = 90000;

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
	// (Action::inTraining) or off. It changes what the others are shown and
	// nothing else. Appended so older authorities reject it as an unknown
	// kind.
	SetTraining,
};
// The last kind, the wire's upper bound: a kind appended after it moves this too.
constexpr ActionKind LastActionKind = ActionKind::SetTraining;

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
	// Server-owned rooms only, from the room's ticket (Admission.hxx): the
	// Ember ID a kick and a duplicate join are keyed on, which is never part of
	// a snapshot, and the player's class, which is.
	Admission admission;
	// The player's Steam profile, only when they chose to show it to the room;
	// none otherwise.
	SteamId steamId;
	// The country the player shows, or none when it is Hidden or unknown.
	CountryCode country;
	// The joiner asks to watch a bound tournament match: a room that does not
	// name its endpoint as a watcher refuses it. A courtesy of a cooperating
	// game, never part of a snapshot.
	bool observer = false;
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
    // The Steam profile this member showed the room when it joined, or none.
    SteamId steamId;
    // The country this member showed when it joined, or none.
    CountryCode country;
    // The class a public room's ticket vouched for (MemberProfile::admission), or none.
    SkillBadge badge;
    // The selected delay belongs to this fighter until Ready captures it.
    // Values are deliberately bounded by Action deserialization
    // (0..InputDelayWireCeiling); the authority records at most MaximumInputDelay.
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
	// Each fighter's Lower input lag at Ready; off until they ready with it.
	bool lowerLag[2] = { false, false };
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
	// Stamped only in SnapshotFor, like holdRemainingMs. Zero means no clock.
	std::uint32_t readyRemainingMs = 0;
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
	// Every game in the room is a Practice match: both fighters' health and
	// gauges fill again, nobody is knocked out, and a game lasts the longest
	// round time. Chosen by the player who created a private room and fixed
	// for its life; never a public or tournament room. Serialized only when true.
	bool practice = false;
	// Whose input delay a game is played at (DelayPolicy). Chosen when the room
	// is created and fixed for its life, public or private. Serialized only
	// when Own, so a Shared room's wire form is unchanged.
	DelayPolicy delayPolicy = DelayPolicy::Shared;
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
	// For this wire recipient, each table's matchGeneration while it is in the
	// native roster frozen when that game began, else 0: who watches the game,
	// which a queued member's status does not say. A host that knows it always
	// sends it (localMatchGenerationsSent); without it the host did not say,
	// which is not the same as in no game.
	bool localMatchGenerationsSent = false;
	std::array<std::uint64_t, TableCount> localMatchGenerations = {};
	std::vector<ChatMessage> chat;
	// The tournament match this room plays at table 0, when it is bound to one.
	TournamentBinding tournament;
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
	// SetTraining: the member is in Training. On the wire it travels as "locked", so peers
	// that predate this field still read it.
	bool inTraining = false;
	MatchResult result = MatchResult::Abort;
	std::uint64_t matchGeneration = 0;
	std::uint8_t inputDelay = 2;
	// Ready: the fighter has Lower input lag on. Written only when set.
	bool lowerLag = false;
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
	// AdvanceTime took `member` out of a seat at `table` for not readying
	// within ReadyTimeoutMs. Every member gets it, for the room chat.
	ReadyTimeout,
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
