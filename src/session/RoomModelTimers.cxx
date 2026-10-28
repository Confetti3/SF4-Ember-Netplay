// Room model timers: activity, recovery pause and resume, the ready, start
// hold and result deadlines, and AdvanceTime.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"
#include <limits>

namespace sf4e { namespace room {
using namespace detail;

void RoomAuthority::NoteActive(MemberId member) {
	// Only a member still in the room: an accepted Leave must not leave one
	// behind, so the map stays bounded by the roster.
	if (!Find(member)) return;
	lastActiveMs_[member] = TimerStart();
	for (std::size_t i = 0; i < TableCount; ++i)
		if (readyWaitingFor_[i] == member) readyWaitingSince_[i] = TimerStart();
}

std::uint32_t RoomAuthority::IdleSeconds(MemberId member) const {
	const auto found = lastActiveMs_.find(member);
	if (found == lastActiveMs_.end()) return 0;
	const std::uint64_t ms = TimerAge(found->second);
	return static_cast<std::uint32_t>((std::min)(ms / 1000, static_cast<std::uint64_t>(MaximumIdleSeconds)));
}

template <typename Visit> void RoomAuthority::ForEachTimer(Visit&& visit) {
	for (std::size_t i = 0; i < TableCount; ++i) {
		if (snapshot_.tables[i].resultPending) visit(resultPendingSince_[i], ResultDisputeTimeoutMs);
		if (snapshot_.tables[i].spectatorHold) visit(startHeldSince_[i], SpectatorStartHoldMs);
		if (readyWaitingFor_[i]) visit(readyWaitingSince_[i], ReadyTimeoutMs);
	}
	for (auto& entry : lastChatMs_) visit(entry.second, 1000);
	for (auto& entry : lastActiveMs_) visit(entry.second, MaximumIdleSeconds * 1000ull);
}

void RoomAuthority::PauseForRecovery() {
    if (recoveryPaused_) return;
    // Still running here: each start becomes its age.
    ForEachTimer([&](std::uint64_t& held, std::uint64_t) { held = TimerAge(held); });
    recoveryPaused_ = true;
}

void RoomAuthority::AdvancePausedTimers(std::uint64_t elapsedMs) {
	if (!recoveryPaused_ || !elapsedMs) return;
	ForEachTimer([&](std::uint64_t& age, std::uint64_t cap) { age = AddCapped(age, elapsedMs, cap); });
}

void RoomAuthority::ResumeRecovery(std::uint64_t nowMs) {
	// Permit timers run on the owner's own clock, not the rebased one below.
	AgePermitHolds(nowMs);
    if (!recoveryPaused_) {
        if (nowMs >= nowMs_) nowMs_ = nowMs;
        return;
    }
	// The clock resumes no earlier than the oldest age, so every age becomes
	// a start on it.
	std::uint64_t rebasedNow = nowMs;
	ForEachTimer([&](std::uint64_t& age, std::uint64_t) { rebasedNow = (std::max)(rebasedNow, age); });
	ForEachTimer([&](std::uint64_t& age, std::uint64_t) { age = rebasedNow - age; });
	nowMs_ = rebasedNow;
    recoveryPaused_ = false;
}

MemberId RoomAuthority::WaitingForReady(const Table& table) const {
	if (snapshot_.closed || BoundTable(table) || table.phase != TablePhase::Waiting ||
		!table.p1 || !table.p2 || table.ready[0] == table.ready[1] || HasOutstandingTerminalReceipt(table.id)) return 0;
	const auto member = table.ready[0] ? table.p2 : table.p1;
	return HasOutstandingTerminalReceiptForMember(member) ? 0 : member;
}

void RoomAuthority::RefreshReadyTimers() {
	for (const auto& table : snapshot_.tables) {
		const auto member = WaitingForReady(table);
		if (readyWaitingFor_[table.id] == member) continue;
		readyWaitingFor_[table.id] = member;
		// Eligibility starts at the opponent's Ready (or when a receipt fence
		// lifts). Later NoteActive calls restart this same deadline.
		readyWaitingSince_[table.id] = member ? TimerStart() : 0;
	}
}

bool RoomAuthority::HasDueTimerTransition(std::uint64_t nowMs) const {
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto& table = snapshot_.tables[i];
		if (readyWaitingFor_[i] && WaitingForReady(table) == readyWaitingFor_[i] &&
			TimerDue(readyWaitingSince_[i], ReadyTimeoutMs, nowMs)) return true;
		if (table.spectatorHold && TimerDue(startHeldSince_[i], SpectatorStartHoldMs, nowMs)) return true;
		// AdvanceTime(nowMs) brings the permit to this same age first.
		if (PermitCalledOff(table, PermitAgeAt(i, nowMs))) return true;
		if (table.resultPending && table.phase == TablePhase::Playing &&
			TimerDue(resultPendingSince_[i], ResultDisputeTimeoutMs, nowMs)) return true;
	}
	return false;
}

std::uint64_t RoomAuthority::NextDueMs(std::uint64_t nowMs) const {
	auto next = (std::numeric_limits<std::uint64_t>::max)();
	// Time left on a table deadline that started at `since`, read on the clock
	// TimerDue reads, so a deadline is zero here exactly when it is due there.
	const auto left = [&](std::uint64_t since, std::uint64_t timeout) {
		const auto age = TimerAgeAt(since, nowMs);
		next = (std::min)(next, age >= timeout ? std::uint64_t(0) : timeout - age);
	};
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto& table = snapshot_.tables[i];
		// The same four conditions as HasDueTimerTransition, in the same order.
		if (readyWaitingFor_[i] && WaitingForReady(table) == readyWaitingFor_[i]) left(readyWaitingSince_[i], ReadyTimeoutMs);
		if (table.spectatorHold) left(startHeldSince_[i], SpectatorStartHoldMs);
		if (table.permitGeneration) {
			const auto age = PermitAgeAt(i, nowMs);
			if (PermitCalledOff(table, age)) return 0;
			// Not called off: the age is under both limits, which are the hold
			// while the permit is pending and the window less its margin once
			// a permit is held (PermitWindow is zero with none).
			if (PermitPending(table)) next = (std::min)(next, PermitHoldMs - age);
			const auto window = PermitWindow(table);
			if (window > PermitStartMarginMs) next = (std::min)(next, window - PermitStartMarginMs - age);
		}
		if (table.resultPending && table.phase == TablePhase::Playing) left(resultPendingSince_[i], ResultDisputeTimeoutMs);
	}
	return next;
}

bool RoomAuthority::TimerDue(std::uint64_t since, std::uint64_t timeout, std::uint64_t nowMs) const {
	return TimerAgeAt(since, nowMs) >= timeout;
}

std::vector<Event> RoomAuthority::AdvanceTime(std::uint64_t nowMs) {
	// The permit timers take the owner's clock as given, before the max below.
	AgePermitHolds(nowMs);
	if (recoveryPaused_) {
		ResumeRecovery(nowMs);
	}
	nowMs = (std::max)(nowMs, nowMs_);
	nowMs_ = nowMs;
	// A fighter in a game is busy, not idle, and a member the room has no
	// activity for (after a checkpoint restore) counts from now.
	for (const auto& member : snapshot_.members)
		if (member.status == MemberStatus::Playing) NoteActive(member.id);
		else lastActiveMs_.emplace(member.id, nowMs_);
	std::vector<Event> events;
	for (std::size_t i = 0; i < TableCount; ++i) {
		Table& table = snapshot_.tables[i];
		if (!table.resultPending || table.phase != TablePhase::Playing ||
			nowMs_ - resultPendingSince_[i] < ResultDisputeTimeoutMs) continue;
		table.phase = TablePhase::Paused;
		Touch(table);
		events.push_back(Event{Event::Kind::ResultDisputed, static_cast<std::uint8_t>(i), table.matchGeneration, resultReporter_[i], pendingResult_[i]});
	}
	// A permit that never reached both fighters, or whose window has passed,
	// calls the start off; they ready again, and a new generation needs a new
	// permit.
	for (auto& table : snapshot_.tables) {
		if (!PermitCalledOff(table, PermitAge(table))) continue;
		ClearReadiness(table);
		ClearPermit(table);
		table.spectatorHold = false;
		startHeldSince_[table.id] = 0;
		table.phase = table.p1 && table.p2 ? TablePhase::Waiting : TablePhase::Idle;
		Touch(table);
		NormalizeTableMembers(table);
		events.push_back(Event{Event::Kind::SnapshotChanged, table.id, 0, 0, MatchResult::Abort});
	}
	ReleaseHeldStarts(events);
	RefreshReadyTimers();
	for (auto& table : snapshot_.tables) {
		const auto member = readyWaitingFor_[table.id];
		if (!member || !TimerDue(readyWaitingSince_[table.id], ReadyTimeoutMs, nowMs_)) continue;
		if (ApplyUnqueue(member, &table).accepted) events.push_back(Event{Event::Kind::ReadyTimeout, table.id, 0, member, MatchResult::Abort});
	}
	return events;
}

bool RoomAuthority::LockedSpectatorReturning(const Table& table) const {
	return std::any_of(table.spectators.begin(), table.spectators.end(), [&](MemberId spectator) {
		const auto* member = Find(spectator);
		return member && member->spectatorLocked && HasOutstandingTerminalReceiptForMember(spectator);
	});
}

bool RoomAuthority::ReleaseHeldStarts(std::vector<Event>& events) {
	bool changed = false;
	for (auto& table : snapshot_.tables) {
		if (!table.spectatorHold) continue;
		// Any other way out of Ready (Unready, a departed fighter, a closed
		// room) ends the hold without a start.
		const bool starting = table.phase == TablePhase::Ready;
		if (starting && LockedSpectatorReturning(table) && !TimerDue(startHeldSince_[table.id], SpectatorStartHoldMs, nowMs_)) continue;
		table.spectatorHold = false;
		startHeldSince_[table.id] = 0;
		Touch(table);
		changed = true;
		if (starting && !PermitPending(table) && !PermitStartPassed(table, PermitAge(table)))
			events.push_back(Event{Event::Kind::MatchReady, table.id, table.matchGeneration, 0, MatchResult::Abort});
	}
	return changed;
}

} }
