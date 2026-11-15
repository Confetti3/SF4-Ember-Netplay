// What a room card shows for a member's ping (MEMBER_LATENCY.md, "What the
// member list shows"): a figure measured through a relay says so and takes no
// band colour, a direct one is plain and banded, the connection check's figure
// on the match's path wins while it counts, and nothing is shown for ourselves,
// for another incarnation of the member, or without a route or a room.
#include "../ui/LatencyMark.hxx"
#include "../common/Localization.hxx"
#include <iostream>
#include <string>

#include "test_support.hxx"

using namespace sf4e;

namespace {
using State = session::MemberLatency::State;
using Kind = ui::PingDecision::Kind;
}

int main() {
	loc::SetActive(loc::Locale::En);
	room::Member member; member.id = 2; member.incarnation = 22;
	session::MemberLatencyTable table;
	std::optional<ui::CheckedPing> checked;
	const auto entry = [&](State state, unsigned rtt = 0, const char* reason = "", bool relay = false) {
		session::MemberLatency latency; latency.incarnation = 22; latency.state = state; latency.rttMs = rtt; latency.reason = reason; latency.relay = relay;
		table[2] = latency;
		return ui::LatencyMarkFor(table, member, 1, checked);
	};
	const ImU32 neutral = ui::LatencyMark().color;

	// A private room's direct figure predicts the match: plain, in the bands' colours.
	auto mark = entry(State::Value, 62);
	CHECK(mark.text == "62 ms" && mark.routed.empty() && mark.tip == "Estimated ping: about 62 ms.");
	CHECK(mark.color == ui::LatencyBandColor(79) && mark.color != ui::LatencyBandColor(80));
	CHECK(entry(State::Value, 80).color == ui::LatencyBandColor(149));
	CHECK(entry(State::Value, 150).color == ui::LatencyBandColor(999));
	CHECK(ui::LatencyBandColor(79) != ui::LatencyBandColor(149) && ui::LatencyBandColor(149) != ui::LatencyBandColor(150));

	// A public room measures through a relay: it says so, in the neutral colour, whatever the number.
	for (unsigned rtt : {20u, 96u, 400u}) {
		mark = entry(State::Value, rtt, "", true);
		CHECK(mark.text == "~" + std::to_string(rtt) + " ms" && mark.routed == "Relay ~" + std::to_string(rtt) + " ms");
		CHECK(mark.color == neutral);
		CHECK(mark.tip == "Measured through the relay. A direct match is usually faster. Check connection measures the real path.");
	}

	// Pending: the ellipsis, never a number.
	mark = entry(State::Measuring);
	CHECK(mark.text == "..." && mark.routed.empty() && mark.color == neutral && mark.tip == "Measuring ping...");
	CHECK(entry(State::Value, 0).text == "...");

	// Unavailable: a tip only, or nothing at all.
	mark = entry(State::Unavailable, 0, "unreachable");
	CHECK(mark.text.empty() && mark.tip == "Ping unavailable: no connection to this member yet.");
	CHECK(entry(State::Unavailable, 0, "refused").tip == mark.tip);
	mark = entry(State::Unavailable, 0, "in_match");
	CHECK(mark.text.empty() && mark.tip == "Ping paused while this member plays.");
	for (const char* reason : {"no_latency_route", "room_unavailable", "anything else"}) {
		mark = entry(State::Unavailable, 0, reason);
		CHECK(mark.text.empty() && mark.tip.empty() && mark.routed.empty());
	}

	// The connection check is the match's own path: it replaces the relay figure and takes the bands' colours.
	checked = ui::CheckedPing{false, 9, 1};
	mark = entry(State::Value, 84, "", true);
	CHECK(mark.text == "9 ms" && mark.routed == "Direct 9 ms" && mark.color == ui::LatencyBandColor(9));
	CHECK(mark.tip == "Checked on the match's path: direct, 9 ms, 1 ms jitter.");
	checked = ui::CheckedPing{true, 84, 6};
	mark = entry(State::Measuring);
	CHECK(mark.routed == "Relay 84 ms" && mark.color == ui::LatencyBandColor(84) && mark.color != neutral);
	CHECK(mark.tip == "Checked on the match's path: relayed, 84 ms, 6 ms jitter.");
	// It stands even while the member's own entry is unavailable.
	CHECK(entry(State::Unavailable, 0, "in_match").routed == "Relay 84 ms");

	// The check lapsed: the row is the member's own figure again.
	checked.reset();
	CHECK(entry(State::Value, 84, "", true).routed == "Relay ~84 ms");
	CHECK(entry(State::Value, 30).text == "30 ms");

	// Another incarnation of the member, ourselves, or no entry: no mark.
	entry(State::Value, 62);
	member.incarnation = 23;
	mark = ui::LatencyMarkFor(table, member, 1, checked);
	CHECK(mark.text.empty() && mark.tip.empty());
	member.incarnation = 22;
	CHECK(!ui::LatencyMarkFor(table, member, 1, checked).text.empty());
	CHECK(ui::LatencyMarkFor(table, member, 2, checked).text.empty());
	table.clear();
	CHECK(ui::LatencyMarkFor(table, member, 1, checked).text.empty());
	// Without an entry a check still counts; for ourselves nothing does.
	CHECK(ui::LatencyMarkFor(table, member, 1, ui::CheckedPing{false, 9, 1}).routed == "Direct 9 ms");
	CHECK(ui::LatencyMarkFor(table, member, 2, ui::CheckedPing{false, 9, 1}).text.empty());
	CHECK(ui::DecidePing(nullptr, std::nullopt).kind == Kind::None);

	// The check's figure for a member: only the opponent across the seat, while the check is finished and measured.
	ui::ShellView view;
	view.room.localMember = 1;
	view.room.members.resize(3);
	for (int i = 0; i < 3; ++i) { view.room.members[i].id = static_cast<room::MemberId>(i + 1); view.room.members[i].table = i < 2 ? 0 : -1; view.room.members[i].seat = i < 2 ? i : -1; }
	view.room.tables[0].p1 = 1; view.room.tables[0].p2 = 2;
	view.probeStatus = "ready"; view.recommendedDelay = 2; view.probeRoute = RouteKind::Relayed;
	view.probeP50Us = 84400; view.probeJitterUs = 5600;
	auto found = ui::CheckedPingOf(view, 2);
	CHECK(found && found->relay && found->medianMs == 84 && found->jitterMs == 6);
	view.probeRoute = RouteKind::Direct; view.probeP50Us = 200;
	found = ui::CheckedPingOf(view, 2);
	CHECK(found && !found->relay && found->medianMs == 1);
	CHECK(!ui::CheckedPingOf(view, 3) && !ui::CheckedPingOf(view, 1));
	view.probeStatus = "checking";
	CHECK(!ui::CheckedPingOf(view, 2));
	view.probeStatus = "invalidated"; view.recommendedDelay = -1;
	CHECK(!ui::CheckedPingOf(view, 2));
	view.probeStatus = "ready"; view.recommendedDelay = 2; view.probeP50Us = 0;
	CHECK(!ui::CheckedPingOf(view, 2));
	view.probeP50Us = 9000;
	CHECK(ui::CheckedPingOf(view, 2));
	view.room.members[0].table = -1; view.room.members[0].seat = -1;
	CHECK(!ui::CheckedPingOf(view, 2));
	std::cout << "Latency mark tests passed\n";
	return 0;
}
