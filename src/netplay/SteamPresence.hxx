#pragma once

// A pure Steam Rich Presence description, mirroring src/discord/Presence.hxx:
// consumes copied room/session state and produces the "status" text and
// "connect" value to publish through the legacy SteamFriends014 API. No
// native pointers, Steam handles or user-authored profile text ever cross
// this boundary. Tested without steam_api.dll in
// src/tests/steam_presence_test.cxx.

#include "../session/RoomModel.hxx"
#include "../netplay/SessionController.hxx"

#include <cstdint>
#include <string>

namespace sf4e {
namespace steam_presence {

struct Input {
	// Whether SteamRuntime attached to the game's already-loaded
	// steam_api.dll this session, and the player's preferences.
	bool attached = false;
	bool enabled = true;
	bool invites = true;
	netplay::Snapshot session;
	room::Snapshot room;
	// The canonical join code resolved for `codeGeneration`, or "" while a
	// short link is still pending or failed. A code resolved for a room
	// generation the session has since left behind is never published.
	std::string code;
	std::uint64_t codeGeneration = 0;
};

struct Presence {
	bool publish = false;
	std::string status, connect;
};

inline Presence Describe(const Input& in) {
	Presence out;
	if (!in.attached || !in.enabled) return out;
	out.publish = true;
	out.status = "In Ember menus";
	const room::Member* local = nullptr;
	for (const auto& member : in.room.members)
		if (member.id == in.room.localMember) local = &member;
	const bool joined = local != nullptr && !in.room.closed && in.session.room == netplay::RoomState::Joined;
	if (!joined) {
		if (in.session.match == netplay::MatchState::Playing)
			out.status = local != nullptr && local->status == room::MemberStatus::Watching
				? "Spectating an Ember match" : "Fighting with Ember rollback";
		return out;
	}
	const bool full = static_cast<int>(in.room.members.size()) >= static_cast<int>(in.room.capacity);
	out.status = (in.room.locked || full) ? "In a private Ember room"
		: "In an Ember room (" + std::to_string(in.room.members.size()) + "/" + std::to_string(in.room.capacity) + ")";
	switch (local->status) {
	case room::MemberStatus::Queued: out.status = "Queued in Ember"; break;
	case room::MemberStatus::Playing: out.status = "Fighting with Ember rollback"; break;
	case room::MemberStatus::Watching: out.status = "Spectating an Ember match"; break;
	default: break;
	}
	const bool codeCurrent = !in.code.empty() && in.codeGeneration == in.session.generation.room;
	if (in.invites && in.session.control == netplay::Health::Healthy && !in.room.locked && !full && codeCurrent)
		out.connect = in.code;
	return out;
}

} // namespace steam_presence
} // namespace sf4e
