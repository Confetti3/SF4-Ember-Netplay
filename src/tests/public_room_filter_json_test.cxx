// The public rooms filter as it is saved (netplay/PublicRoomFilterJson.hxx), through the settings store.
#include "../netplay/SettingsStore.hxx"
#include "../netplay/PublicRoomFilterJson.hxx"
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>

#include "test_support.hxx"
#include "temp_root.hxx"
using Json = nlohmann::json;
using Path = std::filesystem::path;
using sf4e::netplay::SettingsStore;

// The public rooms filter is one object in the netplay section. An older profile
// has none, and a field out of range, of another type or naming a place that is
// not an area (a relay's code, the old US West) loads as All (a flag as off) on its own; the one-area
// shape builds before several areas saved still loads.
static void RoomFilterRoundTrips(const Path& root) {
    using namespace sf4e;
    using netplay::publicrooms::RoomFilter;
    const auto folder=root/L"room-filter";
    CHECK(std::filesystem::create_directory(folder));
    SettingsStore store(folder.wstring()); std::string error;
    const auto picked=[](std::initializer_list<Area> ticked,RoomFilter::Type type,bool seats,bool wired) {
        RoomFilter filter; for (const Area area : ticked) filter.SetArea(area,true);
        filter.type=type; filter.openSeats=seats; filter.wiredOnly=wired; return filter;
    };
    auto nearMe=picked({},RoomFilter::Type::King,true,false); nearMe.SetNear(true);
    for (const auto& pick : {RoomFilter(), nearMe, picked({Area::EuropeWest},RoomFilter::Type::Classic,false,true),
                             picked({Area::Oceania},RoomFilter::Type::Arcade,true,true),
                             picked({Area::NaEast,Area::NaCentral,Area::SouthAmerica},RoomFilter::Type::All,false,false),
                             picked({Area::NaWest,Area::NaCentral,Area::NaEast,Area::CentralAmerica,Area::SouthAmerica,Area::EuropeWest,Area::EuropeEast,
                                     Area::MiddleEastAfrica,Area::SouthAsia,Area::EastAsia,Area::SouthEastAsia,Area::Oceania},RoomFilter::Type::King,true,true)}) {
        netplay::PlayerPreferences prefs; prefs.roomFilter=pick; CHECK(prefs.Valid());
        Json saved; netplay::WriteRoomFilter(prefs,saved);
        CHECK(store.SaveLauncher(saved,error));
        std::ifstream file(folder/L"settings.json",std::ios::binary);
        CHECK(Json::parse(file)["netplay"]["roomFilter"]["wired"]==pick.wiredOnly);
        Json loaded; CHECK(store.LoadLauncher(loaded,error));
        CHECK(netplay::ReadRoomFilter(loaded)==pick);
    }
    CHECK(netplay::ReadRoomFilter(Json::object())==RoomFilter());
    CHECK(netplay::ReadRoomFilter(Json("filter"))==RoomFilter());
    CHECK(netplay::ReadRoomFilter({{"roomFilter",7}})==RoomFilter());
    // A place that is not an area falls back to All without losing the rest: a relay's code a build
    // before areas saved, the old US West no build could report, or anything else.
    for (const char* gone : {"mars","use1","usw1","euc1","aps1","other","NAW"}) {
        const auto read=netplay::ReadRoomFilter({{"roomFilter",{{"region",gone},{"type","arcade"},{"seats",true},{"wired",true}}}});
        CHECK(read.scope==RoomFilter::Place::All&&read.areas.Empty()&&read.type==RoomFilter::Type::Arcade&&read.openSeats&&read.wiredOnly&&read.Valid());
    }
    // The shape before several areas: one area's code as the region is that one area, still valid.
    const auto old=netplay::ReadRoomFilter({{"roomFilter",{{"region","sam"},{"type","king"},{"seats",true}}}});
    CHECK(old.scope==RoomFilter::Place::Areas&&old.areas.Count()==1&&old.areas.Has(Area::SouthAmerica)&&old.type==RoomFilter::Type::King&&old.openSeats&&old.Valid());
    // The saved shape: "areas" is read under "areas" alone, and codes that are not areas are dropped.
    const auto several=netplay::ReadRoomFilter({{"roomFilter",{{"region","areas"},{"areas",{"nae","mars",4,"nac","NAE"}},{"wired",true}}}});
    CHECK(several.scope==RoomFilter::Place::Areas&&several.areas.Count()==2&&several.areas.Has(Area::NaEast)&&several.areas.Has(Area::NaCentral)&&several.wiredOnly&&several.Valid());
    for (const Json& none : {Json::array(),Json::array({"mars"}),Json("naw"),Json(nullptr)}) {
        const auto read=netplay::ReadRoomFilter({{"roomFilter",{{"region","areas"},{"areas",none},{"seats",true}}}});
        CHECK(read.scope==RoomFilter::Place::All&&read.areas.Empty()&&read.openSeats&&read.Valid());
    }
    CHECK(netplay::ReadRoomFilter({{"roomFilter",{{"region","areas"}}}})==RoomFilter());
    // "areas" beside any other region is ignored; the file says what it was written as.
    CHECK(netplay::ReadRoomFilter({{"roomFilter",{{"region","near"},{"areas",Json::array({"naw"})}}}}).scope==RoomFilter::Place::Near);
    CHECK(netplay::ReadRoomFilter({{"roomFilter",{{"region","all"},{"areas",Json::array({"naw"})}}}})==RoomFilter());
    Json written; netplay::PlayerPreferences chosen; chosen.roomFilter=picked({Area::Oceania,Area::NaWest},RoomFilter::Type::All,false,false);
    netplay::WriteRoomFilter(chosen,written);
    CHECK(written["roomFilter"]["region"]=="areas"&&written["roomFilter"]["areas"]==Json::array({"naw","oce"}));
    netplay::WriteRoomFilter(netplay::PlayerPreferences(),written);
    CHECK(written["roomFilter"]["region"]=="all"&&written["roomFilter"]["areas"].empty());
    const auto odd=netplay::ReadRoomFilter({{"roomFilter",{{"region",3},{"type","brawl"},{"seats","yes"},{"wired",1}}}});
    CHECK(odd==RoomFilter());
    CHECK(netplay::ReadRoomFilter({{"roomFilter",{{"region","near"}}}}).scope==RoomFilter::Place::Near);
    // Preferences carrying a filter that names no area to filter by are invalid.
    netplay::PlayerPreferences invalid; invalid.roomFilter.scope=RoomFilter::Place::Areas; CHECK(!invalid.Valid());
    invalid.roomFilter.SetArea(Area::NaEast,true); CHECK(invalid.Valid());
}

int main() {
    const auto root = MakeTempRoot(L"sf4e-room-filter-test-");
    CHECK(std::filesystem::create_directory(root));
    RoomFilterRoundTrips(root);
    RemoveTempRoot(root);
    std::cout << "Public room filter settings checks passed\n";
    return 0;
}
