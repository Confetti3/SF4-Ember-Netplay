#include "TournamentAnswers.hxx"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>

#include "../common/RoomRules.hxx"
#include "../common/SkillBadgeJson.hxx"

namespace sf4e { namespace session {
namespace {
using nlohmann::json;
using netplay::tournament::Binding;
using netplay::tournament::ClaimReply;
using netplay::tournament::PrepareReply;
using netplay::tournament::WatchReply;
using netplay::publicrooms::Room;

// A bounded string field, or an exception the decoder turns into "malformed".
std::string Text(const json& object, const char* key, std::size_t maximum) {
	const auto& value = object.at(key);
	if (!value.is_string()) throw std::invalid_argument(key);
	auto text = value.get<std::string>();
	if (text.size() > maximum) throw std::invalid_argument(key);
	return text;
}

std::string OptionalText(const json& object, const char* key, std::size_t maximum) {
	const auto found = object.find(key);
	if (found == object.end() || found->is_null()) return {};
	return Text(object, key, maximum);
}

// A canonical decimal counter, as the helper writes them.
std::uint64_t Counter(const json& object, const char* key) {
	const auto text = Text(object, key, 20);
	if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
		(text.size() > 1 && text.front() == '0')) throw std::invalid_argument(key);
	return std::stoull(text);
}

std::uint64_t Seconds(const json& object, const char* key) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned()) throw std::invalid_argument(key);
	return value.get<std::uint64_t>();
}

// A time the bridge may or may not send: absent or not an unsigned number is 0
// (unknown), so a newer or older bridge never costs the player a row.
std::uint64_t OptionalSeconds(const json& object, const char* key) {
	const auto found = object.find(key);
	return found != object.end() && found->is_number_unsigned() ? found->get<std::uint64_t>() : 0;
}

bool IsHex(const std::string& text, std::size_t length) {
	return text.size() == length && std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A bounded unsigned number, or an exception the decoder turns into "malformed".
unsigned Small(const json& object, const char* key, std::uint64_t maximum) {
	const auto& value = object.at(key);
	if (!value.is_number_unsigned() || value.get<std::uint64_t>() > maximum) throw std::invalid_argument(key);
	return static_cast<unsigned>(value.get<std::uint64_t>());
}

// A room name is another player's text: kept to one line.
std::string SingleLine(std::string text) {
	for (auto& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
	return text;
}

// Unicode White_Space the bridge's own trim removes, as it appears in UTF-8:
// the ASCII ones and U+00A0, U+3000.
bool IsPad(const std::string& text, std::size_t at, std::size_t& length) {
	const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
	if (byte(at) == ' ') { length = 1; return true; }
	if (at + 1 < text.size() && byte(at) == 0xC2 && byte(at + 1) == 0xA0) { length = 2; return true; }
	if (at + 2 < text.size() && byte(at) == 0xE3 && byte(at + 1) == 0x80 && byte(at + 2) == 0x80) { length = 3; return true; }
	return false;
}

// A moderator's name is another player's text: one line, trimmed, and cut to
// MaxHostNameBytes on a character boundary.
std::string HostName(std::string text) {
	text = SingleLine(std::move(text));
	std::size_t length = 0, start = 0;
	while (start < text.size() && IsPad(text, start, length)) start += length;
	text.erase(0, start);
	if (text.size() > netplay::publicrooms::MaxHostNameBytes) {
		std::size_t cut = netplay::publicrooms::MaxHostNameBytes;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
		text.erase(cut);
	}
	for (bool trimmed = true; trimmed && !text.empty();) {
		trimmed = false;
		if (text.back() == ' ') { text.pop_back(); trimmed = true; }
		else if (text.size() >= 2 && static_cast<unsigned char>(text[text.size() - 2]) == 0xC2 && static_cast<unsigned char>(text.back()) == 0xA0) { text.resize(text.size() - 2); trimmed = true; }
		else if (text.size() >= 3 && static_cast<unsigned char>(text[text.size() - 3]) == 0xE3 && static_cast<unsigned char>(text[text.size() - 2]) == 0x80 && static_cast<unsigned char>(text.back()) == 0x80) { text.resize(text.size() - 3); trimmed = true; }
	}
	return text;
}

// The optional details of a RoomSummary. Each field stands alone: one that is
// missing, of the wrong type or out of range is ignored and the row still
// decodes, so a newer bridge's details never cost a player the list.
void DecodeDetails(const json& view, Room& room) {
	if (const auto found = view.find("host_name"); found != view.end() && found->is_string()) {
		room.hostName = HostName(found->get<std::string>());
		room.hasDetails = true;
	}
	if (const auto found = view.find("locked"); found != view.end() && found->is_boolean()) {
		room.locked = found->get<bool>();
		room.hasDetails = true;
	}
	if (const auto found = view.find("fighters"); found != view.end() && found->is_array() &&
		found->size() <= netplay::publicrooms::MaxRoomFighters && found->size() <= room.members) {
		// The bridge's 255 is no fighter; ids it validates are below 64.
		std::vector<int> faces;
		for (const auto& item : *found) {
			if (!item.is_number_unsigned()) { faces.clear(); break; }
			const auto id = item.get<std::uint64_t>();
			if (id == 255) faces.push_back(-1);
			else if (id < 64) faces.push_back(static_cast<int>(id));
			else { faces.clear(); break; }
		}
		if (faces.size() == found->size()) { room.fighters = std::move(faces); room.hasDetails = true; }
	}
	// Sent only for a room that takes wired players alone; an absent key is a room open to every connection.
	if (const auto found = view.find("wired_only"); found != view.end() && found->is_boolean()) {
		room.wiredOnly = found->get<bool>();
		room.hasDetails = true;
	}
	if (const auto found = view.find("set_format"); found != view.end() && found->is_number_unsigned()) {
		const auto value = found->get<std::uint64_t>();
		if (value <= static_cast<std::uint64_t>(room::MaxSetLength)) { room.setFormat = static_cast<int>(value); room.hasDetails = true; }
	}
	if (const auto found = view.find("rotation"); found != view.end() && found->is_number_unsigned() && found->get<std::uint64_t>() <= 2) {
		room.rotation = static_cast<int>(found->get<std::uint64_t>());
		room.hasDetails = true;
	}
	if (const auto found = view.find("room_type"); found == view.end()) {
		if (room.hasDetails) room.type = room::RoomType::Classic;
	} else if (found->is_number_unsigned() && found->get<std::uint64_t>() <= static_cast<std::uint64_t>(room::RoomType::King)) {
		room.type = static_cast<room::RoomType>(found->get<std::uint64_t>());
		room.hasDetails = true;
	}
}

Room DecodeRoom(const json& view) {
	Room room;
	room.id = Text(view, "room_id", 32);
	room.name = SingleLine(Text(view, "name", 64));
	room.region = Text(view, "region", 16);
	room.members = Small(view, "members", 16);
	room.capacity = Small(view, "capacity", 16);
	room.playing = Small(view, "tables_playing", 255);
	room.createdAt = Seconds(view, "created_at");
	// Only an own-delay room says so; a key of any other kind is not read.
	if (const auto found = view.find("own_delay"); found != view.end() && found->is_boolean()) room.ownDelay = found->get<bool>();
	if (!IsHex(room.id, 32) || room.name.empty() || room.capacity < 2 || room.members > room.capacity) throw std::invalid_argument("room");
	DecodeDetails(view, room);
	return room;
}

Binding DecodeBinding(const json& view) {
	Binding binding;
	binding.roomId = Text(view, "room_id", 32);
	binding.room.matchId = Text(view, "match_id", 64);
	binding.room.assignmentGeneration = Counter(view, "assignment_generation");
	binding.room.bindingRevision = Counter(view, "binding_revision");
	const auto games = view.at("games_to_win");
	if (!games.is_number_unsigned() || games.get<std::uint64_t>() > static_cast<std::uint64_t>(room::MaxSetLength)) throw std::invalid_argument("games_to_win");
	binding.room.gamesToWin = static_cast<std::uint8_t>(games.get<std::uint64_t>());
	const auto& fighters = view.at("fighters");
	if (!fighters.is_array() || fighters.size() != 2) throw std::invalid_argument("fighters");
	for (std::size_t slot = 0; slot < 2; ++slot) {
		binding.room.fighters[slot].endpoint = Text(fighters.at(slot), "endpoint_id", 64);
		binding.room.fighters[slot].emberId = Text(fighters.at(slot), "ember_id", 64);
	}
	// A helper that predates starting wins sends none: the set starts at 0-0.
	if (const auto found = view.find("starting_wins"); found != view.end()) {
		if (!found->is_array() || found->size() != 2) throw std::invalid_argument("starting_wins");
		for (std::size_t slot = 0; slot < 2; ++slot) {
			const auto& wins = found->at(slot);
			if (!wins.is_number_unsigned() || wins.get<std::uint64_t>() >= binding.room.gamesToWin)
				throw std::invalid_argument("starting_wins");
			binding.room.startingWins[slot] = static_cast<std::uint8_t>(wins.get<std::uint64_t>());
		}
	}
	// A helper without match watch sends none: nobody watches.
	if (const auto found = view.find("watchers"); found != view.end()) {
		if (!found->is_array() || found->size() > room::MaxWatchers) throw std::invalid_argument("watchers");
		for (const auto& watcher : *found)
			binding.room.watchers.push_back({Text(watcher, "endpoint_id", 64), Text(watcher, "ember_id", 64)});
	}
	const auto& slot = view.at("local_slot");
	if (!slot.is_number_unsigned() || slot.get<std::uint64_t>() > 1) throw std::invalid_argument("local_slot");
	binding.localSlot = static_cast<int>(slot.get<std::uint64_t>());
	if (!IsHex(binding.roomId, 32) || !binding.room.Valid()) throw std::invalid_argument("binding");
	return binding;
}
}

bool IsTournamentPlayOp(const std::string& op) {
	return op == "match_claim" || op == "room_publish" || op == "game_prepare" || op == "game_report" ||
		op == "match_leave" || op == "assignment_list" || op == "match_watch" || op == "match_unwatch" ||
		op == "room_list" || op == "room_create" || op == "room_ticket" ||
		op == "ranking_standing" || op == "ranking_choose" || op == "ranking_board" ||
		op == "points_me" || op == "cards_lookup" || op == "friends_sync" || op == "friends_act" ||
		op == "replay_send" || op == "replay_archive" || op == "replay_pack";
}

std::optional<TournamentAnswer> ReadTournamentAnswer(const nlohmann::json& event) {
	try {
		TournamentAnswer answer;
		answer.op = Text(event, "op", 64);
		if (!IsTournamentPlayOp(answer.op)) return std::nullopt;
		const auto& id = event.at("request_id");
		if (!id.is_number_unsigned()) return std::nullopt;
		answer.requestId = id.get<std::uint64_t>();
		const auto& ok = event.at("ok");
		if (!ok.is_boolean()) return std::nullopt;
		answer.ok = ok.get<bool>();
		answer.reason = OptionalText(event, "reason", 64);
		const auto data = event.find("data");
		if (data != event.end()) answer.data = *data;
		return answer;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<ClaimReply> DecodeClaimReply(const nlohmann::json& data) {
	try {
		ClaimReply reply;
		const auto role = Text(data, "role", 16);
		if (role == "host") {
			reply.role = ClaimReply::Role::Host;
			reply.leaseId = Text(data, "lease_id", 64);
			reply.fence = std::to_string(Counter(data, "fence"));
		} else if (role == "wait") {
			reply.role = ClaimReply::Role::Wait;
			reply.retryAfterMs = (std::min)(Seconds(data, "retry_after"), std::uint64_t(60)) * 1000;
		} else if (role == "room") {
			reply.role = ClaimReply::Role::Room;
			reply.roomId = Text(data, "room_id", 32);
			reply.invitation = Text(data, "invitation", 4096);
			if (!IsHex(reply.roomId, 32) || reply.invitation.empty()) return std::nullopt;
			const auto& binding = data.at("binding");
			if (!binding.is_null()) reply.binding = DecodeBinding(binding);
			if (reply.binding && reply.binding->roomId != reply.roomId) return std::nullopt;
		} else {
			return std::nullopt;
		}
		return reply;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<WatchReply> DecodeWatchReply(const nlohmann::json& data) {
	try {
		WatchReply reply;
		const auto role = Text(data, "role", 16);
		if (role == "waiting") {
			reply.role = WatchReply::Role::Waiting;
			reply.retryAfterMs = (std::min)(Seconds(data, "retry_after"), std::uint64_t(60)) * 1000;
			reply.reason = Text(data, "reason", 32);
		} else if (role == "room") {
			reply.role = WatchReply::Role::Room;
			reply.roomId = Text(data, "room_id", 32);
			reply.invitation = Text(data, "invitation", 4096);
			if (!IsHex(reply.roomId, 32) || reply.invitation.empty()) return std::nullopt;
		} else {
			return std::nullopt;
		}
		return reply;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<PrepareReply> DecodePrepareReply(const nlohmann::json& data) {
	try {
		PrepareReply reply;
		const auto state = Text(data, "state", 16);
		if (state == "pending") {
			reply.retryAfterMs = (std::min)(Seconds(data, "retry_after"), std::uint64_t(60)) * 1000;
		} else if (state == "permitted") {
			reply.permitted = true;
			reply.permitId = Text(data, "permit_id", 64);
			reply.generation = Counter(data, "match_generation");
			// The signed start deadline as a window: both times are the bridge's,
			// so this PC's clock never enters it.
			const auto issuedAt = Seconds(data, "issued_at");
			const auto startBy = Seconds(data, "start_by");
			if (!room::ValidPermitId(reply.permitId) || !reply.generation || startBy <= issuedAt ||
				startBy - issuedAt > room::MaximumPermitWindowMs / 1000) return std::nullopt;
			reply.startWindowMs = (startBy - issuedAt) * 1000;
		} else {
			return std::nullopt;
		}
		return reply;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<std::vector<Assignment>> DecodeAssignments(const nlohmann::json& data) {
	try {
		const auto& rows = data.at("assignments");
		if (!rows.is_array()) return std::nullopt;
		std::vector<Assignment> assignments;
		for (const auto& row : rows) {
			if (assignments.size() >= 50) break;
			Assignment item;
			item.matchId = Text(row, "match_id", 64);
			item.state = Text(row, "state", 32);
			item.provider = OptionalText(row, "provider", 64);
			item.roundLabel = OptionalText(row, "round_label", 128);
			item.profile = OptionalText(row, "native_rules_profile", 64);
			item.createdAt = OptionalSeconds(row, "created_at");
			item.expiresAt = OptionalSeconds(row, "expires_at");
			const auto& slot = row.at("slot");
			if (!slot.is_number_unsigned() || slot.get<std::uint64_t>() > 1) return std::nullopt;
			item.slot = static_cast<int>(slot.get<std::uint64_t>());
			if (row.contains("games_to_win")) {
				const auto& games = row.at("games_to_win");
				if (!games.is_number_unsigned() || !room::ValidGamesToWin(games.get<long long>())) return std::nullopt;
				item.gamesToWin = games.get<int>();
			}
			if (row.contains("opponent") && row.at("opponent").is_object())
				item.opponentFingerprint = OptionalText(row.at("opponent"), "fingerprint", 32);
			if (row.contains("wins")) {
				const auto& wins = row.at("wins");
				if (!wins.is_array() || wins.size() != 2) return std::nullopt;
				for (std::size_t i = 0; i < 2; ++i) {
					// The winner of the longest set reaches MaxSetLength.
					if (!wins.at(i).is_number_unsigned() || wins.at(i).get<std::uint64_t>() > static_cast<std::uint64_t>(room::MaxSetLength)) return std::nullopt;
					item.wins[i] = static_cast<unsigned>(wins.at(i).get<std::uint64_t>());
				}
			}
			assignments.push_back(std::move(item));
		}
		return assignments;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<netplay::publicrooms::RoomList> DecodeRoomList(const nlohmann::json& data) {
	try {
		const auto& rows = data.at("rooms");
		if (!rows.is_array() || rows.size() > netplay::publicrooms::MaxRooms) return std::nullopt;
		netplay::publicrooms::RoomList list;
		for (const auto& row : rows) list.rooms.push_back(DecodeRoom(row));
		// The bridge's clock, only from a bridge that sends details; an invalid one is ignored.
		if (const auto found = data.find("listed_at"); found != data.end() && found->is_number_unsigned())
			list.listedAt = found->get<std::uint64_t>();
		return list;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

namespace {
using netplay::rankings::Listing;

// An object whose every key is one of `allowed`: the bridge's ranking objects
// refuse unknown fields, and so does their reader here.
const json& Strict(const json& object, std::initializer_list<const char*> allowed) {
	if (!object.is_object()) throw std::invalid_argument("object");
	for (auto it = object.begin(); it != object.end(); ++it)
		if (std::none_of(allowed.begin(), allowed.end(), [&](const char* key) { return it.key() == key; })) throw std::invalid_argument("field");
	return object;
}

// An object, for the answers that ignore members they do not know: a field a
// later bridge adds to a points or card answer must not break a build already
// out, so only the keys read are checked.
const json& Object(const json& value) {
	if (!value.is_object()) throw std::invalid_argument("object");
	return value;
}

// {"name":s,"country":s?}: another player's name under the bridge's rules,
// and a country of two upper-case letters, which the game's table may lack
// (shown without a flag).
Listing DecodeListing(const json& value) {
	const auto& object = Strict(value, {"name", "country"});
	Listing listing;
	listing.name = Text(object, "name", netplay::rankings::MaxNameBytes);
	if (!netplay::rankings::IsListingName(listing.name)) throw std::invalid_argument("name");
	if (object.find("country") != object.end()) {
		const auto country = Text(object, "country", 2);
		if (country.size() != 2 || !std::all_of(country.begin(), country.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
			throw std::invalid_argument("country");
		listing.country = CountryCode::Parsed(country);
	}
	return listing;
}

// A badge as rooms carry it (SkillBadgeJson.hxx).
SkillBadge DecodeBadge(const json& object) {
	SkillBadge badge;
	if (!ParseSkillBadge(Strict(object, {"band", "settled", "developer"}), badge)) throw std::invalid_argument("badge");
	return badge;
}
}

std::optional<netplay::rankings::Standing> DecodeStanding(const json& data) {
	using Kind = netplay::rankings::Standing::Kind;
	try {
		netplay::rankings::Standing standing;
		const auto ranking = Text(data, "ranking", 16);
		// Each shape carries its own fields and no others.
		if (ranking == "no_account") { Strict(data, {"ranking"}); return standing; }
		if (ranking == "opted_out") Strict(data, {"ranking", "change_in"});
		else if (ranking == "ranked") Strict(data, {"ranking", "badge", "listing", "change_in"});
		else return std::nullopt;
		if (data.find("change_in") != data.end()) standing.changeIn = Seconds(data, "change_in");
		if (ranking == "opted_out") {
			standing.kind = Kind::OptedOut;
			return standing;
		}
		standing.kind = Kind::Ranked;
		if (const auto badge = data.find("badge"); badge != data.end()) {
			standing.classesShown = true;
			standing.badge = DecodeBadge(*badge);
		}
		if (const auto listing = data.find("listing"); listing != data.end()) {
			standing.listed = true;
			standing.listing = DecodeListing(*listing);
		}
		return standing;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<std::vector<netplay::rankings::BoardEntry>> DecodeBoard(const json& data) {
	try {
		const auto& rows = Strict(data, {"players"}).at("players");
		if (!rows.is_array() || rows.size() > netplay::rankings::MaxBoard) return std::nullopt;
		std::vector<netplay::rankings::BoardEntry> board;
		board.reserve(rows.size());
		for (const auto& row : rows) {
			netplay::rankings::BoardEntry entry;
			entry.listing = DecodeListing(Strict(row, {"listing", "band", "own"}).at("listing"));
			entry.band = static_cast<std::uint8_t>(Small(row, "band", SkillBadge::Classes));
			const auto own = row.find("own");
			if (entry.band == 0 || (own != row.end() && !own->is_boolean()) || (!board.empty() && board.back().band < entry.band))
				return std::nullopt;
			entry.own = own != row.end() && own->get<bool>();
			board.push_back(std::move(entry));
		}
		return board;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

namespace {
// A counter of any size, as the points answers carry them.
std::uint64_t Unsigned(const json& object, const char* key) {
	return Seconds(object, key);
}

// The fighter id of an entry, which must come after the one before it so that
// no fighter appears twice.
std::size_t NextFighter(const json& object, int& previous) {
	const auto fighter = Small(object, "fighter", selection::FighterCount - 1);
	if (static_cast<int>(fighter) <= previous) throw std::invalid_argument("fighter");
	previous = static_cast<int>(fighter);
	return fighter;
}

// A tier, 0 to TierCount - 1.
std::uint8_t ReadTier(const json& object) {
	return static_cast<std::uint8_t>(Small(object, "tier", netplay::levels::TierCount - 1));
}

// One count of points, a fighter's or the account's, checked as strictly as
// the bridge checks it. A fighter is listed only with a counted game; the
// account is always sent, with no game and no `since` until one is counted.
netplay::levels::Level DecodeLevel(const json& entry, bool listedFighter) {
	netplay::levels::Level level;
	Object(entry);
	level.points = Unsigned(entry, "points");
	level.games = Unsigned(entry, "games");
	level.wins = Unsigned(entry, "wins");
	level.tier = ReadTier(entry);
	level.floor = Unsigned(entry, "floor");
	if (level.wins > level.games || level.games > UINT64_MAX - level.wins) throw std::invalid_argument("wins");
	// The points are the games and wins.
	if (listedFighter && level.games == 0) throw std::invalid_argument("games");
	if (level.points != level.games + level.wins) throw std::invalid_argument("points");
	// The first counted game is told exactly when there is one.
	if ((entry.find("since") != entry.end()) != (level.games > 0)) throw std::invalid_argument("since");
	if (level.games > 0) level.since = Unsigned(entry, "since");
	// A tier begins at or below the points, and the first begins at 0.
	if (level.floor > level.points || (level.tier == 0 && level.floor != 0)) throw std::invalid_argument("floor");
	// The last tier has no next one; every other has one still ahead.
	const bool hasNext = entry.find("next") != entry.end();
	if (hasNext != (level.tier != netplay::levels::TierCount - 1)) throw std::invalid_argument("next");
	if (hasNext) {
		level.next = Unsigned(entry, "next");
		if (level.next <= level.points) throw std::invalid_argument("next");
	}
	return level;
}

// A count the bridge may or may not send, up to `maximum`: absent is 0 (a
// bridge from before it), and one that is there is checked as strictly as the
// counts beside it.
std::uint64_t OptionalCount(const json& object, const char* key, std::uint64_t maximum) {
	if (object.find(key) == object.end()) return 0;
	return Small(object, key, maximum);
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

netplay::levels::Card DecodeCard(const json& value) {
	netplay::levels::Card card;
	const auto& object = Object(value);
	card.relation = DecodeRelation(object);
	const auto& account = Object(object.at("account"));
	card.account.points = Unsigned(account, "points");
	card.account.tier = ReadTier(account);
	// The milestones behind the awards a player wears: marks, never counts, all
	// absent from an older bridge.
	if (const auto marks = object.find("marks"); marks != object.end()) {
		Object(*marks);
		card.marks.games = OptionalCount(*marks, "games", UINT64_MAX);
		card.marks.wins = OptionalCount(*marks, "wins", UINT64_MAX);
		card.marks.streak = static_cast<std::uint32_t>(OptionalCount(*marks, "streak", UINT32_MAX));
		card.marks.peakClass = static_cast<std::uint8_t>(OptionalCount(*marks, "class", SkillBadge::Classes));
	}
	// A card with no `fighters` is of a player who has played none.
	if (const auto fighters = object.find("fighters"); fighters != object.end()) {
		if (!fighters->is_array() || fighters->size() > static_cast<std::size_t>(selection::FighterCount)) throw std::invalid_argument("fighters");
		int previous = -1;
		for (const auto& row : *fighters) {
			Object(row);
			netplay::levels::CardFighter fighter;
			fighter.fighter = static_cast<std::uint8_t>(NextFighter(row, previous));
			fighter.points = Unsigned(row, "points");
			if (fighter.points == 0) throw std::invalid_argument("points");
			fighter.tier = ReadTier(row);
			card.fighters.push_back(fighter);
		}
	}
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
		own.account = DecodeLevel(data.at("account"), false);
		const auto& account = data.at("account");
		own.bestStreak = static_cast<std::uint32_t>(OptionalCount(account, "best_streak", UINT32_MAX));
		own.peakClass = static_cast<std::uint8_t>(OptionalCount(account, "peak_class", SkillBadge::Classes));
		// A run of wins cannot be longer than the games played.
		if (own.bestStreak > own.account.games) return std::nullopt;
		const auto& rows = data.at("fighters");
		if (!rows.is_array() || rows.size() > static_cast<std::size_t>(selection::FighterCount)) return std::nullopt;
		int previous = -1;
		for (const auto& row : rows) {
			const auto fighter = NextFighter(row, previous);
			own.fighters[fighter] = DecodeLevel(row, true);
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
			if (row.is_null()) cards.emplace_back();
			else cards.emplace_back(DecodeCard(row));
		}
		return cards;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

std::optional<netplay::publicrooms::Admission> DecodeAdmission(const nlohmann::json& data) {
	try {
		netplay::publicrooms::Admission admission;
		admission.room = DecodeRoom(data.at("room"));
		admission.invitation = Text(data, "invitation", 4096);
		const auto& ticket = data.at("ticket");
		if (!ticket.is_object()) return std::nullopt;
		admission.ticket = ticket.dump();
		if (admission.invitation.empty() || admission.ticket.size() > netplay::publicrooms::MaxTicketBytes) return std::nullopt;
		return admission;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

} }
