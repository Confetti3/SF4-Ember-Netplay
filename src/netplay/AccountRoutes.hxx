#pragma once
// Which bridge each kind of request goes to, for a product whose player's
// account lives on a bridge apart from the game's own
// (ember::ProductIdentity::accountService; docs/design/EMBER_LIBRARY.md in
// SFV Ember, "Friends across games"). The account bridge holds what belongs to
// the player in every game: the friend list and its actions, the friend code,
// and cards found through it (a friend, a request, a code, or a code search's
// hit). The game's bridge holds what belongs to one game: rooms and their
// tickets, rankings, points, the player's own card, and the cards of the
// players met there (a room member's token, a name search's hit, a recent
// opponent, the most played rival). A card token opens only on the bridge
// that sealed it, so a token is asked of the service it came from.
//
// This table is the one place the split is decided; the runtime asks it for
// every request it sends. A product without an account bridge of its own has
// one bridge for both, and every kind goes there as it always has.
//
// C++17, no Windows headers: tested on its own (account_routes_test.cxx).
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {

// The bridge a request belongs on.
enum class Service : std::uint8_t { Game, Account };

// What a request is for.
enum class Ask : std::uint8_t {
	FriendsSync,     // the presence in, the friends state out
	FriendsAction,   // one action on the list, a private room's join included
	FriendCard,      // a card by friend id ("fr_")
	RequestCard,     // a card by a received request's id ("rq_")
	CodeCard,        // a card by friend code
	CodeSearch,      // Find a player by friend code
	NameSearch,      // Find a player by name, and a name's profile
	MemberCard,      // a card by a room member's token, a recent opponent's or a rival's
	RoomTicket,      // a public room's ticket, a friend's visit included
	OwnCard,         // the player's own card saved
	OwnPoints,       // the player's own points
};

constexpr Service ServiceOf(Ask ask) {
	switch (ask) {
	case Ask::FriendsSync: case Ask::FriendsAction:
	case Ask::FriendCard: case Ask::RequestCard: case Ask::CodeCard: case Ask::CodeSearch:
		return Service::Account;
	case Ask::NameSearch: case Ask::MemberCard: case Ask::RoomTicket: case Ask::OwnCard: case Ask::OwnPoints:
		return Service::Game;
	}
	return Service::Game;
}

// The two bridges a runtime asks, by id. `account` is empty while the product
// has an account bridge the player does not trust yet: its requests then go
// nowhere, and the game's go on.
struct Bridges {
	std::string game, account;
	const std::string& Of(Service service) const { return service == Service::Account ? account : game; }
	const std::string& Of(Ask ask) const { return Of(ServiceOf(ask)); }
	// Two kinds share a request only when they go to the same bridge.
	bool Together(Ask a, Ask b) const { return Of(a) == Of(b); }
};

// Where a player stands with one bridge Connect Discord connects: trusted, and a Discord account known connected there.
struct ServiceLink {
	bool trusted = false;
	bool connected = false;
};

// The one press of Connect Discord on a product with an account bridge apart connects every bridge in `links` (the
// game's, then the account's), each with its own Discord link: the one to work on next is the first not trusted or
// without a Discord account known connected, and `count` once every one has.
inline std::size_t NextToConnect(const ServiceLink* links, std::size_t count) {
	for (std::size_t i = 0; i < count; ++i)
		if (!links[i].trusted || !links[i].connected) return i;
	return count;
}

} }
