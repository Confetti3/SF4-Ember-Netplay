// The room host's side of a public room's own delay: the supervisor's
// configuration line (server/ember-rooms/src/protocol.rs ChildConfig) names it
// only for a room that has it, the room host makes the room with that policy,
// and its status line reports it only then. The same line the supervisor's own
// tests build is parsed here.
#include "../../server/roomhost/RoomHost.hxx"
#include <string>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
using nlohmann::json;

namespace {
// The line the supervisor writes for a room (ChildConfig): the request's fields,
// own_delay only for a room that asked for it, then the helper and the ports.
std::string Line(const std::string& ownDelay) {
	return std::string(R"({"room_id":"0123456789abcdef0123456789abcdef","name":"Friday night","capacity":8,)") +
		R"("build_id":"b1","creator":"emb_abc","bridge_id":"brg_abc","ticket_key":"a2V5","ticket_kid":"k1")" + ownDelay +
		R"(,"helper":"helper-path","port":45800,"coordination_port":45801})";
}

void TestParse() {
	roomhost::Config config;
	std::string error;
	// No key: the shared delay.
	CHECK(roomhost::ParseConfig(Line(""), config, error) && !config.ownDelay);
	CHECK(config.name == "Friday night" && config.capacity == 8 && config.port == 45800 && config.coordinationPort == 45801);
	// An explicit false is the shared delay too, and a parse starts from it.
	config.ownDelay = true;
	CHECK(roomhost::ParseConfig(Line(R"(,"own_delay":false)"), config, error) && !config.ownDelay);
	config.ownDelay = true;
	CHECK(roomhost::ParseConfig(Line(""), config, error) && !config.ownDelay);
	CHECK(roomhost::ParseConfig(Line(R"(,"own_delay":true)"), config, error) && config.ownDelay);
	// Anything but a boolean is not a configuration.
	for (const char* bad : {R"(,"own_delay":1)", R"(,"own_delay":"true")", R"(,"own_delay":null)", R"(,"own_delay":[true])"}) {
		roomhost::Config rejected;
		error.clear();
		CHECK(!roomhost::ParseConfig(Line(bad), rejected, error) && error == "own_delay");
	}
}

// From the configuration line to the room's snapshot and its status line.
void TestRoomFollowsTheConfiguration() {
	for (const bool own : {false, true}) {
		roomhost::Config config;
		std::string error;
		CHECK(roomhost::ParseConfig(Line(own ? R"(,"own_delay":true)" : ""), config, error));
		SessionServer server("room-host", "b1", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(new MockTransport()));
		CHECK(roomhost::EnableConfiguredRoom(server, config, 91));
		const auto* snapshot = server.RoomSnapshot();
		CHECK(snapshot && snapshot->serverOwned && snapshot->name == "Friday night" && snapshot->capacity == 8);
		CHECK(snapshot->delayPolicy == (own ? room::DelayPolicy::Own : room::DelayPolicy::Shared));
		// The room members get is the one that policy describes.
		CHECK(json(*snapshot).contains("delay") == own);
		// And so does the status line the bridge reads.
		const auto details = roomhost::DetailsOf(*snapshot);
		CHECK(details.ownDelay == own);
		const auto line = json::parse(roomhost::StatusLine({0, 0, "sf4e3:x", {}, details})).at("details");
		CHECK(line.contains("own_delay") == own);
		if (own) CHECK(line.at("own_delay") == true);
		// A second room cannot be made over it.
		CHECK(!roomhost::EnableConfiguredRoom(server, config, 91));
	}
}
}

int main() {
	TestParse();
	TestRoomFollowsTheConfiguration();
	std::puts("Room host config test passed");
	return 0;
}
