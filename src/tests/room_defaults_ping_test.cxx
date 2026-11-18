// Max ping as a room default and a template's (netplay::RoomPreferences):
// written only when on, off when the key is missing, and a limit no room
// offers does not read.
#include "../netplay/RoomPreferences.hxx"

#include <cstdio>

#include "test_support.hxx"

using Json = nlohmann::json;
using namespace sf4e;

int main() {
	netplay::PlayerPreferences defaults;
	const Json plainDefaults = netplay::RoomPreferences(defaults);
	CHECK(!plainDefaults.contains("maxPingMs") && defaults.roomMaxPingMs == 0);

	auto limited = defaults;
	limited.roomMaxPingMs = 120;
	netplay::RoomTemplate saved;
	saved.name = "Near"; saved.roomName = "Low ping room"; saved.maxPingMs = 80;
	saved.tables = netplay::TableSetup::Same(defaults.tableRules);
	limited.roomTemplates = {saved};
	const Json document{{"roomDefaults", netplay::RoomPreferences(limited)}};
	CHECK(document["roomDefaults"].at("maxPingMs") == 120 && document["roomDefaults"]["templates"][0].at("maxPingMs") == 80);
	netplay::PlayerPreferences read;
	CHECK(netplay::ReadRoomPreferences(document, read) && read.roomMaxPingMs == 120 && read.roomTemplates.size() == 1 &&
		read.roomTemplates[0] == saved);

	auto plain = document;
	plain["roomDefaults"].erase("maxPingMs");
	plain["roomDefaults"]["templates"][0].erase("maxPingMs");
	CHECK(netplay::ReadRoomPreferences(plain, read) && read.roomMaxPingMs == 0 && read.roomTemplates.size() == 1 && read.roomTemplates[0].maxPingMs == 0);

	for (const Json& invalid : {Json(90), Json("120"), Json(true)}) {
		auto bad = document;
		bad["roomDefaults"]["maxPingMs"] = invalid;
		CHECK(!netplay::ReadRoomPreferences(bad, read));
	}
	// A template whose limit no room offers is left out, and the defaults still load.
	auto badTemplate = document;
	badTemplate["roomDefaults"]["templates"][0]["maxPingMs"] = 90;
	CHECK(netplay::ReadRoomPreferences(badTemplate, read) && read.roomMaxPingMs == 120 && read.roomTemplates.empty());
	// So is a preference no room offers.
	auto odd = limited;
	odd.roomMaxPingMs = 90;
	CHECK(!odd.Valid());

	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room defaults max ping passed.\n");
	return 0;
}
