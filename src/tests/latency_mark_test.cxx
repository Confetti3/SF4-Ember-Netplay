// What a room card shows for a member's ping (MEMBER_LATENCY.md 5, C++ 13):
// the text and its band, the tip, and nothing for ourselves, for another
// incarnation of the member, or without a route or a room.
#include "../ui/LatencyMark.hxx"
#include "../common/Localization.hxx"
#include <iostream>
#include <string>

#include "test_support.hxx"

using namespace sf4e;

int main() {
	loc::SetActive(loc::Locale::En);
	room::Member member; member.id = 2; member.incarnation = 22;
	session::MemberLatencyTable table;
	const auto entry = [&](session::MemberLatency::State state, unsigned rtt = 0, const char* reason = "", bool relay = false) {
		session::MemberLatency latency; latency.incarnation = 22; latency.state = state; latency.rttMs = rtt; latency.reason = reason; latency.relay = relay;
		table[2] = latency;
		return ui::LatencyMarkFor(table, member, 1);
	};
	using State = session::MemberLatency::State;

	auto mark = entry(State::Value, 62);
	CHECK(mark.text == "~62 ms" && mark.tip == "Estimated ping: about 62 ms.");
	CHECK(mark.routed == "Direct 62 ms" && entry(State::Value, 96, "", true).routed == "Relay 96 ms");
	CHECK(mark.color == ui::LatencyBandColor(79) && mark.color != ui::LatencyBandColor(80));
	CHECK(entry(State::Value, 80).color == ui::LatencyBandColor(149));
	CHECK(entry(State::Value, 150).color == ui::LatencyBandColor(999));
	CHECK(ui::LatencyBandColor(79) != ui::LatencyBandColor(149) && ui::LatencyBandColor(149) != ui::LatencyBandColor(150));

	mark = entry(State::Measuring);
	CHECK(mark.text == "..." && mark.tip == "Measuring ping...");
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

	// Another incarnation of the member, ourselves, or no entry: no mark.
	entry(State::Value, 62);
	member.incarnation = 23;
	mark = ui::LatencyMarkFor(table, member, 1);
	CHECK(mark.text.empty() && mark.tip.empty());
	member.incarnation = 22;
	CHECK(!ui::LatencyMarkFor(table, member, 1).text.empty());
	CHECK(ui::LatencyMarkFor(table, member, 2).text.empty());
	table.clear();
	CHECK(ui::LatencyMarkFor(table, member, 1).text.empty());
	std::cout << "Latency mark tests passed\n";
	return 0;
}
