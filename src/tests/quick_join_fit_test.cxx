// Quick join's choice among the rooms it may join (netplay/QuickJoinFit.hxx): ping first,
// then class, then a waiting player, then the list's own order. The candidates are given
// as the rooms list orders them (PublicRoomsPanel's Before), each joinable and listed by the
// filter, as PublicRoomsPanel::QuickJoin hands them over. The cases are those of
// docs/design/QUICK_JOIN.md: the seeker plays from NA East at class 5 and wants similar
// classes unless a case says otherwise.
#include "../netplay/PublicRoomFilterMatch.hxx"
#include "../netplay/QuickJoinFit.hxx"

#include <set>
#include <string>
#include <vector>

#include "test_support.hxx"

using namespace sf4e;
using namespace sf4e::netplay::publicrooms;
using Rank = QuickJoinFit::Rank;

namespace {
constexpr Seeker Seeking{Area::NaEast, 5};

Room Make(const char* id, Area area, unsigned members, unsigned capacity, std::vector<WaitingPlayer> waiting = {},
	std::vector<std::uint8_t> classes = {}, room::RoomType type = room::RoomType::Classic) {
	Room room;
	room.id = id; room.name = id; room.area = area;
	room.members = members; room.capacity = capacity;
	room.waiting = std::move(waiting); room.classes = std::move(classes);
	room.type = type; room.hasDetails = true;
	return room;
}

// The rooms Quick join may ask for, in the list's order: joinable, not refused, and listed by the filter.
std::vector<const Room*> Candidates(const std::vector<Room>& rooms, const RoomFilter& filter = {}, const std::set<std::string>& refused = {}) {
	std::vector<const Room*> out;
	for (const auto& room : rooms)
		if (room.Joinable() && !refused.count(room.id) && Matches(filter, room, Seeking.home)) out.push_back(&room);
	return out;
}

std::string Picked(const std::vector<Room>& rooms, const Seeker& seeker = Seeking, const RoomFilter& filter = {}, const std::set<std::string>& refused = {}) {
	const auto pick = PickQuickJoin(Candidates(rooms, filter, refused), seeker, filter.opponents);
	return pick ? pick->room : std::string("none");
}

void ClassesApart() {
	EXPECT(RankBetween(5, 5) == Rank::Close && RankBetween(5, 6) == Rank::Close && RankBetween(6, 5) == Rank::Close);
	EXPECT(RankBetween(5, 7) == Rank::Wide && RankBetween(5, 8) == Rank::Far && RankBetween(10, 1) == Rank::Far);
	EXPECT(RankBetween(0, 5) == Rank::Unknown && RankBetween(5, 0) == Rank::Unknown);
	// Unknown ranks between Wide and Far, so a player still placing gets games.
	QuickJoinFit wide, unknown, far;
	wide.rank = Rank::Wide; unknown.rank = Rank::Unknown; far.rank = Rank::Far;
	EXPECT(wide.Better(unknown) && unknown.Better(far) && !far.Better(unknown));
}

void TheSeekersClass() {
	netplay::rankings::Standing standing;
	standing.kind = netplay::rankings::Standing::Kind::Ranked; standing.classesShown = true; standing.badge = SkillBadge::OfClass(7, false);
	EXPECT(SeekerBand(standing) == 7);
	auto hidden = standing; hidden.classesShown = false; EXPECT(SeekerBand(hidden) == 0);
	auto placing = standing; placing.badge = SkillBadge::Placing(); EXPECT(SeekerBand(placing) == 0);
	auto developer = standing; developer.badge = SkillBadge::Developer(); EXPECT(SeekerBand(developer) == 0);
	auto out = standing; out.kind = netplay::rankings::Standing::Kind::OptedOut; EXPECT(SeekerBand(out) == 0);
}

void DesignCases() {
	// 1. Nothing new listed: today's pick, the near room with the most free seats.
	EXPECT(Picked({Make("n", Area::NaEast, 2, 8), Make("m", Area::NaEast, 5, 8), Make("x", Area::EuropeWest, 1, 8)}) == "n");
	// 2. A close class beats more free seats.
	EXPECT(Picked({Make("b", Area::NaEast, 1, 8, {{9, Area::NaEast}}), Make("a", Area::NaEast, 4, 8, {{6, Area::NaEast}})}) == "a");
	// 3. Ping before rank.
	EXPECT(Picked({Make("a", Area::NaEast, 2, 8, {{9, Area::NaEast}}), Make("b", Area::EuropeWest, 2, 8, {{5, Area::EuropeWest}})}) == "a");
	// 4. The waiting player's own area counts, not the room's.
	EXPECT(Picked({Make("a", Area::NaEast, 2, 8, {{5, Area::Oceania}}), Make("b", Area::None, 2, 8, {{6, Area::NaCentral}})}) == "b");
	// 5. A waiting player who hid their area counts from the room's.
	EXPECT(Picked({Make("a", Area::NaEast, 2, 8, {{5, Area::None}}), Make("b", Area::EuropeWest, 2, 8, {{5, Area::EuropeWest}})}) == "a");
	// 6. Wide, then Unknown, then Far.
	const std::vector<Room> three{Make("f", Area::NaEast, 2, 8, {{8, Area::NaEast}}), Make("u", Area::NaEast, 2, 8, {{0, Area::NaEast}}),
		Make("w", Area::NaEast, 2, 8, {{7, Area::NaEast}})};
	EXPECT(Picked(three) == "w" && Picked(three, Seeking, {}, {"w"}) == "u" && Picked(three, Seeking, {}, {"w", "u"}) == "f");
	// 7. Nobody waiting: the lower median of the members' classes, the ones without a class left out.
	EXPECT(Picked({Make("q", Area::NaEast, 2, 8, {}, {9, 10}), Make("p", Area::NaEast, 4, 8, {}, {0, 3, 5, 6})}) == "p");
	// 8. At equal rank, a waiting player wins over more free seats.
	EXPECT(Picked({Make("s", Area::NaEast, 1, 8, {}, {5, 5}), Make("r", Area::NaEast, 6, 8, {{5, Area::NaEast}})}) == "r");
	// 9. A room that seats by cabinet meets whoever waited longest; a Classic room's newcomer picks the table.
	EXPECT(Picked({Make("k", Area::NaEast, 2, 8, {{9, Area::NaEast}, {5, Area::NaEast}}, {}, room::RoomType::Arcade),
		Make("c", Area::NaEast, 4, 8, {{9, Area::NaEast}, {5, Area::NaEast}})}) == "c");
	// 10. A seeker without a class: Unknown everywhere, so the room where someone waits.
	EXPECT(Picked({Make("t", Area::NaEast, 1, 8), Make("s", Area::NaEast, 2, 8, {{9, Area::NaEast}})}, Seeker{Area::NaEast, 0}) == "s");
	// 11. Any class: rank says nothing, so the list's order decides.
	RoomFilter any; any.opponents = RoomFilter::Opponents::Any;
	EXPECT(Picked({Make("b", Area::NaEast, 2, 8, {{10, Area::NaEast}}), Make("a", Area::NaEast, 4, 8, {{5, Area::NaEast}})}, Seeking, any) == "b");
	// 12. The filter still rules: only Arcade rooms, and a locked one is never asked for.
	RoomFilter arcade; arcade.type = RoomFilter::Type::Arcade;
	auto locked = Make("l", Area::NaEast, 2, 8, {{5, Area::NaEast}}, {}, room::RoomType::Arcade); locked.locked = true;
	const std::vector<Room> typed{Make("c", Area::NaEast, 2, 8, {{5, Area::NaEast}}), Make("k", Area::EuropeWest, 2, 8, {{9, Area::EuropeWest}}, {}, room::RoomType::Arcade), locked};
	EXPECT(Picked(typed, Seeking, arcade) == "k" && Picked(typed, Seeking, arcade, {"k"}) == "none");
}

void WidenedOnlyWhenClassesWereThere() {
	const auto widened = [](const std::vector<Room>& rooms, const Seeker& seeker, RoomFilter::Opponents opponents) {
		const auto pick = PickQuickJoin(Candidates(rooms), seeker, opponents);
		return pick && pick->widened;
	};
	const std::vector<Room> far{Make("f", Area::NaEast, 2, 8, {{9, Area::NaEast}})}, close{Make("c", Area::NaEast, 2, 8, {{6, Area::NaEast}})},
		unlisted{Make("u", Area::NaEast, 2, 8)};
	EXPECT(widened(far, Seeking, RoomFilter::Opponents::Similar));
	EXPECT(!widened(close, Seeking, RoomFilter::Opponents::Similar));
	// No room listed classes (an older service): nothing to say about classes.
	EXPECT(!widened(unlisted, Seeking, RoomFilter::Opponents::Similar));
	// The player asked for any class, or has none.
	EXPECT(!widened(far, Seeking, RoomFilter::Opponents::Any) && !widened(far, Seeker{Area::NaEast, 0}, RoomFilter::Opponents::Similar));
	EXPECT(!PickQuickJoin({}, Seeking, RoomFilter::Opponents::Similar));
}
}

int main() {
	ClassesApart();
	TheSeekersClass();
	DesignCases();
	WidenedOnlyWhenClassesWereThere();
	if (!TEST_FAILURES()) std::printf("Quick join fit tests passed\n");
	return TEST_EXIT_CODE();
}
