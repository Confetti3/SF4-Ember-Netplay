// The public rooms filter (netplay/PublicRoomFilter.hxx): which rooms it lists
// by area, type, free seats and connection, how its settings step, and what
// Quick join takes from it.
#include "../netplay/PublicRoomFilterMatch.hxx"

#include <string>
#include <vector>

#include "test_support.hxx"

using namespace sf4e;
using namespace sf4e::netplay::publicrooms;
using namespace sf4e::room;

namespace {
// A room with a free seat whose creator is in `area`; Classic unless `type` says none or other.
Room Make(Area area, std::optional<RoomType> type = RoomType::Classic) {
	Room room;
	room.id = "r"; room.name = "Open room"; room.area = area;
	room.members = 2; room.capacity = 8;
	room.type = type;
	return room;
}

void DefaultListsEverything() {
	const RoomFilter filter;
	EXPECT(!filter.Active() && filter.Valid());
	Room full = Make(Area::EuropeWest, std::nullopt); full.members = 8; full.locked = true; full.wiredOnly = true;
	EXPECT(Matches(filter, full, Area::None) && Matches(filter, full, Area::NaEast) && Matches(filter, Make(Area::EastAsia, RoomType::Arcade), Area::NaEast));
	EXPECT(Matches(filter, Make(Area::None), Area::NaEast));
}

void NearIsTheSameAreaOrANeighbour() {
	RoomFilter near; near.scope = RoomFilter::Place::Near;
	EXPECT(near.Active() && near.Valid());
	EXPECT(Matches(near, Make(Area::NaWest), Area::NaWest) && Matches(near, Make(Area::NaCentral), Area::NaWest));
	EXPECT(!Matches(near, Make(Area::NaEast), Area::NaWest) && !Matches(near, Make(Area::EuropeWest), Area::NaWest));
	EXPECT(Matches(near, Make(Area::CentralAmerica), Area::SouthAmerica) && Matches(near, Make(Area::MiddleEastAfrica), Area::EuropeEast));
	// A room whose creator gave no area is never near.
	EXPECT(!Matches(near, Make(Area::None), Area::NaWest));
	// Near with no area of the player's own cannot say which rooms are near: every room is listed.
	EXPECT(Matches(near, Make(Area::EuropeWest), Area::None) && Matches(near, Make(Area::None), Area::None));
}

void OneAreaIsThatAreaAlone() {
	RoomFilter one; one.scope = RoomFilter::Place::One; one.area = Area::EuropeWest;
	EXPECT(one.Valid() && Matches(one, Make(Area::EuropeWest), Area::NaEast) && Matches(one, Make(Area::EuropeWest), Area::None));
	// Not its neighbours, and not a room without an area.
	EXPECT(!Matches(one, Make(Area::EuropeEast), Area::EuropeWest) && !Matches(one, Make(Area::None), Area::EuropeWest));
	// One without an area is invalid, and an area without One too.
	one.area = Area::None; EXPECT(!one.Valid());
	RoomFilter stray; stray.area = Area::NaEast; EXPECT(!stray.Valid());
}

void TypeNeedsASentType() {
	RoomFilter filter;
	filter.type = RoomFilter::Type::Arcade;
	EXPECT(Matches(filter, Make(Area::NaEast, RoomType::Arcade), Area::None) && !Matches(filter, Make(Area::NaEast, RoomType::Classic), Area::None) &&
		!Matches(filter, Make(Area::NaEast, RoomType::King), Area::None));
	// A room that sent no type passes under All only.
	EXPECT(!Matches(filter, Make(Area::NaEast, std::nullopt), Area::None));
	filter.type = RoomFilter::Type::Classic;
	EXPECT(Matches(filter, Make(Area::NaEast, RoomType::Classic), Area::None) && !Matches(filter, Make(Area::NaEast, std::nullopt), Area::None));
	filter.type = RoomFilter::Type::King;
	EXPECT(Matches(filter, Make(Area::NaEast, RoomType::King), Area::None) && !Matches(filter, Make(Area::NaEast, RoomType::Arcade), Area::None));
	filter.type = RoomFilter::Type::All;
	EXPECT(Matches(filter, Make(Area::NaEast, std::nullopt), Area::None));
}

void SeatsAndWired() {
	RoomFilter seats; seats.openSeats = true;
	Room full = Make(Area::NaEast); full.members = 8;
	Room locked = Make(Area::NaEast); locked.locked = true;
	EXPECT(Matches(seats, Make(Area::NaEast), Area::None) && !Matches(seats, full, Area::None) && !Matches(seats, locked, Area::None));
	RoomFilter wired; wired.wiredOnly = true;
	Room only = Make(Area::NaEast); only.wiredOnly = true;
	EXPECT(Matches(wired, only, Area::None) && !Matches(wired, Make(Area::NaEast), Area::None));
	RoomFilter all; all.scope = RoomFilter::Place::One; all.area = Area::NaEast; all.type = RoomFilter::Type::Classic; all.openSeats = true; all.wiredOnly = true;
	EXPECT(Matches(all, only, Area::None) && !Matches(all, Make(Area::NaEast), Area::None) && !Matches(all, Make(Area::EuropeWest), Area::None));
	Room onlyFull = only; onlyFull.members = 8;
	EXPECT(!Matches(all, onlyFull, Area::None));
}

void PlaceStepsInOrder() {
	RoomFilter filter;
	// All, Near me, then the twelve areas in the table's order, and round.
	const auto at = [&] {
		return filter.scope == RoomFilter::Place::All ? std::string("all") : filter.scope == RoomFilter::Place::Near ? std::string("near") : AreaCode(filter.area);
	};
	std::vector<std::string> order = {"all", "near"};
	for (const auto& row : areas::Table) order.push_back(row.code);
	const int count = static_cast<int>(order.size());
	EXPECT(count == 14);
	for (int i = 1; i <= count; ++i) { filter.StepPlace(1, Area::NaWest); EXPECT(at() == order[i % count] && filter.Valid()); }
	filter.StepPlace(-1, Area::NaWest); EXPECT(at() == "oce");
	filter.StepPlace(-1, Area::NaWest); EXPECT(at() == "sea");
	// Near is not offered while the player's area is not set.
	filter = RoomFilter();
	filter.StepPlace(1, Area::None); EXPECT(at() == "naw");
	filter.StepPlace(-1, Area::None); EXPECT(at() == "all");
	filter.StepPlace(-1, Area::None); EXPECT(at() == "oce");
	// A saved Near with no area set steps off it.
	filter = RoomFilter(); filter.scope = RoomFilter::Place::Near;
	filter.StepPlace(1, Area::None); EXPECT(at() == "naw");
}

void TypeSteps() {
	RoomFilter filter;
	const RoomFilter::Type order[] = {RoomFilter::Type::Classic, RoomFilter::Type::Arcade, RoomFilter::Type::King, RoomFilter::Type::All};
	for (const auto type : order) { filter.StepType(1); EXPECT(filter.type == type); }
	filter.StepType(-1); EXPECT(filter.type == RoomFilter::Type::King);
	filter.StepType(-1); EXPECT(filter.type == RoomFilter::Type::Arcade);
}

void QuickJoinStaysNearUnlessAskedOtherwise() {
	RoomFilter filter;
	EXPECT(filter.ForQuickJoin().scope == RoomFilter::Place::Near && filter.ForQuickJoin().type == RoomFilter::Type::All);
	EXPECT(filter.scope == RoomFilter::Place::All);
	// The player's own choice of area, and every other setting, is kept.
	filter.scope = RoomFilter::Place::One; filter.area = Area::EastAsia; filter.type = RoomFilter::Type::Classic; filter.openSeats = true; filter.wiredOnly = true;
	EXPECT(filter.ForQuickJoin() == filter);
}
}

int main() {
	DefaultListsEverything();
	NearIsTheSameAreaOrANeighbour();
	OneAreaIsThatAreaAlone();
	TypeNeedsASentType();
	SeatsAndWired();
	PlaceStepsInOrder();
	TypeSteps();
	QuickJoinStaysNearUnlessAskedOtherwise();
	if (!TEST_FAILURES()) std::printf("Public room filter tests passed\n");
	return TEST_EXIT_CODE();
}
