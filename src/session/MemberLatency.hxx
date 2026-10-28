#pragma once
// The ping to each room member, as the helper reports it while the room board
// is open (MEMBER_LATENCY.md 3.2 and 3.9), and the board subscription that asks
// for it. Header only: the Iroh room, the runtime and the board share it.
#include <array>
#include <cstdint>
#include <exception>
#include <map>
#include <string>
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

// One board subscription at a time. View numbers never repeat; a view counts
// as active only once its latency_watch was queued, and a table is taken only
// for the active view of the epoch and room it was started in.
class LatencySubscription {
public:
	// How often an open subscription is renewed; the helper lets it lapse after 15 s.
	static constexpr std::uint64_t RenewMs = 5000;
	std::uint64_t Active() const { return active_; }
	const MemberLatencyTable& Table() const { return table_; }
	// The view to send now with on: true: a new number when none is active, the
	// active one when its renewal is due, or 0. A change of epoch or room ends
	// the active view first.
	std::uint64_t Due(std::uint64_t epoch, const std::array<std::uint8_t, 16>& room, std::uint64_t nowMs) {
		if (active_ && (epoch != epoch_ || room != room_)) Stop();
		if (!active_) return ++next_;
		return nowMs - sentMs_ >= RenewMs ? active_ : 0;
	}
	// The latency_watch for `view` was queued.
	void Sent(std::uint64_t view, std::uint64_t epoch, const std::array<std::uint8_t, 16>& room, std::uint64_t nowMs) {
		active_ = view; epoch_ = epoch; room_ = room; sentMs_ = nowMs;
	}
	// Ends the active view and drops its table; returns the view to send on: false for, or 0.
	std::uint64_t Stop() {
		const auto view = active_;
		active_ = 0; table_.clear();
		return view;
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
			table_ = std::move(table);
			return true;
		} catch (const std::exception&) { return false; }
	}

private:
	std::uint64_t next_ = 0, active_ = 0, epoch_ = 0, sentMs_ = 0;
	std::array<std::uint8_t, 16> room_{};
	MemberLatencyTable table_;
};

} }
