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

// Answers after which claiming the match again cannot help.
bool Final(const std::string& code) {
	return code == "stale_revision" || code == "not_found" || code == "unsupported_rules" ||
		code == "incompatible_build" || code == "forbidden" || code == "bridge_not_approved";
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
	Fail(Phase::Idle, {});
}

void TournamentPlay::Abandon(const std::string& reason) {
	if (phase_ == Phase::Claiming || phase_ == Phase::Opening || phase_ == Phase::InRoom) Fail(Phase::Failed, reason);
}

void TournamentPlay::Fail(Phase phase, const std::string& reason) {
	const bool active = phase_ == Phase::Claiming || phase_ == Phase::Opening || phase_ == Phase::InRoom;
	if (active && (hostRequested_ || joinRequested_ || !targetRoom_.empty())) pending_.push_back(Make(Output::Kind::Leave));
	// A report still on its way needs the permit the helper would forget.
	if (active && reportsInFlight_) forgetAfterReports_ = true;
	else if (active) pending_.push_back(Make(Output::Kind::Forget));
	phase_ = phase;
	reason_ = reason;
	waitingForOpponent_ = waitingForPermit_ = false;
}

std::vector<Output> TournamentPlay::Tick(std::uint64_t nowMs, const RoomView& room) {
	std::vector<Output> out = std::move(pending_);
	pending_.clear();
	if (phase_ == Phase::Claiming || phase_ == Phase::Opening || phase_ == Phase::InRoom) {
		if (!claimInFlight_ && nowMs >= nextClaimMs_) {
			out.push_back(Make(Output::Kind::Claim));
			claimInFlight_ = true;
		}
		FollowClaim(nowMs, room, out);
		TrackPermits(nowMs, room, out);
	}
	return out;
}

void TournamentPlay::OnReported() {
	if (reportsInFlight_) --reportsInFlight_;
	if (!reportsInFlight_ && forgetAfterReports_) {
		forgetAfterReports_ = false;
		pending_.push_back(Make(Output::Kind::Forget));
	}
}

void TournamentPlay::FollowClaim(std::uint64_t nowMs, const RoomView& room, std::vector<Output>& out) {
	const bool inTarget = room.joined && !targetRoom_.empty() && room.roomId == targetRoom_;
	waitingForOpponent_ = false;
	if (!targetRoom_.empty()) {
		if (inTarget) {
			phase_ = Phase::InRoom;
			joinRequested_ = leaving_ = false;
		} else if (room.joined) {
			// In another room: one this game hosted while the lease moved on.
			if (!leaving_) { out.push_back(Make(Output::Kind::Leave)); leaving_ = true; }
		} else if (!room.opening && (!joinRequested_ || nowMs - openedAtMs_ >= OpenTimeoutMs)) {
			auto join = Make(Output::Kind::Join);
			join.invitation = targetInvitation_;
			out.push_back(join);
			joinRequested_ = true;
			leaving_ = false;
			openedAtMs_ = nowMs;
			phase_ = Phase::Opening;
		}
	} else if (!leaseId_.empty()) {
		if (!room.joined && !room.opening && (!hostRequested_ || nowMs - openedAtMs_ >= OpenTimeoutMs)) {
			out.push_back(Make(Output::Kind::Host));
			hostRequested_ = true;
			openedAtMs_ = nowMs;
			phase_ = Phase::Opening;
		} else if (room.joined && hostRequested_ && !publishInFlight_ && nowMs >= nextPublishMs_ &&
			!room.roomId.empty() && !room.invitation.empty()) {
			// Only a room this game opened for the match is published.
			auto publish = Make(Output::Kind::Publish);
			publish.roomId = room.roomId;
			publish.invitation = room.invitation;
			publish.leaseId = leaseId_;
			publish.fence = fence_;
			out.push_back(publish);
			publishingInvitation_ = room.invitation;
			publishInFlight_ = true;
		}
	} else {
		waitingForOpponent_ = true;
	}
	if (!inTarget) return;
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
	if (generation && table.permits[slot].empty()) {
		const auto held = permits_.find(generation);
		if (held != permits_.end()) {
			// Tell the room until its snapshot shows this seat's permit.
			auto& told = permitToldMs_[generation];
			if (!told || nowMs - told >= PrepareIntervalMs) {
				auto ready = Make(Output::Kind::PermitReady);
				ready.generation = generation;
				ready.permitId = held->second;
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
		out.push_back(cancel);
		reported_.insert(held);
		++reportsInFlight_;
	}
}

void TournamentPlay::OnRoom(Output::Kind request, const ClaimReply& reply, std::uint64_t nowMs) {
	if (request == Output::Kind::Claim) {
		claimInFlight_ = false;
		nextClaimMs_ = nowMs + ClaimIntervalMs;
	} else {
		publishInFlight_ = false;
	}
	if (phase_ != Phase::Claiming && phase_ != Phase::Opening && phase_ != Phase::InRoom) return;
	switch (reply.role) {
	case ClaimReply::Role::Host:
		if (targetRoom_.empty()) { leaseId_ = reply.leaseId; fence_ = reply.fence; }
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
		leaseId_.clear();
		fence_.clear();
		if (targetRoom_ != reply.roomId) {
			targetRoom_ = reply.roomId;
			joinRequested_ = false;
			if (request == Output::Kind::Claim) published_ = false;
		}
		targetInvitation_ = reply.invitation;
		if (reply.binding && reply.binding->roomId == reply.roomId &&
			(!binding_ || binding_->roomId != reply.binding->roomId || binding_->room.SupersededBy(reply.binding->room)))
			binding_ = reply.binding;
		break;
	}
}

void TournamentPlay::OnPrepare(const PrepareReply& reply, std::uint64_t nowMs) {
	prepareInFlight_ = false;
	if (reply.permitted && reply.generation && !reply.permitId.empty()) {
		permits_[reply.generation] = reply.permitId;
		permitToldMs_[reply.generation] = 0;
		return;
	}
	nextPrepareMs_ = nowMs + (std::max)(reply.retryAfterMs, PrepareIntervalMs);
}

void TournamentPlay::OnFailure(Output::Kind request, const std::string& code, std::uint64_t nowMs) {
	switch (request) {
	case Output::Kind::Claim:
		claimInFlight_ = false;
		// A finished match answers stale_revision: it is over, not broken.
		if (Final(code)) Fail(code == "stale_revision" ? Phase::Finished : Phase::Failed, code);
		else nextClaimMs_ = nowMs + RetryMs;
		break;
	case Output::Kind::Publish:
		publishInFlight_ = false;
		nextPublishMs_ = nowMs + RetryMs;
		// The lease moved on: claim again to learn the room to join.
		if (code == "lease_conflict") { leaseId_.clear(); fence_.clear(); nextClaimMs_ = nowMs; }
		break;
	case Output::Kind::Prepare:
		prepareInFlight_ = false;
		nextPrepareMs_ = nowMs + (code == "stale_revision" ? PrepareIntervalMs : RetryMs);
		// A stale binding: a claim brings the current one.
		if (code == "stale_revision") nextClaimMs_ = nowMs;
		break;
	case Output::Kind::Report:
		if (code == "report_not_saved") reason_ = code;
		OnReported();
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
	pending_.push_back(report);
	reported_.insert(generation);
	started_.insert(generation);
	// Counted now, not when sent: a Stop before the next tick must still
	// hold the helper's Forget until this report is answered.
	++reportsInFlight_;
}

} } }
