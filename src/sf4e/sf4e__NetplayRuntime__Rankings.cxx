// Rankings (docs/design/SKILL_BADGE.md, "Rankings"): the player's own standing,
// their choice to take part, and the board of classes. Requests go to the
// helper on the tournament channel and their answers come back through
// flights, like the public room ones; netplay::rankings::Requests decides what
// each answer does, and the interface reads the result from the published
// status.
#include "sf4e__NetplayRuntime.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;
using internal::SendFlight;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }

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
	if (!answer.ok) return netplay::BridgeFlights::Reason(answer);
	return decoded ? std::string() : "bridge_invalid_response";
}

// The answers to the requests that went as `flight`.
void TakeStanding(std::uint64_t flight, const session::TournamentAnswer& answer) {
	auto standing = answer.ok ? session::DecodeStanding(answer.data) : std::nullopt;
	runtime->rankings.FinishStanding(flight, Failure(answer, standing.has_value()), std::move(standing), GetTickCount64());
}

void TakeChoice(std::uint64_t flight, const session::TournamentAnswer& answer) {
	auto standing = answer.ok ? session::DecodeStanding(answer.data) : std::nullopt;
	const auto failure = Failure(answer, standing.has_value());
	if (!failure.empty()) spdlog::info("Rankings: choice refused: {}", failure);
	runtime->rankings.FinishChoice(flight, failure, std::move(standing), GetTickCount64());
}

void TakeBoard(std::uint64_t flight, const session::TournamentAnswer& answer) {
	auto board = answer.ok ? session::DecodeBoard(answer.data) : std::nullopt;
	runtime->rankings.FinishBoard(flight, Failure(answer, board.has_value()), std::move(board), GetTickCount64());
}

// Sends `request` as `flight`, whose answer `take` handles.
void Send(const json& request, bool helperReady, std::uint64_t flight, void (*take)(std::uint64_t, const session::TournamentAnswer&)) {
	SendFlight(request, helperReady, [flight, take](const session::TournamentAnswer& answer) { take(flight, answer); });
}
}

namespace internal {
void DispatchRankings(const Command& command, bool helperReady) {
	if (command.bridgeId.empty()) return;
	auto& requests = runtime->rankings;
	requests.Scope(command.bridgeId, CurrentId());
	switch (command.op) {
	case Command::Op::RankingStanding:
		if (requests.MayReadStanding())
			Send({{"op", "ranking_standing"}, {"bridge_id", command.bridgeId}}, helperReady, requests.BeginStanding(), TakeStanding);
		return;
	case Command::Op::RankingBoard:
		if (requests.MayReadBoard())
			Send({{"op", "ranking_board"}, {"bridge_id", command.bridgeId}}, helperReady, requests.BeginBoard(), TakeBoard);
		return;
	case Command::Op::RankingChoose:
		if (!command.request) return;
		spdlog::info("Rankings: {} requested", command.ranked ? "taking part" : "opting out");
		Send(Choice(command), helperReady, requests.BeginChoice(command.request), TakeChoice);
		return;
	default:
		return;
	}
}

void TickRankings() {
	auto& requests = runtime->rankings;
	// Another Ember ID's class is not shown, not even on Online play.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
