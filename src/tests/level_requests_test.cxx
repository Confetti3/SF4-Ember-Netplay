// The runtime's level reads (netplay/LevelRequests.hxx): one read at a time, a
// failed refresh that keeps the value, a change of service or Ember ID, reads
// the helper lost, and the reads due at start and after a set this PC fought.
#include "../netplay/LevelRequests.hxx"

#include <cstdio>
#include <cstdlib>
#include "test_support.hxx"

using namespace sf4e::netplay::levels;
using sf4e::netplay::Answered;

static Own Counted(std::uint8_t level) {
	Own own;
	own.kind = Own::Kind::Counted;
	own.fighters[3].level = level;
	return own;
}

// A read is in flight until it is answered, and only its own id is awaited.
static void OneAtATime() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.MayRead() && !requests.Awaiting(0));
	requests.Sent(10, 0);
	EXPECT(!requests.MayRead() && requests.Awaiting(10) && !requests.Awaiting(11) && requests.Published().loading);
	requests.Finish("", Counted(42), 500);
	const auto& answered = requests.Published();
	EXPECT(requests.MayRead() && !requests.Awaiting(10) && !answered.loading && answered.loaded);
	EXPECT(answered.value.fighters[3].level == 42 && answered.revision == 1 && answered.finished == 1 && answered.installedMs == 500);
}

// A failed refresh ends the read but keeps the value and its revision; a bridge
// without levels installs an answer of that kind.
static void FailedRefresh() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.Sent(1, 0);
	requests.Finish("", Counted(9), 1000);
	requests.Sent(2, 2000);
	requests.Finish("bridge_unreachable", std::nullopt, 3000);
	const auto& answered = requests.Published();
	EXPECT(answered.value.fighters[3].level == 9 && answered.revision == 1 && answered.finished == 2 && answered.installedMs == 1000);
	EXPECT(answered.error == "bridge_unreachable" && !answered.loading);
	requests.Sent(3, 4000);
	EXPECT(requests.Published().error.empty() && requests.Published().loading);
	requests.Finish(Requests::Unavailable, Own{}, 5000);
	EXPECT(requests.Published().value.kind == Own::Kind::Unavailable && requests.Published().loaded &&
		requests.Published().error == Requests::Unavailable && requests.Published().revision == 2);
}

// Another service or Ember ID forgets the value and retires the read on its
// way; the counts carry on, and the same scope again changes nothing.
static void ScopeChange() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.Sent(1, 0);
	requests.Finish("", Counted(5), 0);
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.Published().loaded);
	requests.Sent(2, 0);
	requests.Scope("brg_1", "emb1_b");
	const auto& answered = requests.Published();
	EXPECT(!answered.loaded && !answered.loading && answered.value.kind == Own::Kind::Unavailable && answered.revision == 1 && answered.finished == 1);
	EXPECT(!requests.Awaiting(2) && requests.MayRead() && requests.EmberId() == "emb1_b" && requests.Bridge() == "brg_1");
}

// A read the helper could not take ends at once; one it never answers ends after the limit.
static void LostReads() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.Sent(0, 0);
	EXPECT(requests.Published().error == "helper_unavailable" && requests.Published().finished == 1 && requests.MayRead());
	requests.Sent(6, 1000);
	requests.Expire(1000 + Requests::GiveUpMs - 1);
	EXPECT(requests.Awaiting(6));
	requests.Expire(1000 + Requests::GiveUpMs);
	EXPECT(!requests.Awaiting(6) && requests.Published().error == "unavailable" && !requests.Published().loading);
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
	requests.Sent(5, 0);
	EXPECT(!requests.TakeDue(10000 + Requests::FirstReadMs));
	requests.Finish("", Counted(1), 18500);
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
