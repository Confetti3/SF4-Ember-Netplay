// What the game reads of the bridge's FriendsState (session/SocialAnswers.hxx):
// a well-formed state decodes completely, keys a later bridge adds are ignored,
// a row or a friend's part that cannot be read is left out alone, and a state
// whose shape is wrong is refused whole and leaves the state alone, however it
// is damaged (a seeded sweep).
#include "../session/SocialAnswers.hxx"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e;
using nlohmann::json;
using netplay::friends::Presence;
using netplay::friends::State;

namespace {
const std::string FriendId = "fr_11111111-1111-4111-8111-111111111111";
const std::string FriendId2 = "fr_22222222-2222-4222-8222-222222222222";
const std::string RequestId = "rq_11111111-1111-4111-8111-111111111111";
const std::string InviteId = "iv_11111111-1111-4111-8111-111111111111";
const std::string BlockId = "bl_11111111-1111-4111-8111-111111111111";
const std::string RoomId = "0f0123456789abcdef0123456789abcd";
const std::string Secret = "emd1:" + std::string(122, 'A');

// As the helper's text arrives: parsed, so a positive number is unsigned, as the decoder expects.
bool Decode(const json& data, State& state) { return session::DecodeFriendsState(json::parse(data.dump()), state); }

json FullState() {
	return {
		{"code", "4F2A-91CX"},
		{"appear_offline", true},
		{"friends", json::array({
			{{"id", FriendId}, {"name", "Player 3"}, {"country", "FR"}, {"since", 1730073600},
				{"main", {{"fighter", 5}, {"points", 1986}, {"tier", 3}}}, {"account", {{"points", 2331}, {"tier", 3}}}, {"badge", {{"band", 7}, {"settled", true}}},
				{"presence", {{"state", "room"}, {"status", "fighting"}, {"table", 0},
					{"room", {{"room_id", RoomId}, {"name", "Player 1's room"}, {"members", 7}, {"capacity", 16}, {"joinable", true}}}}},
				{"record", {{"won", 14}, {"lost", 9}, {"last_at", 1731000000}}}},
			{{"id", FriendId2}, {"name", "Player 4"}, {"since", 5}, {"presence", {{"state", "offline"}, {"seen_at", 99}}}},
		})},
		{"incoming", json::array({{{"id", RequestId}, {"name", "Player 9"}, {"country", "DE"}, {"sent_at", 1731100000},
			{"played", {{"games", 6}, {"last_at", 1731090000}}}}})},
		{"outgoing", json::array({{{"id", RequestId}, {"label", "Player 4"}, {"sent_at", 1731100000}}})},
		{"invites", json::array({
			{{"id", InviteId}, {"friend", FriendId}, {"room", {{"room_id", RoomId}}}, {"expires_at", 1731100600}},
			{{"id", InviteId}, {"friend", FriendId}, {"room", {{"secret", Secret}}}, {"expires_at", 1731100600}}})},
		{"blocked", json::array({{{"id", BlockId}, {"name", "Player 7"}, {"at", 1731000000}}})},
	};
}

bool Decodes(const json& data) { State state; return Decode(data, state); }

void ReadsAWholeState() {
	State state;
	EXPECT(Decode(FullState(), state));
	EXPECT(state.kind == State::Kind::Listed && state.code == "4F2A-91CX" && state.appearOffline);
	EXPECT(state.friends.size() == 2 && state.incoming.size() == 1 && state.outgoing.size() == 1 && state.invites.size() == 2 && state.blocked.size() == 1);
	const auto& in_room = state.friends[0];
	EXPECT(in_room.id == FriendId && in_room.name == "Player 3" && in_room.country.ToString() == "FR" && in_room.since == 1730073600);
	EXPECT(in_room.hasMain && in_room.main.fighter == 5 && in_room.main.showsPoints && in_room.main.points == 1986 && in_room.main.tier == 3);
	EXPECT(in_room.hasAccount && in_room.account.points == 2331 && in_room.account.tier == 3);
	EXPECT(in_room.badge.HasClass() && in_room.badge.band == 7 && in_room.badge.settled);
	EXPECT(in_room.presence.kind == Presence::Kind::InRoom && in_room.presence.status == netplay::friends::Status::Fighting && in_room.presence.table == 0);
	EXPECT(in_room.presence.hasRoom && !in_room.presence.privateRoom && in_room.presence.room.id == RoomId && in_room.presence.room.joinable &&
		in_room.presence.room.members == 7 && in_room.presence.room.capacity == 16 && in_room.presence.room.name == "Player 1's room");
	EXPECT(in_room.hasRecord && in_room.record.won == 14 && in_room.record.lost == 9 && in_room.record.lastAt == 1731000000);
	const auto& offline = state.friends[1];
	EXPECT(!offline.hasAccount && offline.account.points == 0 && !offline.hasMain && !offline.badge.Present() && !offline.hasRecord && !offline.country.Present());
	EXPECT(offline.presence.kind == Presence::Kind::Offline && offline.presence.seenAt == 99);
	EXPECT(state.incoming[0].games == 6 && state.incoming[0].lastGameAt == 1731090000 && state.incoming[0].country.ToString() == "DE");
	EXPECT(state.outgoing[0].label == "Player 4" && state.blocked[0].name == "Player 7");
	EXPECT(state.invites[0].room.kind == netplay::friends::InviteRoom::Kind::RoomId && state.invites[0].room.value == RoomId);
	EXPECT(state.invites[1].room.kind == netplay::friends::InviteRoom::Kind::Secret && state.invites[1].room.value == Secret);
	EXPECT(state.FindFriend(FriendId) == &state.friends[0] && !state.FindFriend("fr_x") && state.FindInvite(InviteId));
}

void ReadsTheSmallestState() {
	State state;
	EXPECT(Decode({{"code", "4F2A-91CX"}, {"friends", json::array()}, {"incoming", json::array()},
		{"outgoing", json::array()}, {"invites", json::array()}, {"blocked", json::array()}}, state));
	EXPECT(state.kind == State::Kind::Listed && !state.appearOffline && state.friends.empty());
	// Online and a private room.
	json data = FullState();
	data["friends"] = json::array({
		{{"id", FriendId}, {"name", "Player 3"}, {"since", 1}, {"presence", {{"state", "online"}, {"activity", "offline_play"}}}},
		{{"id", FriendId2}, {"name", "Player 4"}, {"since", 1}, {"presence", {{"state", "room"}, {"status", "queued"}, {"private", true}}}}});
	EXPECT(Decode(data, state));
	EXPECT(state.friends[0].presence.kind == Presence::Kind::Online && state.friends[0].presence.activity == netplay::friends::Activity::OfflinePlay);
	EXPECT(state.friends[1].presence.privateRoom && !state.friends[1].presence.hasRoom && state.friends[1].presence.table == -1);
	// A main that does not show points has neither.
	data["friends"][0]["main"] = {{"fighter", 43}};
	EXPECT(Decode(data, state) && state.friends[0].hasMain && !state.friends[0].main.showsPoints);
}

// A later bridge's additions, at every level, change nothing.
void IgnoresUnknownKeys() {
	json data = FullState();
	data["extra"] = 1;
	for (auto& friend_ : data["friends"]) { friend_["extra"] = "x"; friend_["presence"]["extra"] = 2; }
	data["friends"][0]["main"]["extra"] = 3;
	data["friends"][0]["account"]["extra"] = 8;
	data["friends"][0]["presence"]["room"]["extra"] = 4;
	data["friends"][0]["record"]["extra"] = 5;
	data["incoming"][0]["extra"] = 6;
	data["incoming"][0]["played"]["extra"] = 6;
	data["outgoing"][0]["extra"] = 7;
	data["invites"][0]["extra"] = 8;
	data["invites"][0]["room"]["extra"] = 9;
	data["blocked"][0]["extra"] = 10;
	State state;
	EXPECT(Decode(data, state) && state.friends.size() == 2 && state.invites.size() == 2);
}

void RefusesWhatIsMalformed() {
	State untouched;
	untouched.code = "keep";
	const auto refused = [&](const char* what, json data) {
		State state = untouched;
		const bool decoded = Decode(data, state);
		EXPECT_MSG(!decoded && state.code == "keep" && state.kind == State::Kind::Unavailable, "%s", what);
	};
	const json good = FullState();
	const auto changed = [&](auto edit) { json data = good; edit(data); return data; };
	refused("not an object", json::array());
	refused("no code", changed([](json& d) { d.erase("code"); }));
	refused("code without its dash", changed([](json& d) { d["code"] = "4F2A91CXY"; }));
	refused("lower-case code", changed([](json& d) { d["code"] = "4f2a-91cx"; }));
	refused("appear_offline not a bool", changed([](json& d) { d["appear_offline"] = 1; }));
	refused("no friends list", changed([](json& d) { d.erase("friends"); }));
	refused("friends not a list", changed([](json& d) { d["friends"] = "none"; }));
	refused("no blocked list", changed([](json& d) { d.erase("blocked"); }));
}

// A row that cannot be read is left out of its list, and every other row and list is read: one damaged friend never
// costs the player the whole friends list.
void LeavesOutRowsItCannotRead() {
	const json good = FullState();
	const auto changed = [&](auto edit) { json data = good; edit(data); return data; };
	const auto leftOut = [&](const char* what, const char* list, json data) {
		State state;
		const bool decoded = Decode(data, state);
		EXPECT_MSG(decoded && state.code == "4F2A-91CX", "%s", what);
		const std::size_t friends = std::string(list) == "friends" ? 1 : 2, incoming = std::string(list) == "incoming" ? 0 : 1,
			outgoing = std::string(list) == "outgoing" ? 0 : 1, invites = std::string(list) == "invites" ? 1 : 2,
			blocked = std::string(list) == "blocked" ? 0 : 1;
		EXPECT_MSG(state.friends.size() == friends && state.incoming.size() == incoming && state.outgoing.size() == outgoing &&
			state.invites.size() == invites && state.blocked.size() == blocked, "%s", what);
	};
	leftOut("friend id of a request", "friends", changed([](json& d) { d["friends"][0]["id"] = RequestId; }));
	leftOut("friend id too short", "friends", changed([](json& d) { d["friends"][0]["id"] = "fr_1"; }));
	leftOut("empty name", "friends", changed([](json& d) { d["friends"][0]["name"] = ""; }));
	leftOut("long name", "friends", changed([](json& d) { d["friends"][0]["name"] = std::string(33, 'n'); }));
	leftOut("name not text", "friends", changed([](json& d) { d["friends"][0]["name"] = 4; }));
	leftOut("lower-case country", "friends", changed([](json& d) { d["friends"][0]["country"] = "fr"; }));
	leftOut("long country", "friends", changed([](json& d) { d["friends"][0]["country"] = "FRA"; }));
	leftOut("negative since", "friends", changed([](json& d) { d["friends"][0]["since"] = -1; }));
	leftOut("no presence", "friends", changed([](json& d) { d["friends"][0].erase("presence"); }));
	leftOut("table past the last", "friends", changed([](json& d) { d["friends"][0]["presence"]["table"] = 8; }));
	leftOut("a room and private", "friends", changed([](json& d) { d["friends"][0]["presence"]["private"] = true; }));
	leftOut("room id not hex", "friends", changed([](json& d) { d["friends"][0]["presence"]["room"]["room_id"] = std::string(32, 'z'); }));
	leftOut("room fuller than its capacity", "friends", changed([](json& d) { d["friends"][0]["presence"]["room"]["members"] = 17; }));
	leftOut("room of one", "friends", changed([](json& d) { d["friends"][0]["presence"]["room"]["capacity"] = 1; d["friends"][0]["presence"]["room"]["members"] = 0; }));
	leftOut("online with no activity", "friends", changed([](json& d) { d["friends"][0]["presence"] = {{"state", "online"}}; }));
	leftOut("request id of a friend", "incoming", changed([](json& d) { d["incoming"][0]["id"] = FriendId; }));
	leftOut("long label", "outgoing", changed([](json& d) { d["outgoing"][0]["label"] = std::string(65, 'l'); }));
	leftOut("invite room of both kinds", "invites", changed([](json& d) { d["invites"][0]["room"]["secret"] = Secret; }));
	leftOut("invite secret too short", "invites", changed([](json& d) { d["invites"][1]["room"]["secret"] = "emd1:AAAA"; }));
	leftOut("invite secret of another kind", "invites", changed([](json& d) { d["invites"][1]["room"]["secret"] = "emx9:" + std::string(122, 'A'); }));
	leftOut("invite from a request id", "invites", changed([](json& d) { d["invites"][0]["friend"] = RequestId; }));
	leftOut("block id of a friend", "blocked", changed([](json& d) { d["blocked"][0]["id"] = FriendId; }));
	// The friend left out is the damaged one.
	State state;
	EXPECT(Decode(changed([](json& d) { d["friends"][0]["name"] = ""; }), state) && state.friends[0].id == FriendId2);
}

// A part of a friend that cannot be read (their main, account or record) is left out, and the friend is listed.
void LeavesOutPartsItCannotRead() {
	const json good = FullState();
	const auto read = [&](const char* what, auto edit) {
		json data = good;
		edit(data);
		State state;
		const bool decoded = Decode(data, state) && state.friends.size() == 2 && state.friends[0].id == FriendId && state.friends[0].name == "Player 3";
		EXPECT_MSG(decoded, "%s", what);
		return state.friends.empty() ? netplay::friends::Friend() : state.friends[0];
	};
	for (const auto& [what, edit] : std::vector<std::pair<const char*, std::function<void(json&)>>>{
			{"fighter past the roster", [](json& d) { d["friends"][0]["main"]["fighter"] = 44; }},
			{"points without a tier", [](json& d) { d["friends"][0]["main"].erase("tier"); }},
			{"main not an object", [](json& d) { d["friends"][0]["main"] = "Ryu"; }}}) {
		const auto entry = read(what, edit);
		EXPECT_MSG(!entry.hasMain && !entry.main.showsPoints && entry.hasAccount && entry.hasRecord, "%s", what);
	}
	for (const auto& [what, edit] : std::vector<std::pair<const char*, std::function<void(json&)>>>{
			{"account without a tier", [](json& d) { d["friends"][0]["account"].erase("tier"); }},
			{"account points not a number", [](json& d) { d["friends"][0]["account"]["points"] = "many"; }},
			{"account not an object", [](json& d) { d["friends"][0]["account"] = 7; }}}) {
		const auto entry = read(what, edit);
		EXPECT_MSG(!entry.hasAccount && entry.account.points == 0 && entry.hasMain, "%s", what);
	}
	for (const auto& [what, edit] : std::vector<std::pair<const char*, std::function<void(json&)>>>{
			{"empty record", [](json& d) { d["friends"][0]["record"] = {{"won", 0}, {"lost", 0}, {"last_at", 1}}; }},
			{"record without a time", [](json& d) { d["friends"][0]["record"].erase("last_at"); }}}) {
		const auto entry = read(what, edit);
		EXPECT_MSG(!entry.hasRecord && entry.record.won == 0 && entry.hasMain, "%s", what);
	}
}

json PrivateOffer() { return {{"name", "Open Mic"}, {"members", 3}, {"capacity", 8}, {"joinable", true}}; }

// The first friend's presence, replaced, and what the state then reads of them.
netplay::friends::Friend ReadPresence(json presence) {
	json data = FullState();
	data["friends"][0]["presence"] = std::move(presence);
	State state;
	EXPECT(Decode(data, state) && state.friends.size() == 2 && state.friends[0].id == FriendId);
	return state.friends.empty() ? netplay::friends::Friend() : state.friends[0];
}

// A friend's game, and the private room they may be joined in: parts of a presence that stand alone.
void ReadsTheGameAndAPrivateOffer() {
	// No game named is the empty id, which is this game's.
	EXPECT(ReadPresence({{"state", "online"}, {"activity", "menus"}}).presence.game.empty());
	EXPECT(ReadPresence({{"state", "online"}, {"activity", "menus"}, {"game", "sfv"}}).presence.game == "sfv");
	auto inPublicRoom = ReadPresence({{"state", "room"}, {"status", "idle"}, {"game", "usf4"},
		{"room", {{"room_id", RoomId}, {"name", "Player 1's room"}, {"members", 7}, {"capacity", 16}, {"joinable", true}}}});
	EXPECT(inPublicRoom.presence.game == "usf4" && inPublicRoom.presence.hasRoom && !inPublicRoom.presence.hasPrivateOffer);
	// A game that is not 1 to 16 of [a-z0-9] is ignored, not the presence.
	for (const json& bad : {json(""), json("USF4"), json("sf-v"), json("a23456789012345678"), json(7), json(nullptr), json::array()}) {
		const auto entry = ReadPresence({{"state", "online"}, {"activity", "offline_play"}, {"game", bad}});
		EXPECT(entry.presence.kind == Presence::Kind::Online && entry.presence.activity == netplay::friends::Activity::OfflinePlay && entry.presence.game.empty());
	}
	EXPECT(ReadPresence({{"state", "online"}, {"activity", "menus"}, {"game", "a2345678901234567"}}).presence.game.empty());
	EXPECT(ReadPresence({{"state", "online"}, {"activity", "menus"}, {"game", "a234567890123456"}}).presence.game == "a234567890123456");
	// An offline friend has no game to show.
	EXPECT(ReadPresence({{"state", "offline"}, {"seen_at", 9}, {"game", "sfv"}}).presence.game.empty());

	// A private room that friends may join: its facts, never a key.
	auto offered = ReadPresence({{"state", "room"}, {"status", "queued"}, {"private", true}, {"private_room", PrivateOffer()}, {"game", "usf4"}});
	const auto& at = offered.presence;
	EXPECT(at.privateRoom && !at.hasRoom && at.hasPrivateOffer && at.privateOffer.name == "Open Mic" && at.privateOffer.members == 3 &&
		at.privateOffer.capacity == 8 && at.privateOffer.joinable && at.game == "usf4" && at.status == netplay::friends::Status::Queued);
	auto notJoinable = PrivateOffer();
	notJoinable["joinable"] = false;
	EXPECT(ReadPresence({{"state", "room"}, {"status", "idle"}, {"private", true}, {"private_room", notJoinable}}).presence.privateOffer.joinable == false);
	// A private room with no offer is as before.
	const auto plain = ReadPresence({{"state", "room"}, {"status", "idle"}, {"private", true}}).presence;
	EXPECT(plain.privateRoom && !plain.hasPrivateOffer && plain.privateOffer.name.empty());

	// A private_room that cannot be read drops only itself: the friend is still listed in a private room.
	const std::vector<std::pair<const char*, std::function<void(json&)>>> malformed = {
		{"not an object", [](json& o) { o = "Open Mic"; }},
		{"no name", [](json& o) { o.erase("name"); }},
		{"empty name", [](json& o) { o["name"] = ""; }},
		{"name too long", [](json& o) { o["name"] = std::string(65, 'n'); }},
		{"members not a number", [](json& o) { o["members"] = "3"; }},
		{"members past capacity", [](json& o) { o["members"] = 9; }},
		{"capacity of one", [](json& o) { o["capacity"] = 1; }},
		{"capacity past the room", [](json& o) { o["capacity"] = 17; }},
		{"joinable not a flag", [](json& o) { o["joinable"] = 1; }},
	};
	for (const auto& [what, edit] : malformed) {
		auto offer = PrivateOffer();
		edit(offer);
		const auto entry = ReadPresence({{"state", "room"}, {"status", "ready"}, {"private", true}, {"private_room", offer}, {"game", "usf4"}});
		EXPECT_MSG(entry.presence.kind == Presence::Kind::InRoom && entry.presence.privateRoom && !entry.presence.hasPrivateOffer &&
			entry.presence.privateOffer.name.empty() && !entry.presence.privateOffer.joinable && entry.presence.game == "usf4" &&
			entry.presence.status == netplay::friends::Status::Ready, "%s", what);
	}
	// It is accepted only with `private`: a public room, or neither, ignores it.
	const auto publicRoom = ReadPresence({{"state", "room"}, {"status", "idle"}, {"private_room", PrivateOffer()},
		{"room", {{"room_id", RoomId}, {"name", "Player 1's room"}, {"members", 7}, {"capacity", 16}, {"joinable", true}}}}).presence;
	EXPECT(publicRoom.hasRoom && !publicRoom.privateRoom && !publicRoom.hasPrivateOffer);
	const auto notPrivate = ReadPresence({{"state", "room"}, {"status", "idle"}, {"private", false}, {"private_room", PrivateOffer()}}).presence;
	EXPECT(!notPrivate.privateRoom && !notPrivate.hasPrivateOffer);
	const auto online = ReadPresence({{"state", "online"}, {"activity", "menus"}, {"private_room", PrivateOffer()}}).presence;
	EXPECT(online.kind == Presence::Kind::Online && !online.hasPrivateOffer);
	// A key to the room is no part of what a presence holds: a secret beside the offer is ignored.
	auto withKey = PrivateOffer();
	withKey["secret"] = Secret;
	EXPECT(ReadPresence({{"state", "room"}, {"status", "idle"}, {"private", true}, {"private_room", withKey}}).presence.hasPrivateOffer);
}

// The answer to a join: the friend and the room's invitation, which are no part of the state.
void ReadsAJoining() {
	netplay::friends::Joining joining;
	json data = FullState();
	EXPECT(!session::DecodeJoining(data, joining));
	data["joining"] = {{"friend", FriendId}, {"secret", Secret}};
	EXPECT(session::DecodeJoining(json::parse(data.dump()), joining) && joining.friendId == FriendId && joining.secret == Secret);
	// It rides the usual state, which still reads, and has no trace of the key.
	State state;
	EXPECT(Decode(data, state) && state.friends.size() == 2);
	const std::string second = "emd2:" + std::string(122, 'B');
	data["joining"]["secret"] = second;
	EXPECT(session::DecodeJoining(json::parse(data.dump()), joining) && joining.secret == second);

	// What cannot be read leaves the answer alone.
	const netplay::friends::Joining before = joining;
	const std::vector<std::pair<const char*, std::function<void(json&)>>> bad = {
		{"no joining", [](json& d) { d.erase("joining"); }},
		{"joining not an object", [](json& d) { d["joining"] = "emd1:"; }},
		{"no friend", [](json& d) { d["joining"].erase("friend"); }},
		{"friend not an id", [](json& d) { d["joining"]["friend"] = "fr_1"; }},
		{"friend of another kind", [](json& d) { d["joining"]["friend"] = RequestId; }},
		{"no secret", [](json& d) { d["joining"].erase("secret"); }},
		{"secret not a string", [](json& d) { d["joining"]["secret"] = 7; }},
		{"secret too short", [](json& d) { d["joining"]["secret"] = "emd1:" + std::string(121, 'A'); }},
		{"secret too long", [](json& d) { d["joining"]["secret"] = "emd1:" + std::string(123, 'A'); }},
		{"secret of another kind", [](json& d) { d["joining"]["secret"] = "emd3:" + std::string(122, 'A'); }},
		{"secret without a prefix", [](json& d) { d["joining"]["secret"] = std::string(127, 'A'); }},
		{"data not an object", [](json& d) { d = json::array(); }},
	};
	for (const auto& [what, edit] : bad) {
		json damaged = data;
		edit(damaged);
		netplay::friends::Joining out = before;
		EXPECT_MSG(!session::DecodeJoining(json::parse(damaged.dump()), out) && out.friendId == before.friendId && out.secret == before.secret, "%s", what);
	}
}

// A friends state from a later bridge: tiers past this build's table, points by a rule it does not know, and members
// it has never seen. All of it reads; a tier past the last is the last this build draws.
void ReadsAStateFromTheFuture() {
	json data = FullState();
	data["friends"][0]["main"] = {{"fighter", 5}, {"points", 0}, {"tier", 9}, {"prestige", 2}};
	data["friends"][0]["account"] = {{"points", 1}, {"tier", 200}, {"season", "s2"}};
	data["incoming"][0]["played"]["games"] = 0;
	data["friends"][0]["streak"] = 4;
	State state;
	EXPECT(Decode(data, state) && state.friends.size() == 2 && state.incoming.size() == 1);
	const auto& entry = state.friends[0];
	EXPECT(entry.hasMain && entry.main.showsPoints && entry.main.points == 0 && entry.main.tier == netplay::levels::TierCount - 1);
	EXPECT(entry.hasAccount && entry.account.points == 1 && entry.account.tier == netplay::levels::TierCount - 1);
	EXPECT(state.incoming[0].games == 0);
}

// What a later bridge adds that this build cannot name is read as the nearest thing it can, never as a reason to refuse
// the whole list (G3): a status is the neutral one in a room, a state is Online, an activity the menus, an invite of an
// unknown kind is left out, and a badge it cannot read is none.
void ToleratesWhatItDoesNotKnow() {
	State state;
	// An unknown status.
	json data = FullState();
	data["friends"][0]["presence"]["status"] = "queueing_arcade";
	EXPECT(Decode(data, state) && state.friends.size() == 2);
	EXPECT(state.friends[0].presence.kind == Presence::Kind::InRoom && state.friends[0].presence.status == netplay::friends::Status::Idle &&
		state.friends[0].presence.hasRoom && state.friends[0].presence.table == 0);
	// An unknown presence state: online, with no room, whatever else it carried.
	data = FullState();
	data["friends"][0]["presence"] = {{"state", "streaming"}, {"room", {{"room_id", RoomId}}}};
	EXPECT(Decode(data, state));
	EXPECT(state.friends[0].presence.kind == Presence::Kind::Online && state.friends[0].presence.activity == netplay::friends::Activity::Menus &&
		!state.friends[0].presence.hasRoom && state.friends[0].presence.table == -1);
	// An unknown activity: the menus.
	data = FullState();
	data["friends"][1]["presence"] = {{"state", "online"}, {"activity", "sleeping"}};
	EXPECT(Decode(data, state) && state.friends[1].presence.kind == Presence::Kind::Online &&
		state.friends[1].presence.activity == netplay::friends::Activity::Menus);
	// An invite of a kind it does not know goes, the others stay.
	data = FullState();
	data["invites"][0]["room"] = {{"arcade_queue", "x"}};
	EXPECT(Decode(data, state) && state.invites.size() == 1 && state.invites[0].room.kind == netplay::friends::InviteRoom::Kind::Secret);
	data["invites"][1]["room"] = json::object();
	EXPECT(Decode(data, state) && state.invites.empty() && state.friends.size() == 2);
	// A badge it cannot read is none; the friend and the rest of the list are read.
	data = FullState();
	data["friends"][0]["badge"]["band"] = 11;
	EXPECT(Decode(data, state) && state.friends.size() == 2 && !state.friends[0].badge.Present());
	data["friends"][0]["badge"] = "gold";
	EXPECT(Decode(data, state) && !state.friends[0].badge.Present() && state.friends[0].name == "Player 3");
	// Wrong rather than new (both kinds of room in one invite, a known kind that is damaged) leaves that invite out
	// as well, and the rest stands.
	data = FullState();
	data["invites"][0]["room"]["secret"] = Secret;
	EXPECT(Decode(data, state) && state.invites.size() == 1 && state.invites[0].room.kind == netplay::friends::InviteRoom::Kind::Secret);
	data = FullState();
	data["invites"][0]["room"] = {{"room_id", "zz"}};
	EXPECT(Decode(data, state) && state.invites.size() == 1 && state.friends.size() == 2);
	// The code's alphabet is the bridge's; the game reads its shape (it needs no more to show it).
	data = FullState();
	data["code"] = "4F2U-91CX";
	EXPECT(Decode(data, state) && state.code == "4F2U-91CX");
}

// The bridge's bounds are the game's: one past each is refused, the bound itself read.
void KeepsTheBounds() {
	const auto padded = [](const char* key, std::size_t count) {
		json data = FullState();
		const auto row = data[key].empty() ? json::object() : data[key][0];
		data[key] = json::array();
		for (std::size_t i = 0; i < count; ++i) data[key].push_back(row);
		return data;
	};
	EXPECT(Decodes(padded("friends", netplay::friends::MaxFriends)) && !Decodes(padded("friends", netplay::friends::MaxFriends + 1)));
	EXPECT(Decodes(padded("incoming", netplay::friends::MaxIncoming)) && !Decodes(padded("incoming", netplay::friends::MaxIncoming + 1)));
	EXPECT(Decodes(padded("invites", netplay::friends::MaxInvites)) && !Decodes(padded("invites", netplay::friends::MaxInvites + 1)));
	EXPECT(Decodes(padded("blocked", netplay::friends::MaxBlocked)) && !Decodes(padded("blocked", netplay::friends::MaxBlocked + 1)));
	EXPECT(Decodes(padded("outgoing", netplay::friends::MaxOutgoing)) && !Decodes(padded("outgoing", netplay::friends::MaxOutgoing + 1)));
}

// The friends answers are read like every other play answer: one reader, one list of ops.
void ReadsTheAnswerEvent() {
	EXPECT(session::IsTournamentPlayOp("friends_sync") && session::IsTournamentPlayOp("friends_act"));
	auto answer = session::ReadTournamentAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", true}, {"data", FullState()}});
	EXPECT(answer && answer->op == "friends_sync" && answer->requestId == 7 && answer->ok && answer->reason.empty() && answer->data.is_object());
	answer = session::ReadTournamentAnswer({{"op", "friends_act"}, {"request_id", 8u}, {"ok", false}, {"reason", "unknown_code"}});
	EXPECT(answer && answer->requestId == 8 && !answer->ok && answer->reason == "unknown_code");
	// A reason that is null reads as none, as the bridge may send it.
	answer = session::ReadTournamentAnswer({{"op", "friends_act"}, {"request_id", 9u}, {"ok", false}, {"reason", nullptr}});
	EXPECT(answer && answer->reason.empty());
	EXPECT(!session::ReadTournamentAnswer({{"op", "friends_sync"}, {"ok", true}}));
	EXPECT(!session::ReadTournamentAnswer({{"op", "friends_sync"}, {"request_id", "7"}, {"ok", true}}));
	EXPECT(!session::ReadTournamentAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", "yes"}}));
	EXPECT(!session::ReadTournamentAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", false}, {"reason", std::string(65, 'r')}}));
	EXPECT(!session::ReadTournamentAnswer(json::array()));
}

// SplitMix64, as the tournament fuzz uses: the same everywhere.
struct Random {
	std::uint64_t state;
	std::uint64_t Next() {
		std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	std::uint64_t Below(std::uint64_t bound) { return bound ? Next() % bound : 0; }
};

json Junk(Random& random) {
	switch (random.Below(8)) {
	case 0: return nullptr;
	case 1: return true;
	case 2: return -1;
	case 3: return 18446744073709551615ull;
	case 4: return std::string(random.Below(200), 'j');
	case 5: return json::array({1, 2});
	case 6: return json::object();
	default: return "x";
	}
}

// Replaces one random value of the state with junk, or drops one key.
void Damage(json& value, Random& random) {
	if (!value.is_structured() || value.empty() || random.Below(4) == 0) { value = Junk(random); return; }
	if (value.is_array()) { Damage(value[random.Below(value.size())], random); return; }
	auto at = value.begin();
	std::advance(at, static_cast<std::ptrdiff_t>(random.Below(value.size())));
	if (random.Below(5) == 0) value.erase(at.key());
	else Damage(*at, random);
}

// However the state is damaged, the decoder neither throws nor installs more than the bounds allow.
void SurvivesDamage() {
	Random random{0x5EEDF00Dull};
	const char* seed = std::getenv("SF4E_FUZZ_SEED");
	if (seed) random.state = std::strtoull(seed, nullptr, 10);
	const char* env = std::getenv("SF4E_FUZZ_ROUNDS");
	const int rounds = env && std::atoi(env) > 0 ? std::atoi(env) : 4000;
	int decoded = 0;
	for (int round = 0; round < rounds; ++round) {
		json data = FullState();
		for (auto damage = 1 + random.Below(3); damage; --damage) Damage(data, random);
		State state;
		if (!Decode(data, state)) { EXPECT(state.kind == State::Kind::Unavailable); continue; }
		++decoded;
		EXPECT(state.kind == State::Kind::Listed && state.friends.size() <= netplay::friends::MaxFriends && state.incoming.size() <= netplay::friends::MaxIncoming &&
			state.invites.size() <= netplay::friends::MaxInvites && state.blocked.size() <= netplay::friends::MaxBlocked && state.code.size() == 9);
		for (const auto& entry : state.friends) EXPECT(entry.id.size() == 39 && !entry.name.empty() && entry.name.size() <= 32 && entry.presence.table <= netplay::friends::MaxTable);
	}
	// Damage that touches only an ignored key must still decode, or the sweep proves nothing.
	EXPECT(decoded > 0);
}
}

int main() {
	ReadsAWholeState();
	ReadsTheSmallestState();
	IgnoresUnknownKeys();
	RefusesWhatIsMalformed();
	LeavesOutRowsItCannotRead();
	LeavesOutPartsItCannotRead();
	ReadsTheGameAndAPrivateOffer();
	ReadsAJoining();
	ReadsAStateFromTheFuture();
	ToleratesWhatItDoesNotKnow();
	KeepsTheBounds();
	ReadsTheAnswerEvent();
	SurvivesDamage();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
