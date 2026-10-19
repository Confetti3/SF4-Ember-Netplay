#include "WatchPlay.hxx"

#include <algorithm>

namespace sf4e { namespace netplay { namespace tournament {

namespace {
WatchOutput Make(WatchOutput::Kind kind) {
	WatchOutput output;
	output.kind = kind;
	return output;
}

// The bridge's word that the match is over (409 stale_revision, reason finished).
bool MatchOver(const std::string& code) { return code == "finished" || code == "stale_revision"; }

// The bridge's reasons for turning this watcher away: refused or revoked for
// this match, one that plays in it, or a service that has no watching.
bool WatcherRefused(const std::string& code) {
	return code == "not_allowed" || code == "revoked" || code == "fighter" || code == "watch_unsupported";
}
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
	if (Active() || room_.Holding()) room_.Release();
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
	if (phase == WatchPhase::Finished) room_.Hold();
	else room_.Release();
	phase_ = phase;
	reason_ = reason;
	full_ = false;
	if (wasWatching_ && notice != WatchNotice::None) Say(notice);
}

std::vector<WatchOutput> WatchPlay::Tick(std::uint64_t nowMs, const RoomView& room) {
	std::vector<WatchOutput> out = std::move(pending_);
	pending_.clear();
	if (room_.Tick(nowMs, room)) out.push_back(Make(WatchOutput::Kind::Leave));
	if (!Active()) return out;
	if (!pollInFlight_ && nowMs >= nextPollMs_) {
		out.push_back(Make(WatchOutput::Kind::Watch));
		pollInFlight_ = polled_ = true;
	}
	Follow(nowMs, room, out);
	return out;
}

void WatchPlay::Follow(std::uint64_t nowMs, const RoomView& room, std::vector<WatchOutput>& out) {
	using Step = MatchRoomFollower::Step;
	switch (room_.Follow(nowMs, room)) {
	case Step::Foreign:
		// The player is in a room this watch did not join: theirs. The watch
		// ends and says why.
		if (polled_) out.push_back(Make(WatchOutput::Kind::Unwatch));
		End(WatchPhase::Refused, "other_room", WatchNotice::None);
		break;
	case Step::InTarget:
		phase_ = WatchPhase::Watching;
		wasWatching_ = true;
		break;
	case Step::Join: {
		auto join = Make(WatchOutput::Kind::Join);
		join.invitation = room_.Invitation();
		out.push_back(join);
		phase_ = WatchPhase::Joining;
		break;
	}
	case Step::Leave:
		// In the room of the match's that the bridge no longer names.
		out.push_back(Make(WatchOutput::Kind::Leave));
		break;
	case Step::None:
		break;
	}
}

void WatchPlay::OnReply(const WatchReply& reply, std::uint64_t nowMs) {
	pollInFlight_ = false;
	if (!Active()) return;
	nextPollMs_ = nowMs + (std::max)(reply.retryAfterMs ? reply.retryAfterMs : WatchIntervalMs, std::uint64_t(1000));
	if (reply.role == WatchReply::Role::Waiting) {
		full_ = reply.reason == "full";
		if (!room_.Aimed()) phase_ = WatchPhase::Waiting;
		return;
	}
	full_ = false;
	room_.Aim(reply.roomId, reply.invitation);
}

void WatchPlay::OnFailure(const std::string& code, std::uint64_t nowMs) {
	pollInFlight_ = false;
	if (!Active()) return;
	if (MatchOver(code)) End(WatchPhase::Finished, code, WatchNotice::Finished);
	else if (BridgeGaveUp(code) || WatcherRefused(code)) End(WatchPhase::Refused, code,
		code == "revoked" || code == "not_allowed" ? WatchNotice::Removed : WatchNotice::None);
	else nextPollMs_ = nowMs + WatchRetryMs;
}

void WatchPlay::OnRoomEnded(WatchRoomEnd end, std::uint64_t nowMs) {
	if (!Active()) return;
	// Whatever the room was, the bridge says what comes next: the match's new
	// room, a wait, or that this watcher is gone.
	room_.Forget();
	nextPollMs_ = nowMs;
	phase_ = WatchPhase::Waiting;
	if (end == WatchRoomEnd::Closed) Say(WatchNotice::RoomClosed);
}

} } }
