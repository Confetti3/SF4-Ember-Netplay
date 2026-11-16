// The runtime's level reads (netplay/LevelRequests.hxx): one read at a time, a
// failed refresh that keeps the value, a change of service or Ember ID, reads
// the helper lost, and the reads due at start and after a set this PC fought.
#include "../netplay/LevelRequests.hxx"

#include <cstdio>
#include <cstdlib>
#include "test_support.hxx"

using namespace sf4e::netplay::levels;
using sf4e::netplay::Answered;

static Own Counted(std::uint64_t points) {
	Own own;
	own.kind = Own::Kind::Counted;
	own.fighters[3].points = points;
	return own;
}

// A read is in flight until it is answered, and only its own flight is awaited.
static void OneAtATime() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.MayRead() && !requests.Awaiting(0));
	const auto flight = requests.Begin();
	EXPECT(flight && !requests.MayRead() && requests.Awaiting(flight) && !requests.Awaiting(flight + 1) && requests.Published().loading);
	requests.Finish(flight + 1, "", Counted(41), 400);
	EXPECT(requests.Awaiting(flight) && !requests.Published().loaded);
	requests.Finish(flight, "", Counted(42), 500);
	const auto& answered = requests.Published();
	EXPECT(requests.MayRead() && !requests.Awaiting(flight) && !answered.loading && answered.loaded);
	EXPECT(answered.value.fighters[3].points == 42 && answered.revision == 1 && answered.finished == 1 && answered.installedMs == 500);
	// The answer counts once.
	requests.Finish(flight, "", Counted(43), 600);
	EXPECT(requests.Published().value.fighters[3].points == 42 && requests.Published().finished == 1);
}

// A failed refresh ends the read but keeps the value and its revision; a bridge
// without levels installs an answer of that kind.
static void FailedRefresh() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.Finish(requests.Begin(), "", Counted(9), 1000);
	requests.Finish(requests.Begin(), "bridge_unreachable", std::nullopt, 3000);
	const auto& answered = requests.Published();
	EXPECT(answered.value.fighters[3].points == 9 && answered.revision == 1 && answered.finished == 2 && answered.installedMs == 1000);
	EXPECT(answered.error == "bridge_unreachable" && !answered.loading);
	const auto third = requests.Begin();
	EXPECT(requests.Published().error.empty() && requests.Published().loading);
	requests.Finish(third, Requests::Unavailable, Own{}, 5000);
	EXPECT(requests.Published().value.kind == Own::Kind::Unavailable && requests.Published().loaded &&
		requests.Published().error == Requests::Unavailable && requests.Published().revision == 2);
}

// Another service or Ember ID forgets the value and retires the read on its
// way, whose late answer is nobody's; the counts carry on, and the same scope
// again changes nothing.
static void ScopeChange() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.Finish(requests.Begin(), "", Counted(5), 0);
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.Published().loaded);
	const auto retired = requests.Begin();
	requests.Scope("brg_1", "emb1_b");
	const auto& answered = requests.Published();
	EXPECT(!answered.loaded && !answered.loading && answered.value.kind == Own::Kind::Unavailable && answered.revision == 1 && answered.finished == 1);
	EXPECT(!requests.Awaiting(retired) && requests.MayRead() && requests.EmberId() == "emb1_b" && requests.Bridge() == "brg_1");
	// The new scope's read is overtaken by nothing: the retired one's answer, coming late, changes nothing.
	const auto fresh = requests.Begin();
	EXPECT(fresh != retired);
	requests.Finish(retired, "", Counted(7), 10);
	EXPECT(requests.Awaiting(fresh) && !requests.Published().loaded && requests.Published().finished == 1);
	requests.Finish(fresh, "", Counted(8), 20);
	EXPECT(requests.Published().value.fighters[3].points == 8 && requests.Published().finished == 2);
}

// A read the helper could not take, or never answered, is ended by BridgeFlights as a refusal: the read ends, and
// another may go.
static void LostReads() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.Finish(requests.Begin(), "helper_unavailable", std::nullopt, 0);
	EXPECT(requests.Published().error == "helper_unavailable" && requests.Published().finished == 1 && requests.MayRead());
	const auto lost = requests.Begin();
	EXPECT(requests.Awaiting(lost) && !requests.MayRead());
	requests.Finish(lost, "unavailable", std::nullopt, 61000);
	EXPECT(!requests.Awaiting(lost) && requests.Published().error == "unavailable" && !requests.Published().loading && requests.MayRead());
}

// One read is due at once, for the start; a set this PC fought ended adds two,
// 8 s and 20 s after; none while a read is in flight, and a due read waits for it.
static void Schedule() {
	Requests requests;
	EXPECT(requests.TakeDue(0) && !requests.TakeDue(1000000));
	requests.NoteSets(1, {0, 0}, 100);
	// A set already ended when the room was first seen is not new.
	requests.NoteSets(2, {7, 0}, 200);
	EXPECT(!requests.TakeDue(1000000));
	// A table whose set this player did not fight reads as 0 and schedules nothing.
	requests.NoteSets(2, {7, 0}, 300);
	requests.NoteSets(2, {7, 8}, 10000);
	EXPECT(!requests.TakeDue(10000 + Requests::FirstReadMs - 1));
	const auto flight = requests.Begin();
	EXPECT(!requests.TakeDue(10000 + Requests::FirstReadMs));
	requests.Finish(flight, "", Counted(1), 18500);
	EXPECT(requests.TakeDue(10000 + Requests::FirstReadMs) && !requests.TakeDue(10000 + Requests::FirstReadMs));
	EXPECT(!requests.TakeDue(10000 + Requests::SecondReadMs - 1) && requests.TakeDue(10000 + Requests::SecondReadMs));
	// A new set in the same room schedules afresh, and a table's changed count is only noted.
	requests.NoteSets(2, {9, 8}, 50000);
	EXPECT(requests.TakeDue(50000 + Requests::FirstReadMs));
	requests.NoteSets(2, {9, 8, 0}, 60000);
	EXPECT(!requests.TakeDue(50000 + Requests::SecondReadMs - 1) && requests.TakeDue(50000 + Requests::SecondReadMs));
}

int main() {
	OneAtATime();
	FailedRefresh();
	ScopeChange();
	LostReads();
	Schedule();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("level request tests passed\n");
	return TEST_EXIT_CODE();
}
