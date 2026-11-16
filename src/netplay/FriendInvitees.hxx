#pragma once
// Whom the player can invite to their room: the one answer the Friends screen's
// Invite and the room's Invite menu both read, so they cannot disagree. A friend
// can be invited while online in this game (Reachable); the room's own list puts
// the most recent opponents first. A friend already in the room is not left out:
// nothing in a private room's presence names the room, so a friend cannot be
// told from another in it, and inviting someone present is harmless.
// Pure: tested without a helper (friends_panel_test.cxx, room_friends_journey_test.cxx).
#include "Friends.hxx"
#include "GameIdentity.hxx"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay { namespace friends {

// Online, and in this game: an invite reaches them. Offline friends and friends in another game cannot be invited or joined.
inline bool Reachable(const Friend& entry) {
	return entry.presence.kind != Presence::Kind::Offline && SameGame(entry.presence.game);
}

// The friends the room's Invite menu lists: the Reachable ones, played most recently first (a friend never played sorts
// after those who were), then by name. At most `limit`; `more` says whether others were left out.
struct Invitees {
	std::vector<const Friend*> shown;
	bool more = false;
};

inline Invitees InviteesOf(const State& state, std::size_t limit) {
	Invitees invitees;
	std::vector<const Friend*> all;
	for (const auto& entry : state.friends)
		if (Reachable(entry)) all.push_back(&entry);
	const auto last = [](const Friend* entry) { return entry->hasRecord ? entry->record.lastAt : std::uint64_t(0); };
	std::stable_sort(all.begin(), all.end(), [&](const Friend* a, const Friend* b) {
		return last(a) != last(b) ? last(a) > last(b) : a->name != b->name ? a->name < b->name : a->id < b->id;
	});
	invitees.more = all.size() > limit;
	if (invitees.more) all.resize(limit);
	invitees.shown = std::move(all);
	return invitees;
}

} } }
