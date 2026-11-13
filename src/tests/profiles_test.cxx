// The runtime's rules for the player's own card and the players met lately
// (netplay/Profiles.hxx, CardBundle.hxx, ProfileWire.hxx, CardPreferenceJson.hxx;
// docs/design/PROFILE.md, "Runtime and view"): recent opponents (20, 7 days, one
// per name and visit, newest first), the card save only on a change and at most
// once a minute, what the bundle holds and the exact requests sent, the card
// preferences' round trip with bad values dropped, and the small enum rules.
#include "../netplay/CardBundle.hxx"
#include "../netplay/CardPreferenceJson.hxx"
#include "../netplay/ProfileWire.hxx"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e;
using nlohmann::json;
using namespace sf4e::netplay::profiles;
using netplay::PlayerPreferences;

namespace {
const char* const Token = "c1.AbCdEf_-0123";

Recent Met(const std::string& name, std::uint64_t at, std::uint64_t visit, const char* token = Token) {
	Recent entry;
	entry.card = token;
	entry.name = name;
	entry.country = CountryCode::Parsed("FR");
	entry.fighter = 5;
	entry.room = "Player 1's room";
	entry.at = at;
	entry.visit = visit;
	return entry;
}

// Looking for is bits, at most two, and a number this build does not know is not shown.
void LookingBitsAreBounded() {
	EXPECT(LookingFromWire(0) == 0 && LookingFromWire(34) == 34 && LookingFromWire(63) == 0 && LookingFromWire(7) == 0);
	EXPECT(LookingFromWire(64) == 0 && LookingFromWire(65) == 1 && LookingFromWire(1ull << 40) == 0);
	// A saved preference keeps the lowest two of more, and drops what is unknown.
	EXPECT(LookingFromSaved(63) == 3 && LookingFromSaved(7) == 3 && LookingFromSaved(34) == 34 && LookingFromSaved(64) == 0 && LookingFromSaved(96) == 32);
	EXPECT(LookingCount(34) == 2 && LookingCount(0) == 0 && LookingCount(63) == 6);
	EXPECT(DeviceFromWire(0) == Device::None && DeviceFromWire(4) == Device::Keyboard && DeviceFromWire(5) == Device::None);
	EXPECT(LinkFromWire(1) == Link::Wired && LinkFromWire(2) == Link::WiFi && LinkFromWire(3) == Link::None);
}

// Automatic follows the controller: the keyboard, a pad only Ember's setup made usable as a stick, any other pad.
void AutomaticDeviceFollowsTheController() {
	using gamepads::Brand;
	using gamepads::PromptBrand;
	EXPECT(AutomaticDevice(input::PadKeyboard, true, PromptBrand{}) == Device::Keyboard);
	EXPECT(AutomaticDevice(input::PadXInput, true, PromptBrand{Brand::Stick}) == Device::Stick);
	EXPECT(AutomaticDevice(input::PadXInput, true, PromptBrand{Brand::Xbox}) == Device::Pad);
	EXPECT(AutomaticDevice(input::PadXInput, true, PromptBrand{Brand::PlayStation}) == Device::Pad);
	EXPECT(AutomaticDevice(input::PadDirectInput, true, PromptBrand{}) == Device::Pad);
	EXPECT(AutomaticDevice(input::PadDirectInput, true, PromptBrand{Brand::Stick}) == Device::Stick);
	// No controller known: left out.
	EXPECT(AutomaticDevice(input::PadXInput, false, PromptBrand{Brand::Xbox}) == Device::None);
	EXPECT(AutomaticDevice(-1, false, PromptBrand{}) == Device::None);
}

void NamesAreFoundInAnyCase() {
	Hit a, b, c;
	a.name = "Player 2"; b.name = "player 2"; c.name = "Player 22";
	a.country = CountryCode::Parsed("FR"); b.country = CountryCode::Parsed("DE"); c.country = CountryCode::Parsed("FR");
	const auto named = NamedHits({a, b, c}, "PLAYER 2");
	EXPECT(named.size() == 2 && named[0].name == "Player 2" && named[1].name == "player 2");
	// The only one is the one; with several the opener's country settles it only when it names one.
	EXPECT(ChooseHit({a}, CountryCode()) == 0);
	EXPECT(ChooseHit(named, CountryCode()) == -1);
	EXPECT(ChooseHit(named, CountryCode::Parsed("DE")) == 1 && ChooseHit(named, CountryCode::Parsed("FR")) == 0);
	EXPECT(ChooseHit(named, CountryCode::Parsed("US")) == -1);
	EXPECT(ChooseHit({a, c}, CountryCode::Parsed("FR")) == -1);
	EXPECT(ChooseHit({}, CountryCode::Parsed("FR")) == -1);
	EXPECT(SameNameAnyCase("Ab", "aB") && !SameNameAnyCase("Ab", "Abc") && !SameNameAnyCase("[", "{"));
}

// The last 20 players, newest first, none kept past 7 days, one per name and room visit.
void RecentOpponentsAreBounded() {
	const std::uint64_t day = 24 * 60 * 60, now = 100 * day;
	RecentList list;
	// Twenty-five different players: the newest twenty stay, the newest first.
	for (std::uint64_t i = 0; i < 25; ++i) EXPECT(list.Note(Met("Player " + std::to_string(i), now + i, 1), now + i));
	EXPECT(list.Entries().size() == MaxRecent);
	EXPECT(list.Entries().front().name == "Player 24" && list.Entries().back().name == "Player 5");
	// One entry per name and visit: a second game replaces it, with the newer time.
	list = RecentList();
	EXPECT(list.Note(Met("Player 2", now, 5), now));
	EXPECT(list.Note(Met("Player 2", now + 60, 5, "c1.Newer_Token-2"), now + 60));
	EXPECT(list.Entries().size() == 1 && list.Entries()[0].at == now + 60 && list.Entries()[0].card == "c1.Newer_Token-2");
	// Another visit, or another name, is another entry; the newest comes first.
	EXPECT(list.Note(Met("Player 2", now + 120, 6), now + 120));
	EXPECT(list.Note(Met("Player 3", now + 180, 6), now + 180));
	EXPECT(list.Entries().size() == 3 && list.Entries()[0].name == "Player 3" && list.Entries()[1].visit == 6 && list.Entries()[2].visit == 5);
	// Of equal times, the one noted last is first.
	EXPECT(list.Note(Met("Player 4", now + 180, 6), now + 180));
	EXPECT(list.Entries()[0].name == "Player 4" && list.Entries()[1].name == "Player 3");
	// Nothing malformed is kept.
	const auto before = list.Entries().size();
	EXPECT(!list.Note(Met("", now, 1), now) && !list.Note(Met("Player 5", now, 1, "not a token"), now) && !list.Note(Met("Player 5", 0, 1), now));
	EXPECT(!list.Note(Met(std::string(33, 'n'), now, 1), now) && !list.Note(Met("Player\n5", now, 1), now) && list.Entries().size() == before);
	Recent unknown = Met("Player 6", now, 1);
	unknown.fighter = selection::FighterCount;
	EXPECT(!list.Note(unknown, now));
	unknown.fighter = -1;
	EXPECT(list.Note(unknown, now));
}

void RecentOpponentsLastAWeek() {
	const std::uint64_t week = RecentLifeSeconds, start = 1000;
	RecentList list;
	EXPECT(list.Note(Met("Player 2", start, 1), start));
	EXPECT(list.Note(Met("Player 3", start + 100, 1), start + 100));
	// On the last second of its week an entry is still shown; on the next it is gone.
	EXPECT(list.Fresh(start + week - 1).size() == 2);
	EXPECT(list.Fresh(start + week).size() == 1 && list.Fresh(start + week)[0].name == "Player 3");
	EXPECT(list.Fresh(start + week + 100).empty());
	// What is past its week goes with the next one noted.
	EXPECT(list.Note(Met("Player 4", start + week + 100, 2), start + week + 100));
	EXPECT(list.Entries().size() == 1 && list.Entries()[0].name == "Player 4");
	EXPECT(!list.Prune(start + week + 100) && list.Prune(start + 3 * week) && list.Entries().empty());
	// A time ahead of this PC's clock is new, not old.
	EXPECT(list.Note(Met("Player 5", start + 5000, 1), start) && list.Fresh(start).size() == 1);
}

// What was saved comes back as far as it is good: malformed, expired and repeated names are dropped one by one.
void RecentOpponentsLoadTolerantly() {
	const std::uint64_t now = RecentLifeSeconds * 3;
	std::vector<Recent> stored = {Met("Player 2", now - 10, 9), Met("", now - 20, 9), Met("Player 3", now - RecentLifeSeconds - 1, 9),
		Met("Player 2", now - 30, 9), Met("Player 4", now - 40, 9, "bad token"), Met("Player 5", now - 50, 9)};
	RecentList list;
	list.Load(stored, now);
	CHECK(list.Entries().size() == 2);
	EXPECT(list.Entries()[0].name == "Player 2" && list.Entries()[0].at == now - 10 && list.Entries()[1].name == "Player 5");
	// A visit is only this run's.
	EXPECT(list.Entries()[0].visit == 0);
	std::vector<Recent> many;
	for (std::uint64_t i = 0; i < 30; ++i) many.push_back(Met("Player " + std::to_string(i), now - i, 0));
	list.Load(many, now);
	EXPECT(list.Entries().size() == MaxRecent && list.Entries().front().name == "Player 0");
}

Bundle Sample() {
	Bundle bundle;
	bundle.name = "Player 1";
	bundle.country = CountryCode::Parsed("DE");
	bundle.main = 3;
	bundle.worn.title = "matches";
	bundle.pins = {"wins", "rival"};
	bundle.style.device = Device::Stick;
	bundle.style.link = Link::Wired;
	bundle.style.region = "euc1";
	bundle.style.looking = LookingLongSets | LookingTeach;
	return bundle;
}

// The card goes when it differs from the last one saved for this service and Ember ID, and at most once a minute.
void TheCardIsSavedOnlyOnAChange() {
	Saver saver;
	const auto bundle = Sample();
	// At start nothing is saved: it goes; and not twice while it is in flight.
	EXPECT(saver.Due("brg_1", "emb_1", bundle, 0));
	saver.Sent("brg_1", "emb_1", bundle, 0);
	EXPECT(saver.InFlight() && !saver.Due("brg_1", "emb_1", bundle, 10 * Saver::MinGapMs));
	saver.Finish("");
	EXPECT(!saver.InFlight());
	// The same card is not sent again, however long it is.
	EXPECT(!saver.Due("brg_1", "emb_1", bundle, 0) && !saver.Due("brg_1", "emb_1", bundle, 1000 * Saver::MinGapMs));
	// A change goes after the minute, not before.
	auto changed = bundle;
	changed.style.looking = LookingRanked;
	EXPECT(!saver.Due("brg_1", "emb_1", changed, 1) && !saver.Due("brg_1", "emb_1", changed, Saver::MinGapMs - 1));
	EXPECT(saver.Due("brg_1", "emb_1", changed, Saver::MinGapMs));
	// Every part of the bundle is a change.
	for (int part = 0; part < 6; ++part) {
		auto other = bundle;
		if (part == 0) other.name = "Player 2";
		if (part == 1) other.country = CountryCode();
		if (part == 2) other.main = 4;
		if (part == 3) other.worn.icon = "streak";
		if (part == 4) other.pins.pop_back();
		if (part == 5) other.style.region.clear();
		EXPECT(other != bundle && saver.Due("brg_1", "emb_1", other, Saver::MinGapMs));
	}
	// Another service or Ember ID is a new card to save, at once.
	EXPECT(saver.Due("brg_2", "emb_1", bundle, 1) && saver.Due("brg_1", "emb_2", bundle, 1));
	EXPECT(!saver.Due("", "emb_1", changed, 1000 * Saver::MinGapMs) && !saver.Due("brg_1", "", changed, 1000 * Saver::MinGapMs));
}

void AFailedSaveIsTriedAgain() {
	const auto bundle = Sample();
	Saver saver;
	saver.Sent("brg_1", "emb_1", bundle, 5);
	saver.Finish("unavailable");
	// Not saved: the same card is due again after the minute.
	EXPECT(!saver.Due("brg_1", "emb_1", bundle, 5 + Saver::MinGapMs - 1) && saver.Due("brg_1", "emb_1", bundle, 5 + Saver::MinGapMs));
	// A refusal that will not change soon waits ten minutes.
	for (const char* code : {"profiles_unavailable", "no_discord_account", "rate_limited"}) {
		Saver refused;
		refused.Sent("brg_1", "emb_1", bundle, 0);
		refused.Finish(code);
		EXPECT(!refused.Due("brg_1", "emb_1", bundle, Saver::MinGapMs) && !refused.Due("brg_1", "emb_1", bundle, Saver::RefusedGapMs - 1));
		EXPECT(refused.Due("brg_1", "emb_1", bundle, Saver::RefusedGapMs));
		// A change does not shorten the wait.
		auto changed = bundle;
		changed.main = 9;
		EXPECT(!refused.Due("brg_1", "emb_1", changed, Saver::MinGapMs));
	}
	// An answer nobody waits for changes nothing.
	Saver idle;
	idle.Finish("");
	EXPECT(idle.Due("brg_1", "emb_1", bundle, 0));
	// A new service forgets what was saved for the one before.
	Saver moved;
	moved.Sent("brg_1", "emb_1", bundle, 0);
	moved.Finish("");
	moved.Sent("brg_2", "emb_1", bundle, 1);
	moved.Finish("rate_limited");
	EXPECT(!moved.Due("brg_2", "emb_1", bundle, 2) && moved.Due("brg_1", "emb_1", bundle, 3));
}

PlayerPreferences Prefs() {
	PlayerPreferences preferences;
	preferences.displayName = "Player 1";
	preferences.mainFighter = 3;
	preferences.wornTitle = "matches";
	return preferences;
}

// What the card says of how the player plays: Automatic or the chosen device, the measured link and the region only
// while shown, and what the player is looking for.
void TheStyleComesFromPreferencesAndMeasures() {
	auto preferences = Prefs();
	auto style = OwnStyle(preferences, Device::Stick, NetworkLink::Wired, "euc1");
	EXPECT(style.device == Device::Stick && style.link == Link::Wired && style.region == "euc1" && style.looking == 0);
	preferences.cardDevice = 3;
	preferences.cardLooking = LookingCasuals | LookingNew;
	style = OwnStyle(preferences, Device::Stick, NetworkLink::Wireless, "usw1");
	EXPECT(style.device == Device::Leverless && style.link == Link::WiFi && style.region == "usw1" && style.looking == 17);
	preferences.cardDevice = 5;
	preferences.cardShowLink = false;
	preferences.cardShowRegion = false;
	style = OwnStyle(preferences, Device::Stick, NetworkLink::Wired, "euc1");
	EXPECT(style.device == Device::None && style.link == Link::None && style.region.empty() && style.looking == 17);
	// Not measured, or no relay region known: not shown, whatever the player allows.
	preferences = Prefs();
	style = OwnStyle(preferences, Device::None, NetworkLink::Unknown, "other");
	EXPECT(!style.Any());
	style = OwnStyle(preferences, Device::None, NetworkLink::Unknown, "");
	EXPECT(!style.Any());
}

void TheBundleHoldsWhatTheCatalogKnows() {
	auto preferences = Prefs();
	preferences.wornIcon = "no-such-award";
	preferences.cardPins = {"wins", "no-such-award", "rival", "wins", "cast", "streak", "friends"};
	const auto bundle = BundleOf(preferences, CountryCode::Parsed("DE"), Style());
	EXPECT(bundle.name == "Player 1" && bundle.country.ToString() == "DE" && bundle.main == 3);
	EXPECT(bundle.worn.title == "matches" && bundle.worn.icon.empty());
	// Only families the catalog has, each once, at most four.
	EXPECT((bundle.pins == std::vector<std::string>{"wins", "rival", "cast", "streak"}));
	// A hidden flag is none, and a picked one is that.
	preferences.country = CountryPreference::Hidden();
	EXPECT(!BundleOf(preferences, CountryCode::Parsed("DE"), Style()).country.Present());
	preferences.country = CountryPreference::Of(CountryCode::Parsed("FR"));
	EXPECT(BundleOf(preferences, CountryCode::Parsed("DE"), Style()).country.ToString() == "FR");
	// A name the bridge would refuse is listed as it lists it.
	preferences.displayName = "  Player 1  ";
	EXPECT(BundleOf(preferences, CountryCode(), Style()).name == "Player 1");
}

// The helper requests, byte for byte.
void TheRequestsHaveTheShapesTheHelperReads() {
	const json save = {{"op", "card_save"}, {"bridge_id", "brg_1"}, {"card", {
		{"listing", {{"name", "Player 1"}, {"country", "DE"}}}, {"main", 3}, {"worn", {{"title", "matches"}}},
		{"pins", json::array({"wins", "rival"})}, {"style", {{"device", 2}, {"link", 1}, {"region", "euc1"}, {"looking", 34}}}}}};
	EXPECT(CardSaveRequest("brg_1", Sample()) == save);
	// Nothing worn, nothing pinned, no flag, no region, no main: empty members, not absent ones the bridge would keep.
	Bundle bare;
	bare.name = "Player 1";
	const json emptied = {{"op", "card_save"}, {"bridge_id", "brg_1"}, {"card", {
		{"listing", {{"name", "Player 1"}}}, {"worn", json::object()}, {"pins", json::array()}, {"style", {{"device", 0}, {"link", 0}, {"looking", 0}}}}}};
	EXPECT(CardSaveRequest("brg_1", bare) == emptied);
	EXPECT(SearchRequest("brg_1", "Play", false) == json({{"op", "cards_search"}, {"bridge_id", "brg_1"}, {"name", "Play"}}));
	EXPECT(SearchRequest("brg_1", "4F2A-91CX", true) == json({{"op", "cards_search"}, {"bridge_id", "brg_1"}, {"code", "4F2A-91CX"}}));
	const std::pair<Key::Kind, const char*> selectors[] = {{Key::Kind::Card, "card"}, {Key::Kind::Friend, "friend"}, {Key::Kind::Request, "request"}, {Key::Kind::Code, "code"}};
	for (const auto& selector : selectors) {
		json one = json::object();
		one[selector.second] = "value-1";
		EXPECT(LookupRequest("brg_1", selector.first, "value-1") == json({{"op", "cards_lookup"}, {"bridge_id", "brg_1"}, {"cards", json::array({one})}}));
	}
	EXPECT(LookupRequest("brg_1", Key::Kind::Name, "Player 2").is_null() && LookupRequest("brg_1", Key::Kind::None, "x").is_null());
}

// The card preferences load as saved, or as their defaults, one value at a time.
void TheCardPreferencesRoundTrip() {
	PlayerPreferences saved;
	EXPECT(saved.cardDevice == 0 && saved.cardShowLink && saved.cardShowRegion && saved.cardLooking == 0 && saved.cardPins.empty() && saved.Valid());
	saved.cardDevice = 5;
	saved.cardShowLink = false;
	saved.cardShowRegion = false;
	saved.cardLooking = LookingCasuals | LookingTeach;
	saved.cardPins = {"wins", "cast"};
	CHECK(saved.Valid());
	json document;
	netplay::WriteCardPreferences(saved, document);
	// Through the text a file holds.
	PlayerPreferences loaded;
	netplay::ReadCardPreferences(json::parse(document.dump()), loaded);
	EXPECT(loaded.cardDevice == 5 && !loaded.cardShowLink && !loaded.cardShowRegion && loaded.cardLooking == 33 && loaded.cardPins == saved.cardPins);
	// A profile from before the card has none of them.
	PlayerPreferences old;
	old.cardDevice = 2;
	old.cardLooking = 1;
	netplay::ReadCardPreferences(json::object(), old);
	EXPECT(old.cardDevice == 0 && old.cardShowLink && old.cardShowRegion && old.cardLooking == 0 && old.cardPins.empty());
	netplay::ReadCardPreferences(json::array(), old);
	EXPECT(old.cardDevice == 0 && old.Valid());
}

void BadCardPreferencesAreDropped() {
	json odd = {{"cardDevice", 9}, {"cardShowLink", 1}, {"cardShowRegion", false}, {"cardLooking", 127},
		{"cardPins", json::array({"wins", "Bad Id", 4, "wins", "cast", "rival", "streak", "mastery"})}};
	PlayerPreferences loaded;
	netplay::ReadCardPreferences(json::parse(odd.dump()), loaded);
	// Out of range and of another type are the defaults; what is not known is dropped, the rest stands.
	EXPECT(loaded.cardDevice == 0 && loaded.cardShowLink && !loaded.cardShowRegion);
	EXPECT(loaded.cardLooking == 3);
	EXPECT((loaded.cardPins == std::vector<std::string>{"wins", "cast", "rival", "streak"}));
	EXPECT(loaded.Valid());
	for (const json& bad : {json{{"cardDevice", -1}}, json{{"cardDevice", "2"}}, json{{"cardDevice", 6}}, json{{"cardDevice", 2.5}}}) {
		PlayerPreferences one;
		netplay::ReadCardPreferences(json::parse(bad.dump()), one);
		EXPECT(one.cardDevice == 0);
	}
	PlayerPreferences strings;
	netplay::ReadCardPreferences(json{{"cardLooking", "3"}, {"cardPins", "wins"}}, strings);
	EXPECT(strings.cardLooking == 0 && strings.cardPins.empty());
}

// What preferences may hold: the device 0 to 5, at most two looking-for bits, at most four distinct family ids.
void PreferencesRefuseWhatTheCardCannotHold() {
	const auto valid = [](auto change) { PlayerPreferences preferences; change(preferences); return preferences.Valid(); };
	EXPECT(valid([](PlayerPreferences& p) { p.cardDevice = 5; }) && !valid([](PlayerPreferences& p) { p.cardDevice = 6; }) && !valid([](PlayerPreferences& p) { p.cardDevice = -1; }));
	EXPECT(valid([](PlayerPreferences& p) { p.cardLooking = 34; }) && !valid([](PlayerPreferences& p) { p.cardLooking = 7; }) && !valid([](PlayerPreferences& p) { p.cardLooking = 64; }));
	EXPECT(valid([](PlayerPreferences& p) { p.cardPins = {"wins", "cast", "rival", "streak"}; }));
	EXPECT(!valid([](PlayerPreferences& p) { p.cardPins = {"wins", "cast", "rival", "streak", "mastery"}; }));
	EXPECT(!valid([](PlayerPreferences& p) { p.cardPins = {"wins", "wins"}; }));
	EXPECT(!valid([](PlayerPreferences& p) { p.cardPins = {"Wins"}; }) && !valid([](PlayerPreferences& p) { p.cardPins = {""}; }));
}
}

int main() {
	LookingBitsAreBounded();
	AutomaticDeviceFollowsTheController();
	NamesAreFoundInAnyCase();
	RecentOpponentsAreBounded();
	RecentOpponentsLastAWeek();
	RecentOpponentsLoadTolerantly();
	TheCardIsSavedOnlyOnAChange();
	AFailedSaveIsTriedAgain();
	TheStyleComesFromPreferencesAndMeasures();
	TheBundleHoldsWhatTheCatalogKnows();
	TheRequestsHaveTheShapesTheHelperReads();
	TheCardPreferencesRoundTrip();
	BadCardPreferencesAreDropped();
	PreferencesRefuseWhatTheCardCannotHold();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("profiles tests passed\n");
	return TEST_EXIT_CODE();
}
