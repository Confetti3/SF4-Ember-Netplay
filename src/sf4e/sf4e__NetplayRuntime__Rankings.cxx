// Rankings (docs/design/SKILL_BADGE.md, "Rankings"): the player's own standing,
// their choice to take part, and the board of classes. Requests go to the
// helper on the tournament channel and are answered by request id, like the
// public room ones; netplay::rankings::Requests decides what each answer
// does, and the interface reads the result from the published status.
#include "sf4e__NetplayRuntime.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;
using internal::SendBridgeRequest;
using Kind = netplay::rankings::Requests::Kind;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }

std::uint64_t SendIf(bool helperReady, const json& request) { return helperReady ? SendBridgeRequest(request) : 0; }

json Choice(const Command& command) {
	json choice = {{"ranked", command.ranked}};
	if (command.ranked && !command.listingName.empty()) {
		choice["listing"] = {{"name", command.listingName}};
		if (!command.listingCountry.empty()) choice["listing"]["country"] = command.listingCountry;
	}
	return {{"op", "ranking_choose"}, {"bridge_id", command.bridgeId}, {"choice", choice}};
}

// The helper's code for a refusal, or why a successful answer gave no value.
std::string Failure(const session::TournamentAnswer& answer, bool decoded) {
	if (!answer.ok) return answer.reason.empty() ? "unavailable" : answer.reason;
	return decoded ? std::string() : "bridge_invalid_response";
}
}

namespace internal {
void DispatchRankings(const Command& command, bool helperReady) {
	if (command.bridgeId.empty()) return;
	auto& requests = runtime->rankings;
	requests.Scope(command.bridgeId, CurrentId());
	const auto now = GetTickCount64();
	switch (command.op) {
	case Command::Op::RankingStanding:
		if (requests.MayReadStanding())
			requests.SentStanding(SendIf(helperReady, {{"op", "ranking_standing"}, {"bridge_id", command.bridgeId}}), now);
		return;
	case Command::Op::RankingBoard:
		if (requests.MayReadBoard())
			requests.SentBoard(SendIf(helperReady, {{"op", "ranking_board"}, {"bridge_id", command.bridgeId}}), now);
		return;
	case Command::Op::RankingChoose:
		if (!command.request) return;
		requests.SentChoice(command.request, SendIf(helperReady, Choice(command)), now);
		spdlog::info("Rankings: {} requested", command.ranked ? "taking part" : "opting out");
		return;
	default:
		return;
	}
}

bool TakeRankingsAnswer(const session::TournamentAnswer& answer) {
	auto& requests = runtime->rankings;
	const auto now = GetTickCount64();
	switch (requests.Awaiting(answer.requestId)) {
	case Kind::Standing: {
		auto standing = answer.ok ? session::DecodeStanding(answer.data) : std::nullopt;
		requests.FinishStanding(Failure(answer, standing.has_value()), std::move(standing), now);
		return true;
	}
	case Kind::Choice: {
		auto standing = answer.ok ? session::DecodeStanding(answer.data) : std::nullopt;
		const auto failure = Failure(answer, standing.has_value());
		if (!failure.empty()) spdlog::info("Rankings: choice refused: {}", failure);
		requests.FinishChoice(failure, std::move(standing), now);
		return true;
	}
	case Kind::Board: {
		auto board = answer.ok ? session::DecodeBoard(answer.data) : std::nullopt;
		requests.FinishBoard(Failure(answer, board.has_value()), std::move(board), now);
		return true;
	}
	default:
		return false;
	}
}

void ExpireRankings(std::uint64_t nowMs) {
	auto& requests = runtime->rankings;
	// Another Ember ID's class is not shown, not even on Online play.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
	requests.Expire(nowMs);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
