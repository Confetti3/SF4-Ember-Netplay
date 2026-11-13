// How the Archive tab groups the listing's games into rows (ui/ArchiveSets.hxx), as the archive page does: a
// tournament set by its set ID; online games of the same two named players, and one named player's games, into a
// set or a session while each follows the last within half an hour; newest first, or by class.
#include "../ui/ArchiveSets.hxx"

#include <cstdio>
#include <cstdlib>
#include "test_support.hxx"

using namespace sf4e;
namespace ar = netplay::archive;
using ui::archivesets::Entry;
using Kind = Entry::Kind;

constexpr std::uint64_t Minute = 60, Hour = 3600;
constexpr std::uint64_t Base = 1800000000;

static unsigned counter = 0;
static ar::Game Online(std::uint64_t at, const char* a, const char* b, int fighterA = 0, int fighterB = 1) {
	ar::Game game;
	char digest[80];
	std::snprintf(digest, sizeof(digest), "%064x", ++counter);
	game.digest = digest;
	game.url = "https://archive.example/games/" + game.digest + ".emberreplay";
	game.savedAt = at;
	game.names[0] = a;
	game.names[1] = b;
	game.fighters = {{fighterA, fighterB}};
	return game;
}
static ar::Game InSet(std::uint64_t at, const char* set, const char* a, const char* b) {
	auto game = Online(at, a, b);
	game.set = set;
	game.event = "Player 1's Cup";
	game.round = "Top 8";
	game.phase = "Winners";
	return game;
}
static ar::Listing Listed(std::vector<ar::Game> games) {
	ar::Listing listing;
	listing.games = std::move(games);
	return listing;
}

static void TournamentSetsGroupBySetId() {
	auto listing = Listed({InSet(Base + 300, "emt_a", "Player 1", "Player 2"), InSet(Base + 200, "emt_b", "Player 3", "Player 4"),
		InSet(Base + 100, "emt_a", "Player 1", "Player 2"), InSet(Base, "emt_a", "Player 2", "Player 1")});
	listing.sets["emt_a"].score = {{2, 1}};
	listing.sets["emt_a"].pack = "https://archive.example/sets/emt_a.emberpack";
	const auto entries = ui::archivesets::Group(listing);
	CHECK(entries.size() == 2);
	// The set whose newest game is newest first, its games oldest first; no gap splits a tournament set.
	EXPECT(entries[0].kind == Kind::Set && entries[0].id == "emt_a" && entries[0].games.size() == 3);
	EXPECT(entries[0].games[0]->savedAt == Base && entries[0].games[2]->savedAt == Base + 300 && entries[0].newest == Base + 300);
	EXPECT(entries[1].kind == Kind::Set && entries[1].id == "emt_b" && entries[1].games.size() == 1);
	// The listing's own score and pack are the set's; a set it has none for shows no score.
	const auto score = ui::archivesets::Score(entries[0]);
	EXPECT(score && (*score)[0] == 2 && (*score)[1] == 1 && entries[0].info && !entries[0].info->pack.empty());
	EXPECT(!ui::archivesets::Score(entries[1]) && !entries[1].info);
}

static void OnlineGamesOfTwoPlayersAreOneSetWhileTheyFollowEachOther() {
	// Newest first; the other way round of names is the same pair.
	const auto listing = Listed({Online(Base + 50 * Minute, "Player 1", "Player 2"), Online(Base + 30 * Minute, "Player 2", "Player 1"),
		Online(Base + 10 * Minute, "Player 1", "Player 2"),
		// More than half an hour before the last of the set so far: another set.
		Online(Base - 21 * Minute, "Player 1", "Player 2")});
	const auto entries = ui::archivesets::Group(listing);
	CHECK(entries.size() == 2);
	EXPECT(entries[0].kind == Kind::Match && entries[0].games.size() == 3 && entries[0].id == listing.games[0].digest && entries[0].player.empty());
	EXPECT(entries[0].games[0]->savedAt == Base + 10 * Minute && entries[0].newest == Base + 50 * Minute);
	EXPECT(entries[1].kind == Kind::Match && entries[1].games.size() == 1);
	EXPECT(!ui::archivesets::Score(entries[0]));
	// Exactly half an hour from the last game still follows it.
	const auto edge = ui::archivesets::Group(Listed({Online(Base + 30 * Minute, "Player 1", "Player 2"), Online(Base, "Player 1", "Player 2")}));
	EXPECT(edge.size() == 1 && edge[0].games.size() == 2);
	const auto past = ui::archivesets::Group(Listed({Online(Base + 30 * Minute + 1, "Player 1", "Player 2"), Online(Base, "Player 1", "Player 2")}));
	EXPECT(past.size() == 2);
	// Other players in between are other sets.
	const auto mixed = ui::archivesets::Group(Listed({Online(Base + 20 * Minute, "Player 1", "Player 2"), Online(Base + 10 * Minute, "Player 3", "Player 4"),
		Online(Base, "Player 1", "Player 2")}));
	EXPECT(mixed.size() == 2 && mixed[0].games.size() == 2 && mixed[1].games.size() == 1);
}

static void OneNamedPlayersGamesAreASession() {
	// The other side was not named (its player did not share the game): the named player's session, whoever they met.
	const auto listing = Listed({Online(Base + 40 * Minute, "Player 1", "", 0, 5), Online(Base + 20 * Minute, "", "Player 1", 7, 0),
		Online(Base, "Player 1", "", 0, 5)});
	const auto entries = ui::archivesets::Group(listing);
	CHECK(entries.size() == 1);
	const Entry& session = entries[0];
	EXPECT(session.kind == Kind::Session && session.player == "Player 1" && session.games.size() == 3);
	EXPECT(ui::archivesets::SideOf(*session.games[0], "Player 1") == 0 && ui::archivesets::SideOf(*session.games[1], "Player 1") == 1);
	// The fighters met, each once, in the order they were met: Player 1 played the second side against 7.
	const auto met = ui::archivesets::FightersMet(session);
	EXPECT(met.size() == 2 && met[0] == 5 && met[1] == 7);
	// One game of a named player alone is a match, not a session.
	const auto single = ui::archivesets::Group(Listed({Online(Base, "Player 1", "")}));
	EXPECT(single.size() == 1 && single[0].kind == Kind::Match && single[0].player.empty());
	// A game nobody named stands alone, as each does.
	const auto unnamed = ui::archivesets::Group(Listed({Online(Base + 60, "", ""), Online(Base, "", "")}));
	EXPECT(unnamed.size() == 2 && unnamed[0].kind == Kind::Match);
	// A named pair's game and a one-sided game of one of them are not the same group.
	const auto apart = ui::archivesets::Group(Listed({Online(Base + 60, "Player 1", "Player 2"), Online(Base, "Player 1", "")}));
	EXPECT(apart.size() == 2);
}

static void ARowsIdIsStableWhileOlderGamesJoin() {
	const auto newest = Online(Base + 3 * Hour, "Player 1", "Player 2");
	const auto older = Online(Base + 3 * Hour - 10 * Minute, "Player 1", "Player 2");
	const auto first = ui::archivesets::Group(Listed({newest}));
	const auto later = ui::archivesets::Group(Listed({newest, older}));
	EXPECT(first.size() == 1 && later.size() == 1 && first[0].id == later[0].id && later[0].games.size() == 2);
}

static void ClassOrderListsTheHighestClassFirst() {
	auto none = Online(Base + 3 * Hour, "Player 1", "Player 2");
	auto top = Online(Base + Hour, "Player 3", "Player 4");
	top.classes = {{0, 10}};
	auto middle = Online(Base + 2 * Hour, "Player 5", "Player 6");
	middle.classes = {{4, 7}};
	auto alsoTop = Online(Base, "Player 7", "Player 8");
	alsoTop.classes = {{10, 0}};
	auto listing = Listed({none, top, middle, alsoTop});
	// Newest order: as played.
	auto entries = ui::archivesets::Group(listing);
	EXPECT(entries.size() == 4 && entries[0].bestClass == 0 && entries[1].bestClass == 7 && entries[2].bestClass == 10 && entries[3].bestClass == 10);
	// By class: grand masters first, newest first among them, the games with no class last.
	listing.order = ar::Order::Class;
	entries = ui::archivesets::Group(listing);
	CHECK(entries.size() == 4);
	EXPECT(entries[0].bestClass == 10 && entries[0].newest == Base + Hour && entries[1].bestClass == 10 && entries[1].newest == Base);
	EXPECT(entries[2].bestClass == 7 && entries[3].bestClass == 0);
	// The listing in another order groups the same: a bridge's class page is not newest first.
	std::reverse(listing.games.begin(), listing.games.end());
	const auto reversed = ui::archivesets::Group(listing);
	CHECK(reversed.size() == 4);
	for (std::size_t i = 0; i < 4; ++i) EXPECT(reversed[i].id == entries[i].id);
}

int main() {
	TournamentSetsGroupBySetId();
	OnlineGamesOfTwoPlayersAreOneSetWhileTheyFollowEachOther();
	OneNamedPlayersGamesAreASession();
	ARowsIdIsStableWhileOlderGamesJoin();
	ClassOrderListsTheHighestClassFirst();
	if (TEST_FAILURES()) { std::printf("%d failed\n", TEST_FAILURES()); return 1; }
	std::printf("ok\n");
	return 0;
}
