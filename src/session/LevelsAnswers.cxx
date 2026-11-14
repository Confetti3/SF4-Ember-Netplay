// The helper's answers to the points requests (points_me, cards_lookup,
// cards_search), as TournamentAnswers.hxx declares them
// (docs/design/FIGHTER_LEVELS.md and PROFILE.md, "Wire contracts").
#include "TournamentAnswers.hxx"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "../common/SkillBadgeJson.hxx"
#include "AnswerReading.hxx"

namespace sf4e { namespace session {
using namespace reading;

namespace {
// An object, for the answers that ignore members they do not know: a field a
// later bridge adds to a points or card answer must not break a build already
// out, so only the keys read are checked.
const json& Object(const json& value) {
	if (!value.is_object()) throw std::invalid_argument("object");
	return value;
}

// A counter of any size, as the points answers carry them.
std::uint64_t Unsigned(const json& object, const char* key) {
	return Seconds(object, key);
}

// The fighter id of an entry, in the roster.
std::size_t ReadFighter(const json& object) {
	return Small(object, "fighter", selection::FighterCount - 1);
}

// A tier as sent: an unsigned number, and one past the tiers this build draws
// (a later bridge's table) is the last it knows. `beyond` says it was past.
std::uint8_t ReadTier(const json& object, bool* beyond = nullptr) {
	const auto tier = Unsigned(object, "tier");
	const auto last = static_cast<std::uint64_t>(netplay::levels::TierCount - 1);
	if (beyond) *beyond = tier > last;
	return static_cast<std::uint8_t>((std::min)(tier, last));
}

// A number the bridge may leave out: absent is 0, and one that is there must
// be an unsigned number.
std::uint64_t OptionalUnsigned(const json& object, const char* key) {
	return object.find(key) == object.end() ? 0 : Unsigned(object, key);
}

// One count of points, a fighter's or the account's. Only its shape is checked:
// how points, games, wins, tiers and their thresholds relate is the bridge's
// rule (a game is a point and a win against the same class or a higher one 1
// more, today), and a later rule must not cost a player their points. `since`
// is absent until a game is counted, and `next` at the last tier.
netplay::levels::Level DecodeLevel(const json& entry) {
	netplay::levels::Level level;
	Object(entry);
	level.points = Unsigned(entry, "points");
	level.games = Unsigned(entry, "games");
	// The screens show wins as a share of games.
	level.wins = (std::min)(Unsigned(entry, "wins"), level.games);
	bool beyond = false;
	level.tier = ReadTier(entry, &beyond);
	level.floor = Unsigned(entry, "floor");
	level.since = OptionalUnsigned(entry, "since");
	level.next = OptionalUnsigned(entry, "next");
	// At the last tier this build draws there is none further to name.
	if (beyond || level.tier == netplay::levels::TierCount - 1) level.next = 0;
	return level;
}

// A count the bridge may or may not send, up to `maximum`: absent is 0 (a
// bridge from before it), one that is there must be an unsigned number, and
// one past `maximum` is `maximum`.
std::uint64_t OptionalCount(const json& object, const char* key, std::uint64_t maximum) {
	return (std::min)(OptionalUnsigned(object, key), maximum);
}

// The counters behind the awards, as the wire says them (ember_protocol::awards::AwardCounts) in an own
// answer's account and in a card's `awards`: each absent from a bridge that does not send it, so 0, and an
// unsigned number when it is there. How they relate is the bridge's to say.
netplay::awards::Counters DecodeAwardCounts(const json& object) {
	netplay::awards::Counters counts;
	counts.games = OptionalCount(object, "games", UINT64_MAX);
	// The screens show wins as a share of games.
	counts.wins = (std::min)(OptionalCount(object, "wins", UINT64_MAX), counts.games);
	counts.bestStreak = static_cast<std::uint32_t>(OptionalCount(object, "best_streak", UINT32_MAX));
	counts.peakClass = static_cast<std::uint32_t>(OptionalCount(object, "peak_class", SkillBadge::Classes));
	counts.rival = static_cast<std::uint32_t>(OptionalCount(object, "rival", UINT32_MAX));
	counts.opponents = static_cast<std::uint32_t>(OptionalCount(object, "opponents", UINT32_MAX));
	return counts;
}

netplay::levels::Relation DecodeRelation(const json& card) {
	using netplay::levels::Relation;
	const auto relation = Text(card, "relation", 16);
	if (relation == "none") return Relation::None;
	if (relation == "friend") return Relation::Friend;
	if (relation == "sent") return Relation::Sent;
	if (relation == "received") return Relation::Received;
	if (relation == "blocked") return Relation::Blocked;
	// A relation a later bridge adds reads as none, not as a failed answer.
	return Relation::None;
}

// What a bridge offering `profiles` adds to a card (docs/design/PROFILE.md, "Wire contracts"). Each member stands
// alone: one that is missing, of the wrong type or out of range is left as the card's default, and a number of an
// enum this build does not know reads as "not shown", so a later bridge never costs a player the card.
std::string ReadText(const json& object, const char* key, std::size_t maximum) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) return {};
	auto text = SingleLine(found->get<std::string>());
	return text.size() <= maximum ? text : std::string();
}

std::uint64_t ReadNumber(const json& object, const char* key) {
	const auto found = object.find(key);
	return found != object.end() && found->is_number_unsigned() ? found->get<std::uint64_t>() : 0;
}

// "fr_" and the uuid, as friends ids are.
bool IsFriendId(const std::string& id) { return id.size() == 39 && id.compare(0, 3, "fr_") == 0; }

CountryCode ReadCountry(const json& object) {
	const auto found = object.find("country");
	return found != object.end() && found->is_string() ? CountryCode::Parsed(found->get<std::string>()) : CountryCode();
}

void DecodeProfileFields(const json& object, netplay::levels::Card& card) {
	namespace profiles = netplay::profiles;
	if (const auto name = ReadText(object, "name", profiles::MaxNameBytes); profiles::NameShaped(name)) card.name = name;
	card.country = ReadCountry(object);
	card.badge = ReadSkillBadge(object);
	if (const auto fighter = object.find("main"); fighter != object.end() && fighter->is_number_unsigned() &&
		fighter->get<std::uint64_t>() < static_cast<std::uint64_t>(selection::FighterCount)) {
		card.hasMain = true;
		card.main = static_cast<std::uint8_t>(fighter->get<std::uint64_t>());
	}
	if (const auto since = ReadNumber(object, "since")) { card.hasSince = true; card.since = since; }
	if (const auto worn = object.find("worn"); worn != object.end() && worn->is_object()) {
		for (const char* key : {"title", "icon"}) {
			const auto id = ReadText(*worn, key, profiles::MaxFamilyId);
			if (profiles::FamilyIdShaped(id)) (key[0] == 't' ? card.worn.title : card.worn.icon) = id;
		}
	}
	if (const auto pins = object.find("pins"); pins != object.end() && pins->is_array()) {
		for (const auto& pin : *pins) {
			if (card.pins.size() == profiles::MaxPins) break;
			if (!pin.is_string()) continue;
			const auto id = pin.get<std::string>();
			if (profiles::FamilyIdShaped(id) && std::find(card.pins.begin(), card.pins.end(), id) == card.pins.end()) card.pins.push_back(id);
		}
	}
	if (const auto style = object.find("style"); style != object.end() && style->is_object()) {
		card.hasStyle = true;
		card.style.device = profiles::DeviceFromWire(ReadNumber(*style, "device"));
		card.style.link = profiles::LinkFromWire(ReadNumber(*style, "link"));
		const auto region = ReadText(*style, "region", profiles::MaxRegionBytes);
		if (profiles::RegionShaped(region)) card.style.region = region;
		card.style.looking = profiles::LookingFromWire(ReadNumber(*style, "looking"));
	}
	if (const auto versus = object.find("versus"); versus != object.end() && versus->is_object()) {
		const auto won = ReadNumber(*versus, "won"), lost = ReadNumber(*versus, "lost");
		if (won || lost) {
			card.hasVersus = true;
			card.versus.won = won;
			card.versus.lost = lost;
			card.versus.lastAt = ReadNumber(*versus, "last_at");
		}
	}
	if (card.relation == netplay::levels::Relation::Friend)
		if (const auto id = ReadText(object, "friend", 39); IsFriendId(id)) card.friendId = id;
}

// The most played opponent of the own points answer. An object that is not a whole one (no card token, no name, no
// games between the two) is left out on its own: the answer's points stand.
bool DecodeMostPlayed(const json& data, netplay::levels::MostPlayed& out) {
	const auto found = data.find("rival");
	if (found == data.end() || !found->is_object()) return false;
	namespace profiles = netplay::profiles;
	netplay::levels::MostPlayed rival;
	rival.card = ReadText(*found, "card", 128);
	rival.name = ReadText(*found, "name", profiles::MaxNameBytes);
	if (!CardToken::Parsed(rival.card).Present() || !profiles::NameShaped(rival.name)) return false;
	const auto won = found->find("won"), lost = found->find("lost");
	if (won == found->end() || lost == found->end() || !won->is_number_unsigned() || !lost->is_number_unsigned()) return false;
	rival.won = won->get<std::uint64_t>();
	rival.lost = lost->get<std::uint64_t>();
	if (rival.won > UINT64_MAX - rival.lost || rival.won + rival.lost == 0) return false;
	rival.country = ReadCountry(*found);
	rival.badge = ReadSkillBadge(*found);
	out = std::move(rival);
	return true;
}

netplay::levels::Card DecodeCard(const json& value) {
	netplay::levels::Card card;
	const auto& object = Object(value);
	card.relation = DecodeRelation(object);
	const auto& account = Object(object.at("account"));
	card.account.points = Unsigned(account, "points");
	card.account.tier = ReadTier(account);
	// The counters behind their awards, all absent from an older bridge.
	if (const auto awards = object.find("awards"); awards != object.end()) card.awards = DecodeAwardCounts(Object(*awards));
	// A card with no `fighters` is of a player who has played none. A row that cannot be read, and a fighter
	// listed again, is left out; the card lists the rest in fighter order.
	if (const auto fighters = object.find("fighters"); fighters != object.end()) {
		if (!fighters->is_array() || fighters->size() > static_cast<std::size_t>(selection::FighterCount)) throw std::invalid_argument("fighters");
		for (const auto& row : *fighters) {
			netplay::levels::CardFighter fighter;
			try {
				Object(row);
				fighter.fighter = static_cast<std::uint8_t>(ReadFighter(row));
				fighter.points = Unsigned(row, "points");
				fighter.tier = ReadTier(row);
			} catch (const std::exception&) {
				continue;
			}
			const auto same = [&](const netplay::levels::CardFighter& one) { return one.fighter == fighter.fighter; };
			if (std::none_of(card.fighters.begin(), card.fighters.end(), same)) card.fighters.push_back(fighter);
		}
		std::sort(card.fighters.begin(), card.fighters.end(),
			[](const netplay::levels::CardFighter& a, const netplay::levels::CardFighter& b) { return a.fighter < b.fighter; });
	}
	DecodeProfileFields(object, card);
	return card;
}
}

std::optional<netplay::levels::Own> DecodeOwnLevels(const json& data) {
	try {
		netplay::levels::Own own;
		const auto tag = Text(Object(data), "points", 16);
		if (tag == "no_account") {
			own.kind = netplay::levels::Own::Kind::NoAccount;
			return own;
		}
		if (tag != "counted") return std::nullopt;
		own.kind = netplay::levels::Own::Kind::Counted;
		own.account = DecodeLevel(data.at("account"));
		own.awards = DecodeAwardCounts(data.at("account"));
		own.hasMostPlayed = DecodeMostPlayed(data, own.mostPlayed);
		const auto& rows = data.at("fighters");
		if (!rows.is_array() || rows.size() > static_cast<std::size_t>(selection::FighterCount)) return std::nullopt;
		// A fighter row that cannot be read, and a fighter listed again, is left out; the account's points stand.
		std::array<bool, selection::FighterCount> listed{};
		for (const auto& row : rows) {
			try {
				const auto fighter = ReadFighter(Object(row));
				auto level = DecodeLevel(row);
				if (listed[fighter]) continue;
				listed[fighter] = true;
				own.fighters[fighter] = level;
			} catch (const std::exception&) {}
		}
		return own;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<std::vector<std::optional<netplay::levels::Card>>> DecodeCards(const json& data) {
	try {
		const auto& rows = Object(data).at("cards");
		if (!rows.is_array() || rows.empty() || rows.size() > netplay::levels::MaxLookup) return std::nullopt;
		std::vector<std::optional<netplay::levels::Card>> cards;
		cards.reserve(rows.size());
		for (const auto& row : rows) {
			// One card that cannot be read is no card (its player shows as having none), never a failed answer.
			if (row.is_null()) { cards.emplace_back(); continue; }
			try {
				cards.emplace_back(DecodeCard(row));
			} catch (const std::exception&) {
				cards.emplace_back();
			}
		}
		return cards;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<netplay::profiles::SearchAnswer> DecodeSearch(const json& data) {
	try {
		namespace profiles = netplay::profiles;
		const auto& rows = Object(data).at("players");
		if (!rows.is_array() || rows.size() > profiles::MaxSearch) return std::nullopt;
		profiles::SearchAnswer answer;
		for (const auto& row : rows) {
			// A hit that cannot be read is left out, never a failed answer.
			if (!row.is_object()) continue;
			profiles::Hit hit;
			hit.card = ReadText(row, "card", 128);
			hit.name = ReadText(row, "name", profiles::MaxNameBytes);
			if (!CardToken::Parsed(hit.card).Present() || !profiles::NameShaped(hit.name)) continue;
			hit.country = ReadCountry(row);
			hit.badge = ReadSkillBadge(row);
			if (const auto fighter = row.find("main"); fighter != row.end() && fighter->is_number_unsigned() &&
				fighter->get<std::uint64_t>() < static_cast<std::uint64_t>(selection::FighterCount)) {
				hit.hasMain = true;
				hit.main = static_cast<std::uint8_t>(fighter->get<std::uint64_t>());
			}
			hit.since = ReadNumber(row, "since");
			answer.players.push_back(std::move(hit));
		}
		if (const auto more = data.find("more"); more != data.end() && more->is_boolean()) answer.more = more->get<bool>();
		return answer;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

} }
