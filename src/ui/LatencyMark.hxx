#pragma once
// The ping a room card shows for a member (MEMBER_LATENCY.md, "What the member
// list shows"). DecidePing says what the number is: measured through a relay
// ("Relay ~80 ms", in the neutral colour, since a relay is not the match's
// path), measured directly ("62 ms"), or the connection check's figure on the
// match's own path ("Direct 9 ms"). The last two are in the input delay bands'
// colours (under 80 ms, under 150 ms, above), as they predict the match.
// "..." while it is measured, and only a tip when it is unavailable.
// MemberTag puts it on a room member's player tag with the rest of what the
// room knows of them.
#include "RoomCards.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include "../session/MemberLatency.hxx"
#include "../session/RoomQueries.hxx"
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace sf4e { namespace ui {

inline ImU32 LatencyBandColor(unsigned milliseconds) {
	return ImGui::ColorConvertFloat4ToU32(ToneColor(milliseconds < 80 ? Tone::Success : milliseconds < 150 ? Tone::Pending : Tone::Error));
}

// What the connection check measured on the match's path to a member.
struct CheckedPing {
	bool relay = false;
	unsigned medianMs = 0, jitterMs = 0;
};

// What a ping row is made of: a kind of number, the number, and whether the
// input delay bands colour it.
struct PingDecision {
	enum class Kind { None, Pending, Relay, Direct, CheckedRelay, CheckedDirect, InMatch, Unreachable };
	Kind kind = Kind::None;
	unsigned ms = 0, jitterMs = 0;
	bool banded = false;
};

// `latency` is the helper's entry for this member's incarnation, none without
// one; `checked` the connection check's result for them, none when there is
// none or it lapsed. A check is the match's path itself, so it wins.
inline PingDecision DecidePing(const session::MemberLatency* latency, const std::optional<CheckedPing>& checked) {
	using Kind = PingDecision::Kind;
	if (checked && checked->medianMs) return {checked->relay ? Kind::CheckedRelay : Kind::CheckedDirect, checked->medianMs, checked->jitterMs, true};
	if (!latency) return {};
	switch (latency->state) {
	case session::MemberLatency::State::Value:
		if (!latency->rttMs) return {Kind::Pending};
		return {latency->relay ? Kind::Relay : Kind::Direct, latency->rttMs, 0, !latency->relay};
	case session::MemberLatency::State::Measuring:
		return {Kind::Pending};
	case session::MemberLatency::State::Unavailable:
		if (latency->reason == "in_match") return {Kind::InMatch};
		if (latency->reason == "unreachable" || latency->reason == "refused") return {Kind::Unreachable};
		break;
	}
	return {};
}

// The decision as the row draws it: the short text, the text with its route
// where it fits, the colour and the tip.
inline LatencyMark MarkOf(const PingDecision& decision) {
	using Kind = PingDecision::Kind;
	LatencyMark mark;
	const unsigned ms = decision.ms;
	if (decision.banded) mark.color = LatencyBandColor(ms);
	switch (decision.kind) {
	case Kind::None: break;
	case Kind::Pending: mark.text = "..."; mark.tip = loc::T("room.latency.tip.measuring"); break;
	case Kind::InMatch: mark.tip = loc::T("room.latency.tip.in_match"); break;
	case Kind::Unreachable: mark.tip = loc::T("room.latency.tip.unavailable"); break;
	case Kind::Relay:
		mark.text = loc::Tf("room.latency.estimate", ms);
		mark.routed = loc::Tf("room.latency.relay_estimate", ms);
		mark.tip = loc::T("room.latency.tip.relay");
		break;
	case Kind::Direct:
		mark.text = loc::Tf("room.latency.value", ms);
		mark.tip = loc::Tf("room.latency.tip.value", ms);
		break;
	case Kind::CheckedRelay:
		mark.text = loc::Tf("room.latency.value", ms);
		mark.routed = loc::Tf("room.latency.relay", ms);
		mark.tip = loc::Tf("room.latency.tip.checked_relay", ms, decision.jitterMs);
		break;
	case Kind::CheckedDirect:
		mark.text = loc::Tf("room.latency.value", ms);
		mark.routed = loc::Tf("room.latency.direct", ms);
		mark.tip = loc::Tf("room.latency.tip.checked_direct", ms, decision.jitterMs);
		break;
	}
	return mark;
}

// The connection check's figure for `member`, while it counts: they are the
// opponent in the seat across from this player's, and the check made against
// them at this table's revision is finished. The runtime fills the view's
// probe fields only for that opponent and that revision, so a check ended by
// a change of the table or of opponent is gone here too.
inline std::optional<CheckedPing> CheckedPingOf(const ShellView& view, room::MemberId member) {
	const auto* local = room::FindMember(view.room, view.room.localMember);
	if (!local || !view.ConnectionMeasured() || !view.probeP50Us) return std::nullopt;
	const auto* opponent = room::SeatOpponent(view.room, static_cast<std::uint8_t>(local->table), local->id);
	if (!opponent || opponent->id != member) return std::nullopt;
	// Whole milliseconds, 1 to 999, as the helper reports the member list's.
	const auto whole = [](std::uint64_t us) { return static_cast<unsigned>((std::min<std::uint64_t>)((us + 500) / 1000, 999)); };
	CheckedPing checked;
	checked.relay = view.probeRoute == RouteKind::Relayed;
	checked.medianMs = (std::max)(whole(view.probeP50Us), 1u);
	checked.jitterMs = whole(view.probeJitterUs);
	return checked;
}

// Nothing for ourselves, for an entry measured for another incarnation of the
// member, or when the member has no route or the room no authority.
inline LatencyMark LatencyMarkFor(const session::MemberLatencyTable& table, const room::Member& member, room::MemberId local,
	const std::optional<CheckedPing>& checked) {
	if (member.id == local) return {};
	const auto found = table.find(member.id);
	const bool current = found != table.end() && found->second.incarnation == member.incarnation;
	return MarkOf(DecidePing(current ? &found->second : nullptr, checked));
}

// A room member's player tag wherever the room draws one: name, flag and
// class, ping and link. Callers add what is their own (a suffix, a status).
inline v2::TagView MemberTag(const ShellView& view, const room::Member& member) {
	v2::TagView tag;
	tag.name = member.name; tag.marks = {member.country, member.badge};
	tag.ping = LatencyMarkFor(view.memberLatency, member, view.room.localMember, CheckedPingOf(view, member.id));
	tag.link = member.link;
	return tag;
}

} }
