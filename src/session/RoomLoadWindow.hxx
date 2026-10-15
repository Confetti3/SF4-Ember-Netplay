#pragma once

// The room's load line: the helper's load reports (every second in a match,
// every five otherwise) folded over a minute, with how the room's commits
// moved in that time. Pure component: no helper or game dependencies.

#include <algorithm>
#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

namespace sf4e { namespace session {

class RoomLoadWindow {
public:
	static constexpr std::uint64_t PeriodMs = 60000;
	// One helper_load event. A helper from before control_queue_peak does not
	// send it, which reads as unavailable rather than as an empty queue.
	struct Report {
		std::uint64_t eventQueueFreeMin = 0;
		std::optional<std::uint64_t> controlQueuePeak;
	};
	static Report Read(const nlohmann::json& event) {
		Report report;
		report.eventQueueFreeMin = event.at("event_queue_free_min").get<std::uint64_t>();
		if (event.contains("control_queue_peak")) report.controlQueuePeak = event.at("control_queue_peak").get<std::uint64_t>();
		return report;
	}
	// The room as this process has applied it when a report arrives.
	// liveMembers is empty until a commit has applied here: a member whose
	// first commit never applies has no roster, but is the stall to report.
	struct Room {
		std::optional<std::size_t> liveMembers;
		std::size_t staged = 0;
		std::uint64_t revision = 0, applied = 0;
		bool markerParked = false;
	};
	// A minute's line: the room at its end, commits made and applied during it,
	// and the worst the helper reported in any of its reports.
	struct Line {
		std::uint64_t seconds = 0, commits = 0, applied = 0, eventQueueFreeMin = 0;
		Room room;
		std::optional<std::uint64_t> controlQueuePeak;
	};
	// Folds a report in. Once a minute has passed since the window began, the
	// window starts over and its line is returned, unless the applied roster
	// shows a room with no live member.
	std::optional<Line> Add(const Report& report, std::uint64_t nowMs, const Room& room) {
		if (!started_) Restart(nowMs, room);
		if (report.controlQueuePeak) controlQueuePeak_ = (std::max)(controlQueuePeak_.value_or(0), *report.controlQueuePeak);
		eventQueueFreeMin_ = (std::min)(eventQueueFreeMin_.value_or(report.eventQueueFreeMin), report.eventQueueFreeMin);
		if (nowMs < startedMs_ || nowMs - startedMs_ < PeriodMs) return std::nullopt;
		Line line;
		line.seconds = (nowMs - startedMs_) / 1000;
		line.commits = room.revision >= revision_ ? room.revision - revision_ : 0;
		line.applied = room.applied >= applied_ ? room.applied - applied_ : 0;
		line.eventQueueFreeMin = *eventQueueFreeMin_;
		line.controlQueuePeak = controlQueuePeak_;
		line.room = room;
		Restart(nowMs, room);
		if (room.liveMembers == std::size_t(0)) return std::nullopt;
		return line;
	}
	void Reset() { *this = RoomLoadWindow(); }

private:
	void Restart(std::uint64_t nowMs, const Room& room) {
		started_ = true; startedMs_ = nowMs; revision_ = room.revision; applied_ = room.applied;
		controlQueuePeak_.reset(); eventQueueFreeMin_.reset();
	}
	bool started_ = false;
	std::uint64_t startedMs_ = 0, revision_ = 0, applied_ = 0;
	std::optional<std::uint64_t> controlQueuePeak_, eventQueueFreeMin_;
};

} }
