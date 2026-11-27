#include "TournamentAnswers.hxx"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

#include "../common/FighterCatalog.hxx"
#include "../common/RoomRules.hxx"
#include "../common/SkillBadgeJson.hxx"
#include "AnswerReading.hxx"

namespace sf4e { namespace session {
using namespace reading;

namespace {
using nlohmann::json;
using netplay::tournament::Binding;
using netplay::tournament::ClaimReply;
using netplay::tournament::PrepareReply;
using netplay::tournament::WatchReply;
using netplay::publicrooms::Room;

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
	// Quick join's facts (docs/design/QUICK_JOIN.md): each list stands alone and is
	// read whole or not at all.
	if (const auto found = view.find("classes"); found != view.end() && found->is_array() && found->size() <= room.members &&
		found->size() <= room::MaxMembers) {
		std::vector<std::uint8_t> classes;
		for (const auto& item : *found)
			if (item.is_number_unsigned() && item.get<std::uint64_t>() <= SkillBadge::Classes) classes.push_back(static_cast<std::uint8_t>(item.get<std::uint64_t>()));
		if (classes.size() == found->size()) { room.classes = std::move(classes); room.hasDetails = true; }
	}
	if (const auto found = view.find("waiting"); found != view.end() && found->is_array() && found->size() <= room.members &&
		found->size() <= room::MaximumWaiting) {
		std::vector<room::WaitingPlayer> waiting;
		for (const auto& item : *found) {
			const auto band = item.is_object() ? item.find("class") : item.end();
			if (band == item.end() || !band->is_number_unsigned() || band->get<std::uint64_t>() > SkillBadge::Classes) break;
			const auto area = item.find("area");
			waiting.push_back({static_cast<std::uint8_t>(band->get<std::uint64_t>()),
				area != item.end() && area->is_string() ? AreaFromCode(area->get<std::string>()) : Area::None});
		}
		if (waiting.size() == found->size()) { room.waiting = std::move(waiting); room.hasDetails = true; }
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
	room.members = Small(view, "members", 16);
	room.capacity = Small(view, "capacity", 16);
	room.playing = Small(view, "tables_playing", 255);
	room.createdAt = Count(view, "created_at");
	// Only an own-delay room says so; a key of any other kind is not read.
	if (const auto found = view.find("own_delay"); found != view.end() && found->is_boolean()) room.ownDelay = found->get<bool>();
	// So is a room whose creator gave an area; anything but an area's code is none.
	if (const auto found = view.find("area"); found != view.end() && found->is_string()) room.area = AreaFromCode(found->get<std::string>());
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
		op == "ranking_standing" || op == "ranking_choose" || op == "ranking_board" || op == "points_board" || op == "ranking_rating" ||
		op == "points_me" || op == "cards_lookup" || op == "cards_search" || op == "card_save" || op == "friends_sync" || op == "friends_act" ||
		op == "replay_send" || op == "replay_archive" || op == "replay_pack" || op == "archive_list" || op == "archive_fetch";
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
			reply.retryAfterMs = (std::min)(Count(data, "retry_after"), std::uint64_t(60)) * 1000;
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
			reply.retryAfterMs = (std::min)(Count(data, "retry_after"), std::uint64_t(60)) * 1000;
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
			reply.retryAfterMs = (std::min)(Count(data, "retry_after"), std::uint64_t(60)) * 1000;
		} else if (state == "permitted") {
			reply.permitted = true;
			reply.permitId = Text(data, "permit_id", 64);
			reply.generation = Counter(data, "match_generation");
			// The signed start deadline as a window: both times are the bridge's,
			// so this PC's clock never enters it.
			const auto issuedAt = Count(data, "issued_at");
			const auto startBy = Count(data, "start_by");
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
			item.createdAt = LenientCount(row, "created_at");
			item.expiresAt = LenientCount(row, "expires_at");
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
		if (data.find("change_in") != data.end()) standing.changeIn = Count(data, "change_in");
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

// Points board: the bridge sends at most MaxBoard lines; a line that cannot be
// read is left out, so the board still comes back with the rest. A line with
// no class shows no class (band 0), and a tier past this build's table shows
// as the last it knows.
std::optional<netplay::rankings::PointsBoard> DecodePointsBoard(const json& data) {
	try {
		const auto& rows = Strict(data, {"listed_at", "fighter", "total", "players"}).at("players");
		if (!rows.is_array() || rows.size() > netplay::rankings::MaxBoard) return std::nullopt;
		netplay::rankings::PointsBoard board;
		if (data.find("fighter") != data.end()) {
			const auto fighter = Count(data, "fighter");
			if (fighter >= selection::FighterCount) return std::nullopt;
			board.fighter = static_cast<int>(fighter);
		}
		if (data.find("total") != data.end()) board.total = Count(data, "total");
		board.lines.reserve(rows.size());
		for (const auto& row : rows) {
			try {
				const auto& object = Strict(row, {"place", "name", "country", "band", "points", "tier", "card"});
				netplay::rankings::PointsLine line;
				line.place = static_cast<std::uint32_t>(Small(object, "place", netplay::rankings::MaxBoard));
				if (line.place == 0) throw std::invalid_argument("place");
				// The bridge writes a line's name and country beside its numbers, as a listing's two members.
				json named = {{"name", object.at("name")}};
				if (const auto country = object.find("country"); country != object.end()) named["country"] = *country;
				line.listing = DecodeListing(named);
				line.band = static_cast<std::uint8_t>(CappedCount(object, "band", SkillBadge::Classes));
				line.points = Count(object, "points");
				line.tier = static_cast<std::uint8_t>(CappedCount(object, "tier", netplay::levels::TierCount - 1));
				if (const auto found = object.find("card"); found != object.end()) {
					if (!found->is_string()) throw std::invalid_argument("card");
					if (CardToken::Parsed(found->get<std::string>()).Present()) line.card = found->get<std::string>();
				}
				board.lines.push_back(std::move(line));
			} catch (const std::exception&) {
				// One line that cannot be read is left out, never a failed answer.
			}
		}
		return board;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

// Rating details: the bridge sends a "rating" object, left out before a rated
// game, and the rules beside it, which Ember does not show. Every number must
// be finite or the answer is refused; an example whose weight is not in
// (0,1] is dropped, never a failed answer. At most eight examples.
std::optional<netplay::rankings::RatingDetails> DecodeRatingDetails(const json& data) {
	try {
		Strict(data, {"rating", "rules"});
		netplay::rankings::RatingDetails details;
		const auto found = data.find("rating");
		if (found == data.end()) return details;
		const auto& rating = Strict(*found, {"takes_part", "rating", "deviation", "volatility", "band", "settled",
			"up_at", "down_below", "faint_in_days", "played_at", "examples"});
		details.known = true;
		details.takesPart = rating.at("takes_part").get<bool>();
		details.settled = rating.at("settled").get<bool>();
		details.rating = Finite(rating, "rating");
		details.deviation = Finite(rating, "deviation");
		details.volatility = Finite(rating, "volatility");
		details.band = static_cast<std::uint8_t>((std::min)(Count(rating, "band"), static_cast<std::uint64_t>(SkillBadge::Classes)));
		details.upAt = OptionalFinite(rating, "up_at");
		details.downBelow = OptionalFinite(rating, "down_below");
		if (rating.find("faint_in_days") != rating.end()) details.faintInDays = Count(rating, "faint_in_days");
		details.playedAt = Count(rating, "played_at");
		const auto& examples = rating.at("examples");
		if (!examples.is_array() || examples.size() > 8) return std::nullopt;
		const auto outcome = [](const json& value) {
			const auto& object = Strict(value, {"weight", "change"});
			const netplay::rankings::RatingOutcome read{Finite(object, "weight"), Finite(object, "change")};
			if (read.weight <= 0.0 || read.weight > 1.0) throw std::invalid_argument("weight");
			return read;
		};
		for (const auto& example : examples) {
			try {
				const auto& object = Strict(example, {"opponent", "band", "win", "loss"});
				netplay::rankings::RatingExample entry;
				entry.opponent = Finite(object, "opponent");
				entry.band = static_cast<std::uint8_t>(CappedCount(object, "band", SkillBadge::Classes));
				entry.win = outcome(object.at("win"));
				entry.loss = outcome(object.at("loss"));
				details.examples.push_back(entry);
			} catch (const std::exception&) {
				// One example that cannot be read is dropped, never a failed answer.
			}
		}
		return details;
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
