#include "TournamentAnswers.hxx"

#include <algorithm>
#include <stdexcept>

namespace sf4e { namespace session {
namespace {
using nlohmann::json;
using netplay::tournament::Binding;
using netplay::tournament::ClaimReply;
using netplay::tournament::PrepareReply;

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

bool IsHex(const std::string& text, std::size_t length) {
	return text.size() == length && std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
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
		op == "match_leave" || op == "assignment_list" || op == "handoff_redeem";
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
			if (!room::ValidPermitId(reply.permitId) || !reply.generation) return std::nullopt;
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

} }
