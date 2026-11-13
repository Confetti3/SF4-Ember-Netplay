// The milestone awards (netplay/Awards.hxx): the catalog's shape, what earns
// each award from a player's own counters and from another player's card, the
// check that keeps a made-up worn award from showing, and the notices that
// announce an award once and never at the first answer of a run.
#include "../netplay/Awards.hxx"

#include <cstdio>
#include <set>
#include <string>
#include "test_support.hxx"

using namespace sf4e::netplay::awards;
using sf4e::netplay::levels::Card;
using sf4e::netplay::levels::CardFighter;
using sf4e::netplay::levels::Own;

static const Award& Get(const char* id) {
	const Award* award = Find(id);
	CHECK(award != nullptr);
	return *award;
}

// Ids are unique, every award has words, a tier of the five and an icon, the
// families run in the wall's order and each family climbs.
static void CatalogShape() {
	std::set<std::string> ids;
	int family = 0;
	for (std::size_t i = 0; i < CatalogSize; ++i) {
		const auto& award = Catalog[i];
		EXPECT(ids.insert(award.id).second);
		EXPECT(IdShaped(award.id));
		EXPECT(award.how && *award.how && award.title);
		EXPECT(award.tier >= 0 && award.tier < sf4e::netplay::levels::TierCount);
		EXPECT(award.icon >= 0 && award.icon < 12);
		EXPECT(static_cast<int>(award.family) >= family && static_cast<int>(award.family) < FamilyCount);
		family = static_cast<int>(award.family);
		if (i > 0 && Catalog[i - 1].family == award.family) {
			EXPECT(Catalog[i - 1].need < award.need && Catalog[i - 1].tier <= award.tier);
			EXPECT(Catalog[i - 1].counter == award.counter);
		}
	}
	EXPECT(CatalogSize == 32);
	// Every family is present.
	std::set<int> present;
	for (const auto& award : Catalog) present.insert(static_cast<int>(award.family));
	EXPECT(static_cast<int>(present.size()) == FamilyCount);
}

// One award a class from the second up to the top, each named by its band, the
// lowest Ash and the top Ember; a class award's words are the shared ones.
static void ClassAwards() {
	int bands = 0, previous = 0, previousTier = -1;
	for (const auto& award : Catalog) {
		if (award.family != Family::Class) { EXPECT(award.band == 0); continue; }
		++bands;
		EXPECT(previous == 0 ? award.band == 2 : award.band == previous + 1);
		previous = award.band;
		EXPECT(award.need == static_cast<std::uint64_t>(award.band) && award.counter == Counter::PeakClass);
		EXPECT(award.tier >= previousTier);
		previousTier = award.tier;
		EXPECT(std::string(award.how) == "award.class.how" && std::string(award.title).empty());
		EXPECT(std::string(award.id) == "class-" + std::to_string(award.band));
	}
	EXPECT(bands == sf4e::SkillBadge::Classes - 1);
	EXPECT(Get("class-2").tier == 0 && Get("class-10").tier == 4);
}

// The thresholds and tiers, and the cast award for every fighter.
static void Thresholds() {
	EXPECT(Get("games-1").need == 1 && Get("games-5000").need == 5000 && Get("games-5000").tier == 4);
	EXPECT(Get("wins-50").need == 50 && Get("wins-2500").tier == 4);
	EXPECT(Get("streak-3").need == 3 && Get("streak-20").need == 20 && Get("streak-10").tier == 3);
	EXPECT(Get("fighter-copper").counter == Counter::TopFighterTier && Get("fighter-copper").need == 1);
	EXPECT(Get("fighter-gold").need == 3 && Get("fighter-ember").need == 4);
	EXPECT(Get("cast-all").need == static_cast<std::uint64_t>(sf4e::selection::FighterCount));
	EXPECT(Get("friends-10").need == 10 && Get("friends-1").tier == 0);
}

// Earned from the need up and never below it; progress never passes the need.
static void EarnedAndProgress() {
	Counters c;
	c.games = 99;
	EXPECT(!Earned(c, Get("games-100")) && Earned(c, Get("games-10")));
	c.games = 100;
	EXPECT(Earned(c, Get("games-100")) && !Earned(c, Get("games-500")));
	auto step = Progress(c, Get("games-500"));
	EXPECT(step.have == 100 && step.need == 500 && step.Part() > .19f && step.Part() < .21f);
	c.games = 100000;
	step = Progress(c, Get("games-500"));
	EXPECT(step.have == 500 && step.need == 500 && step.Part() == 1.f);
	// Each counter feeds its own awards.
	c = {};
	c.wins = 50;
	EXPECT(Earned(c, Get("wins-50")) && !Earned(c, Get("games-1")));
	c = {};
	c.bestStreak = 5;
	EXPECT(Earned(c, Get("streak-5")) && !Earned(c, Get("streak-10")));
	c = {};
	c.peakClass = 6;
	EXPECT(Earned(c, Get("class-6")) && Earned(c, Get("class-2")) && !Earned(c, Get("class-7")));
	c = {};
	c.topFighterTier = 3;
	EXPECT(Earned(c, Get("fighter-gold")) && Earned(c, Get("fighter-copper")) && !Earned(c, Get("fighter-ember")));
	c = {};
	c.fightersPlayed = sf4e::selection::FighterCount;
	EXPECT(Earned(c, Get("cast-all")) && Earned(c, Get("cast-15")));
	c = {};
	c.friends = 1;
	EXPECT(Earned(c, Get("friends-1")) && !Earned(c, Get("friends-10")));
	EXPECT(Counts(Get("games-1")) && Counts(Get("cast-5")) && Counts(Get("friends-10")) && !Counts(Get("class-4")) && !Counts(Get("fighter-gold")));
	// Nothing counted earns nothing, and the first game earns the first award.
	EXPECT(EarnedCount({}) == 0);
	c = {};
	c.games = 1;
	EXPECT(EarnedCount(c) == 1);
}

static Own Counted() {
	Own own;
	own.kind = Own::Kind::Counted;
	return own;
}

// The player's own counters from the answer: the account's games and wins, the
// two counters the bridge adds, the fighters with games and the highest frame.
static void OwnCounters() {
	auto own = Counted();
	own.account.games = 120;
	own.account.wins = 70;
	own.bestStreak = 6;
	own.peakClass = 5;
	own.fighters[3].games = 80;
	own.fighters[3].tier = 2;
	own.fighters[7].games = 5;
	own.fighters[7].tier = 4;
	// A fighter with no game counts for nothing, whatever its tier says.
	own.fighters[9].tier = 4;
	const auto c = CountersOf(own, 2);
	EXPECT(c.games == 120 && c.wins == 70 && c.bestStreak == 6 && c.peakClass == 5 && c.fightersPlayed == 2 && c.topFighterTier == 4 && c.friends == 2);
	// Not counted: nothing but the friends.
	Own none;
	none.kind = Own::Kind::NoAccount;
	none.account.games = 9;
	const auto zero = CountersOf(none, 3);
	EXPECT(zero.games == 0 && zero.peakClass == 0 && zero.friends == 3);
	EXPECT(CountersOf(Own{}, 0).games == 0);
	// An older bridge sends no streak and no class: those awards are locked, the rest are earned.
	auto older = Counted();
	older.account.games = 1000;
	older.account.wins = 600;
	const auto o = CountersOf(older, 0);
	EXPECT(Earned(o, Get("games-1000")) && Earned(o, Get("wins-250")) && !Earned(o, Get("streak-3")) && !Earned(o, Get("class-2")));
}

// Another player's counters from their card's marks and fighters.
static void CardCounters() {
	Card card;
	card.marks.games = 100;
	card.marks.wins = 50;
	card.marks.streak = 5;
	card.marks.peakClass = 6;
	card.fighters.push_back(CardFighter{3, 900, 2});
	card.fighters.push_back(CardFighter{7, 2600, 4});
	const auto c = CountersOf(card);
	EXPECT(c.games == 100 && c.wins == 50 && c.bestStreak == 5 && c.peakClass == 6 && c.fightersPlayed == 2 && c.topFighterTier == 4 && c.friends == 0);
	EXPECT(Earned(c, Get("games-100")) && !Earned(c, Get("games-500")) && Earned(c, Get("fighter-ember")) && !Earned(c, Get("friends-1")));
	// A card from a bridge without marks earns only what its fighters show.
	Card bare;
	bare.fighters.push_back(CardFighter{3, 900, 2});
	const auto b = CountersOf(bare);
	EXPECT(b.games == 0 && b.fightersPlayed == 1 && !Earned(b, Get("games-1")) && !Earned(b, Get("cast-5")));
}

// A worn id shows only while the counters earn it: an unknown, malformed,
// removed or unearned id is nothing, never an error.
static void WornClaims() {
	Counters c;
	c.games = 10;
	EXPECT(Worn(c, "games-10") == &Get("games-10"));
	EXPECT(Worn(c, "games-100") == nullptr);
	for (const char* id : {"", "entrant", "launch-night", "organizer", "GAMES-10", "games 10", "games-10;", "../games-10", "class-0", "class-11",
			"a-very-long-made-up-award-id-that-no-catalog-has"})
		EXPECT(Worn(c, id) == nullptr);
	EXPECT(!IdShaped("") && !IdShaped("Games") && !IdShaped("a b") && IdShaped("class-10") && !IdShaped(std::string(25, 'a')));
}

// Awards earned since the answer before are listed once; the first answer of a
// run and answers that are not counted only seed or change nothing.
static void Notices() {
	Watch watch;
	Counters c;
	c.games = 10;
	EXPECT(watch.Update(c, false).empty());
	// The first counted answer seeds: games-1 and games-10 are not announced.
	EXPECT(watch.Update(c, true).empty());
	EXPECT(watch.Update(c, true).empty());
	// 90 games more: one award earned since.
	c.games = 100;
	auto fresh = watch.Update(c, true);
	EXPECT(fresh.size() == 1 && fresh[0] == &Get("games-100"));
	EXPECT(watch.Update(c, true).empty());
	// Two at once, in the catalog's order, and the ones before stay quiet.
	c.wins = 1;
	c.bestStreak = 3;
	fresh = watch.Update(c, true);
	EXPECT(fresh.size() == 2 && fresh[0] == &Get("wins-1") && fresh[1] == &Get("streak-3"));
	// An answer that is not counted changes nothing, and a counter that falls and rises is not announced again.
	EXPECT(watch.Update({}, false).empty());
	EXPECT(watch.Update({}, true).empty());
	EXPECT(watch.Update(c, true).empty());
	// Friends awards are seeded without a notice, whenever they come.
	Counters friends = c;
	friends.friends = 4;
	EXPECT(watch.Update(friends, true).empty());
	// A second run starts fresh: its first answer seeds again.
	Watch next;
	EXPECT(next.Update(c, true).empty());
}

int main() {
	CatalogShape();
	ClassAwards();
	Thresholds();
	EarnedAndProgress();
	OwnCounters();
	CardCounters();
	WornClaims();
	Notices();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("award tests passed\n");
	return TEST_EXIT_CODE();
}
