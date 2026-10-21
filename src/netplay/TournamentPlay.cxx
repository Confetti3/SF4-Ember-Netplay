#include "TournamentPlay.hxx"

#include <algorithm>

namespace sf4e { namespace netplay { namespace tournament {

const char* ResultName(room::MatchResult result) {
	switch (result) {
	case room::MatchResult::P1Win: return "p1_win";
	case room::MatchResult::P2Win: return "p2_win";
	case room::MatchResult::Draw: return "draw";
	case room::MatchResult::Cancel: return "cancel";
	default: return "abort";
	}
}

namespace {
Output Make(Output::Kind kind) {
	Output output;
	output.kind = kind;
	return output;
}

// Report answers after which handing the same report over again cannot help.
bool ReportRefused(const std::string& code) {
	return code == "unknown_permit" || code == "invalid_request" || code == "identity_unavailable";
}
}

void TournamentPlay::Start(std::string bridgeId, std::string matchId, std::uint64_t nowMs) {
	*this = TournamentPlay();
	bridgeId_ = std::move(bridgeId);
	matchId_ = std::move(matchId);
	phase_ = Phase::Claiming;
	nextClaimMs_ = nowMs;
}

void TournamentPlay::Stop() {
	if (phase_ == Phase::Idle) return;
	// A Leave held for the last game's battle goes now.
	if (room_.Holding()) room_.Release();
	Fail(Phase::Idle, {});
}

void TournamentPlay::Abandon(const std::string& reason) {
	if (PlayUnderWay(phase_)) Fail(Phase::Failed, reason);
}

void TournamentPlay::Fail(Phase phase, const std::string& reason) {
	const bool active = PlayUnderWay(phase_);
	// A finished match's last game may still be on screen: its room is left
	// once that battle has closed. Anything else leaves at once. Either way
	// only a room this match opened is left.
	if (active && phase == Phase::Finished) room_.Hold();
	else if (active) room_.Release();
	// A report the helper has not saved needs the permit it would forget.
	if (active && !unsaved_.empty()) forgetAfterReports_ = true;
	else if (active) pending_.push_back(Make(Output::Kind::Forget));
	phase_ = phase;
	reason_ = reason;
	waitingForOpponent_ = waitingForPermit_ = false;
}

std::vector<Output> TournamentPlay::Tick(std::uint64_t nowMs, const RoomView& room) {
	std::vector<Output> out = std::move(pending_);
	pending_.clear();
	// A report is the game's result: it is handed over again whatever the
	// match does meanwhile.
	ResendReports(nowMs, out);
	if (room_.Tick(nowMs, room)) out.push_back(Make(Output::Kind::Leave));
	if (PlayUnderWay(phase_)) {
		// The bridge tells the tournament who has not pressed Ready, so a
		// change goes out with a claim now rather than the next one.
		const bool ready = SeatReady(room);
		if (ready != claimedReady_ && !claimFailed_)
			nextClaimMs_ = (std::min)(nextClaimMs_, claimedAtMs_ + ReadyClaimSpacingMs);
		if (!claimInFlight_ && nowMs >= nextClaimMs_) {
			auto claim = Make(Output::Kind::Claim);
			claim.ready = ready;
			out.push_back(claim);
			claimInFlight_ = true;
			claimedReady_ = ready;
			claimedAtMs_ = nowMs;
		}
		FollowClaim(nowMs, room, out);
		TrackPermits(nowMs, room, out);
	}
	return out;
}

void TournamentPlay::OnConnectionLost(std::uint64_t generation) {
	lostGeneration_ = generation;
}

void TournamentPlay::SendReport(Output report, std::vector<Output>& out) {
	lastLostConnection_ = report.result == ResultName(room::MatchResult::Abort) && report.generation == lostGeneration_;
	auto& entry = unsaved_[report.generation];
	entry.report = report;
	entry.attempts = 1;
	entry.inFlight = true;
	out.push_back(std::move(report));
}

void TournamentPlay::ResendReports(std::uint64_t nowMs, std::vector<Output>& out) {
	for (auto& [generation, entry] : unsaved_) {
		if (entry.inFlight || nowMs < entry.retryAtMs) continue;
		++entry.attempts;
		entry.inFlight = true;
		out.push_back(entry.report);
	}
}

void TournamentPlay::OnReported(std::uint64_t generation) {
	unsaved_.erase(generation);
	ReportsSettled();
}

void TournamentPlay::OnReportFailed(std::uint64_t generation, const std::string& code, std::uint64_t nowMs) {
	const auto entry = unsaved_.find(generation);
	if (entry == unsaved_.end()) return;
	// The helper may be busy for a moment (another identity task, a full
	// queue): the same report goes again shortly.
	if (!ReportRefused(code) && entry->second.attempts < MaxReportAttempts) {
		entry->second.inFlight = false;
		entry->second.retryAtMs = nowMs + RetryMs;
		return;
	}
	unsaved_.erase(entry);
	// A result that could not be saved stops the match here (spec 16.7):
	// no further official game starts until the organizer has reconciled
	// it, and the player sees why. The other fighter's report sends the
	// game to review on the bridge.
	if (PlayUnderWay(phase_)) Fail(Phase::Failed, "report_not_saved");
	ReportsSettled();
}

void TournamentPlay::ReportsSettled() {
	if (unsaved_.empty() && forgetAfterReports_) {
		forgetAfterReports_ = false;
		pending_.push_back(Make(Output::Kind::Forget));
	}
}

bool TournamentPlay::SeatReady(const RoomView& room) const {
	if (!room_.InTarget(room) || !room.snapshot || !room.snapshot->tournament.Active()) return false;
	const auto& snapshot = *room.snapshot;
	const auto place = room::PlaceOf(snapshot, snapshot.localMember);
	return place.kind == room::Place::Kind::Seat && place.table == room::TournamentTable &&
		snapshot.tables[room::TournamentTable].ready[place.seat];
}

void TournamentPlay::FollowClaim(std::uint64_t nowMs, const RoomView& room, std::vector<Output>& out) {
	using Step = MatchRoomFollower::Step;
	waitingForOpponent_ = false;
	// The named room could not be joined twice running: its host left or its
	// game closed. This game hosts a new room and publishes it in that one's
	// place, which the bridge refuses while a game of the match is undecided;
	// none is tried while this game holds a permit or a report not yet settled.
	// Decided before Follow, so a tick never sends a join and a host at once.
	if (room_.Aimed() && room_.FailedJoins() >= ReplaceAfterFailedJoins && !room.joined && !room.opening && AttemptsSettled()) {
		replacing_ = room_.Target();
		retiredRooms_.insert(replacing_);
		room_.Forget();
		published_ = false;
	}
	const auto step = room_.Follow(nowMs, room);
	switch (step) {
	case Step::Foreign:
		// The player is in a room of their own: it is not left, joined over or
		// published. The match goes on claiming and opens its room once they leave.
		return;
	case Step::InTarget:
		phase_ = Phase::InRoom;
		break;
	case Step::Join: {
		auto join = Make(Output::Kind::Join);
		join.invitation = room_.Invitation();
		out.push_back(join);
		phase_ = Phase::Opening;
		break;
	}
	case Step::Leave:
		// In another room: one this game hosted while the lease moved on.
		out.push_back(Make(Output::Kind::Leave));
		break;
	case Step::None:
		break;
	}
	// No room named yet: the fighter holding the lease, or replacing a room,
	// hosts and publishes one; the other waits for it.
	if (!room_.Aimed() && leaseId_.empty() && replacing_.empty()) {
		waitingForOpponent_ = true;
	} else if (!room_.Aimed()) {
		if (room_.Host(nowMs, room)) {
			out.push_back(Make(Output::Kind::Host));
			phase_ = Phase::Opening;
		} else if (room.joined && !publishInFlight_ && nowMs >= nextPublishMs_ &&
			!room.roomId.empty() && !room.invitation.empty()) {
			// The room this game hosted for the match (a room of the player's own is Foreign).
			auto publish = Make(Output::Kind::Publish);
			publish.roomId = room.roomId;
			publish.invitation = room.invitation;
			publish.leaseId = leaseId_;
			publish.fence = fence_;
			publish.replaces = replacing_;
			out.push_back(publish);
			publishingInvitation_ = room.invitation;
			publishInFlight_ = true;
		}
	}
	if (step != Step::InTarget) return;
	// The room renews its invitation as it ages; the bridge keeps the latest.
	if (published_ && !room.invitation.empty() && room.invitation != publishedInvitation_ &&
		!publishInFlight_ && nowMs >= nextPublishMs_) {
		auto refresh = Make(Output::Kind::Publish);
		refresh.roomId = room.roomId;
		refresh.invitation = room.invitation;
		out.push_back(refresh);
		publishingInvitation_ = room.invitation;
		publishInFlight_ = true;
	}
	if (!binding_) { waitingForOpponent_ = true; return; }
	// Only the game that applies the room's changes binds it, from its own
	// helper's checked copy; every fighter keeps one for when it leads.
	if (binding_->roomId != room.roomId || !room.authorityWritable || !room.snapshot) return;
	const auto& wanted = binding_->room;
	const auto& current = room.snapshot->tournament;
	const bool applied = current.matchId == wanted.matchId && current.assignmentGeneration == wanted.assignmentGeneration &&
		current.bindingRevision == wanted.bindingRevision;
	const bool recent = offeredGeneration_ == wanted.assignmentGeneration &&
		offeredRevision_ == wanted.bindingRevision && nowMs - boundAtMs_ < 1000;
	if (applied || recent || (current.Active() && !current.SupersededBy(wanted))) return;
	// A casual game under way at the table holds the first binding back
	// (RoomAuthority::BindTournament), so it is not offered until that ends.
	if (!current.Active() && room::GameUnderWay(room.snapshot->tables[room::TournamentTable])) return;
	auto bind = Make(Output::Kind::Bind);
	bind.binding = wanted;
	out.push_back(bind);
	offeredGeneration_ = wanted.assignmentGeneration;
	offeredRevision_ = wanted.bindingRevision;
	boundAtMs_ = nowMs;
}

void TournamentPlay::TrackPermits(std::uint64_t nowMs, const RoomView& room, std::vector<Output>& out) {
	waitingForPermit_ = false;
	if (!binding_ || !room.snapshot || binding_->localSlot < 0 || binding_->localSlot > 1 ||
		room.roomId != binding_->roomId || !room.snapshot->tournament.Active()) return;
	const auto& table = room.snapshot->tables[room::TournamentTable];
	const auto slot = static_cast<std::size_t>(binding_->localSlot);
	if (table.phase == room::TablePhase::Playing && table.matchGeneration) started_.insert(table.matchGeneration);
	const auto generation = table.permitGeneration;
	if (generation) waitingForPermit_ = room::PermitPending(table);
	// No further game is prepared while a result is unsaved.
	if (generation && table.permits[slot].empty() && unsaved_.empty()) {
		const auto held = permits_.find(generation);
		if (held != permits_.end()) {
			// Tell the room until its snapshot shows this seat's permit.
			auto& told = permitToldMs_[generation];
			if (!told || nowMs - told >= PrepareIntervalMs) {
				auto ready = Make(Output::Kind::PermitReady);
				ready.generation = generation;
				ready.permitId = held->second.id;
				ready.startWindowMs = held->second.startWindowMs;
				out.push_back(ready);
				told = nowMs;
			}
		} else if (!prepareInFlight_ && nowMs >= nextPrepareMs_) {
			auto prepare = Make(Output::Kind::Prepare);
			prepare.generation = generation;
			out.push_back(prepare);
			prepareInFlight_ = true;
		}
	}
	// A permit for a start that was called off before the game began: report
	// it cancelled, so the bridge closes that game instead of waiting on it.
	for (const auto& [held, permit] : permits_) {
		if (reported_.count(held) || started_.count(held) || held == table.permitGeneration ||
			held == table.matchGeneration) continue;
		auto cancel = Make(Output::Kind::Report);
		cancel.generation = held;
		cancel.result = ResultName(room::MatchResult::Cancel);
		reported_.insert(held);
		SendReport(std::move(cancel), out);
	}
}

void TournamentPlay::OnRoom(Output::Kind request, const ClaimReply& reply, std::uint64_t nowMs) {
	if (request == Output::Kind::Claim) {
		claimInFlight_ = false;
		claimFailed_ = false;
		nextClaimMs_ = nowMs + ClaimIntervalMs;
	} else {
		publishInFlight_ = false;
	}
	if (!PlayUnderWay(phase_)) return;
	switch (reply.role) {
	case ClaimReply::Role::Host:
		if (!room_.Aimed()) { leaseId_ = reply.leaseId; fence_ = reply.fence; }
		break;
	case ClaimReply::Role::Wait:
		// The other fighter holds the lease now; a room opened late is not the match's.
		leaseId_.clear();
		fence_.clear();
		if (request == Output::Kind::Claim)
			nextClaimMs_ = nowMs + (std::max)(reply.retryAfterMs, std::uint64_t(1000));
		break;
	case ClaimReply::Role::Room:
		// The answer to this game's own publish: its room is the match's room.
		if (request == Output::Kind::Publish) {
			published_ = true;
			publishedInvitation_ = publishingInvitation_;
		}
		// A room the match moved away from (replaced, or being replaced) never
		// comes back: an answer naming it was sent before the move.
		if (retiredRooms_.count(reply.roomId)) break;
		const std::string previous = room_.Target();
		const bool hadRoom = room_.Aimed() || !replacing_.empty() || binding_.has_value();
		const bool moving = hadRoom && reply.roomId != previous;
		// A move waits for the last room's reports to be saved, under the
		// permits they were played with; the next claim brings the room again.
		if (moving && !unsaved_.empty()) break;
		replacing_.clear();
		leaseId_.clear();
		fence_.clear();
		if (room_.Aim(reply.roomId, reply.invitation) && request == Output::Kind::Claim) published_ = false;
		// A room after another (one replaced, by either fighter) numbers its
		// games from 1 again: the last room's games are retired, so the new
		// room's game 1 asks for a permit and reports of its own.
		if (moving) {
			if (!previous.empty()) retiredRooms_.insert(previous);
			RetireRoomAttempts();
		}
		if (reply.binding && reply.binding->roomId == reply.roomId &&
			(!binding_ || binding_->roomId != reply.binding->roomId || binding_->room.SupersededBy(reply.binding->room)))
			binding_ = reply.binding;
		break;
	}
}

void TournamentPlay::RetireRoomAttempts() {
	// Reports of the last room are all saved by now (OnRoom waits for them).
	permits_.clear();
	permitToldMs_.clear();
	started_.clear();
	reported_.clear();
	// A prepare sent for the last room answers for it: that answer is dropped.
	dropPrepare_ = prepareInFlight_;
}

bool TournamentPlay::AttemptsSettled() const {
	return unsaved_.empty() && !prepareInFlight_ &&
		std::all_of(permits_.begin(), permits_.end(), [&](const auto& held) { return reported_.count(held.first) != 0; });
}

void TournamentPlay::OnPrepare(const PrepareReply& reply, std::uint64_t nowMs) {
	prepareInFlight_ = false;
	if (dropPrepare_) { dropPrepare_ = false; return; }
	if (reply.permitted && reply.generation && !reply.permitId.empty()) {
		permits_[reply.generation] = {reply.permitId, reply.startWindowMs};
		permitToldMs_[reply.generation] = 0;
		return;
	}
	nextPrepareMs_ = nowMs + (std::max)(reply.retryAfterMs, PrepareIntervalMs);
}

void TournamentPlay::OnFailure(Output::Kind request, const std::string& code, std::uint64_t nowMs) {
	switch (request) {
	case Output::Kind::Claim:
		claimInFlight_ = false;
		claimFailed_ = true;
		// A finished match answers stale_revision: it is over, not broken. One
		// whose last game was aborted for a lost connection may be set up
		// again by the bridge as a new match.
		if (code == "stale_revision") Fail(Phase::Finished, lastLostConnection_ ? ConnectionLostReason : code);
		else if (BridgeGaveUp(code)) Fail(Phase::Failed, code);
		else nextClaimMs_ = nowMs + RetryMs;
		break;
	case Output::Kind::Publish:
		publishInFlight_ = false;
		nextPublishMs_ = nowMs + RetryMs;
		// The lease moved on, or the room could not be replaced (the other
		// fighter replaced it first, or a game is undecided): claim again to
		// learn the room to join.
		if (code == "lease_conflict") {
			leaseId_.clear();
			fence_.clear();
			// A room that could not be replaced is still the match's.
			if (!replacing_.empty()) retiredRooms_.erase(replacing_);
			replacing_.clear();
			nextClaimMs_ = nowMs;
		}
		break;
	case Output::Kind::Prepare:
		prepareInFlight_ = false;
		dropPrepare_ = false;
		nextPrepareMs_ = nowMs + (code == "stale_revision" ? PrepareIntervalMs : RetryMs);
		// A stale binding: a claim brings the current one.
		if (code == "stale_revision") nextClaimMs_ = nowMs;
		break;
	default:
		break;
	}
}

void TournamentPlay::OnTerminal(std::uint64_t generation, room::MatchResult result, std::uint64_t captureFrame, std::uint64_t confirmedFrame) {
	if (!permits_.count(generation) || reported_.count(generation)) return;
	auto report = Make(Output::Kind::Report);
	report.generation = generation;
	report.result = ResultName(result);
	const bool native = result == room::MatchResult::P1Win || result == room::MatchResult::P2Win || result == room::MatchResult::Draw;
	report.captureFrame = native ? captureFrame : 0;
	report.confirmedFrame = native ? confirmedFrame : 0;
	reported_.insert(generation);
	started_.insert(generation);
	// Kept now, not when sent: a Stop before the next tick must still hold
	// the helper's Forget until this report is saved.
	SendReport(std::move(report), pending_);
}

} } }
