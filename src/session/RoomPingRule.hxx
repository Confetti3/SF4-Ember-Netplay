#pragma once
// A room's Max ping rule (ROOM_PING_RULE.md, Snapshot::maxPingMs): who a seat
// is compared against, and the verdict over the figures a seat request
// carries. The limits a host can choose are common/RoomRules.hxx's
// (MaxPingChoices). The authority and the board both ask these, so what the
// board offers and what the room takes cannot drift apart.
#include "RoomQueries.hxx"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sf4e { namespace room {

// Everyone a Queue at `table` could pair `member` with: in a room that seats
// from a table's queue, that table's fighters and its queue, since rotation
// can bring any of them across; in one that seats by cabinet, everyone at a
// cabinet, since the room pairs whoever waited longest. A member already at a
// cabinet who moves to another is not sitting down, and is compared with nobody.
inline std::vector<MemberId> PingOpponents(const Snapshot& room, MemberId member, std::size_t table) {
	std::vector<MemberId> opponents;
	const auto add = [&](MemberId other) {
		if (other && other != member && std::find(opponents.begin(), opponents.end(), other) == opponents.end()) opponents.push_back(other);
	};
	if (RulesOf(room).cabinets) {
		if (CabinetOf(room, member) >= 0) return opponents;
		for (const auto holder : room.cabinets) add(holder);
	} else if (table < TablesInPlay(room)) {
		const auto& at = room.tables[table];
		add(at.p1); add(at.p2);
		for (const auto queued : at.queue) add(queued);
	}
	return opponents;
}

// The first of `opponents` whose figure is over the room's limit, and that
// figure; none while the rule is off. An opponent with no figure, whose ping
// was not measured, never counts.
struct PingVerdict {
	MemberId opponent = 0;
	std::uint16_t ms = 0;
	bool Over() const { return opponent != 0; }
};
inline PingVerdict PingOverLimit(const Snapshot& room, const std::vector<MemberId>& opponents, const std::vector<PingFigure>& figures) {
	if (!room.maxPingMs) return {};
	for (const auto opponent : opponents)
		for (const auto& figure : figures)
			if (figure.member == opponent && figure.ms > room.maxPingMs) return {opponent, figure.ms};
	return {};
}

} }
