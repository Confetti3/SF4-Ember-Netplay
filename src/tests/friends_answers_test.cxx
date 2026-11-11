// What the game reads of the bridge's FriendsState (session/SocialAnswers.hxx):
// a well-formed state decodes completely, keys a later bridge adds are ignored,
// and anything malformed is refused whole and leaves the state alone, however
// it is damaged (a seeded sweep).
#include "../session/SocialAnswers.hxx"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
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
	refused("code with a letter outside the alphabet", changed([](json& d) { d["code"] = "4F2U-91CX"; }));
	refused("lower-case code", changed([](json& d) { d["code"] = "4f2a-91cx"; }));
	refused("appear_offline not a bool", changed([](json& d) { d["appear_offline"] = 1; }));
	refused("no friends list", changed([](json& d) { d.erase("friends"); }));
	refused("friends not a list", changed([](json& d) { d["friends"] = "none"; }));
	refused("no blocked list", changed([](json& d) { d.erase("blocked"); }));
	refused("friend id of a request", changed([](json& d) { d["friends"][0]["id"] = RequestId; }));
	refused("friend id too short", changed([](json& d) { d["friends"][0]["id"] = "fr_1"; }));
	refused("empty name", changed([](json& d) { d["friends"][0]["name"] = ""; }));
	refused("long name", changed([](json& d) { d["friends"][0]["name"] = std::string(33, 'n'); }));
	refused("name not text", changed([](json& d) { d["friends"][0]["name"] = 4; }));
	refused("lower-case country", changed([](json& d) { d["friends"][0]["country"] = "fr"; }));
	refused("long country", changed([](json& d) { d["friends"][0]["country"] = "FRA"; }));
	refused("negative since", changed([](json& d) { d["friends"][0]["since"] = -1; }));
	refused("no presence", changed([](json& d) { d["friends"][0].erase("presence"); }));
	refused("unknown presence state", changed([](json& d) { d["friends"][0]["presence"]["state"] = "away"; }));
	refused("unknown status", changed([](json& d) { d["friends"][0]["presence"]["status"] = "dancing"; }));
	refused("table past the last", changed([](json& d) { d["friends"][0]["presence"]["table"] = 8; }));
	refused("a room and private", changed([](json& d) { d["friends"][0]["presence"]["private"] = true; }));
	refused("room id not hex", changed([](json& d) { d["friends"][0]["presence"]["room"]["room_id"] = std::string(32, 'z'); }));
	refused("room fuller than its capacity", changed([](json& d) { d["friends"][0]["presence"]["room"]["members"] = 17; }));
	refused("room of one", changed([](json& d) { d["friends"][0]["presence"]["room"]["capacity"] = 1; d["friends"][0]["presence"]["room"]["members"] = 0; }));
	refused("unknown activity", changed([](json& d) {
		d["friends"][1]["presence"] = {{"state", "online"}, {"activity", "sleeping"}}; }));
	refused("fighter past the roster", changed([](json& d) { d["friends"][0]["main"]["fighter"] = 44; }));
	refused("points without a tier", changed([](json& d) { d["friends"][0]["main"].erase("tier"); }));
	refused("tier past the last", changed([](json& d) { d["friends"][0]["main"]["tier"] = 5; }));
	refused("zero points", changed([](json& d) { d["friends"][0]["main"]["points"] = 0; }));
	refused("account tier past the last", changed([](json& d) { d["friends"][0]["account"]["tier"] = 5; }));
	refused("account without a tier", changed([](json& d) { d["friends"][0]["account"].erase("tier"); }));
	refused("account points not a number", changed([](json& d) { d["friends"][0]["account"]["points"] = "many"; }));
	refused("account not an object", changed([](json& d) { d["friends"][0]["account"] = 7; }));
	refused("badge band past the classes", changed([](json& d) { d["friends"][0]["badge"]["band"] = 11; }));
	refused("empty record", changed([](json& d) { d["friends"][0]["record"] = {{"won", 0}, {"lost", 0}, {"last_at", 1}}; }));
	refused("record without a time", changed([](json& d) { d["friends"][0]["record"].erase("last_at"); }));
	refused("request id of a friend", changed([](json& d) { d["incoming"][0]["id"] = FriendId; }));
	refused("played nothing", changed([](json& d) { d["incoming"][0]["played"]["games"] = 0; }));
	refused("long label", changed([](json& d) { d["outgoing"][0]["label"] = std::string(65, 'l'); }));
	refused("invite room of both kinds", changed([](json& d) { d["invites"][0]["room"]["secret"] = Secret; }));
	refused("invite room of neither", changed([](json& d) { d["invites"][0]["room"] = json::object(); }));
	refused("invite secret too short", changed([](json& d) { d["invites"][1]["room"]["secret"] = "emd1:AAAA"; }));
	refused("invite secret of another kind", changed([](json& d) { d["invites"][1]["room"]["secret"] = "emx9:" + std::string(122, 'A'); }));
	refused("invite from a request id", changed([](json& d) { d["invites"][0]["friend"] = RequestId; }));
	refused("block id of a friend", changed([](json& d) { d["blocked"][0]["id"] = FriendId; }));
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

void ReadsTheAnswerEvent() {
	session::TournamentAnswer answer;
	EXPECT(session::IsSocialOp("friends_sync") && session::IsSocialOp("friends_act") && !session::IsSocialOp("points_me"));
	EXPECT(session::ReadSocialAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", true}, {"data", FullState()}}, answer));
	EXPECT(answer.op == "friends_sync" && answer.requestId == 7 && answer.ok && answer.reason.empty() && answer.data.is_object());
	EXPECT(session::ReadSocialAnswer({{"op", "friends_act"}, {"request_id", 8u}, {"ok", false}, {"reason", "unknown_code"}}, answer));
	EXPECT(answer.requestId == 8 && !answer.ok && answer.reason == "unknown_code");
	const session::TournamentAnswer before = answer;
	EXPECT(!session::ReadSocialAnswer({{"op", "points_me"}, {"request_id", 7u}, {"ok", true}}, answer));
	EXPECT(!session::ReadSocialAnswer({{"op", "friends_sync"}, {"ok", true}}, answer));
	EXPECT(!session::ReadSocialAnswer({{"op", "friends_sync"}, {"request_id", "7"}, {"ok", true}}, answer));
	EXPECT(!session::ReadSocialAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", "yes"}}, answer));
	EXPECT(!session::ReadSocialAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", false}, {"reason", std::string(65, 'r')}}, answer));
	EXPECT(!session::ReadSocialAnswer(json::array(), answer));
	EXPECT(answer.requestId == before.requestId && answer.reason == before.reason);
	// The tournament reader leaves these answers to this one.
	EXPECT(!session::ReadTournamentAnswer({{"op", "friends_sync"}, {"request_id", 7u}, {"ok", true}}).has_value());
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
	KeepsTheBounds();
	ReadsTheAnswerEvent();
	SurvivesDamage();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
