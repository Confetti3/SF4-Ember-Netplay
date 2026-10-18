// Numbered results share one frozen native roster and one transport generation.
// Neither a result nor its retry emits MatchEnded. Only an agreed finish (or
// the existing abort/disconnect path) closes the native transport.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

namespace sf4e { namespace room {

Result RoomAuthority::ApplyContinuousResult(MemberId member, const Action& action, Table& table) {
	if (!IsParticipant(table, member)) return Reject(RejectReason::NotSeated);
	if (!action.matchGeneration || action.matchGeneration != table.matchGeneration || !action.continuousGame)
		return Reject(RejectReason::WrongGeneration);
	auto& series = table.continuous;
	if (!series.enabled || series.closed || table.phase != TablePhase::Playing)
		return Reject(RejectReason::WrongPhase);
	if (action.result != MatchResult::P1Win && action.result != MatchResult::P2Win && action.result != MatchResult::Draw)
		return Reject(RejectReason::WrongPhase);
	// A stale packet cannot reopen a game, even after a later action advanced
	// the sender's watermark. The newest receipt also detects a changed retry.
	if (action.continuousGame <= series.games) {
		if (action.continuousGame == series.games && action.result != series.lastResult)
			return Reject(RejectReason::WrongPhase);
		return Reject(RejectReason::DuplicateResult);
	}
	if (series.games == (std::numeric_limits<std::uint64_t>::max)() || action.continuousGame != series.games + 1)
		return Reject(RejectReason::WrongGeneration);
	const auto goal = static_cast<std::uint32_t>(table.rules.format);
	if (series.finishMask || (goal && (table.score[0] >= goal || table.score[1] >= goal)))
		return Reject(RejectReason::WrongPhase);
	const int winner = action.result == MatchResult::P1Win ? 0 : action.result == MatchResult::P2Win ? 1 : -1;
	if (winner >= 0 && table.score[winner] == (std::numeric_limits<std::uint32_t>::max)())
		return Reject(RejectReason::WrongPhase);
	if (table.resultPending) {
		if (resultReporter_[table.id] == member)
			return Reject(pendingResult_[table.id] == action.result ? RejectReason::DuplicateResult : RejectReason::WrongPhase);
		if (pendingResult_[table.id] != action.result) {
			table.phase = TablePhase::Paused;
			Touch(table);
			return Accept({Event{Event::Kind::ResultDisputed, table.id, table.matchGeneration, member, action.result}});
		}
		// Commit a point once, but retain the generation, seats, readiness and
		// native recipients until both players choose to leave the transport.
		if (winner >= 0) { ++table.score[winner]; ++series.wins[winner]; }
		++series.games;
		series.lastResult = action.result;
		table.resultPending = false;
		resultReporter_[table.id] = 0;
		resultPendingSince_[table.id] = 0;
		pendingResult_[table.id] = MatchResult::Abort;
		Touch(table);
		return Accept();
	}
	resultReporter_[table.id] = member;
	pendingResult_[table.id] = action.result;
	table.resultPending = true;
	resultPendingSince_[table.id] = nowMs_;
	Touch(table);
	return Accept();
}

Result RoomAuthority::ApplyFinishContinuous(MemberId member, const Action& action, Table& table) {
	// A lost finish reply may be retried after seat rotation or a new game.
	// Only the exact receipt's two fighters can replay that terminal event.
	for (const auto& receipt : terminalReceipts_) {
		if (receipt.table == action.table && receipt.generation == action.matchGeneration &&
			receipt.continuousGames && receipt.continuousGames == action.continuousGame &&
			(receipt.fighters[0] == member || receipt.fighters[1] == member)) {
			auto result = Accept({Event{Event::Kind::MatchEnded, action.table, action.matchGeneration, 0, receipt.result, true}});
			result.terminalReplay = true;
			return result;
		}
	}
	if (!IsParticipant(table, member)) return Reject(RejectReason::NotSeated);
	auto& series = table.continuous;
	if (!action.matchGeneration || action.matchGeneration != table.matchGeneration ||
		!action.continuousGame || action.continuousGame != series.games) return Reject(RejectReason::WrongGeneration);
	if (!series.enabled || series.closed || table.phase != TablePhase::Playing || table.resultPending)
		return Reject(RejectReason::WrongPhase);
	const auto mask = static_cast<std::uint8_t>(series.finishMask | (table.p1 == member ? 1 : 2));
	if (mask == 3) {
		// EndMatch can refuse if its durable terminal ledger is full. Keep
		// that refusal atomic: the second vote must then remain retryable.
		const auto priorMask = series.finishMask;
		series.finishMask = mask;
		auto result = EndMatch(table.id, table.matchGeneration, series.lastResult, true);
		if (!result.accepted) { series.finishMask = priorMask; return Reject(result.reason); }
		return result;
	}
	if (series.finishMask != mask) { series.finishMask = mask; Touch(table); }
	return Accept();
}

} }
