// Friends across games (netplay/AccountRoutes.hxx): which bridge each kind of request goes to, for a product whose
// friends live on an account bridge apart from its game's and for one whose game bridge is both (SF4 Ember, whose
// requests must all go where they always went); the profile and search asks that follow it; the order Connect Discord
// connects two bridges in; the listing a friends sync sends to each; the friend code a card shows, under its privacy
// switches; and a friend's public room that the account bridge cannot list.
#include "../netplay/AccountRoutes.hxx"
#include "../netplay/CardBundle.hxx"
#include "../netplay/FriendVisits.hxx"
#include "../netplay/FriendWire.hxx"
#include "../netplay/ProfileRequests.hxx"
#include "../netplay/ProfileWire.hxx"

#include <cstdio>
#include <string>

#include "test_support.hxx"

using namespace sf4e::netplay;

namespace {
constexpr Ask AllAsks[] = {Ask::FriendsSync, Ask::FriendsAction, Ask::FriendCard, Ask::RequestCard, Ask::CodeCard,
	Ask::CodeSearch, Ask::NameSearch, Ask::MemberCard, Ask::RoomTicket, Ask::OwnCard, Ask::OwnPoints};

void EachKindHasItsService() {
	for (const Ask ask : {Ask::FriendsSync, Ask::FriendsAction, Ask::FriendCard, Ask::RequestCard, Ask::CodeCard, Ask::CodeSearch})
		EXPECT(ServiceOf(ask) == Service::Account);
	for (const Ask ask : {Ask::NameSearch, Ask::MemberCard, Ask::RoomTicket, Ask::OwnCard, Ask::OwnPoints})
		EXPECT(ServiceOf(ask) == Service::Game);
}

void TwoBridgesSplitTheKinds() {
	const Bridges split{"brg_game", "brg_account"};
	for (const Ask ask : AllAsks)
		EXPECT(split.Of(ask) == (ServiceOf(ask) == Service::Account ? "brg_account" : "brg_game"));
	// Friends' cards ride apart from room members', and the main stays off the friends' bridge.
	EXPECT(!split.Together(Ask::FriendCard, Ask::MemberCard));
	EXPECT(!split.Together(Ask::FriendsSync, Ask::OwnCard));
	EXPECT(split.Together(Ask::FriendCard, Ask::CodeSearch) && split.Together(Ask::MemberCard, Ask::RoomTicket));
	// An account bridge the player does not trust yet: its kinds go nowhere, the game's go on.
	const Bridges untrusted{"brg_game", ""};
	EXPECT(untrusted.Of(Ask::FriendsSync).empty() && untrusted.Of(Ask::RoomTicket) == "brg_game");
}

// SF4 Ember: one bridge is both, so every kind goes to it, and every two kinds share a request as they always have.
void OneBridgeTakesEverything() {
	const Bridges one{"brg_1", "brg_1"};
	for (const Ask ask : AllAsks) {
		EXPECT(one.Of(ask) == "brg_1");
		for (const Ask other : AllAsks) EXPECT(one.Together(ask, other));
	}
}

void ProfilesAskTheBridgeOfTheirKey() {
	using profiles::Key;
	using profiles::Requests;
	EXPECT(Requests::ServiceOfKey(Key::OfCard("c1.member")) == Service::Game);
	EXPECT(Requests::ServiceOfKey(Key::OfCard("c1.found", Service::Account)) == Service::Account);
	EXPECT(Requests::ServiceOfKey(Key::OfFriend("fr_11111111-1111-4111-8111-111111111111")) == Service::Account);
	EXPECT(Requests::ServiceOfKey(Key::OfRequest("rq_22222222-2222-4222-8222-222222222222")) == Service::Account);
	EXPECT(Requests::ServiceOfKey(Key::OfCode("4F2A-91CX")) == Service::Account);
	EXPECT(Requests::ServiceOfKey(Key::OfName("Player 2")) == Service::Game);
	// The same token from two bridges is two keys: each opens only where it was sealed.
	EXPECT(Key::OfCard("c1.same") != Key::OfCard("c1.same", Service::Account));

	// A friend's profile is looked up on the account bridge; a room member's on the game's.
	Requests requests;
	requests.Scope("brg_game", "ember_1");
	requests.Open(Key::OfFriend("fr_11111111-1111-4111-8111-111111111111"), {}, 1000);
	auto ask = requests.Take(1000);
	EXPECT(ask.kind == Requests::Ask::Kind::Lookup && ask.selector == Key::Kind::Friend && ask.service == Service::Account);
	requests.Open(Key::OfCard("c1.member"), {}, 2000);
	ask = requests.Take(2000);
	EXPECT(ask.kind == Requests::Ask::Kind::Lookup && ask.selector == Key::Kind::Card && ask.service == Service::Game);
	// A name is searched where room members are met.
	requests.Open(Key::OfName("Player 2"), {}, 3000);
	ask = requests.Take(3000);
	EXPECT(ask.kind == Requests::Ask::Kind::Search && ask.service == Service::Game);

	// Find a player: a code is searched on the account bridge, whose hits' tokens open there; a name on the game's.
	requests.Search("4F2A-91CX", true, 4000);
	EXPECT(requests.SearchPublished().from == Service::Account);
	requests.Search("Player", false, 5000);
	EXPECT(requests.SearchPublished().from == Service::Game);
}

void ConnectDiscordGoesInOrder() {
	ServiceLink links[2];
	// Neither: the game's first, trusted or not.
	EXPECT(NextToConnect(links, 2) == 0);
	links[0].trusted = true;
	EXPECT(NextToConnect(links, 2) == 0);
	// The game's connected: the account bridge next, trusted or not, connected or not known.
	links[0].connected = true;
	EXPECT(NextToConnect(links, 2) == 1);
	links[1].trusted = true;
	EXPECT(NextToConnect(links, 2) == 1);
	// Both: nothing left; Connect Discord then shows the game's.
	links[1].connected = true;
	EXPECT(NextToConnect(links, 2) == 2);
	// Only the account bridge connected (a player who used SF4 Ember first): the game's still comes first.
	links[0].connected = false;
	EXPECT(NextToConnect(links, 2) == 0);
	// A product with one bridge connects one.
	EXPECT(NextToConnect(links, 1) == 0);
}

void NoMainOnTheAccountWire() {
	friends::Listing listing;
	listing.name = "Player 1";
	listing.country = "DE";
	listing.main = 44;
	friends::Report menus;
	const auto account = friends::SyncRequest("brg_account", "build", friends::ListingFor(listing, false), menus);
	EXPECT(!account["sync"].contains("main"));
	EXPECT(account["sync"]["game"] == GameId() && account["sync"]["listing"]["name"] == "Player 1" && account["sync"]["listing"]["country"] == "DE");
	EXPECT(account["bridge_id"] == "brg_account");
	// The game's own bridge still takes the main.
	const auto game = friends::SyncRequest("brg_game", "build", friends::ListingFor(listing, true), menus);
	EXPECT(game["sync"]["main"] == 44);
}

void TheCardShowsTheCodeOnlyWhenShared() {
	friends::State listed;
	listed.kind = friends::State::Kind::Listed;
	listed.code = "4F2A-91CX";
	EXPECT(profiles::SharedCode(true, &listed) == "4F2A-91CX");
	// Friends on the card's own bridge have no code to show there.
	EXPECT(profiles::SharedCode(false, &listed).empty());
	// Appearing offline hides it.
	auto hidden = listed;
	hidden.appearOffline = true;
	EXPECT(profiles::SharedCode(true, &hidden).empty());
	// Friends off, no Discord account, nothing read yet, no state: none.
	for (const auto kind : {friends::State::Kind::Off, friends::State::Kind::NoAccount, friends::State::Kind::Unavailable}) {
		auto other = listed;
		other.kind = kind;
		EXPECT(profiles::SharedCode(true, &other).empty());
	}
	EXPECT(profiles::SharedCode(true, nullptr).empty());

	// The save carries it when there is one, and nothing otherwise (a save without it clears it on the bridge).
	profiles::Bundle bundle;
	bundle.name = "Player 1";
	EXPECT(!profiles::CardSaveRequest("brg_game", bundle)["card"].contains("friend_code"));
	bundle.friendCode = "4F2A-91CX";
	const auto saved = profiles::CardSaveRequest("brg_game", bundle);
	EXPECT(saved["op"] == "card_save" && saved["card"]["friend_code"] == "4F2A-91CX");
	// A code that changes is a card that changed: it goes again.
	profiles::Bundle before = bundle;
	before.friendCode.clear();
	EXPECT(before != bundle);
}

void APublicRoomTheAccountBridgeCannotList() {
	using friends::Destination;
	friends::State state;
	state.kind = friends::State::Kind::Listed;
	friends::Friend entry;
	entry.id = "fr_11111111-1111-4111-8111-111111111111";
	entry.name = "Player 3";
	entry.presence.kind = friends::Presence::Kind::InRoom;
	entry.presence.status = friends::Status::Idle;
	// The friend's own game, in a public room the bridge does not list: no room to join from the list.
	state.friends.push_back(entry);
	EXPECT(friends::ToFriend(state, entry.id, false).refusal == Destination::Refusal::NotInRoom);
	EXPECT(friends::ToFriend(state, entry.id, false, false).refusal == Destination::Refusal::RoomElsewhere);
	EXPECT(friends::ToFriend(state, entry.id, true, false).refusal == Destination::Refusal::RoomElsewhere);
}
}

int main() {
	EachKindHasItsService();
	TwoBridgesSplitTheKinds();
	OneBridgeTakesEverything();
	ProfilesAskTheBridgeOfTheirKey();
	ConnectDiscordGoesInOrder();
	NoMainOnTheAccountWire();
	TheCardShowsTheCodeOnlyWhenShared();
	APublicRoomTheAccountBridgeCannotList();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
