// The runtime's card look-ups (netplay/CardRequests.hxx): a join and a set end
// trigger them, a look-up is up to 16 tokens, one at a time and one every 10 s,
// null is remembered until the next trigger, leaving forgets, and a bridge
// without levels stops them until the room changes. Then the profile open and Find a player's
// search (netplay/ProfileRequests.hxx): one profile at a time, a name searched and its one hit
// looked up, a newer search dropping the older, a card kept fresh, a service without profiles.
#include "../netplay/CardRequests.hxx"
#include "../netplay/ProfileRequests.hxx"

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

static Card Related(Relation relation) {
	Card card = Showing(2, 10);
	card.relation = relation;
	return card;
}

// A friends action that went through: the member it named shows what it made of them at once, every card is asked
// again at the next look-up, and a look-up asked before the action does not undo what it shows.
static void ActionRefreshes() {
	Cards cards;
	Scope(cards);
	const Batch both = {"c1.a", "c1.b"};
	cards.Observe(1, both, {0}, 0);
	Look(cards, 1, 0, Answers{Related(Relation::None), std::nullopt});
	EXPECT(cards.Take(1000000).empty());
	// Add friend went through: Request sent at once, on a new map; a token without a card is left alone.
	const auto before = cards.Published();
	cards.Refresh();
	cards.Relate("c1.a", Relation::Sent);
	cards.Relate("c1.b", Relation::Sent);
	cards.Relate("c1.z", Relation::Sent);
	EXPECT(cards.Published() != before && before->at("c1.a").relation == Relation::None);
	EXPECT(cards.Published()->at("c1.a").relation == Relation::Sent && cards.Published()->size() == 1);
	// Both are asked again after the gap, and the answer says how it really stands.
	EXPECT(cards.Take(Cards::MinGapMs - 1).empty() && cards.Take(Cards::MinGapMs) == both);
	Look(cards, 2, Cards::MinGapMs, Answers{Related(Relation::Friend), std::nullopt});
	EXPECT(cards.Published()->at("c1.a").relation == Relation::Friend && cards.Take(1000000).empty());
	// A look-up in flight as an action goes through answers with the card as it stood before: not shown, asked again.
	Cards flying;
	Scope(flying);
	flying.Observe(1, both, {0}, 0);
	Look(flying, 1, 0, Answers{Related(Relation::None), Related(Relation::Received)});
	flying.Observe(1, both, {9}, 1000);
	flying.Observe(1, both, {9}, 1000 + Cards::SetEndedMs);
	flying.Sent(2, flying.Take(Cards::MinGapMs), Cards::MinGapMs);
	flying.Refresh();
	flying.Relate("c1.b", Relation::Friend);
	flying.Finish("", Answers{Related(Relation::None), Related(Relation::Received)});
	EXPECT(flying.Published()->at("c1.b").relation == Relation::Friend && flying.Published()->at("c1.a").relation == Relation::None);
	EXPECT(flying.Take(2 * Cards::MinGapMs) == both);
	Look(flying, 3, 2 * Cards::MinGapMs, Answers{Related(Relation::None), Related(Relation::Friend)});
	EXPECT(flying.Published()->at("c1.b").relation == Relation::Friend && flying.Take(1000000).empty());
}


// ---- The profile open and Find a player's search (netplay/ProfileRequests.hxx) ----

namespace profiles = sf4e::netplay::profiles;
using Profile = profiles::Profile;
using Asked = profiles::Requests::Ask;

static Card WithProfile(const std::string& name) {
	Card card;
	card.account = {2100, 2};
	card.name = name;
	card.hasSince = true;
	card.since = 1748736000;
	return card;
}

static profiles::Looked Found(const Card& card) {
	profiles::Looked looked;
	looked.read = true;
	looked.card = card;
	return looked;
}

static profiles::Looked NoCard() {
	profiles::Looked looked;
	looked.read = true;
	return looked;
}

static profiles::Hit HitOf(const std::string& token, const std::string& name, const char* country = "FR", std::uint8_t main = 5) {
	profiles::Hit hit;
	hit.card = token;
	hit.name = name;
	hit.country = sf4e::CountryCode::Parsed(country);
	hit.hasMain = true;
	hit.main = main;
	return hit;
}

static profiles::SearchAnswer Players(std::vector<profiles::Hit> hits, bool more = false) {
	profiles::SearchAnswer answer;
	answer.players = std::move(hits);
	answer.more = more;
	return answer;
}

static void OpenIn(profiles::Requests& requests, const profiles::Key& key, std::uint64_t nowMs = 0) {
	requests.Scope("brg_1", "emb_1");
	requests.Open(key, profiles::Known(), nowMs);
}

// A token, a friend id, a request and a code are each looked up at once, by their own selector; the page has what the
// opener knew until the card arrives, and only the token is something the player can be acted on by.
static void ProfileIsLookedUpBySelector() {
	struct Case { profiles::Key key; profiles::Key::Kind selector; const char* token; };
	const Case cases[] = {{profiles::Key::OfCard("c1.a"), profiles::Key::Kind::Card, "c1.a"}, {profiles::Key::OfFriend("fr_1"), profiles::Key::Kind::Friend, ""},
		{profiles::Key::OfRequest("rq_1"), profiles::Key::Kind::Request, ""}, {profiles::Key::OfCode("4F2A-91CX"), profiles::Key::Kind::Code, ""}};
	for (const auto& one : cases) {
		profiles::Requests requests;
		requests.Scope("brg_1", "emb_1");
		profiles::Known known;
		known.name = "Player 2";
		known.fighter = 5;
		requests.Open(one.key, known, 0);
		auto shown = requests.Published();
		EXPECT(shown.state == Profile::State::Reading && shown.known.name == "Player 2" && shown.known.fighter == 5 && shown.key == one.key);
		EXPECT(shown.card.loading && !shown.card.loaded && shown.token == one.token && shown.opened == 1 && shown.Open());
		const auto ask = requests.Take(0);
		EXPECT(ask && ask.kind == Asked::Kind::Lookup && ask.selector == one.selector && ask.value == one.key.value && ask.ticket != 0);
		// One at a time: nothing more until it is answered.
		EXPECT(!requests.Take(1000000));
		EXPECT(requests.FinishLookup(ask.ticket, "", Found(WithProfile("Player 2")), 100));
		shown = requests.Published();
		EXPECT(shown.state == Profile::State::Card && shown.card.loaded && !shown.card.loading && shown.card.value.name == "Player 2");
		EXPECT(shown.card.finished == 1 && shown.card.revision == 1 && shown.card.installedMs == 100 && shown.error.empty());
		EXPECT(requests.Offering() == profiles::Service::Offered);
		// The answer counts once.
		EXPECT(!requests.FinishLookup(ask.ticket, "", Found(WithProfile("Player 2")), 100));
	}
	profiles::Requests none;
	none.Scope("brg_1", "emb_1");
	none.Open(profiles::Key(), profiles::Known(), 0);
	EXPECT(none.Published().state == Profile::State::Closed && !none.Take(0));
}

// One profile at a time: opening another replaces the one open, and the answer asked for the one before is dropped.
static void OneProfileAtATime() {
	profiles::Requests requests;
	OpenIn(requests, profiles::Key::OfCard("c1.a"));
	const auto first = requests.Take(0);
	OpenIn(requests, profiles::Key::OfCard("c1.b"), 10);
	EXPECT(requests.Published().opened == 2 && requests.Published().key.value == "c1.b" && requests.Published().state == Profile::State::Reading);
	EXPECT(!requests.FinishLookup(first.ticket, "", Found(WithProfile("A")), 20));
	EXPECT(requests.Published().state == Profile::State::Reading && !requests.Published().card.loaded);
	const auto second = requests.Take(10);
	EXPECT(second && second.value == "c1.b" && second.ticket != first.ticket);
	EXPECT(requests.FinishLookup(second.ticket, "", Found(WithProfile("B")), 30) && requests.Published().card.value.name == "B");
	// Closing drops what is on its way, and nothing is asked for a closed profile.
	OpenIn(requests, profiles::Key::OfCard("c1.c"), 40);
	const auto third = requests.Take(40);
	requests.Close();
	EXPECT(requests.Published().state == Profile::State::Closed && !requests.Published().Open());
	EXPECT(!requests.FinishLookup(third.ticket, "", Found(WithProfile("C")), 50) && !requests.Take(1000000));
	EXPECT(requests.Published().opened == 3);
	// The same key opened again while its card is on show only refreshes it, at the pace a refresh keeps.
	OpenIn(requests, profiles::Key::OfCard("c1.d"), 60);
	const auto fourth = requests.Take(60);
	requests.FinishLookup(fourth.ticket, "", Found(WithProfile("D")), 70);
	requests.Open(profiles::Key::OfCard("c1.d"), profiles::Known(), 80);
	EXPECT(requests.Published().opened == 4 && requests.Published().state == Profile::State::Card && requests.Published().card.loaded);
	EXPECT(!requests.Take(60 + profiles::Requests::MinRefreshMs - 1));
	EXPECT(requests.Take(60 + profiles::Requests::MinRefreshMs));
}

// A name is searched for first, in any case; the one player found is looked up, and what the hit says fills in what the
// opener did not know.
static void ANameWithOneHitIsLookedUp() {
	profiles::Requests requests;
	OpenIn(requests, profiles::Key::OfName("Player 2"));
	EXPECT(requests.Published().state == Profile::State::Reading && requests.Published().token.empty());
	const auto search = requests.Take(0);
	EXPECT(search && search.kind == Asked::Kind::Search && search.selector == profiles::Key::Kind::Name && search.value == "Player 2");
	EXPECT(!requests.Take(1000000));
	// Names equal to the text come first, others after them; only the equal ones count.
	EXPECT(requests.FinishSearch(search.ticket, "", Players({HitOf("c1.a", "player 2"), HitOf("c1.b", "Player 22")}), 10));
	auto shown = requests.Published();
	EXPECT(shown.state == Profile::State::Reading && shown.token == "c1.a" && shown.known.name == "player 2" && shown.known.country.ToString() == "FR" && shown.known.fighter == 5);
	const auto lookup = requests.Take(10);
	EXPECT(lookup && lookup.kind == Asked::Kind::Lookup && lookup.selector == profiles::Key::Kind::Card && lookup.value == "c1.a");
	EXPECT(requests.FinishLookup(lookup.ticket, "", Found(WithProfile("player 2")), 20));
	shown = requests.Published();
	EXPECT(shown.state == Profile::State::Card && shown.card.loaded && shown.token == "c1.a" && shown.card.finished == 1);
	EXPECT(requests.Offering() == profiles::Service::Offered);
	// What the opener knew is not replaced by the hit.
	profiles::Requests knowing;
	knowing.Scope("brg_1", "emb_1");
	profiles::Known known;
	known.name = "Player 2";
	known.country = sf4e::CountryCode::Parsed("DE");
	known.fighter = 9;
	knowing.Open(profiles::Key::OfName("Player 2"), known, 0);
	knowing.FinishSearch(knowing.Take(0).ticket, "", Players({HitOf("c1.a", "Player 2")}), 1);
	EXPECT(knowing.Published().known.country.ToString() == "DE" && knowing.Published().known.fighter == 9 && knowing.Published().known.name == "Player 2");
}

// Nobody goes by it, or several do: the page says so, or asks which; the opener's country settles it when it names one.
static void ANameWithNoOrManyHits() {
	profiles::Requests nobody;
	OpenIn(nobody, profiles::Key::OfName("Player 2"));
	EXPECT(nobody.FinishSearch(nobody.Take(0).ticket, "", Players({HitOf("c1.b", "Player 22")}), 5));
	EXPECT(nobody.Published().state == Profile::State::Nobody && nobody.Published().hits.empty() && !nobody.Published().card.loading && !nobody.Take(1000000));
	profiles::Requests emptied;
	OpenIn(emptied, profiles::Key::OfName("Player 2"));
	EXPECT(emptied.FinishSearch(emptied.Take(0).ticket, "", Players({}), 5) && emptied.Published().state == Profile::State::Nobody);
	profiles::Requests several;
	OpenIn(several, profiles::Key::OfName("Player 2"));
	EXPECT(several.FinishSearch(several.Take(0).ticket, "", Players({HitOf("c1.a", "Player 2", "FR"), HitOf("c1.b", "PLAYER 2", "DE"), HitOf("c1.c", "Player 22")}), 5));
	EXPECT(several.Published().state == Profile::State::Several && several.Published().hits.size() == 2 && several.Published().hits[1].card == "c1.b");
	EXPECT(!several.Take(1000000) && several.Published().token.empty());
	// The page opens the one chosen, by its token, in place of this one.
	several.Open(profiles::Key::OfCard("c1.b"), profiles::Known(), 6);
	EXPECT(several.Published().state == Profile::State::Reading && several.Published().hits.empty() && several.Take(6).selector == profiles::Key::Kind::Card);
	// The country of the opener names one of them.
	profiles::Requests chosen;
	chosen.Scope("brg_1", "emb_1");
	chosen.Open(profiles::Key::OfName("Player 2", sf4e::CountryCode::Parsed("DE")), profiles::Known(), 0);
	chosen.FinishSearch(chosen.Take(0).ticket, "", Players({HitOf("c1.a", "Player 2", "FR"), HitOf("c1.b", "Player 2", "DE")}), 5);
	EXPECT(chosen.Published().state == Profile::State::Reading && chosen.Published().token == "c1.b" && chosen.Take(5).value == "c1.b");
	// A country that names none of them settles nothing.
	profiles::Requests unsettled;
	unsettled.Scope("brg_1", "emb_1");
	unsettled.Open(profiles::Key::OfName("Player 2", sf4e::CountryCode::Parsed("US")), profiles::Known(), 0);
	unsettled.FinishSearch(unsettled.Take(0).ticket, "", Players({HitOf("c1.a", "Player 2", "FR"), HitOf("c1.b", "Player 2", "DE")}), 5);
	EXPECT(unsettled.Published().state == Profile::State::Several);
}

// A search that fails is unreachable; a service without profiles is unsupported and is not asked again until it is
// believed to have changed.
static void ANameNeedsProfiles() {
	profiles::Requests down;
	OpenIn(down, profiles::Key::OfName("Player 2"));
	EXPECT(down.FinishSearch(down.Take(0).ticket, "http_502", std::nullopt, 5));
	EXPECT(down.Published().state == Profile::State::Unreachable && down.Published().error == "http_502" && down.Offering() == profiles::Service::Unknown);
	profiles::Requests unreadable;
	OpenIn(unreadable, profiles::Key::OfName("Player 2"));
	EXPECT(unreadable.FinishSearch(unreadable.Take(0).ticket, "", std::nullopt, 5) && unreadable.Published().error == "bridge_invalid_response");
	profiles::Requests missing;
	OpenIn(missing, profiles::Key::OfName("Player 2"));
	EXPECT(missing.FinishSearch(missing.Take(0).ticket, profiles::Unavailable, std::nullopt, 1000));
	EXPECT(missing.Published().state == Profile::State::Unsupported && missing.Published().error == "profiles_unavailable" && missing.Offering() == profiles::Service::Missing);
	// A name, a request or a code is then answered at once without asking; a token or a friend id is still asked.
	for (const auto& key : {profiles::Key::OfName("Player 3"), profiles::Key::OfRequest("rq_1"), profiles::Key::OfCode("4F2A-91CX")}) {
		missing.Open(key, profiles::Known(), 2000);
		EXPECT(missing.Published().state == Profile::State::Unsupported && !missing.Take(2000));
	}
	missing.Open(profiles::Key::OfFriend("fr_1"), profiles::Known(), 2000);
	EXPECT(missing.Published().state == Profile::State::Reading && missing.Take(2000).selector == profiles::Key::Kind::Friend);
	// After a while it is asked again, so a service upgraded meanwhile is heard.
	missing.Tick(1000 + profiles::Requests::RecheckMs - 1);
	EXPECT(missing.Offering() == profiles::Service::Missing);
	missing.Tick(1000 + profiles::Requests::RecheckMs);
	EXPECT(missing.Offering() == profiles::Service::Unknown);
	missing.Open(profiles::Key::OfName("Player 3"), profiles::Known(), 1000 + profiles::Requests::RecheckMs);
	EXPECT(missing.Published().state == Profile::State::Reading && missing.Take(1000 + profiles::Requests::RecheckMs).kind == Asked::Kind::Search);
	// A card save or a search that says the same is heard too.
	profiles::Requests told;
	told.Scope("brg_1", "emb_1");
	told.NoteService(profiles::Service::Missing, 50);
	EXPECT(told.Offering() == profiles::Service::Missing);
	told.NoteService(profiles::Service::Offered, 60);
	EXPECT(told.Offering() == profiles::Service::Offered);
}

// No card is no card; a card on show stays when its refresh fails; a service without cards says so.
static void ProfileAnswersThatAreNotACard() {
	profiles::Requests none;
	OpenIn(none, profiles::Key::OfCard("c1.a"));
	EXPECT(none.FinishLookup(none.Take(0).ticket, "", NoCard(), 5));
	EXPECT(none.Published().state == Profile::State::NoCard && !none.Published().card.loaded && !none.Published().card.loading && none.Published().card.finished == 1);
	profiles::Requests failed;
	OpenIn(failed, profiles::Key::OfCard("c1.a"));
	EXPECT(failed.FinishLookup(failed.Take(0).ticket, "http_502", profiles::Looked(), 5));
	EXPECT(failed.Published().state == Profile::State::Unreachable && failed.Published().error == "http_502");
	profiles::Requests unread;
	OpenIn(unread, profiles::Key::OfCard("c1.a"));
	EXPECT(unread.FinishLookup(unread.Take(0).ticket, "", profiles::Looked(), 5) && unread.Published().error == "bridge_invalid_response");
	profiles::Requests levelless;
	OpenIn(levelless, profiles::Key::OfCard("c1.a"));
	EXPECT(levelless.FinishLookup(levelless.Take(0).ticket, profiles::Requests::NoPoints, profiles::Looked(), 5));
	EXPECT(levelless.Published().state == Profile::State::Unsupported && levelless.Published().error == "points_unavailable");
	// A profile code asked of a service without profiles.
	profiles::Requests coded;
	OpenIn(coded, profiles::Key::OfCode("4F2A-91CX"));
	EXPECT(coded.FinishLookup(coded.Take(0).ticket, profiles::Unavailable, profiles::Looked(), 5));
	EXPECT(coded.Published().state == Profile::State::Unsupported && coded.Offering() == profiles::Service::Missing);
	// A refresh that fails keeps the card; one that finds no card says there is none.
	profiles::Requests shown;
	OpenIn(shown, profiles::Key::OfCard("c1.a"));
	shown.FinishLookup(shown.Take(0).ticket, "", Found(WithProfile("A")), 5);
	shown.Refresh();
	const auto again = shown.Take(profiles::Requests::MinRefreshMs);
	EXPECT(again && again.kind == Asked::Kind::Lookup && shown.Published().card.loading && shown.Published().card.loaded);
	shown.FinishLookup(again.ticket, "unavailable", profiles::Looked(), 6000);
	EXPECT(shown.Published().state == Profile::State::Card && shown.Published().card.value.name == "A" && shown.Published().card.error == "unavailable" && !shown.Published().card.loading);
	EXPECT(shown.Published().card.finished == 2 && shown.Published().card.revision == 1);
	shown.Refresh();
	const auto gone = shown.Take(2 * profiles::Requests::MinRefreshMs + 6000);
	shown.FinishLookup(gone.ticket, "", NoCard(), 20000);
	EXPECT(shown.Published().state == Profile::State::NoCard && !shown.Published().card.loaded);
}

// The card is looked up again 8 s after a set in the room ends, and after a friends action, never closer together than
// MinRefreshMs.
static void TheCardIsKeptFresh() {
	profiles::Requests requests;
	OpenIn(requests, profiles::Key::OfCard("c1.a"));
	requests.FinishLookup(requests.Take(0).ticket, "", Found(WithProfile("A")), 5);
	requests.Observe(1, {0, 0}, 0);
	EXPECT(!requests.Take(100000));
	requests.Observe(1, {9, 0}, 20000);
	requests.Observe(1, {9, 0}, 20000 + profiles::Requests::SetEndedMs - 1);
	EXPECT(!requests.Take(20000 + profiles::Requests::SetEndedMs - 1));
	requests.Observe(1, {9, 0}, 20000 + profiles::Requests::SetEndedMs);
	const auto fresh = requests.Take(20000 + profiles::Requests::SetEndedMs);
	EXPECT(fresh && fresh.kind == Asked::Kind::Lookup && fresh.value == "c1.a");
	requests.FinishLookup(fresh.ticket, "", Found(WithProfile("A2")), 30000);
	EXPECT(requests.Published().card.value.name == "A2" && requests.Published().card.revision == 2 && !requests.Take(1000000));
	// The first look at a room only notes what ended; the same sets seen again change nothing.
	requests.Observe(2, {5, 0}, 40000);
	requests.Observe(2, {5, 0}, 60000);
	EXPECT(!requests.Take(1000000));
	// A friends action refreshes it.
	requests.Refresh();
	const auto action = requests.Take(1000000);
	EXPECT(action && action.kind == Asked::Kind::Lookup);
	requests.FinishLookup(action.ticket, "", Found(WithProfile("A3")), 1000001);
	// One asked before the action went through is older than what the card shows: kept, and asked again after the gap.
	requests.Refresh();
	const auto flying = requests.Take(2000000);
	requests.Refresh();
	requests.FinishLookup(flying.ticket, "", Found(WithProfile("A4")), 2000001);
	EXPECT(requests.Published().card.value.name == "A4" && !requests.Take(2000000 + profiles::Requests::MinRefreshMs - 1));
	EXPECT(requests.Take(2000000 + profiles::Requests::MinRefreshMs));
	// A profile that is not open is not refreshed.
	profiles::Requests idle;
	idle.Scope("brg_1", "emb_1");
	idle.Refresh();
	idle.Observe(1, {0}, 0);
	idle.Observe(1, {3}, 1);
	idle.Observe(1, {3}, 100000);
	EXPECT(!idle.Take(1000000));
}

// A friends action named the player: their card shows what it made of them at once, until the look-up says how it stands.
static void AnActionRelatesTheCard() {
	profiles::Requests requests;
	OpenIn(requests, profiles::Key::OfCard("c1.a"));
	Card card = WithProfile("A");
	requests.FinishLookup(requests.Take(0).ticket, "", Found(card), 5);
	requests.Relate("c1.b", Relation::Friend);
	EXPECT(requests.Published().card.value.relation == Relation::None && requests.Published().card.revision == 1);
	requests.Relate("c1.a", Relation::Sent);
	EXPECT(requests.Published().card.value.relation == Relation::Sent && requests.Published().card.revision == 2);
	EXPECT(requests.Take(profiles::Requests::MinRefreshMs).kind == Asked::Kind::Lookup);
	// A request to a player who had asked first is a friendship; a block is a block.
	profiles::Requests asked;
	OpenIn(asked, profiles::Key::OfCard("c1.a"));
	card.relation = Relation::Received;
	asked.FinishLookup(asked.Take(0).ticket, "", Found(card), 5);
	asked.Relate("c1.a", Relation::Sent);
	EXPECT(asked.Published().card.value.relation == Relation::Friend);
	asked.Relate("c1.a", Relation::Blocked);
	EXPECT(asked.Published().card.value.relation == Relation::Blocked);
	// A profile opened by a friend id has no token to name; one with no card shows nothing.
	profiles::Requests byFriend;
	OpenIn(byFriend, profiles::Key::OfFriend("fr_1"));
	byFriend.Relate("c1.a", Relation::Friend);
	EXPECT(byFriend.Published().card.value.relation == Relation::None);
	byFriend.FinishLookup(byFriend.Take(0).ticket, "", Found(WithProfile("F")), 5);
	byFriend.Relate("", Relation::Friend);
	byFriend.Relate("c1.a", Relation::Friend);
	EXPECT(byFriend.Published().card.value.relation == Relation::None);
}

// Find a player: one search in flight; a newer text drops the older answer; the same text asked again while it is on
// its way is not asked twice.
static void ANewerSearchDropsTheOlder() {
	profiles::Requests requests;
	requests.Scope("brg_1", "emb_1");
	requests.Search("Pla", false, 0);
	auto view = requests.SearchPublished();
	EXPECT(view.text == "Pla" && !view.byCode && view.answer.loading && !view.answer.loaded);
	const auto first = requests.Take(0);
	EXPECT(first && first.kind == Asked::Kind::Search && first.selector == profiles::Key::Kind::Name && first.value == "Pla");
	// Typed again as it is: no second search.
	requests.Search("Pla", false, 10);
	EXPECT(!requests.Take(10));
	requests.Search("Play", false, 20);
	EXPECT(requests.SearchPublished().text == "Play" && requests.SearchPublished().answer.loading);
	// The older answer arrives late: it is dropped, and the newer is asked for.
	EXPECT(!requests.FinishSearch(first.ticket, "", Players({HitOf("c1.a", "Player 2")}), 30));
	EXPECT(requests.SearchPublished().answer.loading && !requests.SearchPublished().answer.loaded);
	const auto second = requests.Take(30);
	EXPECT(second && second.value == "Play" && second.ticket != first.ticket);
	EXPECT(requests.FinishSearch(second.ticket, "", Players({HitOf("c1.a", "Player 2"), HitOf("c1.b", "Player 3")}, true), 40));
	view = requests.SearchPublished();
	EXPECT(view.text == "Play" && view.answer.loaded && !view.answer.loading && view.answer.value.players.size() == 2 && view.answer.value.more);
	EXPECT(view.answer.finished == 1 && view.answer.revision == 1 && view.answer.installedMs == 40 && view.answer.error.empty());
	EXPECT(requests.Offering() == profiles::Service::Offered);
	// The same text again, after it was answered, is asked again (the player pressed Search).
	requests.Search("Play", false, 50);
	EXPECT(requests.Take(50).value == "Play");
	// A friend code is its own kind of search.
	requests.Search("4F2A-91CX", true, 60);
	const auto coded = requests.Take(60);
	EXPECT(coded.selector == profiles::Key::Kind::Code && coded.value == "4F2A-91CX" && requests.SearchPublished().byCode);
	// An empty text clears the search and drops what is on its way.
	requests.Search("", false, 70);
	EXPECT(requests.SearchPublished().text.empty() && !requests.SearchPublished().answer.loading && !requests.SearchPublished().answer.loaded);
	EXPECT(!requests.FinishSearch(coded.ticket, "", Players({}), 80) && !requests.Take(1000000));
}

static void ASearchThatFails() {
	profiles::Requests requests;
	requests.Scope("brg_1", "emb_1");
	requests.Search("Play", false, 0);
	EXPECT(requests.FinishSearch(requests.Take(0).ticket, "", Players({HitOf("c1.a", "Player 2")}), 5));
	requests.Search("Playe", false, 10);
	EXPECT(requests.FinishSearch(requests.Take(10).ticket, "rate_limited", std::nullopt, 15));
	const auto view = requests.SearchPublished();
	// The older answer was dropped when the newer text was asked, and a refusal leaves its code.
	EXPECT(!view.answer.loaded && !view.answer.loading && view.answer.error == "rate_limited" && view.answer.value.players.empty() && view.answer.finished == 2);
	requests.Search("Player", false, 20);
	EXPECT(requests.FinishSearch(requests.Take(20).ticket, "", std::nullopt, 25));
	EXPECT(requests.SearchPublished().answer.error == "bridge_invalid_response");
	// A service without profiles is told at once, and not asked.
	requests.Search("Player 2", false, 30);
	EXPECT(requests.FinishSearch(requests.Take(30).ticket, profiles::Unavailable, std::nullopt, 35));
	EXPECT(requests.Offering() == profiles::Service::Missing && requests.SearchPublished().answer.error == "profiles_unavailable");
	requests.Search("Player 3", false, 40);
	EXPECT(!requests.Take(40) && requests.SearchPublished().answer.error == "profiles_unavailable" && !requests.SearchPublished().answer.loading);
	EXPECT(requests.SearchPublished().text == "Player 3");
}

// The profile and the search are separate slots: each is asked for in turn, answered on its own, and neither replaces
// the other.
static void TheSlotsAreSeparate() {
	profiles::Requests requests;
	requests.Scope("brg_1", "emb_1");
	requests.Open(profiles::Key::OfCard("c1.a"), profiles::Known(), 0);
	requests.Search("Play", false, 0);
	const auto lookup = requests.Take(0);
	const auto search = requests.Take(0);
	EXPECT(lookup.kind == Asked::Kind::Lookup && search.kind == Asked::Kind::Search && lookup.ticket != search.ticket && !requests.Take(0));
	EXPECT(requests.FinishSearch(search.ticket, "", Players({HitOf("c1.b", "Player 3")}), 5));
	EXPECT(requests.Published().state == Profile::State::Reading && requests.SearchPublished().answer.loaded);
	EXPECT(requests.FinishLookup(lookup.ticket, "", Found(WithProfile("A")), 6));
	EXPECT(requests.Published().state == Profile::State::Card && requests.SearchPublished().answer.value.players.size() == 1);
	// A lookup's ticket never answers a search.
	requests.Search("Playe", false, 10);
	const auto next = requests.Take(10);
	EXPECT(!requests.FinishLookup(next.ticket, "", Found(WithProfile("X")), 11) && requests.SearchPublished().answer.loading);
	requests.Close();
	EXPECT(requests.SearchPublished().text == "Playe");
}

// The friend the Friends tab focuses is looked up with the members' cards, by friend id: alone it waits the short gap, a
// look-up with a member's token the long one, newest friend first, the last MaxFriends are kept, and a failure puts the long
// gap back (the bridge counts every look-up against one budget).
static void FriendCards() {
	EXPECT(Cards::IsFriendKey("fr_1") && !Cards::IsFriendKey("c1.abc") && !Cards::IsFriendKey("rq_1"));
	{
		Cards cards;
		Scope(cards);
		cards.Observe(0, {}, {}, 0);
		cards.WantFriend("c1.abc");
		EXPECT(cards.Take(0).empty());
		cards.WantFriend("fr_1");
		EXPECT((cards.Take(0) == Batch{"fr_1"}));
		cards.Sent(1, {"fr_1"}, 0);
		cards.Finish("", Answers{Showing(3, 31)});
		EXPECT(cards.Published()->size() == 1 && cards.Published()->at("fr_1").fighters[0].points == 31 && cards.Take(1000000).empty());
		cards.WantFriend("fr_2");
		cards.WantFriend("fr_3");
		EXPECT(cards.Take(Cards::FriendGapMs - 1).empty());
		EXPECT((cards.Take(Cards::FriendGapMs) == Batch{"fr_3", "fr_2"}));
		// Asked for again, a friend moves to the front, and their answered card is not asked for twice.
		cards.WantFriend("fr_1");
		EXPECT((cards.Take(Cards::FriendGapMs) == Batch{"fr_3", "fr_2"}));
		// Another room forgets the answers and keeps the wishes.
		cards.Observe(2, {}, {}, 5000);
		EXPECT(cards.Published()->empty() && (cards.Take(Cards::FriendGapMs) == Batch{"fr_1", "fr_3", "fr_2"}));
	}
	{
		// A member's token goes first and makes the look-up wait the long gap; friends alone may come sooner after.
		Cards cards;
		Scope(cards);
		cards.WantFriend("fr_1");
		cards.Observe(1, {"c1.a"}, {0}, 0);
		EXPECT((cards.Take(0) == Batch{"c1.a", "fr_1"}));
		cards.Sent(1, cards.Take(0), 0);
		cards.Finish("", Answers{Showing(1, 1), std::nullopt});
		EXPECT(cards.Published()->size() == 1 && cards.Take(1000000).empty());
		cards.Observe(1, {"c1.a", "c1.b"}, {0}, 100);
		EXPECT(cards.Take(Cards::FriendGapMs).empty() && (cards.Take(Cards::MinGapMs) == Batch{"c1.b"}));
		cards.Sent(2, {"c1.b"}, Cards::MinGapMs);
		cards.Finish("", Answers{Showing(2, 2)});
		cards.WantFriend("fr_2");
		EXPECT((cards.Take(Cards::MinGapMs + Cards::FriendGapMs) == Batch{"fr_2"}));
	}
	{
		// A look-up that failed is asked again no sooner than the room's gap.
		Cards cards;
		Scope(cards);
		cards.WantFriend("fr_1");
		cards.Sent(1, cards.Take(0), 0);
		cards.Finish("rate_limited", std::nullopt);
		EXPECT(cards.Take(Cards::FriendGapMs).empty() && cards.Take(Cards::MinGapMs).size() == 1);
	}
	{
		// The last MaxFriends are kept, newest first; one at most MaxLookup to a look-up.
		Cards cards;
		Scope(cards);
		for (int i = 0; i < 20; ++i) cards.WantFriend("fr_" + std::to_string(i));
		const Batch batch = cards.Take(0);
		EXPECT(batch.size() == Cards::MaxFriends && batch.front() == "fr_19" && batch.back() == "fr_4");
	}
	{
		// A wish made before any service was set stays; another service drops them, as it does the answers.
		Cards cards;
		cards.WantFriend("fr_1");
		cards.Scope("brg_1");
		EXPECT((cards.Take(0) == Batch{"fr_1"}));
		cards.Scope("brg_2");
		EXPECT(cards.Take(0).empty());
	}
}

// A new service or Ember ID forgets everything; the counts carry on, so a reader can tell a newer answer.
static void AnotherServiceForgets() {
	profiles::Requests requests;
	OpenIn(requests, profiles::Key::OfCard("c1.a"));
	requests.FinishLookup(requests.Take(0).ticket, "", Found(WithProfile("A")), 5);
	requests.Search("Play", false, 6);
	requests.FinishSearch(requests.Take(6).ticket, "", Players({}), 7);
	requests.Scope("brg_1", "emb_1");
	EXPECT(requests.Published().state == Profile::State::Card && requests.SearchPublished().answer.loaded);
	requests.Scope("brg_2", "emb_1");
	EXPECT(requests.Published().state == Profile::State::Closed && requests.SearchPublished().text.empty() && requests.Offering() == profiles::Service::Unknown);
	EXPECT(requests.Published().opened == 1 && requests.Published().card.finished == 1 && requests.Published().card.revision == 1 && requests.SearchPublished().answer.finished == 1);
	EXPECT(requests.Bridge() == "brg_2" && requests.EmberId() == "emb_1" && !requests.Take(1000000));
	requests.Open(profiles::Key::OfCard("c1.b"), profiles::Known(), 100);
	EXPECT(requests.Published().opened == 2);
	requests.Scope("brg_2", "emb_2");
	EXPECT(requests.Published().state == Profile::State::Closed);
}

int main() {
	JoinAsks();
	Limits();
	SetEndAsks();
	Pruning();
	Unavailable();
	ActionRefreshes();
	ProfileIsLookedUpBySelector();
	OneProfileAtATime();
	ANameWithOneHitIsLookedUp();
	ANameWithNoOrManyHits();
	ANameNeedsProfiles();
	ProfileAnswersThatAreNotACard();
	TheCardIsKeptFresh();
	AnActionRelatesTheCard();
	ANewerSearchDropsTheOlder();
	ASearchThatFails();
	TheSlotsAreSeparate();
	AnotherServiceForgets();
	FriendCards();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("card request tests passed\n");
	return TEST_EXIT_CODE();
}
