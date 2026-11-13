// What the game reads of the points answers (docs/design/FIGHTER_LEVELS.md): the player's own points
// in one of two shapes, with the counters behind the awards, and a card look-up as one card or
// nothing per player. Anything the bridge would not send reads as nothing; a member it adds later
// is ignored, and a relation it adds reads as none. What a card says of its player is in
// profile_answers_test.cxx.
#include "../session/TournamentAnswers.hxx"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sf4e;
using nlohmann::json;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); if (++failures > 20) std::exit(1); } } while (false)

static void TestLevels() {
	using Kind = netplay::levels::Own::Kind;
	using netplay::levels::Relation;
	CHECK(session::DecodeOwnLevels({{"points", "no_account"}})->kind == Kind::NoAccount);
	const json entry = {{"fighter", 3u}, {"points", 1900u}, {"games", 1000u}, {"wins", 900u}, {"tier", 3u}, {"floor", 1500u}, {"next", 2500u}, {"since", 1729468800u}};
	const json account = {{"points", 13u}, {"games", 9u}, {"wins", 4u}, {"tier", 0u}, {"floor", 0u}, {"next", 500u}, {"since", 1729468800u}};
	const auto counted = [&](const json& fighters) { return json{{"points", "counted"}, {"account", account}, {"fighters", fighters}}; };
	const auto own = session::DecodeOwnLevels(counted(json::array({entry})));
	CHECK(own && own->kind == Kind::Counted && own->fighters[3].points == 1900 && own->fighters[3].games == 1000 &&
		own->fighters[3].wins == 900 && own->fighters[3].tier == 3 && own->fighters[3].floor == 1500 && own->fighters[3].next == 2500 &&
		own->fighters[3].since == 1729468800 && own->fighters[2].points == 0);
	CHECK(own->account.points == 13 && own->account.games == 9 && own->account.wins == 4 && own->account.tier == 0 && own->account.floor == 0 &&
		own->account.next == 500 && own->account.since == 1729468800);
	// An account with no game counted yet has no `since`, and is 0 points.
	const json nothing = {{"points", 0u}, {"games", 0u}, {"wins", 0u}, {"tier", 0u}, {"floor", 0u}, {"next", 500u}};
	const auto fresh = session::DecodeOwnLevels({{"points", "counted"}, {"account", nothing}, {"fighters", json::array()}});
	CHECK(fresh && fresh->account.points == 0 && fresh->account.since == 0 && fresh->account.next == 500);
	// The last tier has no next one, and any other tier has one.
	auto maxed = entry;
	maxed["tier"] = 4u;
	maxed.erase("next");
	CHECK(session::DecodeOwnLevels(counted(json::array({maxed}))));
	maxed["next"] = 2500u;
	CHECK(!session::DecodeOwnLevels(counted(json::array({maxed}))));
	const auto changed = [&](const char* key, const json& value) {
		auto row = entry;
		row[key] = value;
		return counted(json::array({row}));
	};
	const auto accountChanged = [&](const char* key, const json& value) {
		auto row = account;
		row[key] = value;
		return json{{"points", "counted"}, {"account", row}, {"fighters", json::array()}};
	};
	auto lacking = entry;
	lacking.erase("next");
	auto unfloored = entry;
	unfloored.erase("floor");
	auto extra = entry;
	extra["rank"] = 1u;
	auto second = entry;
	second["fighter"] = 2u;
	auto unsinced = account;
	unsinced.erase("since");
	auto sincedNothing = nothing;
	sincedNothing["since"] = 1u;
	auto accountLacking = account;
	accountLacking.erase("next");
	for (const json& invalid : {json{{"points", "ranked"}}, json{{"points", "counted"}},
			json{{"points", "counted"}, {"fighters", json::array()}},
			json{{"points", "counted"}, {"account", account}},
			json{{"points", "counted"}, {"account", 1u}, {"fighters", json::array()}},
			changed("fighter", 44u), changed("tier", 5u), changed("points", 1899u), changed("points", 0u), changed("wins", 1001u),
			changed("next", 1900u), changed("next", 1899u), changed("floor", 1901u), changed("floor", -1),
			counted(json::array({unfloored})), changed("since", -1), changed("games", "1000"),
			counted(json::array({lacking})),
			counted(json::array({entry, entry})),
			counted(json::array({entry, second})),
			accountChanged("tier", 5u), accountChanged("points", 14u), accountChanged("wins", 10u), accountChanged("next", 13u),
			accountChanged("floor", 14u), accountChanged("floor", 1u), accountChanged("since", -1),
			json{{"points", "counted"}, {"account", unsinced}, {"fighters", json::array()}},
			json{{"points", "counted"}, {"account", sincedNothing}, {"fighters", json::array()}},
			json{{"points", "counted"}, {"account", accountLacking}, {"fighters", json::array()}}})
		CHECK(!session::DecodeOwnLevels(invalid));
	// The old `shown` member is a member the reader does not know.
	const auto tolerated = session::DecodeOwnLevels({{"points", "counted"}, {"rank", 1u}, {"shown", true}, {"account", account}, {"fighters", json::array({extra})}});
	CHECK(tolerated && tolerated->fighters[3].points == 1900);
	CHECK(session::DecodeOwnLevels({{"points", "no_account"}, {"fighters", json::array()}, {"rank", 1u}})->kind == Kind::NoAccount);

	// The award counters are absent from an older bridge and read as 0; when they
	// are there they are in range, and a run is never longer than the games.
	CHECK(own->awards.bestStreak == 0 && own->awards.peakClass == 0);
	auto counters = account;
	counters["best_streak"] = 3u;
	counters["peak_class"] = 6u;
	counters["rival"] = 12u;
	counters["opponents"] = 30u;
	const auto withCounters = session::DecodeOwnLevels({{"points", "counted"}, {"account", counters}, {"fighters", json::array()}});
	CHECK(withCounters && withCounters->awards.bestStreak == 3 && withCounters->awards.peakClass == 6 &&
		withCounters->awards.rival == 12 && withCounters->awards.opponents == 30);
	// The most played opponent: absent from an older bridge, read when whole, left out alone when it is not,
	// and unknown members are ignored.
	CHECK(!own->hasMostPlayed);
	const json rival = {{"card", "c1.abc_DEF-123"}, {"name", "Player 7"}, {"country", "US"}, {"badge", {{"band", 8u}, {"settled", true}}}, {"won", 41u}, {"lost", 33u}};
	const auto withRival = [&](const json& rivalObject) {
		return session::DecodeOwnLevels({{"points", "counted"}, {"account", account}, {"fighters", json::array()}, {"rival", rivalObject}});
	};
	const auto named = withRival(rival);
	CHECK(named && named->hasMostPlayed && named->mostPlayed.card == "c1.abc_DEF-123" && named->mostPlayed.name == "Player 7" &&
		named->mostPlayed.country.Present() && named->mostPlayed.badge.HasClass() && named->mostPlayed.badge.band == 8 &&
		named->mostPlayed.won == 41 && named->mostPlayed.lost == 33);
	auto plain = rival;
	plain.erase("country");
	plain.erase("badge");
	plain["extra"] = true;
	const auto bare = withRival(plain);
	CHECK(bare && bare->hasMostPlayed && !bare->mostPlayed.country.Present() && !bare->mostPlayed.badge.HasClass());
	const auto changedRival = [&](const char* key, const json& value) { auto row = rival; row[key] = value; return row; };
	for (const auto& bad : {changedRival("card", "nope"), changedRival("card", 4), changedRival("name", ""), changedRival("won", -1), changedRival("lost", "3"),
			json{{"card", "c1.abc"}, {"name", "Player 7"}, {"won", 0u}, {"lost", 0u}}, json{{"card", "c1.abc"}, {"name", "Player 7"}, {"won", 5u}}, json::array(), json("x"), json()}) {
		const auto dropped = withRival(bad);
		CHECK(dropped && dropped->kind == Kind::Counted && !dropped->hasMostPlayed && dropped->account.points == 13);
	}
	for (const auto& bad : {json{{"best_streak", 10u}}, json{{"best_streak", -1}}, json{{"peak_class", 11u}}, json{{"peak_class", "6"}}, json{{"rival", -1}}, json{{"opponents", "3"}}}) {
		auto row = account;
		row.update(bad);
		CHECK(!session::DecodeOwnLevels({{"points", "counted"}, {"account", row}, {"fighters", json::array()}}));
	}

	const json cardAccount = {{"points", 2100u}, {"tier", 2u}};
	const auto cards = session::DecodeCards({{"cards", json::array({
		json{{"account", cardAccount}, {"fighters", json::array({json{{"fighter", 3u}, {"points", 1986u}, {"tier", 3u}}})}, {"relation", "none"}}, nullptr,
		json{{"account", cardAccount}, {"relation", "friend"}}, json{{"account", {{"points", 0u}, {"tier", 0u}}}, {"fighters", json::array()}, {"relation", "blocked"}}})}});
	CHECK(cards && cards->size() == 4 && (*cards)[0] && (*cards)[0]->relation == Relation::None &&
		(*cards)[0]->account.points == 2100 && (*cards)[0]->account.tier == 2 &&
		(*cards)[0]->fighters.size() == 1 && (*cards)[0]->fighters[0].fighter == 3 && (*cards)[0]->fighters[0].points == 1986 &&
		(*cards)[0]->fighters[0].tier == 3 && !(*cards)[1] && (*cards)[2] && (*cards)[2]->fighters.empty() &&
		(*cards)[2]->account.points == 2100 && (*cards)[2]->relation == Relation::Friend && (*cards)[3]->fighters.empty() &&
		(*cards)[3]->account.points == 0 && (*cards)[3]->relation == Relation::Blocked);
	// The counters are absent from an older bridge (0), and are read as sent when there.
	CHECK((*cards)[0]->awards.games == 0 && (*cards)[0]->awards.wins == 0 && (*cards)[0]->awards.bestStreak == 0 && (*cards)[0]->awards.peakClass == 0);
	const auto marked = session::DecodeCards({{"cards", json::array({json{{"account", cardAccount}, {"relation", "none"},
		{"awards", {{"games", 100u}, {"wins", 50u}, {"best_streak", 5u}, {"peak_class", 6u}, {"rival", 10u}, {"opponents", 50u}, {"later", 1u}}}}})}});
	CHECK(marked && (*marked)[0] && (*marked)[0]->awards.games == 100 && (*marked)[0]->awards.wins == 50 && (*marked)[0]->awards.bestStreak == 5 &&
		(*marked)[0]->awards.peakClass == 6 && (*marked)[0]->awards.rival == 10 && (*marked)[0]->awards.opponents == 50);
	for (const json& badMarks : {json{{"peak_class", 11u}}, json{{"games", -1}}, json{{"wins", "5"}}, json(1u)})
	{
		const auto unread = session::DecodeCards({{"cards", json::array({json{{"account", cardAccount}, {"relation", "none"}, {"awards", badMarks}}})}});
		CHECK(unread && unread->size() == 1 && !(*unread)[0]);
	}
	for (const char* relation : {"sent", "received"}) CHECK(session::DecodeCards({{"cards", json::array({json{{"account", cardAccount}, {"relation", relation}}})}}));
	json seventeen = json::array();
	for (std::size_t i = 0; i <= netplay::levels::MaxLookup; ++i) seventeen.push_back(json{{"account", cardAccount}, {"relation", "none"}});
	const auto card = [&](const json& fighter) { return json{{"cards", json::array({json{{"account", cardAccount}, {"fighters", json::array({fighter})}, {"relation", "none"}}})}}; };
	// An answer with no card, or too many, is no answer.
	for (const json& invalid : {json{{"cards", json::array()}}, json{{"cards", seventeen}}})
		CHECK(!session::DecodeCards(invalid));
	// One card that cannot be read is no card, and the answer stands.
	for (const json& invalid : {json{{"cards", json::array({json::object()})}},
			json{{"cards", json::array({json{{"account", cardAccount}, {"relation", 1u}}})}},
			json{{"cards", json::array({json{{"account", cardAccount}, {"fighters", json::array()}}})}},
			// A card always has its account, with a count and a tier in range.
			json{{"cards", json::array({json{{"relation", "none"}}})}},
			json{{"cards", json::array({json{{"fighters", json::array()}, {"relation", "none"}}})}},
			json{{"cards", json::array({json{{"account", 1u}, {"relation", "none"}}})}},
			json{{"cards", json::array({json{{"account", {{"points", 1u}, {"tier", 5u}}}, {"relation", "none"}}})}},
			json{{"cards", json::array({json{{"account", json{{"tier", 0u}}}, {"relation", "none"}}})}},
			card(json{{"fighter", 3u}, {"level", 31u}, {"games", 612u}}),
			card(json{{"fighter", 44u}, {"points", 1986u}, {"tier", 3u}}), card(json{{"fighter", 3u}, {"points", 0u}, {"tier", 0u}}),
			card(json{{"fighter", 3u}, {"points", 1986u}, {"tier", 5u}}),
			json{{"cards", json::array({json{{"account", cardAccount}, {"fighters", json::array({json{{"fighter", 3u}, {"points", 1u}, {"tier", 0u}},
				json{{"fighter", 3u}, {"points", 1u}, {"tier", 0u}}})}, {"relation", "none"}}})}}})
	{
		const auto unread = session::DecodeCards(invalid);
		CHECK(unread && unread->size() == 1 && !(*unread)[0]);
	}
	// Members the reader does not know are ignored; an unknown relation is none.
	for (const json& ignored : {json{{"cards", json::array({json{{"account", cardAccount}, {"relation", "none"}, {"name", "Player 1"}}})}},
			json{{"cards", json::array({nullptr})}, {"total", 1u}},
			json{{"cards", json::array({json{{"account", {{"points", 1u}, {"tier", 0u}, {"games", 1u}}}, {"relation", "none"}}})}},
			card(json{{"fighter", 3u}, {"points", 1986u}, {"tier", 3u}, {"wins", 300u}}),
			card(json{{"fighter", 3u}, {"points", 1986u}, {"tier", 3u}, {"games", 612u}})})
		CHECK(session::DecodeCards(ignored));
	const auto enemy = session::DecodeCards({{"cards", json::array({json{{"account", cardAccount}, {"relation", "enemy"}}})}});
	CHECK(enemy && (*enemy)[0] && (*enemy)[0]->relation == Relation::None);
	for (const char* op : {"points_me", "cards_lookup"})
		CHECK(session::ReadTournamentAnswer({{"op", op}, {"request_id", 7u}, {"ok", true}, {"data", json::object()}}).has_value());
}

int main() {
	TestLevels();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("levels answers tests passed\n");
	return failures ? 1 : 0;
}
