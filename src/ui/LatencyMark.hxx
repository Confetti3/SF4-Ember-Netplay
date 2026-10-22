#pragma once
// The ping a room card shows for a member (MEMBER_LATENCY.md 3.9): "~62 ms"
// ("Relay 62 ms" in V2) in its band's colour (under 80 ms, under 150 ms, above), "..." while it is
// measured, and only a tip when it is unavailable.
#include "RoomCards.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../session/MemberLatency.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {

inline ImU32 LatencyBandColor(unsigned milliseconds) {
	return ImGui::ColorConvertFloat4ToU32(ToneColor(milliseconds < 80 ? Tone::Success : milliseconds < 150 ? Tone::Pending : Tone::Error));
}

// Nothing for ourselves, for an entry measured for another incarnation of the
// member, or when the member has no route or the room no authority.
inline LatencyMark LatencyMarkFor(const session::MemberLatencyTable& table, const room::Member& member, room::MemberId local) {
	LatencyMark mark;
	if (member.id == local) return mark;
	const auto found = table.find(member.id);
	if (found == table.end() || found->second.incarnation != member.incarnation) return mark;
	const auto& latency = found->second;
	switch (latency.state) {
	case session::MemberLatency::State::Value:
		mark.text = loc::Tf("room.latency.estimate", latency.rttMs);
		mark.routed = loc::Tf(latency.relay ? "room.latency.relay" : "room.latency.direct", latency.rttMs);
		mark.tip = loc::Tf("room.latency.tip.value", latency.rttMs);
		mark.color = LatencyBandColor(latency.rttMs);
		break;
	case session::MemberLatency::State::Measuring:
		mark.text = "...";
		mark.tip = loc::T("room.latency.tip.measuring");
		break;
	case session::MemberLatency::State::Unavailable:
		if (latency.reason == "in_match") mark.tip = loc::T("room.latency.tip.in_match");
		else if (latency.reason == "unreachable" || latency.reason == "refused") mark.tip = loc::T("room.latency.tip.unavailable");
		break;
	}
	return mark;
}

} }
