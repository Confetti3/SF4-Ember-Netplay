// Pure unit tests for the room's once-a-minute load line.

#include "../session/RoomLoadWindow.hxx"

#include "test_support.hxx"

using nlohmann::json;
using sf4e::session::RoomLoadWindow;

static RoomLoadWindow::Room Room(std::size_t liveMembers, std::uint64_t revision, std::uint64_t applied) {
	RoomLoadWindow::Room room;
	room.liveMembers = liveMembers; room.revision = revision; room.applied = applied;
	return room;
}

static RoomLoadWindow::Report Load(std::uint64_t freeMin, std::uint64_t controlPeak) {
	RoomLoadWindow::Report report;
	report.eventQueueFreeMin = freeMin; report.controlQueuePeak = controlPeak;
	return report;
}

// A helper from before control_queue_peak reads as unavailable, and fields a
// newer helper adds are ignored.
static void TestReadsOlderAndNewerHelpers() {
	const auto older = RoomLoadWindow::Read(json{{"type", "helper_load"}, {"epoch", 3}, {"actor_tick_lag_max_us", 1},
		{"actor_tick_body_max_us", 2}, {"event_queue_free_min", 90}});
	CHECK(older.eventQueueFreeMin == 90 && !older.controlQueuePeak);
	const auto newer = RoomLoadWindow::Read(json{{"type", "helper_load"}, {"epoch", 3}, {"actor_tick_lag_max_us", 1},
		{"actor_tick_body_max_us", 2}, {"event_queue_free_min", 90}, {"control_queue_peak", 47}, {"later_field", "x"}});
	CHECK(newer.eventQueueFreeMin == 90 && newer.controlQueuePeak == std::uint64_t(47));
}

// One line a minute holds the worst of every report in it: a busy report early
// in the minute is not hidden by the quiet ones after it.
static void TestMinuteKeepsTheWorstReport() {
	RoomLoadWindow window;
	CHECK(!window.Add(Load(128, 0), 1000, Room(3, 10, 10)));
	CHECK(!window.Add(Load(12, 47), 6000, Room(3, 14, 12)));
	for (std::uint64_t now = 11000; now < 61000; now += 5000) CHECK(!window.Add(Load(127, 1), now, Room(3, 20, 18)));
	const auto line = window.Add(Load(128, 0), 61000, Room(3, 25, 25));
	CHECK(line && line->seconds == 60 && line->commits == 15 && line->applied == 15);
	CHECK(line->eventQueueFreeMin == 12 && line->controlQueuePeak == std::uint64_t(47) && line->room.liveMembers == std::size_t(3));
	// The next minute starts over.
	const auto next = window.Add(Load(120, 2), 121000, Room(3, 25, 25));
	CHECK(next && next->commits == 0 && next->eventQueueFreeMin == 120 && next->controlQueuePeak == std::uint64_t(2));
}

// Commits arriving and none applying is the stall the line is for.
static void TestStalledRoom() {
	RoomLoadWindow window;
	window.Add(Load(128, 0), 0, Room(4, 100, 100));
	RoomLoadWindow::Room stalled = Room(4, 106, 100);
	stalled.staged = 4; stalled.markerParked = true;
	const auto line = window.Add(Load(128, 0), 60000, stalled);
	CHECK(line && line->commits == 6 && line->applied == 0 && line->room.staged == 4 && line->room.markerParked);
}

// A member that joined and whose first commit never applied has no roster yet.
// That is the stall, so it is reported, with the member count unknown.
static void TestFirstCommitNeverApplies() {
	RoomLoadWindow window;
	RoomLoadWindow::Room joining;
	joining.revision = 40;
	window.Add(Load(128, 0), 0, joining);
	joining.revision = 46; joining.staged = 4; joining.markerParked = true;
	const auto line = window.Add(Load(128, 0), 60000, joining);
	CHECK(line && !line->room.liveMembers && line->commits == 6 && line->applied == 0 && line->room.staged == 4);
}

// A room host with nobody in its room logs nothing, and an older helper's
// window has no control queue peak to report.
static void TestQuietCases() {
	RoomLoadWindow empty;
	empty.Add(Load(128, 5), 0, Room(0, 1, 1));
	CHECK(!empty.Add(Load(128, 5), 60000, Room(0, 1, 1)));
	RoomLoadWindow older;
	RoomLoadWindow::Report report; report.eventQueueFreeMin = 100;
	older.Add(report, 0, Room(2, 1, 1));
	const auto line = older.Add(report, 60000, Room(2, 1, 1));
	CHECK(line && !line->controlQueuePeak && line->eventQueueFreeMin == 100);
}

int main() {
	TestReadsOlderAndNewerHelpers();
	TestMinuteKeepsTheWorstReport();
	TestStalledRoom();
	TestFirstCommitNeverApplies();
	TestQuietCases();
	return 0;
}
