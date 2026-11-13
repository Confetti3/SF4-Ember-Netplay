// What the game reads of a card's profile members and of a search answer
// (session/TournamentAnswers.hxx; docs/design/PROFILE.md, "Wire contracts"): a
// well-formed answer decodes completely, members a later bridge adds are
// ignored, a missing optional member takes its default, a number of an enum
// this build does not know reads as "not shown", and one bad card or hit never
// fails the whole answer.
#include "../session/TournamentAnswers.hxx"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e;
using nlohmann::json;
using netplay::levels::Card;
using netplay::levels::Relation;
namespace profiles = netplay::profiles;

namespace {
const char* const Token = "c1.AbCdEf_-0123";
const std::string FriendId = "fr_11111111-1111-4111-8111-111111111111";

// As the helper's text arrives: parsed, so a positive number is unsigned, as the decoder expects.
json Parsed(const json& data) { return json::parse(data.dump()); }

json Base(const char* relation = "none") {
	return {{"account", {{"points", 2100}, {"tier", 2}}}, {"relation", relation}};
}

json FullCard() {
	json card = Base("friend");
	card["fighters"] = json::array({{{"fighter", 3}, {"points", 1986}, {"tier", 3}}});
	card["marks"] = {{"games", 2840}, {"wins", 1164}, {"streak", 12}, {"class", 7}, {"rival", 40}, {"opponents", 210}};
	card["name"] = "Player 2";
	card["country"] = "FR";
	card["main"] = 5;
	card["badge"] = {{"band", 7}, {"settled", true}};
	card["since"] = 1748736000;
	card["worn"] = {{"title", "matches"}, {"icon", "streak"}};
	card["pins"] = json::array({"rank", "wins"});
	card["style"] = {{"device", 2}, {"link", 1}, {"region", "euc1"}, {"looking", 34}};
	card["versus"] = {{"won", 3}, {"lost", 5}, {"last_at", 1731000000}};
	card["friend"] = FriendId;
	return card;
}

std::optional<Card> One(const json& card) {
	const auto cards = session::DecodeCards(Parsed({{"cards", json::array({card})}}));
	CHECK(cards && cards->size() == 1);
	return (*cards)[0];
}

void ReadsAFullCard() {
	const auto card = One(FullCard());
	CHECK(card);
	EXPECT(card->relation == Relation::Friend && card->account.points == 2100 && card->fighters.size() == 1);
	EXPECT(card->marks.games == 2840 && card->marks.wins == 1164 && card->marks.streak == 12 && card->marks.peakClass == 7);
	EXPECT(card->name == "Player 2" && card->country.ToString() == "FR" && card->hasMain && card->main == 5);
	EXPECT(card->badge.HasClass() && card->badge.band == 7 && card->badge.settled);
	EXPECT(card->hasSince && card->since == 1748736000);
	EXPECT(card->worn.title == "matches" && card->worn.icon == "streak");
	EXPECT((card->pins == std::vector<std::string>{"rank", "wins"}));
	EXPECT(card->hasStyle && card->style.device == profiles::Device::Stick && card->style.link == profiles::Link::Wired &&
		card->style.region == "euc1" && card->style.looking == (profiles::LookingLongSets | profiles::LookingTeach));
	EXPECT(card->hasVersus && card->versus.won == 3 && card->versus.lost == 5 && card->versus.lastAt == 1731000000);
	EXPECT(card->friendId == FriendId);
}

// A card from a bridge without profiles is the 1.6.0 card: every new member is at its default.
void ReadsAnOlderCard() {
	const auto card = One(Base());
	CHECK(card);
	EXPECT(card->name.empty() && !card->country.Present() && !card->badge.Present() && !card->hasMain && !card->hasSince && !card->worn.Any());
	EXPECT(card->pins.empty() && !card->hasStyle && !card->style.Any() && !card->hasVersus && card->friendId.empty());
	EXPECT(card->account.points == 2100 && card->relation == Relation::None);
}

// A number of an enum this build does not know is "not shown"; the rest of the style stands.
void UnknownEnumValuesReadAsNone() {
	auto card = Base();
	card["style"] = {{"device", 9}, {"link", 7}, {"region", "EU!"}, {"looking", 65}};
	auto read = One(card);
	CHECK(read && read->hasStyle);
	EXPECT(read->style.device == profiles::Device::None && read->style.link == profiles::Link::None && read->style.region.empty());
	// A bit this build does not know is dropped, and the known one stays.
	EXPECT(read->style.looking == profiles::LookingCasuals);
	// More than two kinds at once is not something the bridge sends: none.
	card["style"] = {{"device", 4}, {"link", 2}, {"looking", 7}};
	read = One(card);
	CHECK(read);
	EXPECT(read->style.device == profiles::Device::Keyboard && read->style.link == profiles::Link::WiFi && read->style.looking == 0);
	// Every known number maps to its own.
	for (unsigned device = 0; device <= 4; ++device) {
		card["style"] = {{"device", device}};
		read = One(card);
		CHECK(read);
		EXPECT(static_cast<unsigned>(read->style.device) == device);
	}
	EXPECT(profiles::DeviceFromWire(5) == profiles::Device::None && profiles::LinkFromWire(3) == profiles::Link::None);
	// A style that is no object is no style.
	card["style"] = 3;
	read = One(card);
	CHECK(read);
	EXPECT(!read->hasStyle);
}

// Each member stands alone: a bad one is left at its default and the card still reads.
void AMemberThatIsBadIsLeftOut() {
	auto card = FullCard();
	card["name"] = 5;
	card["country"] = "zz";
	card["main"] = 44;
	card["badge"] = {{"band", 11}, {"settled", true}};
	card["since"] = "June";
	card["worn"] = {{"title", "Not An Id"}, {"icon", 4}};
	card["friend"] = "friend-1";
	card["versus"] = {{"won", 0}, {"lost", 0}};
	const auto read = One(card);
	CHECK(read);
	EXPECT(read->name.empty() && !read->country.Present() && !read->hasMain && !read->badge.Present() && !read->hasSince);
	EXPECT(!read->worn.Any() && read->friendId.empty() && !read->hasVersus);
	EXPECT(read->account.points == 2100 && read->relation == Relation::Friend && read->hasStyle);
	// A name over 32 bytes or with a control character is not shown as it came.
	card = Base();
	card["name"] = std::string(33, 'a');
	EXPECT(One(card)->name.empty());
	card["name"] = "Line\nBreak";
	EXPECT(One(card)->name == "Line Break");
}

// Pins are at most four distinct family ids; one of another shape is skipped.
void PinsAreBounded() {
	auto card = Base();
	card["pins"] = json::array({"rank", "rank", "Wins", 3, "streak", "cast", "friends", "matches"});
	const auto read = One(card);
	CHECK(read);
	EXPECT((read->pins == std::vector<std::string>{"rank", "streak", "cast", "friends"}));
	card["pins"] = "rank";
	EXPECT(One(card)->pins.empty());
}

// The friendship's id is read only for a friend.
void TheFriendIdIsForFriends() {
	auto card = Base("none");
	card["friend"] = FriendId;
	EXPECT(One(card)->friendId.empty());
	card["relation"] = "friend";
	EXPECT(One(card)->friendId == FriendId);
}

// One card that cannot be read is no card; the others in the answer stand. An answer with no list, no card or too many fails.
void OneBadCardDoesNotFailTheAnswer() {
	const auto cards = session::DecodeCards(Parsed({{"cards", json::array({FullCard(), json::object(), nullptr, Base("sent")})}}));
	CHECK(cards && cards->size() == 4);
	EXPECT((*cards)[0] && !(*cards)[1] && !(*cards)[2] && (*cards)[3] && (*cards)[3]->relation == Relation::Sent);
	json broken = Base();
	broken["account"] = {{"points", 1}, {"tier", 9}};
	EXPECT(!(*session::DecodeCards(Parsed({{"cards", json::array({broken})}})))[0]);
	EXPECT(!session::DecodeCards(Parsed({{"cards", json::array()}})));
	json many = json::array();
	for (std::size_t i = 0; i <= netplay::levels::MaxLookup; ++i) many.push_back(Base());
	EXPECT(!session::DecodeCards(Parsed({{"cards", many}})));
	EXPECT(!session::DecodeCards(Parsed(json::object())));
}

json Hit(const std::string& token, const std::string& name) {
	return {{"card", token}, {"name", name}, {"country", "FR"}, {"main", 5}, {"badge", {{"band", 7}, {"settled", true}}}, {"since", 1748736000}};
}

void ReadsSearchHits() {
	const auto answer = session::DecodeSearch(Parsed({{"players", json::array({Hit(Token, "Player 2"), Hit("c1.Other_Token-1", "Player 22")})}, {"more", true}}));
	CHECK(answer && answer->players.size() == 2 && answer->more);
	const auto& hit = answer->players[0];
	EXPECT(hit.card == Token && hit.name == "Player 2" && hit.country.ToString() == "FR" && hit.hasMain && hit.main == 5);
	EXPECT(hit.badge.HasClass() && hit.badge.band == 7 && hit.since == 1748736000);
	EXPECT(answer->players[1].name == "Player 22");
	// Members a later bridge adds are ignored; a hit with only its card and name reads with defaults.
	auto extra = Hit(Token, "Player 3");
	extra["later"] = 1;
	const auto plain = session::DecodeSearch(Parsed({{"players", json::array({extra, json{{"card", Token}, {"name", "Player 4"}}})}, {"total", 2}}));
	CHECK(plain && plain->players.size() == 2 && !plain->more);
	EXPECT(!plain->players[1].country.Present() && !plain->players[1].hasMain && !plain->players[1].badge.Present() && plain->players[1].since == 0);
}

// A hit that cannot be read is left out; the answer still reads. No list, or too long a one, is no answer.
void ABadHitIsLeftOut() {
	auto lowercase = Hit(Token, "Player 5");
	lowercase["country"] = "fr";
	const auto answer = session::DecodeSearch(Parsed({{"players", json::array({
		Hit("not-a-token", "Player 6"), Hit(Token, ""), Hit(Token, std::string(33, 'n')), 4, json{{"name", "Player 7"}}, lowercase, Hit(Token, "Player 8")})}}));
	CHECK(answer && answer->players.size() == 2);
	EXPECT(answer->players[0].name == "Player 5" && !answer->players[0].country.Present() && answer->players[1].name == "Player 8");
	EXPECT(session::DecodeSearch(Parsed({{"players", json::array()}})) && session::DecodeSearch(Parsed({{"players", json::array()}}))->players.empty());
	EXPECT(!session::DecodeSearch(Parsed(json::object())) && !session::DecodeSearch(Parsed({{"players", 3}})) && !session::DecodeSearch(Parsed(json::array())));
	json many = json::array();
	for (std::size_t i = 0; i <= profiles::MaxSearch; ++i) many.push_back(Hit(Token, "Player 9"));
	EXPECT(!session::DecodeSearch(Parsed({{"players", many}})));
	json exact = json::array();
	for (std::size_t i = 0; i < profiles::MaxSearch; ++i) exact.push_back(Hit(Token, "Player 9"));
	EXPECT(session::DecodeSearch(Parsed({{"players", exact}}))->players.size() == profiles::MaxSearch);
}

// card_save and cards_search answer on the play channel, with the request's id.
void TheNewOpsRideThePlayChannel() {
	for (const char* op : {"card_save", "cards_search"}) {
		EXPECT(session::IsTournamentPlayOp(op));
		const auto answer = session::ReadTournamentAnswer(Parsed({{"op", op}, {"request_id", 7}, {"ok", true}, {"data", json::object()}}));
		EXPECT(answer && answer->ok && answer->requestId == 7 && answer->op == op);
		const auto refused = session::ReadTournamentAnswer(Parsed({{"op", op}, {"request_id", 8}, {"ok", false}, {"reason", profiles::Unavailable}}));
		EXPECT(refused && !refused->ok && refused->reason == "profiles_unavailable");
	}
}
}

int main() {
	ReadsAFullCard();
	ReadsAnOlderCard();
	UnknownEnumValuesReadAsNone();
	AMemberThatIsBadIsLeftOut();
	PinsAreBounded();
	TheFriendIdIsForFriends();
	OneBadCardDoesNotFailTheAnswer();
	ReadsSearchHits();
	ABadHitIsLeftOut();
	TheNewOpsRideThePlayChannel();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("profile answer tests passed\n");
	return TEST_EXIT_CODE();
}
