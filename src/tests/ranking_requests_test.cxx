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
	requests.SentStanding(10, 0);
	requests.SentChoice(7, 11, 0);
	EXPECT(requests.Awaiting(10) == Kind::None && !requests.Published(0).standing.loading);
	EXPECT(requests.Awaiting(11) == Kind::Choice);
	requests.FinishChoice("", OptedOut(), 0);
	const auto& status = requests.Published(0);
	EXPECT(status.chose == 7 && status.choosing == 0 && status.choiceFailure.empty());
	EXPECT(status.standing.value.kind == Standing::Kind::OptedOut && status.standing.revision == 1);
}

// No read goes while a choice is in flight: the choice answers with the standing after it.
static void ChoiceThenRead() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentChoice(3, 20, 0);
	EXPECT(!requests.MayReadStanding() && requests.MayReadBoard());
	requests.FinishChoice("choice_too_soon", std::nullopt, 0);
	EXPECT(requests.MayReadStanding() && requests.Published(0).choiceFailure == "choice_too_soon" && requests.Published(0).chose == 3);
	EXPECT(requests.Published(0).standing.revision == 0);
}

// A failed refresh ends the request but keeps the value and its revision.
static void FailedRefresh() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentStanding(1, 0);
	requests.FinishStanding("", Ranked(5), 1000);
	requests.SentStanding(2, 0);
	requests.FinishStanding("bridge_unreachable", std::nullopt, 90000);
	const auto status = requests.Published(91000);
	const auto& standing = status.standing;
	EXPECT(standing.loaded && standing.value.badge.band == 5 && standing.revision == 1 && standing.finished == 2);
	// Its age counts from when it arrived, not from the failed refresh or from when it is shown.
	EXPECT(status.AgeSeconds(standing) == 90);
	EXPECT(standing.error == "bridge_unreachable" && !standing.loading);
	requests.SentBoard(3, 0);
	requests.FinishBoard("bridge_invalid_response", std::nullopt, 0);
	EXPECT(!requests.Published(0).board.loaded && requests.Published(0).board.revision == 0 && requests.Published(0).board.finished == 1);
}

// Another service or Ember ID forgets every answer, retires the requests on
// their way, and answers the choice in flight as unavailable under its own
// identity. The counts carry on.
static void ScopeChange() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentStanding(1, 0);
	requests.FinishStanding("", Ranked(8), 0);
	requests.SentBoard(2, 0);
	requests.SentChoice(9, 3, 0);
	requests.Scope("brg_1", "emb1_b");
	const auto& status = requests.Published(0);
	EXPECT(status.emberId == "emb1_b" && !status.standing.loaded && !status.board.loaded && !status.board.loading);
	EXPECT(status.chose == 9 && status.choiceFailure == "unavailable" && status.choosing == 0);
	EXPECT(status.standing.finished == 1 && status.standing.revision == 1);
	for (const std::uint64_t late : {1u, 2u, 3u}) EXPECT(requests.Awaiting(late) == Kind::None);
	// The same scope again changes nothing.
	requests.SentStanding(4, 0);
	requests.Scope("brg_1", "emb1_b");
	EXPECT(requests.Awaiting(4) == Kind::Standing);
}

// A request the helper could not take ends at once; one it never answers ends after the limit.
static void LostRequests() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentStanding(0, 0);
	EXPECT(requests.Published(0).standing.error == "helper_unavailable" && requests.Published(0).standing.finished == 1);
	requests.SentChoice(5, 0, 0);
	EXPECT(requests.Published(0).chose == 5 && requests.Published(0).choiceFailure == "helper_unavailable");
	requests.SentBoard(6, 1000);
	requests.Expire(1000 + Requests::GiveUpMs - 1);
	EXPECT(requests.Awaiting(6) == Kind::Board);
	requests.Expire(1000 + Requests::GiveUpMs);
	EXPECT(requests.Awaiting(6) == Kind::None && requests.Published(0).board.error == "unavailable");
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
