// The runtime's ranking requests (netplay/RankingRequests.hxx): a choice and a
// read of the standing in either order, a failed refresh, a change of service
// or Ember ID while requests are on their way, and requests the helper lost.
#include "../netplay/RankingRequests.hxx"

#include <cstdio>
#include <cstdlib>
#include "test_support.hxx"

using namespace sf4e::netplay::rankings;
using Kind = Requests::Kind;

static Standing Ranked(std::uint8_t band) {
	Standing standing;
	standing.kind = Standing::Kind::Ranked; standing.classesShown = true; standing.badge = sf4e::SkillBadge::OfClass(band, true);
	return standing;
}
static Standing OptedOut() {
	Standing standing;
	standing.kind = Standing::Kind::OptedOut; standing.changeIn = 86400;
	return standing;
}

// A read on its way when a choice is sent may have read the standing before
// the choice: it is retired, and its late answer replaces nothing.
static void ReadThenChoice() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	const auto read = requests.BeginStanding();
	const auto choice = requests.BeginChoice(7);
	EXPECT(read && choice && read != choice);
	EXPECT(requests.Awaiting(read) == Kind::None && !requests.Published(0).standing.loading);
	EXPECT(requests.Awaiting(choice) == Kind::Choice);
	// The retired read's answer, and an answer under the wrong kind, change nothing.
	requests.FinishStanding(read, "", Ranked(9), 0);
	requests.FinishStanding(choice, "", Ranked(9), 0);
	EXPECT(requests.Published(0).standing.revision == 0 && requests.Awaiting(choice) == Kind::Choice);
	requests.FinishChoice(choice, "", OptedOut(), 0);
	const auto& status = requests.Published(0);
	EXPECT(status.chose == 7 && status.choosing == 0 && status.choiceFailure.empty());
	EXPECT(status.standing.value.kind == Standing::Kind::OptedOut && status.standing.revision == 1);
}

// No read goes while a choice is in flight: the choice answers with the standing after it.
static void ChoiceThenRead() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	const auto choice = requests.BeginChoice(3);
	EXPECT(!requests.MayReadStanding() && requests.MayReadBoard());
	requests.FinishChoice(choice, "choice_too_soon", std::nullopt, 0);
	EXPECT(requests.MayReadStanding() && requests.Published(0).choiceFailure == "choice_too_soon" && requests.Published(0).chose == 3);
	EXPECT(requests.Published(0).standing.revision == 0);
}

// A failed refresh ends the request but keeps the value and its revision.
static void FailedRefresh() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.FinishStanding(requests.BeginStanding(), "", Ranked(5), 1000);
	requests.FinishStanding(requests.BeginStanding(), "bridge_unreachable", std::nullopt, 90000);
	const auto status = requests.Published(91000);
	const auto& standing = status.standing;
	EXPECT(standing.loaded && standing.value.badge.band == 5 && standing.revision == 1 && standing.finished == 2);
	// Its age counts from when it arrived, not from the failed refresh or from when it is shown.
	EXPECT(status.AgeSeconds(standing) == 90);
	EXPECT(standing.error == "bridge_unreachable" && !standing.loading);
	requests.FinishBoard(requests.BeginBoard(), "bridge_invalid_response", std::nullopt, 0);
	EXPECT(!requests.Published(0).board.loaded && requests.Published(0).board.revision == 0 && requests.Published(0).board.finished == 1);
}

// Another service or Ember ID forgets every answer, retires the requests on
// their way, and answers the choice in flight as unavailable under its own
// identity. The counts carry on.
static void ScopeChange() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	const auto first = requests.BeginStanding();
	requests.FinishStanding(first, "", Ranked(8), 0);
	const auto board = requests.BeginBoard();
	const auto choice = requests.BeginChoice(9);
	requests.Scope("brg_1", "emb1_b");
	const auto& status = requests.Published(0);
	EXPECT(status.emberId == "emb1_b" && !status.standing.loaded && !status.board.loaded && !status.board.loading);
	EXPECT(status.chose == 9 && status.choiceFailure == "unavailable" && status.choosing == 0);
	EXPECT(status.standing.finished == 1 && status.standing.revision == 1);
	for (const auto late : {first, board, choice}) EXPECT(requests.Awaiting(late) == Kind::None);
	// Their late answers are nobody's.
	requests.FinishBoard(board, "", std::vector<BoardEntry>{}, 0);
	requests.FinishChoice(choice, "", Ranked(2), 0);
	EXPECT(!requests.Published(0).board.loaded && !requests.Published(0).standing.loaded && requests.Published(0).chose == 9);
	// The same scope again changes nothing.
	const auto again = requests.BeginStanding();
	requests.Scope("brg_1", "emb1_b");
	EXPECT(requests.Awaiting(again) == Kind::Standing);
}

// A request the helper could not take, or never answered, is ended by BridgeFlights as a refusal: each ends as
// its kind does.
static void LostRequests() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.FinishStanding(requests.BeginStanding(), "helper_unavailable", std::nullopt, 0);
	EXPECT(requests.Published(0).standing.error == "helper_unavailable" && requests.Published(0).standing.finished == 1);
	requests.FinishChoice(requests.BeginChoice(5), "helper_unavailable", std::nullopt, 0);
	EXPECT(requests.Published(0).chose == 5 && requests.Published(0).choiceFailure == "helper_unavailable");
	const auto lost = requests.BeginBoard();
	EXPECT(requests.Awaiting(lost) == Kind::Board && !requests.MayReadBoard());
	requests.FinishBoard(lost, "unavailable", std::nullopt, 61000);
	EXPECT(requests.Awaiting(lost) == Kind::None && requests.Published(0).board.error == "unavailable" && requests.MayReadBoard());
}

int main() {
	ReadThenChoice();
	ChoiceThenRead();
	FailedRefresh();
	ScopeChange();
	LostRequests();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("ranking request tests passed\n");
	return TEST_EXIT_CODE();
}
