// The milestone awards (netplay/Awards.hxx): the catalog's shape (one award a
// family, each with levels in tiers), what reaches each level from a player's own
// counters and from another player's card, the check that keeps a made-up worn
// award from showing, and the notices that announce a level once and never at the
// first answer of a run.
#include "../netplay/Awards.hxx"

#include <cstdio>
#include <set>
#include <string>
#include "test_support.hxx"

using namespace sf4e::netplay::awards;
using sf4e::netplay::levels::Card;
using sf4e::netplay::levels::CardFighter;
using sf4e::netplay::levels::Own;

static const Family& Get(const char* id) {
	const Family* family = Find(id);
	CHECK(family != nullptr);
	return *family;
}

// Families are unique with their own icon and name; levels climb in need and
// never fall in tier, and every level has words and a tier of the five.
static void CatalogShape() {
	std::set<std::string> ids;
	std::set<int> icons;
	for (const auto& family : Catalog) {
		EXPECT(ids.insert(family.id).second && IdShaped(family.id));
		EXPECT(icons.insert(family.icon).second);
		EXPECT(family.icon >= 0 && family.icon < 12 && family.name && *family.name);
		EXPECT(family.count >= 1);
		for (std::size_t i = 0; i < family.count; ++i) {
			const auto& level = family.levels[i];
			EXPECT(level.how && *level.how && level.title);
			EXPECT(level.tier >= 0 && level.tier < sf4e::netplay::levels::TierCount);
			if (i > 0) EXPECT(family.levels[i - 1].need < level.need && family.levels[i - 1].tier <= level.tier);
		}
	}
	EXPECT(CatalogSize == 9);
	std::size_t total = 0;
	for (const auto& family : Catalog) total += family.count;
	EXPECT(total == LevelTotal() && total == 5 + 5 + 5 + 9 + 4 + 4 + 3 + 4 + 4);
}

// The rank family has a level for each class above the first, named by its
// band, the top alone Ember and the lowest Ash.
static void RankLevelsAreClasses() {
	const auto& rank = Get("rank");
	EXPECT(rank.count == static_cast<std::size_t>(sf4e::SkillBadge::Classes - 1));
	for (std::size_t i = 0; i < rank.count; ++i) {
		EXPECT(rank.levels[i].band == static_cast<int>(i) + 2 && rank.levels[i].need == static_cast<std::uint64_t>(rank.levels[i].band));
		EXPECT(std::string(rank.levels[i].title).empty() && std::string(rank.levels[i].how) == "award.rank.how");
	}
	EXPECT(rank.levels[0].tier == 0 && rank.levels[rank.count - 1].tier == 4 && rank.levels[rank.count - 2].tier == 3 && rank.levels[rank.count - 3].tier == 3);
	for (const auto& family : Catalog) if (std::string(family.id) != "rank") for (std::size_t i = 0; i < family.count; ++i) EXPECT(family.levels[i].band == 0);
}

// The levels and tiers as agreed, and the tiers meaning the same difficulty across families.
static void Levels() {
	const auto tiers = [](const char* id) { std::string out; for (std::size_t i = 0; i < Get(id).count; ++i) out += static_cast<char>('0' + Get(id).levels[i].tier); return out; };
	const auto needs = [](const char* id) { std::vector<std::uint64_t> out; for (std::size_t i = 0; i < Get(id).count; ++i) out.push_back(Get(id).levels[i].need); return out; };
	EXPECT(needs("matches") == std::vector<std::uint64_t>({1, 100, 500, 1000, 5000}) && tiers("matches") == "01234");
	EXPECT(needs("wins") == std::vector<std::uint64_t>({1, 50, 250, 1000, 2500}) && tiers("wins") == "01234");
	EXPECT(needs("streak") == std::vector<std::uint64_t>({3, 5, 10, 15, 20}) && tiers("streak") == "01234");
	EXPECT(needs("mastery") == std::vector<std::uint64_t>({1, 2, 3, 4}) && tiers("mastery") == "1234");
	EXPECT(needs("cast") == std::vector<std::uint64_t>({5, 15, 30, static_cast<std::uint64_t>(sf4e::selection::FighterCount)}) && tiers("cast") == "0123");
	EXPECT(needs("friends") == std::vector<std::uint64_t>({1, 5, 15}) && tiers("friends") == "012");
	EXPECT(needs("rival") == std::vector<std::uint64_t>({10, 50, 100, 250}) && needs("opponents") == std::vector<std::uint64_t>({10, 50, 200, 500}));
	EXPECT(tiers("rank") == "000012334");
}

// A level is reached from its need up and never below it; the top has no next level.
static void LevelOfAndNext() {
	const auto& matches = Get("matches");
	Counters c;
	EXPECT(LevelOf(c, matches) == 0 && !Earned(c, matches) && LevelAt(matches, 0) == nullptr);
	auto step = Next(c, matches);
	EXPECT(!step.top && step.have == 0 && step.need == 1 && step.Part() == 0.f);
	c.games = 1;
	EXPECT(LevelOf(c, matches) == 1 && Earned(c, matches));
	c.games = 99;
	EXPECT(LevelOf(c, matches) == 1);
	step = Next(c, matches);
	EXPECT(step.have == 99 && step.need == 100 && step.Part() > .98f);
	c.games = 100;
	EXPECT(LevelOf(c, matches) == 2 && LevelAt(matches, 2)->tier == 1);
	c.games = 4999;
	EXPECT(LevelOf(c, matches) == 4);
	step = Next(c, matches);
	EXPECT(step.have == 4999 && step.need == 5000);
	c.games = 100000;
	EXPECT(LevelOf(c, matches) == 5 && LevelAt(matches, 5)->tier == 4 && LevelAt(matches, 6) == nullptr);
	step = Next(c, matches);
	EXPECT(step.top && step.Part() == 1.f);
	// Each counter feeds its own family.
	c = {};
	c.wins = 50; c.bestStreak = 15; c.peakClass = 6; c.topFighterTier = 3; c.fightersPlayed = sf4e::selection::FighterCount;
	c.friends = 5; c.rival = 100; c.opponents = 49;
	EXPECT(LevelOf(c, Get("wins")) == 2 && LevelOf(c, Get("streak")) == 4 && LevelOf(c, Get("rank")) == 5 && LevelOf(c, Get("mastery")) == 3);
	EXPECT(LevelOf(c, Get("cast")) == 4 && LevelOf(c, Get("friends")) == 2 && LevelOf(c, Get("rival")) == 3 && LevelOf(c, Get("opponents")) == 1);
	EXPECT(LevelOf(c, Get("matches")) == 0 && LevelsEarned(c) == 2 + 4 + 5 + 3 + 4 + 2 + 3 + 1);
	EXPECT(Counts(Get("wins")) && Counts(Get("friends")) && !Counts(Get("rank")) && !Counts(Get("mastery")));
	// The starting class earns an award below B; the top class the last level.
	c = {};
	c.peakClass = 1;
	EXPECT(LevelOf(c, Get("rank")) == 0);
	c.peakClass = 10;
	EXPECT(LevelOf(c, Get("rank")) == 9 && LevelAt(Get("rank"), 9)->tier == 4);
	EXPECT(LevelsEarned({}) == 0);
}

static Own Counted() {
	Own own;
	own.kind = Own::Kind::Counted;
	return own;
}

// The player's own counters from the answer.
static void OwnCounters() {
	auto own = Counted();
	own.account.games = 120;
	own.account.wins = 70;
	own.bestStreak = 6;
	own.peakClass = 5;
	own.rival = 12;
	own.opponents = 20;
	own.fighters[3].games = 80;
	own.fighters[3].tier = 2;
	own.fighters[7].games = 5;
	own.fighters[7].tier = 4;
	// A fighter with no game counts for nothing, whatever its tier says.
	own.fighters[9].tier = 4;
	const auto c = CountersOf(own, 2);
	EXPECT(c.games == 120 && c.wins == 70 && c.bestStreak == 6 && c.peakClass == 5 && c.fightersPlayed == 2 && c.topFighterTier == 4 && c.friends == 2);
	EXPECT(c.rival == 12 && c.opponents == 20);
	Own none;
	none.kind = Own::Kind::NoAccount;
	none.account.games = 9;
	const auto zero = CountersOf(none, 3);
	EXPECT(zero.games == 0 && zero.peakClass == 0 && zero.friends == 3);
	EXPECT(CountersOf(Own{}, 0).games == 0);
	// An older bridge sends no streak, class or rivals: those families are locked, the rest are reached.
	auto older = Counted();
	older.account.games = 1000;
	older.account.wins = 600;
	const auto o = CountersOf(older, 0);
	EXPECT(LevelOf(o, Get("matches")) == 4 && LevelOf(o, Get("wins")) == 3 && LevelOf(o, Get("streak")) == 0 && LevelOf(o, Get("rank")) == 0 && LevelOf(o, Get("rival")) == 0);
}

// Another player's counters from their card's marks and fighters.
static void CardCounters() {
	Card card;
	card.marks.games = 100;
	card.marks.wins = 50;
	card.marks.streak = 5;
	card.marks.peakClass = 6;
	card.marks.rival = 10;
	card.fighters.push_back(CardFighter{3, 900, 2});
	card.fighters.push_back(CardFighter{7, 2600, 4});
	const auto c = CountersOf(card);
	EXPECT(c.games == 100 && c.wins == 50 && c.bestStreak == 5 && c.peakClass == 6 && c.fightersPlayed == 2 && c.topFighterTier == 4 && c.friends == 0 && c.rival == 10);
	EXPECT(LevelOf(c, Get("matches")) == 2 && LevelOf(c, Get("mastery")) == 4 && LevelOf(c, Get("friends")) == 0 && LevelOf(c, Get("rival")) == 1);
	// A card from a bridge without marks reaches only what its fighters show.
	Card bare;
	bare.fighters.push_back(CardFighter{3, 900, 2});
	const auto b = CountersOf(bare);
	EXPECT(b.games == 0 && b.fightersPlayed == 1 && LevelOf(b, Get("matches")) == 0 && LevelOf(b, Get("cast")) == 0 && LevelOf(b, Get("mastery")) == 2);
}

// A worn id shows only while the counters reach a level of it, at that level and in its colour:
// an unknown, malformed, removed or unreached id is nothing, never an error.
static void WornClaims() {
	Counters c;
	c.games = 150;
	const auto worn = WornOf(c, "matches");
	EXPECT(worn && worn.family == &Get("matches") && worn.level == 2 && worn.Current().tier == 1);
	c.games = 600;
	const auto higher = WornOf(c, "matches");
	EXPECT(higher && higher.level == 3 && higher.Current().tier == 2);
	EXPECT(!WornOf(c, "wins"));
	for (const char* id : {"", "entrant", "launch-night", "games-10", "MATCHES", "matches ", "../matches", "rank;", "a-very-long-made-up-award-id-that-no-catalog-has"})
		EXPECT(!WornOf(c, id));
	// On another player's card the claim is held to their card's marks: a made-up one never shows.
	Card card;
	card.marks.games = 100;
	EXPECT(WornOnCard(card, "matches") && WornOnCard(card, "matches").level == 2);
	EXPECT(!WornOnCard(card, "wins") && !WornOnCard(card, "friends") && !WornOnCard(Card{}, "matches"));
	EXPECT(!IdShaped("") && !IdShaped("Matches") && !IdShaped("a b") && IdShaped("opponents") && !IdShaped(std::string(25, 'a')));
}

// Levels reached since the answer before are listed once, low to high; the first
// answer of a run and answers that are not counted only seed or change nothing.
static void Notices() {
	Watch watch;
	Counters c;
	c.games = 10;
	EXPECT(watch.Update(c, false).empty());
	// The first counted answer seeds: nothing is announced.
	EXPECT(watch.Update(c, true).empty());
	EXPECT(watch.Update(c, true).empty());
	// 90 games more: one level reached.
	c.games = 100;
	auto gained = watch.Update(c, true);
	EXPECT(gained.size() == 1 && gained[0].family == &Get("matches") && gained[0].level == 2);
	EXPECT(watch.Update(c, true).empty());
	// A first win and a run of three at once, in the catalog's order; a jump of two levels says both.
	c.wins = 1;
	c.bestStreak = 5;
	gained = watch.Update(c, true);
	EXPECT(gained.size() == 3 && gained[0].family == &Get("wins") && gained[0].level == 1 && gained[1].family == &Get("streak") && gained[1].level == 1 && gained[2].level == 2);
	// An answer that is not counted changes nothing, and a counter that falls and rises is not announced again.
	EXPECT(watch.Update({}, false).empty());
	EXPECT(watch.Update({}, true).empty());
	EXPECT(watch.Update(c, true).empty());
	// Friends are seeded without a notice, whenever they come.
	Counters friends = c;
	friends.friends = 4;
	EXPECT(watch.Update(friends, true).empty());
	// A second run starts fresh: its first answer seeds again.
	Watch next;
	EXPECT(next.Update(c, true).empty());
}

int main() {
	CatalogShape();
	RankLevelsAreClasses();
	Levels();
	LevelOfAndNext();
	OwnCounters();
	CardCounters();
	WornClaims();
	Notices();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("award tests passed\n");
	return TEST_EXIT_CODE();
}
