// The runtime's card look-ups (netplay/CardRequests.hxx): a join and a set end
// trigger them, a look-up is up to 16 tokens, one at a time and one every 10 s,
// null is remembered until the next trigger, leaving forgets, and a bridge
// without levels stops them until the room changes.
#include "../netplay/CardRequests.hxx"

#include <cstdio>
#include <cstdlib>
#include "test_support.hxx"

using namespace sf4e::netplay::levels;
using Batch = std::vector<std::string>;
using Answers = std::vector<std::optional<Card>>;

static Card Showing(std::uint8_t fighter, std::uint64_t points) {
	Card card;
	card.account = {points, 0};
	card.fighters.push_back({fighter, points, 0});
	return card;
}

static void Scope(Cards& cards) { cards.Scope("brg_1"); }

// Asks for what Take offers and answers it with `answers`.
static void Look(Cards& cards, std::uint64_t id, std::uint64_t nowMs, const Answers& answers) {
	cards.Sent(id, cards.Take(nowMs), nowMs);
	cards.Finish("", answers);
}

// A member with a token joining triggers a look-up; the answer is published by
// token, a null one as no card, and nothing is asked again once all are answered.
static void JoinAsks() {
	Cards cards;
	Scope(cards);
	cards.Observe(1, {}, {0, 0}, 0);
	EXPECT(cards.Take(0).empty());
	cards.Observe(1, {"c1.a"}, {0, 0}, 100);
	EXPECT((cards.Take(100) == Batch{"c1.a"}));
	cards.Sent(7, {"c1.a"}, 100);
	EXPECT(cards.Awaiting(7) && !cards.Awaiting(8) && cards.Take(100 + Cards::MinGapMs).empty());
	cards.Finish("", Answers{Showing(3, 31)});
	EXPECT(!cards.Awaiting(7) && cards.Published()->size() == 1 && cards.Published()->at("c1.a").fighters[0].points == 31);
	// A second member joins: it waits for the gap.
	cards.Observe(1, {"c1.a", "c1.b"}, {0, 0}, 200);
	EXPECT(cards.Take(200).empty());
	EXPECT((cards.Take(100 + Cards::MinGapMs) == Batch{"c1.b"}));
	Look(cards, 8, 100 + Cards::MinGapMs, Answers{std::nullopt});
	EXPECT(cards.Published()->size() == 1 && cards.Take(1000000).empty());
}

// At most one look-up every 10 s, of at most 16 tokens: the rest wait for the next.
static void Limits() {
	Cards cards;
	Scope(cards);
	Batch tokens;
	for (int i = 0; i < 20; ++i) tokens.push_back("c1.t" + std::to_string(i));
	cards.Observe(1, tokens, {0}, 0);
	const Batch first = cards.Take(1000);
	EXPECT(first.size() == MaxLookup && first.front() == "c1.t0" && first.back() == "c1.t15");
	cards.Sent(1, first, 1000);
	cards.Finish("", Answers(MaxLookup, std::nullopt));
	EXPECT(cards.Take(1000 + Cards::MinGapMs - 1).empty());
	const Batch second = cards.Take(1000 + Cards::MinGapMs);
	EXPECT((second == Batch{"c1.t16", "c1.t17", "c1.t18", "c1.t19"}));
	// A look-up the helper could not take ends at once, and is not retried before the gap.
	cards.Sent(0, second, 1000 + Cards::MinGapMs);
	EXPECT(cards.Error() == "helper_unavailable" && !cards.Awaiting(0));
	EXPECT(cards.Take(1000 + Cards::MinGapMs + 1).empty() && cards.Take(1000 + 2 * Cards::MinGapMs).size() == 4);
	// One that is never answered ends after the limit and keeps its tokens wanted.
	cards.Sent(2, second, 50000);
	cards.Expire(50000 + Cards::GiveUpMs - 1);
	EXPECT(cards.Awaiting(2));
	cards.Expire(50000 + Cards::GiveUpMs);
	EXPECT(!cards.Awaiting(2) && cards.Error() == "unavailable" && cards.Take(50000 + Cards::GiveUpMs).size() == 4);
}

// A set ending anywhere in the room asks again for every token 8 s later; the
// old card stays on show meanwhile, and a null is asked about again then.
static void SetEndAsks() {
	Cards cards;
	Scope(cards);
	const Batch both = {"c1.a", "c1.b"};
	cards.Observe(1, both, {5, 0}, 0);
	Look(cards, 1, 0, Answers{Showing(1, 2), std::nullopt});
	// A set already ended when the room was first seen is not new.
	cards.Observe(1, both, {5, 0}, 20000);
	EXPECT(cards.Take(30000).empty());
	cards.Observe(1, both, {5, 9}, 40000);
	cards.Observe(1, both, {5, 9}, 40000 + Cards::SetEndedMs - 1);
	EXPECT(cards.Take(40000 + Cards::SetEndedMs - 1).empty());
	cards.Observe(1, both, {5, 9}, 40000 + Cards::SetEndedMs);
	EXPECT(cards.Take(40000 + Cards::SetEndedMs) == both);
	EXPECT(cards.Published()->size() == 1);
	Look(cards, 2, 40000 + Cards::SetEndedMs, Answers{Showing(1, 3), Showing(4, 5)});
	EXPECT(cards.Published()->size() == 2 && cards.Published()->at("c1.a").fighters[0].points == 3);
	EXPECT(cards.Take(1000000).empty());
}

// A token no member carries is dropped, and leaving the room or another
// service forgets all.
static void Pruning() {
	Cards cards;
	Scope(cards);
	cards.Observe(1, {"c1.a", "c1.b"}, {0}, 0);
	Look(cards, 1, 0, Answers{Showing(1, 2), Showing(2, 3)});
	cards.Observe(1, {"c1.b"}, {0}, 100);
	EXPECT(cards.Published()->size() == 1 && cards.Published()->count("c1.b") == 1);
	// An answer for a token that left while it was awaited is not kept.
	cards.Observe(1, {"c1.b", "c1.c"}, {0}, 200);
	cards.Sent(2, cards.Take(20000), 20000);
	cards.Observe(1, {"c1.b"}, {0}, 20100);
	cards.Finish("", Answers{Showing(5, 5)});
	EXPECT(cards.Published()->size() == 1 && cards.Published()->count("c1.c") == 0);
	// The snapshot a reader holds is never changed under it.
	const auto shown = cards.Published();
	cards.Observe(0, {}, {}, 30000);
	EXPECT(cards.Published()->empty() && shown->size() == 1);
	cards.Observe(1, {"c1.b"}, {0}, 40000);
	EXPECT(cards.Take(40000) == Batch{"c1.b"});
	Look(cards, 3, 40000, Answers{Showing(1, 1)});
	cards.Scope("brg_2");
	EXPECT(cards.Published()->empty() && cards.Take(100000).empty());
	cards.Observe(1, {"c1.b"}, {0}, 100000);
	EXPECT(cards.Take(100000) == Batch{"c1.b"});
	// A look-up in flight when the room changed is not awaited any more.
	cards.Sent(4, cards.Take(100000), 100000);
	cards.Observe(2, {"c1.b"}, {0}, 100100);
	EXPECT(!cards.Awaiting(4));
}

// A bridge without levels stops look-ups until the room changes; a failed
// look-up only waits for the gap.
static void Unavailable() {
	Cards cards;
	Scope(cards);
	cards.Observe(1, {"c1.a"}, {0}, 0);
	cards.Sent(1, cards.Take(0), 0);
	cards.Finish("bridge_unreachable", std::nullopt);
	EXPECT(cards.Error() == "bridge_unreachable" && cards.Take(Cards::MinGapMs - 1).empty() && cards.Take(Cards::MinGapMs).size() == 1);
	cards.Sent(2, cards.Take(Cards::MinGapMs), Cards::MinGapMs);
	cards.Finish(Cards::Unavailable, std::nullopt);
	EXPECT(cards.Take(1000000).empty());
	cards.Observe(1, {"c1.a", "c1.b"}, {0}, 1000000);
	EXPECT(cards.Take(1000000).empty());
	cards.Observe(2, {"c1.a"}, {0}, 1000100);
	EXPECT(cards.Take(1000100).size() == 1);
	// An answer of the wrong length installs nothing.
	cards.Sent(3, cards.Take(1000100), 1000100);
	cards.Finish("", Answers{});
	EXPECT(cards.Published()->empty() && cards.Take(1000100 + Cards::MinGapMs).size() == 1);
}

int main() {
	JoinAsks();
	Limits();
	SetEndAsks();
	Pruning();
	Unavailable();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("card request tests passed\n");
	return TEST_EXIT_CODE();
}
