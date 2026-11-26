// The public rooms filter (netplay/PublicRoomFilter.hxx): which rooms it lists
// by area, type, free seats and connection, how its settings step, and Quick
// join's choice of opponents beside it.
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

void TickedAreasAreThoseAreasAlone() {
	RoomFilter one; one.SetArea(Area::EuropeWest, true);
	EXPECT(one.scope == RoomFilter::Place::Areas && one.Valid() && one.Active());
	EXPECT(Matches(one, Make(Area::EuropeWest), Area::NaEast) && Matches(one, Make(Area::EuropeWest), Area::None));
	// Not its neighbours, and not a room without an area.
	EXPECT(!Matches(one, Make(Area::EuropeEast), Area::EuropeWest) && !Matches(one, Make(Area::None), Area::EuropeWest));
	// Several: a room in any of them is listed, one outside them is not, wherever the player is.
	RoomFilter several = one;
	several.SetArea(Area::NaEast, true); several.SetArea(Area::NaCentral, true); several.SetArea(Area::SouthAmerica, true);
	EXPECT(several.areas.Count() == 4 && several.Valid());
	EXPECT(Matches(several, Make(Area::EuropeWest), Area::Oceania) && Matches(several, Make(Area::NaEast), Area::Oceania));
	EXPECT(Matches(several, Make(Area::NaCentral), Area::None) && Matches(several, Make(Area::SouthAmerica), Area::None));
	EXPECT(!Matches(several, Make(Area::NaWest), Area::NaEast) && !Matches(several, Make(Area::CentralAmerica), Area::SouthAmerica));
	EXPECT(!Matches(several, Make(Area::None), Area::NaEast));
	// Areas with no ticked area is invalid, and a ticked area without Areas too.
	RoomFilter bare; bare.scope = RoomFilter::Place::Areas; EXPECT(!bare.Valid());
	RoomFilter stray; stray.areas = stray.areas.With(Area::NaEast, true); EXPECT(!stray.Valid());
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
	RoomFilter all; all.SetArea(Area::NaEast, true); all.type = RoomFilter::Type::Classic; all.openSeats = true; all.wiredOnly = true;
	EXPECT(Matches(all, only, Area::None) && !Matches(all, Make(Area::NaEast), Area::None) && !Matches(all, Make(Area::EuropeWest), Area::None));
	Room onlyFull = only; onlyFull.members = 8;
	EXPECT(!Matches(all, onlyFull, Area::None));
}

void TickingMovesBetweenAllNearAndAreas() {
	RoomFilter filter;
	filter.SetArea(Area::EastAsia, true); filter.SetArea(Area::Oceania, true);
	EXPECT(filter.scope == RoomFilter::Place::Areas && filter.areas.Count() == 2 && filter.Valid());
	// Unticking one keeps the other; unticking the last is All, and an area never ticked changes nothing.
	filter.SetArea(Area::EastAsia, false); filter.SetArea(Area::NaWest, false);
	EXPECT(filter.areas.Has(Area::Oceania) && !filter.areas.Has(Area::EastAsia) && filter.scope == RoomFilter::Place::Areas);
	filter.SetArea(Area::Oceania, false);
	EXPECT(filter == RoomFilter() && filter.Valid());
	// Ticking an area leaves Near me, and Near me clears the ticked areas.
	filter.SetNear(true); EXPECT(filter.scope == RoomFilter::Place::Near && filter.Valid());
	filter.SetArea(Area::NaWest, true); EXPECT(filter.scope == RoomFilter::Place::Areas && filter.areas.Count() == 1 && filter.areas.Has(Area::NaWest));
	filter.SetNear(true); EXPECT(filter.scope == RoomFilter::Place::Near && filter.areas.Empty());
	filter.SetNear(false); EXPECT(filter == RoomFilter());
	// None is no area: it cannot be ticked.
	filter.SetArea(Area::None, true); EXPECT(filter == RoomFilter());
}

void LeftAndRightSwitchAllAndNear() {
	RoomFilter filter;
	filter.StepNear(Area::NaWest); EXPECT(filter.scope == RoomFilter::Place::Near);
	filter.StepNear(Area::NaWest); EXPECT(filter == RoomFilter());
	// Ticked areas step to All, whichever way.
	filter.SetArea(Area::NaWest, true); filter.SetArea(Area::EuropeEast, true);
	filter.StepNear(Area::NaWest); EXPECT(filter == RoomFilter());
	// Near is not offered while the player's area is not set, and a saved Near steps off it.
	filter.StepNear(Area::None); EXPECT(filter == RoomFilter());
	filter.SetNear(true); filter.StepNear(Area::None); EXPECT(filter == RoomFilter());
}

void AreaSetsKeepTheirOrderAndIgnoreNone() {
	AreaSet set;
	EXPECT(set.Empty() && set.Count() == 0 && !set.Has(Area::None) && set.Codes().empty());
	set = set.With(Area::Oceania, true).With(Area::NaWest, true).With(Area::None, true);
	EXPECT(set.Count() == 2 && !set.Has(Area::None) && set.Codes() == (std::vector<std::string>{"naw", "oce"}));
	// Every area at once, and each one's bit is its own.
	for (const auto& row : areas::Table) set = set.With(row.area, true);
	EXPECT(set.Count() == areas::Count);
	set = set.With(Area::SouthAmerica, false); EXPECT(set.Count() == areas::Count - 1 && !set.Has(Area::SouthAmerica) && set.Has(Area::CentralAmerica));
}

void TypeSteps() {
	RoomFilter filter;
	const RoomFilter::Type order[] = {RoomFilter::Type::Classic, RoomFilter::Type::Arcade, RoomFilter::Type::King, RoomFilter::Type::All};
	for (const auto type : order) { filter.StepType(1); EXPECT(filter.type == type); }
	filter.StepType(-1); EXPECT(filter.type == RoomFilter::Type::King);
	filter.StepType(-1); EXPECT(filter.type == RoomFilter::Type::Arcade);
}

// A room with no area is listed under All and never under Near.
void ARoomWithNoAreaIsListedUnderAllOnly() {
	const Room placeless = Make(Area::None);
	RoomFilter near; near.SetNear(true);
	EXPECT(Matches(RoomFilter(), placeless, Area::NaEast) && !Matches(near, placeless, Area::NaEast));
	// Without the player's own area Near cannot tell rooms apart, so it lists them all.
	EXPECT(Matches(near, placeless, Area::None));
}

// Quick join's choice of opponents hides no room: it is not an active filter, and
// Clear filters keeps it while clearing everything else.
void QuickJoinsOpponentsAreNoFilter() {
	RoomFilter filter;
	EXPECT(filter.opponents == RoomFilter::Opponents::Similar);
	filter.opponents = RoomFilter::Opponents::Any;
	EXPECT(!filter.Active() && filter != RoomFilter() && filter.Cleared() == filter);
	filter.SetArea(Area::EastAsia, true); filter.type = RoomFilter::Type::Classic; filter.openSeats = true; filter.wiredOnly = true;
	EXPECT(filter.Active());
	const RoomFilter cleared = filter.Cleared();
	EXPECT(!cleared.Active() && cleared.opponents == RoomFilter::Opponents::Any && cleared.scope == RoomFilter::Place::All &&
		cleared.type == RoomFilter::Type::All && !cleared.openSeats && !cleared.wiredOnly);
	// It lists the same rooms either way.
	const Room room = Make(Area::EastAsia);
	RoomFilter similar = filter; similar.opponents = RoomFilter::Opponents::Similar;
	EXPECT(Matches(filter, room, Area::NaEast) == Matches(similar, room, Area::NaEast));
}
}

int main() {
	DefaultListsEverything();
	NearIsTheSameAreaOrANeighbour();
	TickedAreasAreThoseAreasAlone();
	TypeNeedsASentType();
	SeatsAndWired();
	TickingMovesBetweenAllNearAndAreas();
	LeftAndRightSwitchAllAndNear();
	AreaSetsKeepTheirOrderAndIgnoreNone();
	TypeSteps();
	ARoomWithNoAreaIsListedUnderAllOnly();
	QuickJoinsOpponentsAreNoFilter();
	if (!TEST_FAILURES()) std::printf("Public room filter tests passed\n");
	return TEST_EXIT_CODE();
}
