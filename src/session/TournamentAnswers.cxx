#include "TournamentAnswers.hxx"

#include <algorithm>
#include <stdexcept>

namespace sf4e { namespace session {
namespace {
using nlohmann::json;
using netplay::tournament::Binding;
using netplay::tournament::ClaimReply;
using netplay::tournament::PrepareReply;
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
	if (const auto found = view.find("set_format"); found != view.end() && found->is_number_unsigned()) {
		const auto value = found->get<std::uint64_t>();
		if (value <= 3 || value == 5) { room.setFormat = static_cast<int>(value); room.hasDetails = true; }
	}
	if (const auto found = view.find("rotation"); found != view.end() && found->is_number_unsigned() && found->get<std::uint64_t>() <= 2) {
		room.rotation = static_cast<int>(found->get<std::uint64_t>());
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
	if (!games.is_number_unsigned() || games.get<std::uint64_t>() > 5) throw std::invalid_argument("games_to_win");
	binding.room.gamesToWin = static_cast<std::uint8_t>(games.get<std::uint64_t>());
	const auto& fighters = view.at("fighters");
	if (!fighters.is_array() || fighters.size() != 2) throw std::invalid_argument("fighters");
	for (std::size_t slot = 0; slot < 2; ++slot) {
		binding.room.fighters[slot].endpoint = Text(fighters.at(slot), "endpoint_id", 64);
		binding.room.fighters[slot].emberId = Text(fighters.at(slot), "ember_id", 64);
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
		op == "match_leave" || op == "assignment_list" ||
		op == "room_list" || op == "room_create" || op == "room_ticket";
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
				if (!games.is_number_unsigned() || games.get<std::uint64_t>() > 5) return std::nullopt;
				item.gamesToWin = static_cast<int>(games.get<std::uint64_t>());
			}
			if (row.contains("opponent") && row.at("opponent").is_object())
				item.opponentFingerprint = OptionalText(row.at("opponent"), "fingerprint", 32);
			if (row.contains("wins")) {
				const auto& wins = row.at("wins");
				if (!wins.is_array() || wins.size() != 2) return std::nullopt;
				for (std::size_t i = 0; i < 2; ++i) {
					if (!wins.at(i).is_number_unsigned() || wins.at(i).get<std::uint64_t>() > 9) return std::nullopt;
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
