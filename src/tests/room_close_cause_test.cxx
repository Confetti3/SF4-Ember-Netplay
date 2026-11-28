// A leave of the room through the netplay runtime's real dispatch names who
// asked for it in the log, so a player's log tells a Leave room row from a
// friend visit, a Discord invitation or the tournament (B156: a public room
// left with no word of why).
#include "runtime_room_fixture.hxx"
#include "../session/RoomModel.hxx"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace sf4e;
namespace facade = sf4e::NetplayFacade;
namespace internal = sf4e::NetplayFacade::internal;
using test::RuntimeRoom;

namespace {
// The log lines a leave asked by `cause` writes, the room left after.
std::vector<std::string> LeaveLines(commands::CloseCause cause) {
	RuntimeRoom r;
	r.runtime->room = r.room;
	room::RoomAuthority authority("Leave room", 16, 95);
	const auto host = authority.Join("Host", room::ConnectionRef{"host", "1"}, true);
	const auto local = authority.Join(r.name, room::ConnectionRef{"host", "2"}, false);
	CHECK(host.accepted && local.accepted);
	r.Snapshot(authority.SnapshotFor(local.snapshot.members.back().id));
	auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(32);
	sink->set_pattern("%v");
	spdlog::default_logger()->sinks().push_back(sink);
	commands::LeaveRoom leave{r.runtime->controller.GetSnapshot().generation};
	leave.cause = cause;
	CHECK(facade::SubmitRuntimeCommand(leave));
	internal::DrainCommands(true);
	auto& sinks = spdlog::default_logger()->sinks();
	sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
	// Still on the roster: the Leave goes to the room, and its ack closes it.
	CHECK(r.runtime->leave.leaveRequested);
	r.runtime->room.reset();
	r.runtime->leave.leaveRequested = false;
	return sink->last_formatted();
}
// The log lines a tick writes when the room's snapshot no longer lists this PC while it is Joined.
// No fake room is bound: a failed one would close the room as its router failing first.
std::vector<std::string> RemovedLines() {
	RuntimeRoom r;
	room::RoomAuthority authority("Leave room", 16, 95);
	const auto host = authority.Join("Host", room::ConnectionRef{"host", "1"}, true);
	CHECK(host.accepted);
	auto snapshot = authority.SnapshotFor(host.snapshot.members.back().id);
	snapshot.localMember = 0;
	r.Snapshot(snapshot);
	auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(32);
	sink->set_pattern("%v");
	spdlog::default_logger()->sinks().push_back(sink);
	facade::TickRuntime();
	auto& sinks = spdlog::default_logger()->sinks();
	sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
	return sink->last_formatted();
}
bool Says(const std::vector<std::string>& lines, const std::string& text) {
	return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}
}

int main() {
	CHECK_MSG(Says(LeaveLines(commands::CloseCause::Menu), "Room: leave asked cause=menu"), "%s", "a Leave room row's leave is not named");
	CHECK_MSG(Says(LeaveLines(commands::CloseCause::FriendVisit), "Room: leave asked cause=friend_visit"), "%s", "a friend visit's leave is not named");
	CHECK_MSG(Says(RemovedLines(), "Room: closing cause=removed "), "%s", "a removal from the room is not named in the closing line");
	std::cout << "Room close cause tests passed\n";
	return 0;
}
