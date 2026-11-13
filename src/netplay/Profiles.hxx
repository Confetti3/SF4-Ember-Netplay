#pragma once
// Another player's profile and the player's own card, as the interface and the
// runtime pass them around (docs/design/PROFILE.md, "Data and wire"): the
// shapes the bridge's cards, searches and saves carry, how a profile was
// opened, the players this PC met lately, and the small rules on them. Plain
// types only, and C++14 (the player preferences include this): the look-up and
// search state is ProfileRequests.hxx, the save is CardBundle.hxx, the wire
// forms are ProfileWire.hxx, and the decoders are session/TournamentAnswers.hxx.
//
// Every enum travels as a small number. A number this build does not know
// reads as "not shown" (the none of the enum), never as a refusal, so a later
// bridge can add a device or a looking-for without locking an older game out.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../common/ButtonPrompts.hxx"
#include "../common/Country.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/PadKind.hxx"
#include "../common/SkillBadge.hxx"
#include "../common/SteamId.hxx"

namespace sf4e { namespace netplay { namespace profiles {

// The helper's code for a service that does not offer profiles (its
// capabilities lack `profiles`, or it answers 404).
constexpr const char* Unavailable = "profiles_unavailable";

// The bridge's limits (ember_protocol::cards and ::awards). A card shows at most MaxPins
// award families; a search lists at most MaxSearch players.
constexpr std::size_t MaxPins = 4, MaxSearch = 20, MaxFamilyId = 32, MaxNameBytes = 32, MaxRegionBytes = 8, MaxRoomBytes = 64;
// A player is looking for at most this many things at once.
constexpr std::size_t MaxLooking = 2;

// What a player plays on, as their card says. None: not shown.
enum class Device : std::uint8_t { None, Pad, Stick, Leverless, Keyboard };
// How their PC reaches the internet. None: not shown.
enum class Link : std::uint8_t { None, Wired, WiFi };

inline Device DeviceFromWire(std::uint64_t value) { return value >= 1 && value <= 4 ? static_cast<Device>(value) : Device::None; }
inline Link LinkFromWire(std::uint64_t value) { return value >= 1 && value <= 2 ? static_cast<Link>(value) : Link::None; }

// What a player is looking for, as bits (the wire's `looking`), in the order a
// screen lists them. At most MaxLooking are set.
constexpr std::size_t LookingKinds = 6;
constexpr std::uint8_t LookingCasuals = 1, LookingLongSets = 2, LookingRanked = 4, LookingPractice = 8, LookingNew = 16, LookingTeach = 32;
constexpr std::uint8_t LookingBitsAll = 63;
constexpr std::uint8_t LookingBit[LookingKinds] = {LookingCasuals, LookingLongSets, LookingRanked, LookingPractice, LookingNew, LookingTeach};

inline std::size_t LookingCount(std::uint64_t bits) {
	std::size_t count = 0;
	for (const auto bit : LookingBit) count += (bits & bit) != 0;
	return count;
}

// A card's `looking`: bits this build does not know are dropped, and more than
// MaxLooking set read as none (the bridge never sends them).
inline std::uint8_t LookingFromWire(std::uint64_t bits) {
	const std::uint64_t known = bits & LookingBitsAll;
	return LookingCount(known) <= MaxLooking ? static_cast<std::uint8_t>(known) : static_cast<std::uint8_t>(0);
}

// A saved preference's `looking`: unknown bits are dropped, and the lowest two
// of more than MaxLooking kept.
inline std::uint8_t LookingFromSaved(std::uint64_t bits) {
	std::uint8_t kept = 0;
	std::size_t count = 0;
	for (const auto bit : LookingBit)
		if ((bits & bit) && count < MaxLooking) { kept = static_cast<std::uint8_t>(kept | bit); ++count; }
	return kept;
}

// The device Automatic shows: the keyboard is Keyboard, a connected pad that
// only Ember's pad setup made usable (Brand::Stick) is Stick, any other
// connected pad is Pad, and no controller known is none. Leverless cannot be
// told from a pad or a stick, so it is only ever chosen. `type` is the
// gameplay device's input::PadKind, `connected` and `brand` as ShellView's
// inputDevice and gameplayBrand have them.
inline Device AutomaticDevice(int type, bool connected, gamepads::PromptBrand brand) {
	if (type == input::PadKeyboard) return Device::Keyboard;
	if ((type == input::PadXInput || type == input::PadDirectInput) && connected)
		return brand.brand == gamepads::Brand::Stick ? Device::Stick : Device::Pad;
	return Device::None;
}

// How a player plays: the chips of a card. Whatever the player hides, or the
// game does not know, is none, empty or 0; a card with all four empty draws no
// row of chips.
struct Style {
	Device device = Device::None;
	Link link = Link::None;
	// A relay region code ("use1", "usw1", "euc1", "aps1" as the bridge knows
	// them; any other shaped code is kept for the screen to call another
	// region), empty for none.
	std::string region;
	std::uint8_t looking = 0;
	bool Any() const { return device != Device::None || link != Link::None || !region.empty() || looking != 0; }
	bool operator==(const Style& other) const {
		return device == other.device && link == other.link && region == other.region && looking == other.looking;
	}
	bool operator!=(const Style& other) const { return !(*this == other); }
};

// The award families (netplay/Awards.hxx ids) a player wears as title and as
// icon; empty for none. A claim: a screen draws a family only at the level the
// card's counters earn (awards::WornOnCard).
struct Worn {
	std::string title, icon;
	bool Any() const { return !title.empty() || !icon.empty(); }
	bool operator==(const Worn& other) const { return title == other.title && icon == other.icon; }
	bool operator!=(const Worn& other) const { return !(*this == other); }
};

// The sets and games between the viewer and the player of a card, from the
// viewer's side, and when they last played (unix seconds, 0 when never).
struct Versus {
	std::uint64_t won = 0, lost = 0, lastAt = 0;
};

// A family id's shape: 1 to MaxFamilyId of a to z, 0 to 9 and hyphens. The one
// statement of the rule in the game (the saved preferences, the card the game
// saves and the card it reads all ask it), and ember_protocol::awards::is_family_id
// states it for the bridge, with the same limit. The shape only: the catalog
// (awards::Find) says which ids are awards.
inline bool FamilyIdShaped(const std::string& id) {
	return !id.empty() && id.size() <= MaxFamilyId && std::all_of(id.begin(), id.end(),
		[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// A relay region code's shape: 1 to MaxRegionBytes of a to z and 0 to 9.
inline bool RegionShaped(const std::string& code) {
	return !code.empty() && code.size() <= MaxRegionBytes && std::all_of(code.begin(), code.end(),
		[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

inline bool PlainText(const std::string& text, std::size_t most) {
	return text.size() <= most && std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 32 || c == 127; });
}

// A name as a card or a hit carries it: 1 to MaxNameBytes bytes of one line.
inline bool NameShaped(const std::string& name) { return !name.empty() && PlainText(name, MaxNameBytes); }

// Names equal in ASCII letters of either case, as the bridge's name column compares them.
inline bool SameNameAnyCase(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
		if (lower(a[i]) != lower(b[i])) return false;
	}
	return true;
}

// How a profile was opened: by what the opener held. Card: a card token
// ("c1.", a room member's); Friend: a friend id ("fr_"); Request: the id of a
// friend request received ("rq_"); Code: a friend code as typed; Name: the
// exact name a list showed (a ranking, an archived set), with `country` to
// choose by when more than one player goes by it.
struct Key {
	enum class Kind : std::uint8_t { None, Card, Friend, Request, Code, Name } kind = Kind::None;
	std::string value;
	CountryCode country;
	static Key OfCard(const std::string& token) { Key key; key.kind = Kind::Card; key.value = token; return key; }
	static Key OfFriend(const std::string& id) { Key key; key.kind = Kind::Friend; key.value = id; return key; }
	static Key OfRequest(const std::string& id) { Key key; key.kind = Kind::Request; key.value = id; return key; }
	static Key OfCode(const std::string& code) { Key key; key.kind = Kind::Code; key.value = code; return key; }
	static Key OfName(const std::string& name, const CountryCode& country = CountryCode()) {
		Key key; key.kind = Kind::Name; key.value = name; key.country = country; return key;
	}
	bool Present() const { return kind != Kind::None && !value.empty(); }
	bool operator==(const Key& other) const { return kind == other.kind && value == other.value && country == other.country; }
	bool operator!=(const Key& other) const { return !(*this == other); }
};

// What the opener of a profile already knew of the player, so the page draws
// at once and fills in as the card arrives. Each part is absent (empty, none,
// -1) when the opener did not know it.
struct Known {
	std::string name;
	CountryCode country;
	SkillBadge badge;
	// The fighter the opener showed them with: a room member's pick, a friend's main. -1: none.
	int fighter = -1;
	// A room member who shares their Steam profile.
	SteamId steam;
};

// One player a search found.
struct Hit {
	// A fresh card token ("c1.", good for 7 days): what a look-up and a friend request name them by.
	std::string card, name;
	CountryCode country;
	bool hasMain = false;
	std::uint8_t main = 0;
	SkillBadge badge;
	// The first second of the month they came to Ember (unix seconds); 0 when not said.
	std::uint64_t since = 0;
};

// A search's answer: at most MaxSearch players, names equal to the text first,
// then names that begin with it, most recently active first; `more` when more matched.
struct SearchAnswer {
	std::vector<Hit> players;
	bool more = false;
};

// The hits whose name is `name` in any case, in the order given.
inline std::vector<Hit> NamedHits(const std::vector<Hit>& hits, const std::string& name) {
	std::vector<Hit> named;
	for (const auto& hit : hits) if (SameNameAnyCase(hit.name, name)) named.push_back(hit);
	return named;
}

// The index in `named` (NamedHits) of the one player a name opens: the only
// one, or the only one of `country` among several. -1 when there is none, or
// still more than one: the page then asks which.
inline int ChooseHit(const std::vector<Hit>& named, const CountryCode& country) {
	if (named.size() == 1) return 0;
	if (named.empty() || !country.Present()) return -1;
	int found = -1;
	for (std::size_t i = 0; i < named.size(); ++i) {
		if (named[i].country != country) continue;
		if (found >= 0) return -1;
		found = static_cast<int>(i);
	}
	return found;
}

// A player this PC fought in a public room, kept on this PC only so the moment
// around a rematch lasts (Find a player). Never sent anywhere.
struct Recent {
	// The opponent's card token, as their ticket carried it (good for 7 days), their name, flag and fighter as the room showed them.
	std::string card, name;
	CountryCode country;
	// -1: not known.
	int fighter = -1;
	// The room's name.
	std::string room;
	// Unix seconds of the game's end.
	std::uint64_t at = 0;
	// The visit to the room this PC played them in (its epoch): games against one name in one visit are one entry.
	// Runtime only, never saved; 0 for an entry read from disk.
	std::uint64_t visit = 0;
};

// The most players kept, and how long each is kept: the life of the card token.
constexpr std::size_t MaxRecent = 20;
constexpr std::uint64_t RecentLifeSeconds = 7ull * 24 * 60 * 60;

inline bool ValidRecent(const Recent& entry) {
	return CardToken::Parsed(entry.card).Present() && NameShaped(entry.name) && entry.fighter >= -1 && entry.fighter < selection::FighterCount &&
		PlainText(entry.room, MaxRoomBytes) && entry.at != 0;
}

// The players met lately, newest first: at most MaxRecent, each kept RecentLifeSeconds, one per name and room visit.
class RecentList {
public:
	const std::vector<Recent>& Entries() const { return entries_; }

	// The entries not yet past their life at `nowSeconds`.
	std::vector<Recent> Fresh(std::uint64_t nowSeconds) const {
		std::vector<Recent> fresh;
		for (const auto& entry : entries_) if (!Expired(entry, nowSeconds)) fresh.push_back(entry);
		return fresh;
	}

	// A fought game's opponent. A second game against one name in the same visit
	// replaces the entry (newer time and token); the list is kept in bounds.
	// True when the list changed (so it is saved).
	bool Note(const Recent& entry, std::uint64_t nowSeconds) {
		if (!ValidRecent(entry)) return false;
		Prune(nowSeconds);
		for (auto found = entries_.begin(); found != entries_.end(); ++found)
			if (found->name == entry.name && found->visit == entry.visit) { entries_.erase(found); break; }
		entries_.insert(entries_.begin(), entry);
		Order();
		return true;
	}

	// Drops what is past its life; true when something went.
	bool Prune(std::uint64_t nowSeconds) {
		const auto before = entries_.size();
		entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Recent& entry) { return Expired(entry, nowSeconds); }), entries_.end());
		return entries_.size() != before;
	}

	// Takes what was saved: an entry that is malformed, expired or a second of one
	// name is dropped one by one (a name met in several visits is one entry once
	// the game has restarted).
	void Load(const std::vector<Recent>& stored, std::uint64_t nowSeconds) {
		entries_.clear();
		for (const auto& entry : stored) {
			if (!ValidRecent(entry) || Expired(entry, nowSeconds)) continue;
			Recent loaded = entry;
			loaded.visit = 0;
			if (std::none_of(entries_.begin(), entries_.end(), [&](const Recent& held) { return held.name == loaded.name; })) entries_.push_back(loaded);
		}
		Order();
	}

private:
	// A time ahead of this PC's clock counts as new.
	static bool Expired(const Recent& entry, std::uint64_t nowSeconds) {
		return nowSeconds > entry.at && nowSeconds - entry.at >= RecentLifeSeconds;
	}
	void Order() {
		std::stable_sort(entries_.begin(), entries_.end(), [](const Recent& a, const Recent& b) { return a.at > b.at; });
		if (entries_.size() > MaxRecent) entries_.resize(MaxRecent);
	}
	std::vector<Recent> entries_;
};

} } }
