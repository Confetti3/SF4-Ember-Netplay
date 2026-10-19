#include "WatchPlay.hxx"

#include <algorithm>

#include "TournamentPlay.hxx"

namespace sf4e { namespace netplay { namespace tournament {

namespace {
WatchOutput Make(WatchOutput::Kind kind) {
	WatchOutput output;
	output.kind = kind;
	return output;
}

// Answers after which asking again cannot help: the bridge's reasons (a
// watcher refused for this match, one that plays in it, one on another
// build), and the codes of a service or match that cannot be watched.
bool Refusal(const std::string& code) {
	return code == "not_allowed" || code == "revoked" || code == "fighter" || code == "incompatible_build" ||
		code == "watch_unsupported" || code == "not_found" || code == "unsupported_rules" || code == "forbidden" ||
		code == "bridge_not_approved";
}

// The bridge's word that the match is over (409 stale_revision, reason finished).
bool MatchOver(const std::string& code) { return code == "finished" || code == "stale_revision"; }
}

void WatchPlay::Start(std::string bridgeId, std::string matchId, std::uint64_t nowMs) {
	*this = WatchPlay();
	bridgeId_ = std::move(bridgeId);
	matchId_ = std::move(matchId);
	phase_ = WatchPhase::Asking;
	nextPollMs_ = nowMs;
}

void WatchPlay::Stop() {
	if (phase_ == WatchPhase::Idle) return;
	// The bridge forgets a watcher that said it was leaving; one it already
	// refused or finished has nothing to forget.
	if (Active() && polled_) pending_.push_back(Make(WatchOutput::Kind::Unwatch));
	// Whether there is a room of this watch's to leave is the next tick's to say.
	if (leaveHeld_ || Active()) leaveWanted_ = true;
	leaveHeld_ = false;
	phase_ = WatchPhase::Idle;
	reason_.clear();
	full_ = false;
}

void WatchPlay::Abandon(const std::string& reason) {
	if (Active()) End(WatchPhase::Refused, reason, WatchNotice::None);
}

void WatchPlay::Say(WatchNotice notice) {
	notice_ = notice;
	++noticeSequence_;
}

void WatchPlay::End(WatchPhase phase, const std::string& reason, WatchNotice notice) {
	// A finished match's last game may still be on screen: its room is left
	// once that battle has closed. Any other end leaves at once. The notice is
	// for a watcher that had been in the room; the phase line says the rest.
	if (phase == WatchPhase::Finished) leaveHeld_ = true;
	else leaveWanted_ = true;
	phase_ = phase;
	reason_ = reason;
	full_ = false;
	if (wasWatching_ && notice != WatchNotice::None) Say(notice);
}

bool WatchPlay::HoldsOwnRoom(std::uint64_t nowMs, const RoomView& room) const {
	if (room.joined) return std::find(ownRooms_.begin(), ownRooms_.end(), room.roomId) != ownRooms_.end();
	// Opening: this watch's join, while it can still be under way.
	return room.opening && joinRequested_ && nowMs - openedAtMs_ < WatchIntervalMs;
}

// Leaves the room this watch joined, when it should, and never any other: the
// player may be in a room of their own.
void WatchPlay::ReleaseRoom(std::uint64_t nowMs, const RoomView& room, std::vector<WatchOutput>& out) {
	if (leaveWanted_) {
		leaveWanted_ = false;
		if (HoldsOwnRoom(nowMs, room)) out.push_back(Make(WatchOutput::Kind::Leave));
	}
	if (!leaveHeld_) return;
	if (!leaveHeldUntilMs_) leaveHeldUntilMs_ = nowMs + FinalBattleHoldMs;
	// A battle that never closes must not keep the watcher in the room.
	if (room.fighting && nowMs < leaveHeldUntilMs_) return;
	if (HoldsOwnRoom(nowMs, room)) out.push_back(Make(WatchOutput::Kind::Leave));
	leaveHeld_ = false;
}

std::vector<WatchOutput> WatchPlay::Tick(std::uint64_t nowMs, const RoomView& room) {
	std::vector<WatchOutput> out = std::move(pending_);
	pending_.clear();
	ReleaseRoom(nowMs, room, out);
	if (!Active()) return out;
	if (!pollInFlight_ && nowMs >= nextPollMs_) {
		out.push_back(Make(WatchOutput::Kind::Watch));
		pollInFlight_ = polled_ = true;
	}
	Follow(nowMs, room, out);
	return out;
}

void WatchPlay::Follow(std::uint64_t nowMs, const RoomView& room, std::vector<WatchOutput>& out) {
	// The player is in a room this watch did not join: theirs. It is not left,
	// joined over or taken for the match's; the watch ends and says why.
	if (room.joined && !HoldsOwnRoom(nowMs, room)) {
		if (polled_) out.push_back(Make(WatchOutput::Kind::Unwatch));
		End(WatchPhase::Refused, "other_room", WatchNotice::None);
		return;
	}
	if (targetRoom_.empty()) return;
	if (room.joined && room.roomId == targetRoom_) {
		phase_ = WatchPhase::Watching;
		wasWatching_ = true;
		joinRequested_ = leaving_ = false;
	} else if (room.joined) {
		// In the room of the match's that the bridge no longer names.
		if (!leaving_) { out.push_back(Make(WatchOutput::Kind::Leave)); leaving_ = true; }
	} else if (!room.opening && (!joinRequested_ || nowMs - openedAtMs_ >= WatchIntervalMs)) {
		auto join = Make(WatchOutput::Kind::Join);
		join.invitation = targetInvitation_;
		out.push_back(join);
		joinRequested_ = true;
		leaving_ = false;
		openedAtMs_ = nowMs;
		phase_ = WatchPhase::Joining;
		// Whatever this join lands in is the match's room, and the watch's to leave.
		if (std::find(ownRooms_.begin(), ownRooms_.end(), targetRoom_) == ownRooms_.end()) ownRooms_.push_back(targetRoom_);
	}
}

void WatchPlay::OnReply(const WatchReply& reply, std::uint64_t nowMs) {
	pollInFlight_ = false;
	if (!Active()) return;
	nextPollMs_ = nowMs + (std::max)(reply.retryAfterMs ? reply.retryAfterMs : WatchIntervalMs, std::uint64_t(1000));
	if (reply.role == WatchReply::Role::Waiting) {
		full_ = reply.reason == "full";
		if (targetRoom_.empty()) phase_ = WatchPhase::Waiting;
		return;
	}
	full_ = false;
	if (targetRoom_ != reply.roomId) {
		targetRoom_ = reply.roomId;
		joinRequested_ = false;
	}
	targetInvitation_ = reply.invitation;
}

void WatchPlay::OnFailure(const std::string& code, std::uint64_t nowMs) {
	pollInFlight_ = false;
	if (!Active()) return;
	if (MatchOver(code)) End(WatchPhase::Finished, code, WatchNotice::Finished);
	else if (Refusal(code)) End(WatchPhase::Refused, code,
		code == "revoked" || code == "not_allowed" ? WatchNotice::Removed : WatchNotice::None);
	else nextPollMs_ = nowMs + WatchRetryMs;
}

void WatchPlay::OnRoomEnded(WatchRoomEnd end, std::uint64_t nowMs) {
	if (!Active()) return;
	// Whatever the room was, the bridge says what comes next: the match's new
	// room, a wait, or that this watcher is gone.
	targetRoom_.clear();
	targetInvitation_.clear();
	joinRequested_ = leaving_ = false;
	nextPollMs_ = nowMs;
	phase_ = WatchPhase::Waiting;
	if (end == WatchRoomEnd::Closed) Say(WatchNotice::RoomClosed);
}

} } }
