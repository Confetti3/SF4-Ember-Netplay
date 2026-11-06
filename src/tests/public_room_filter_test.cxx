// The public rooms filter (netplay/PublicRoomFilter.hxx): which rooms it lists
// by region, type, free seats and connection, how its settings step, and what
// Quick join takes from it.
#include "../netplay/PublicRoomFilterMatch.hxx"

#include <string>

#include "test_support.hxx"

using namespace sf4e::netplay::publicrooms;
using namespace sf4e::room;

namespace {
// A room with a free seat in `region`; Classic unless `type` says none or other.
Room Make(const std::string& region, std::optional<RoomType> type = ClassicRoom) {
	Room room;
	room.id = "r"; room.name = "Open room"; room.region = region;
	room.members = 2; room.capacity = 8;
	room.type = type;
	return room;
}

void DefaultListsEverything() {
	const RoomFilter filter;
	EXPECT(!filter.Active() && filter.Valid());
	Room full = Make("euc1", std::nullopt); full.members = 8; full.locked = true; full.wiredOnly = true;
	EXPECT(Matches(filter, full, "") && Matches(filter, full, "use1") && Matches(filter, Make("aps1", ArcadeRoom), "use1"));
}

void RegionNearAndOne() {
	RoomFilter near; near.scope = RoomFilter::Region::Near;
	EXPECT(near.Active() && near.Valid());
	EXPECT(Matches(near, Make("use1"), "use1") && !Matches(near, Make("euc1"), "use1"));
	// Near with no home region cannot say which rooms are near: every region is listed.
	EXPECT(Matches(near, Make("euc1"), ""));
	RoomFilter one; one.scope = RoomFilter::Region::One; one.region = "euc1";
	EXPECT(one.Valid() && Matches(one, Make("euc1"), "use1") && Matches(one, Make("euc1"), "") && !Matches(one, Make("use1"), "use1"));
	// A region that is not one to filter by is invalid, and a region without the One scope too.
	one.region = "mars"; EXPECT(!one.Valid());
	one.region = ""; EXPECT(!one.Valid());
	RoomFilter stray; stray.region = "use1"; EXPECT(!stray.Valid());
	for (const char* code : FilterRegions()) EXPECT(KnownFilterRegion(code));
	EXPECT(!KnownFilterRegion("other") && !KnownFilterRegion(""));
}

void TypeNeedsASentType() {
	RoomFilter filter;
	filter.type = RoomFilter::Type::Arcade;
	EXPECT(Matches(filter, Make("use1", ArcadeRoom), "") && !Matches(filter, Make("use1", ClassicRoom), "") && !Matches(filter, Make("use1", KingRoom), ""));
	// A room that sent no type passes under All only.
	EXPECT(!Matches(filter, Make("use1", std::nullopt), ""));
	filter.type = RoomFilter::Type::Classic;
	EXPECT(Matches(filter, Make("use1", ClassicRoom), "") && !Matches(filter, Make("use1", std::nullopt), ""));
	filter.type = RoomFilter::Type::King;
	EXPECT(Matches(filter, Make("use1", KingRoom), "") && !Matches(filter, Make("use1", ArcadeRoom), ""));
	filter.type = RoomFilter::Type::All;
	EXPECT(Matches(filter, Make("use1", std::nullopt), ""));
}

void SeatsAndWired() {
	RoomFilter seats; seats.openSeats = true;
	Room full = Make("use1"); full.members = 8;
	Room locked = Make("use1"); locked.locked = true;
	EXPECT(Matches(seats, Make("use1"), "") && !Matches(seats, full, "") && !Matches(seats, locked, ""));
	RoomFilter wired; wired.wiredOnly = true;
	Room only = Make("use1"); only.wiredOnly = true;
	EXPECT(Matches(wired, only, "") && !Matches(wired, Make("use1"), ""));
	RoomFilter all; all.scope = RoomFilter::Region::One; all.region = "use1"; all.type = RoomFilter::Type::Classic; all.openSeats = true; all.wiredOnly = true;
	EXPECT(Matches(all, only, "") && !Matches(all, Make("use1"), "") && !Matches(all, Make("euc1"), ""));
	Room onlyFull = only; onlyFull.members = 8;
	EXPECT(!Matches(all, onlyFull, ""));
}

void RegionStepsInOrder() {
	RoomFilter filter;
	const char* order[] = {"all", "near", "use1", "usw1", "euc1", "aps1"};
	const auto at = [&] {
		return filter.scope == RoomFilter::Region::All ? std::string("all") : filter.scope == RoomFilter::Region::Near ? std::string("near") : filter.region;
	};
	for (int i = 1; i <= 6; ++i) { filter.StepRegion(1, "use1"); EXPECT(at() == order[i % 6] && filter.Valid()); }
	filter.StepRegion(-1, "use1"); EXPECT(at() == "aps1");
	filter.StepRegion(-1, "use1"); EXPECT(at() == "euc1");
	// Near is not offered while the home region is unknown.
	filter = RoomFilter();
	filter.StepRegion(1, ""); EXPECT(at() == "use1");
	filter.StepRegion(-1, ""); EXPECT(at() == "all");
	filter.StepRegion(-1, ""); EXPECT(at() == "aps1");
	// A saved Near with no region known steps off it, and an unknown region steps from All.
	filter = RoomFilter(); filter.scope = RoomFilter::Region::Near;
	filter.StepRegion(1, ""); EXPECT(at() == "use1");
	filter.scope = RoomFilter::Region::One; filter.region = "mars";
	filter.StepRegion(1, "use1"); EXPECT(at() == "near" && filter.Valid());
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
	EXPECT(filter.ForQuickJoin().scope == RoomFilter::Region::Near && filter.ForQuickJoin().type == RoomFilter::Type::All);
	EXPECT(filter.scope == RoomFilter::Region::All);
	// The player's own region choice, and every other setting, is kept.
	filter.scope = RoomFilter::Region::One; filter.region = "aps1"; filter.type = RoomFilter::Type::Classic; filter.openSeats = true; filter.wiredOnly = true;
	EXPECT(filter.ForQuickJoin() == filter);
}
}

int main() {
	DefaultListsEverything();
	RegionNearAndOne();
	TypeNeedsASentType();
	SeatsAndWired();
	RegionStepsInOrder();
	TypeSteps();
	QuickJoinStaysNearUnlessAskedOtherwise();
	if (!TEST_FAILURES()) std::printf("Public room filter tests passed\n");
	return TEST_EXIT_CODE();
}
