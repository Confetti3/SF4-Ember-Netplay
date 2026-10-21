#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../session/RoomModel.hxx"

// Following the room the bridge names for a tournament match, shared by
// playing a match (TournamentPlay) and watching one (WatchPlay): when to join
// the named room, when to leave an old one, and when to leave once the match
// is done. It acts only on rooms it opened itself. A room the player opened or
// joined by hand is never left, joined over or taken for the match's: the
// machine is told it is there (Step::Foreign) and decides what that means.
namespace sf4e { namespace netplay { namespace tournament {

// How long a finished match's room waits for its last game's battle to close
// by itself before it is left all the same.
constexpr std::uint64_t FinalBattleHoldMs = 60000;

// Answers after which asking the bridge about the match again cannot help: it
// is over (stale_revision), gone, or not one this game can take part in.
inline bool BridgeGaveUp(const std::string& code) {
	return code == "stale_revision" || code == "not_found" || code == "unsupported_rules" ||
		code == "incompatible_build" || code == "forbidden" || code == "bridge_not_approved";
}

// The room as the runtime sees it this tick.
struct RoomView {
	// In an open room. `opening` while a host or join is under way.
	bool joined = false;
	bool opening = false;
	// 32 lowercase hex digits, and the invitation another player joins with.
	std::string roomId;
	std::string invitation;
	// This game applies the room's changes (the coordination leader).
	bool authorityWritable = false;
	const room::Snapshot* snapshot = nullptr;
	// A game is being prepared or played on this PC (netplay::Fighting).
	bool fighting = false;
};

class MatchRoomFollower {
public:
	// `retryMs`: how long a join or host may take before it is tried again, and
	// how long an open still opening counts as this follower's.
	explicit MatchRoomFollower(std::uint64_t retryMs) : retryMs_(retryMs) {}

	// The room the bridge names. True when it is another than before: that one
	// is joined afresh.
	bool Aim(const std::string& roomId, const std::string& invitation) {
		const bool changed = target_ != roomId;
		if (changed) { target_ = roomId; requested_ = false; failedJoins_ = 0; }
		invitation_ = invitation;
		return changed;
	}
	// The bridge names no room for now (the one joined ended): it says the next.
	void Forget() {
		target_.clear();
		invitation_.clear();
		requested_ = leaving_ = openUnderWay_ = false;
		failedJoins_ = 0;
	}
	bool Aimed() const { return !target_.empty(); }
	const std::string& Target() const { return target_; }
	// Joins of the named room that failed in a row: its host may be gone.
	unsigned FailedJoins() const { return failedJoins_; }
	const std::string& Invitation() const { return invitation_; }
	bool InTarget(const RoomView& room) const { return room.joined && Aimed() && room.roomId == target_; }

	// First on every tick: notes the room its own host landed in, and says
	// whether a Leave is due now (after Release, or Hold once the battle closed).
	bool Tick(std::uint64_t nowMs, const RoomView& room) {
		// A join names its room before it is sent; a host learns it here.
		if (room.joined && openUnderWay_ && hosting_) Own(room.roomId);
		// The open has landed or failed once nothing is opening; a join that
		// left the player in no room failed.
		if (!room.opening) {
			if (openUnderWay_ && !hosting_ && !room.joined) ++failedJoins_;
			openUnderWay_ = false;
		}
		if (wanted_) {
			wanted_ = false;
			return Holds(nowMs, room);
		}
		if (!held_) return false;
		if (!heldUntilMs_) heldUntilMs_ = nowMs + FinalBattleHoldMs;
		// A battle that never closes must not keep the player in the room.
		if (room.fighting && nowMs < heldUntilMs_) return false;
		held_ = false;
		return Holds(nowMs, room);
	}

	enum class Step { None, Foreign, InTarget, Join, Leave };
	// What following the named room asks this tick. Join and Leave are taken
	// as sent; Foreign is the player's own room, which it never acts on.
	Step Follow(std::uint64_t nowMs, const RoomView& room) {
		if (room.joined && !Holds(nowMs, room)) return Step::Foreign;
		if (!Aimed()) return Step::None;
		if (InTarget(room)) {
			requested_ = leaving_ = false;
			failedJoins_ = 0;
			return Step::InTarget;
		}
		if (room.joined) {
			// In a room of its own the bridge no longer names: left once.
			if (leaving_) return Step::None;
			leaving_ = true;
			return Step::Leave;
		}
		if (!OpenDue(nowMs, room)) return Step::None;
		Opened(nowMs, false);
		leaving_ = false;
		// Whatever this join lands in is the match's room, and this follower's to leave.
		Own(target_);
		return Step::Join;
	}
	// With no room named: true when a room is to be hosted now, taken as sent.
	bool Host(std::uint64_t nowMs, const RoomView& room) {
		if (room.joined || !OpenDue(nowMs, room)) return false;
		Opened(nowMs, true);
		return true;
	}

	// Done with the room: it is left on the next tick if it is this follower's.
	void Release() { wanted_ = true; held_ = false; }
	// Done with a finished match's room: it is left once its last game's battle
	// has closed by itself, so the result stays on screen, or after
	// FinalBattleHoldMs.
	void Hold() { held_ = true; heldUntilMs_ = 0; }
	bool Holding() const { return held_; }

private:
	// The room the player is in, or is opening, is one this follower opened.
	bool Holds(std::uint64_t nowMs, const RoomView& room) const {
		if (room.joined) return std::find(ownRooms_.begin(), ownRooms_.end(), room.roomId) != ownRooms_.end();
		// An open that failed is not under way: the room that opens next is someone else's.
		return room.opening && openUnderWay_ && nowMs - openedAtMs_ < retryMs_;
	}
	bool OpenDue(std::uint64_t nowMs, const RoomView& room) const {
		return !room.opening && (!requested_ || nowMs - openedAtMs_ >= retryMs_);
	}
	void Opened(std::uint64_t nowMs, bool hosting) {
		requested_ = openUnderWay_ = true;
		hosting_ = hosting;
		openedAtMs_ = nowMs;
	}
	void Own(const std::string& roomId) {
		if (std::find(ownRooms_.begin(), ownRooms_.end(), roomId) == ownRooms_.end()) ownRooms_.push_back(roomId);
	}

	std::uint64_t retryMs_;
	// The room the bridge names for the match, and its invitation.
	std::string target_, invitation_;
	// A join or host (hosting_) was sent at openedAtMs_ (paces the retry); one is
	// under way until the first tick that shows nothing opening, so a failed
	// open's successor is never taken for it; a Leave of a room no longer named is out.
	bool requested_ = false, openUnderWay_ = false, hosting_ = false, leaving_ = false;
	std::uint64_t openedAtMs_ = 0;
	unsigned failedJoins_ = 0;
	// Every room its joins named or its hosts landed in: the only rooms it leaves.
	std::vector<std::string> ownRooms_;
	// A Leave is wanted on the next tick, or held for the last battle.
	bool wanted_ = false, held_ = false;
	std::uint64_t heldUntilMs_ = 0;
};

} } }
