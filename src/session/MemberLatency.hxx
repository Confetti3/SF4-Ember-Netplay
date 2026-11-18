#pragma once
// The ping to each room member, as the helper reports it while the room board
// is open (MEMBER_LATENCY.md 3.2 and 3.9), and the board subscription that asks
// for it. Header only: the Iroh room, the runtime and the board share it.
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "RoomModel.hxx"

namespace sf4e { namespace session {

struct MemberLatency {
	enum class State { Measuring, Value, Unavailable };
	// The member's process incarnation the entry was measured for.
	std::uint64_t incarnation = 0;
	State state = State::Measuring;
	// A value's estimate, in whole milliseconds.
	unsigned rttMs = 0;
	// Whether a value was measured through a relay rather than directly.
	bool relay = false;
	// Why it is unavailable: unreachable, refused, in_match, no_latency_route or room_unavailable.
	std::string reason;
	bool operator==(const MemberLatency& other) const {
		return incarnation == other.incarnation && state == other.state && rttMs == other.rttMs && relay == other.relay && reason == other.reason;
	}
	bool operator!=(const MemberLatency& other) const { return !(*this == other); }
};
using MemberLatencyTable = std::map<room::MemberId, MemberLatency>;

// `table`'s entry for `member`, none when it has none or measured another
// incarnation of them.
inline const MemberLatency* CurrentLatency(const MemberLatencyTable& table, const room::Member& member) {
	const auto found = table.find(member.id);
	return found != table.end() && found->second.incarnation == member.incarnation ? &found->second : nullptr;
}

// The last value the helper measured to a member, for the incarnation it was
// measured for. The helper measures nothing while this PC or that member is
// in a game, watching one included, and nothing while the board is closed, so
// the room's Max ping weighs the last value measured before then.
struct KnownPing {
	std::uint64_t incarnation = 0;
	std::uint16_t ms = 0;
};
using KnownPings = std::map<room::MemberId, KnownPing>;

// This PC's ping to `member` as a seat request carries it: the helper's
// value now, or else the last one it measured to that incarnation of them.
inline std::optional<std::uint16_t> PingFigureOf(const MemberLatencyTable& table, const KnownPings& known, const room::Member& member) {
	const auto* latency = CurrentLatency(table, member);
	// Read() takes a value only from 1 to 999 ms, the figure's own range.
	if (latency && latency->state == MemberLatency::State::Value) return static_cast<std::uint16_t>(latency->rttMs);
	const auto found = known.find(member.id);
	if (found != known.end() && found->second.incarnation == member.incarnation) return found->second.ms;
	return std::nullopt;
}

// This PC's ping to every other member of `room` it has a figure for, as a
// seat request carries it (room::Action::pings): the room weighs the ones the
// seat could pair it with (room::PingOverLimit). A member never measured, or
// measured only for an earlier incarnation, has none.
inline std::vector<room::PingFigure> MeasuredPings(const MemberLatencyTable& table, const KnownPings& known, const room::Snapshot& room) {
	std::vector<room::PingFigure> figures;
	for (const auto& member : room.members) {
		if (member.id == room.localMember) continue;
		if (const auto ms = PingFigureOf(table, known, member)) figures.push_back({member.id, *ms});
	}
	return figures;
}

// Why a seat cannot be weighed yet, when this PC has no figure for one of
// `opponents` it could still measure. Measuring: the helper is measuring them
// now. AfterGame: this PC is in a game (`localInGame`, watching one included),
// so its helper measures nobody, and the opponent is out of one; their ping is
// measured once this PC leaves the game. An opponent in a game, one the helper
// cannot reach and one on a board with no table yet hold nothing: their ping
// cannot be measured (ROOM_PING_RULE.md).
enum class PingPending : std::uint8_t { None, Measuring, AfterGame };
inline PingPending PendingPing(const MemberLatencyTable& table, const KnownPings& known, const room::Snapshot& room,
	const std::vector<room::MemberId>& opponents, bool localInGame) {
	auto pending = PingPending::None;
	for (const auto opponent : opponents) {
		const auto* member = room::FindMember(room, opponent);
		if (!member || PingFigureOf(table, known, *member)) continue;
		const auto* latency = CurrentLatency(table, *member);
		if (latency && latency->state == MemberLatency::State::Measuring) return PingPending::Measuring;
		// The helper names every member room_unavailable while this PC is in a game.
		const bool measuresNobody = !latency || (latency->state == MemberLatency::State::Unavailable && latency->reason == "room_unavailable");
		if (localInGame && measuresNobody && !room::InGame(room, opponent)) pending = PingPending::AfterGame;
	}
	return pending;
}

// One board subscription at a time. View numbers never repeat; a view counts
// as active only once its latency_watch was queued, and a table is taken only
// for the active view of the epoch and room it was started in.
class LatencySubscription {
public:
	// How often an open subscription is renewed; the helper lets it lapse after 15 s.
	static constexpr std::uint64_t RenewMs = 5000;
	std::uint64_t Active() const { return active_; }
	const MemberLatencyTable& Table() const { return table_; }
	// The last value measured to each member the helper's last table listed,
	// kept when the view ends and when the helper stops measuring, until the
	// room epoch ends (Reset).
	const KnownPings& Known() const { return known_; }
	// The view to send now with on: true: a new number when none is active, the
	// active one when its renewal is due, or 0. A change of epoch or room ends
	// the active view and forgets its figures first.
	std::uint64_t Due(std::uint64_t epoch, const std::array<std::uint8_t, 16>& room, std::uint64_t nowMs) {
		if (active_ && (epoch != epoch_ || room != room_)) Reset();
		if (!active_) return ++next_;
		return nowMs - sentMs_ >= RenewMs ? active_ : 0;
	}
	// The latency_watch for `view` was queued.
	void Sent(std::uint64_t view, std::uint64_t epoch, const std::array<std::uint8_t, 16>& room, std::uint64_t nowMs) {
		active_ = view; epoch_ = epoch; room_ = room; sentMs_ = nowMs;
	}
	// Ends the active view and drops its table; returns the view to send on: false for, or 0.
	// The figures it measured stay known.
	std::uint64_t Stop() {
		const auto view = active_;
		active_ = 0; table_.clear();
		return view;
	}
	// A new room epoch: ends the active view as Stop does, and forgets every figure.
	std::uint64_t Reset() {
		known_.clear();
		return Stop();
	}
	// A member_latency event: taken only for the active view, epoch and room,
	// with at most one row per other member. False when it was dropped.
	bool Read(const nlohmann::json& event, std::uint64_t epoch, const std::array<std::uint8_t, 16>& room) {
		try {
			if (!active_ || event.at("view").get<std::uint64_t>() != active_ || event.at("epoch").get<std::uint64_t>() != epoch ||
				epoch != epoch_ || event.at("room").get<std::array<std::uint8_t, 16>>() != room || room != room_) return false;
			const auto& rows = event.at("members");
			if (!rows.is_array() || rows.size() > room::MaxMembers - 1) return false;
			MemberLatencyTable table;
			for (const auto& row : rows) {
				MemberLatency entry;
				entry.incarnation = row.at("incarnation").get<std::uint64_t>();
				const auto state = row.at("state").get<std::string>();
				if (state == "value") {
					entry.state = MemberLatency::State::Value;
					entry.rttMs = row.at("rtt_ms").get<unsigned>();
					if (!entry.rttMs || entry.rttMs > 999) return false;
					const auto route = row.at("route").get<std::string>();
					if (route != "direct" && route != "relay") return false;
					entry.relay = route == "relay";
				} else if (state == "unavailable") {
					entry.state = MemberLatency::State::Unavailable;
					entry.reason = row.at("reason").get<std::string>();
				} else if (state != "measuring") return false;
				const auto member = row.at("member").get<room::MemberId>();
				if (!member || !table.emplace(member, std::move(entry)).second) return false;
			}
			// A value is the member's figure now; any other row keeps the one
			// measured last to the same incarnation. A member the table no
			// longer lists, or lists as another incarnation, has none left.
			KnownPings known;
			for (const auto& row : table) {
				const auto kept = known_.find(row.first);
				if (row.second.state == MemberLatency::State::Value)
					known[row.first] = {row.second.incarnation, static_cast<std::uint16_t>(row.second.rttMs)};
				else if (kept != known_.end() && kept->second.incarnation == row.second.incarnation)
					known.insert(*kept);
			}
			table_ = std::move(table);
			known_ = std::move(known);
			return true;
		} catch (const std::exception&) { return false; }
	}

private:
	std::uint64_t next_ = 0, active_ = 0, epoch_ = 0, sentMs_ = 0;
	std::array<std::uint8_t, 16> room_{};
	MemberLatencyTable table_;
	KnownPings known_;
};

} }
