#pragma once
// Whether the player's Ember ID has a Discord account linked: one answer for
// every screen that asks (the ranked notice on Public rooms and Create, Settings'
// Discord account row, Connect Discord on the Card tab and the awards wall),
// made from every read that says so by one rule, so no screen derives it again.
// Pure, so the rule is tested without a frame (discord_link_test.cxx).
#include <optional>
#include <string>

namespace sf4e { namespace netplay {

struct DiscordLink {
	enum class State { Unknown, Linked, None } state = State::Unknown;
	// The account's name, when the Ember ID screens read it; empty otherwise.
	std::string name;
};

// What has been read of it, each for the Ember ID and service in use.
struct DiscordLinkReads {
	// There is no Ember ID to play under.
	bool noEmberId = false;
	// The Ember ID screens' read of the account on the service: its name, empty for none; no value while unread.
	std::optional<std::string> account;
	// What the ranking standing, the player's own points and the friends say: true, an account; false, none.
	std::optional<bool> standing, points, friends;
};

// No Ember ID is none. The Ember ID screens' read decides. Otherwise any read that finds an account wins over one
// that found none, since those go stale when the player links one; with nothing read, nothing is known.
inline DiscordLink LinkOf(const DiscordLinkReads& reads) {
	using State = DiscordLink::State;
	if (reads.noEmberId) return {State::None, {}};
	if (reads.account) return reads.account->empty() ? DiscordLink{State::None, {}} : DiscordLink{State::Linked, *reads.account};
	bool none = false;
	for (const auto& read : {reads.standing, reads.points, reads.friends}) {
		if (read && *read) return {State::Linked, {}};
		none = none || read.has_value();
	}
	return {none ? State::None : State::Unknown, {}};
}

} }
