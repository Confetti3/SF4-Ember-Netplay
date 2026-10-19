#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TournamentPlay.hxx"

// Watching one bridge-run tournament match (match watch, spec 14.6), as a pure
// state machine the runtime ticks beside TournamentPlay. It asks the bridge
// for the match's room, joins that room as an observer, keeps asking while it
// watches (that keeps the watcher live on the bridge, and picks up a replaced
// room or a revocation), and leaves when told to or when the match is over.
// It only ever watches: it never hosts, publishes, binds, prepares or reports,
// so its outputs are the four below and nothing else. It also acts only on the
// room it joined for the match: a room the player is in for another reason is
// never left, joined over or hidden by it. The runtime turns each Output into
// the real call and feeds every answer back.
namespace sf4e { namespace netplay { namespace tournament {

// How often a watcher asks the bridge, and how long it waits before joining a
// room again after a refused join or a handshake closed `not admitted` (the
// bind that names it has not landed yet).
constexpr std::uint64_t WatchIntervalMs = 10000;
// How soon a failed ask is tried again.
constexpr std::uint64_t WatchRetryMs = 5000;

// The answer to an ask: the match's room to join, or a wait with the
// bridge's reason (no_room, not_bound or full).
struct WatchReply {
	enum class Role { Waiting, Room } role = Role::Waiting;
	std::string roomId, invitation;
	std::string reason;
	std::uint64_t retryAfterMs = 0;
};

// What the watcher asks of the runtime.
struct WatchOutput {
	enum class Kind {
		Watch, Unwatch,  // helper requests: match_watch, match_unwatch
		Join, Leave,     // room commands: join as an observer, leave
	} kind = Kind::Watch;
	std::string invitation;  // Join
};

enum class WatchPhase { Idle, Asking, Waiting, Joining, Watching, Finished, Refused };

// Asking, waiting, joining or in the room: a watch under way. Idle, finished
// and refused ones hold nothing.
inline bool WatchUnderWay(WatchPhase phase) {
	return phase == WatchPhase::Asking || phase == WatchPhase::Waiting || phase == WatchPhase::Joining ||
		phase == WatchPhase::Watching;
}

// Why a watcher stopped being in a room it had joined, said to the player once.
enum class WatchNotice : std::uint8_t { None, Finished, Removed, RoomClosed };

// The runtime's report of how a joined room ended for the watcher.
enum class WatchRoomEnd : std::uint8_t {
	Removed,     // the room sent this game away: a rebind no longer names it
	Closed,      // the room closed
};

class WatchPlay {
public:
	void Start(std::string bridgeId, std::string matchId, std::uint64_t nowMs);
	// Stops watching: tells the bridge, then leaves the room. Safe in any phase.
	void Stop();
	// Gives the watch up for a reason outside it (the helper is gone): Refused
	// with `reason`. Nothing when it is not under way.
	void Abandon(const std::string& reason);
	std::vector<WatchOutput> Tick(std::uint64_t nowMs, const RoomView& room);

	// The answer to a Watch.
	void OnReply(const WatchReply& reply, std::uint64_t nowMs);
	// A Watch failed. `code` is the bridge's details.reason when it gave one,
	// else its code or the helper's.
	void OnFailure(const std::string& code, std::uint64_t nowMs);
	// The room this game had joined ended for it. It asks the bridge at once
	// what to do next.
	void OnRoomEnded(WatchRoomEnd end, std::uint64_t nowMs);

	WatchPhase GetPhase() const { return phase_; }
	bool Active() const { return WatchUnderWay(phase_); }
	const std::string& BridgeId() const { return bridgeId_; }
	const std::string& MatchId() const { return matchId_; }
	// Why the watch was refused or finished: a stable code, empty otherwise.
	const std::string& Reason() const { return reason_; }
	// The bridge has no place for this watcher yet: its organizer's watcher
	// limit is reached.
	bool Full() const { return full_; }
	// The last room-ending notice and a count that moves with each one, so a
	// reader can tell a new one.
	WatchNotice Notice() const { return notice_; }
	std::uint64_t NoticeSequence() const { return noticeSequence_; }

private:
	void End(WatchPhase phase, const std::string& reason, WatchNotice notice);
	void Follow(std::uint64_t nowMs, const RoomView& room, std::vector<WatchOutput>& out);
	void ReleaseRoom(std::uint64_t nowMs, const RoomView& room, std::vector<WatchOutput>& out);
	// The room the player is in, or is opening, is one this watch joined.
	bool HoldsOwnRoom(std::uint64_t nowMs, const RoomView& room) const;
	void Say(WatchNotice notice);

	WatchPhase phase_ = WatchPhase::Idle;
	std::string bridgeId_, matchId_, reason_;
	std::vector<WatchOutput> pending_;
	bool pollInFlight_ = false, polled_ = false, full_ = false;
	// The watcher has been in the match's room at some point.
	bool wasWatching_ = false;
	std::uint64_t nextPollMs_ = 0, openedAtMs_ = 0;
	// The room the bridge names for the match.
	std::string targetRoom_, targetInvitation_;
	bool joinRequested_ = false, leaving_ = false;
	// Every room this watch asked to join or was seen in as the match's: the
	// only rooms it leaves. A room the player opened or joined by hand is not
	// among them.
	std::vector<std::string> ownRooms_;
	// The watch ended: leave the room it joined, once the next tick shows it.
	bool leaveWanted_ = false;
	// A finished match's room is left once its last game's battle has closed
	// by itself, bounded as TournamentPlay's is (FinalBattleHoldMs).
	bool leaveHeld_ = false;
	std::uint64_t leaveHeldUntilMs_ = 0;
	WatchNotice notice_ = WatchNotice::None;
	std::uint64_t noticeSequence_ = 0;
};

} } }
