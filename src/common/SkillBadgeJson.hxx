#pragma once
// The wire forms of a skill badge (the helper's "connected" event and the room
// snapshot's members) and of a won game (the room checkpoint and the room
// host's status line).
#include "SkillBadge.hxx"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace sf4e {

// The "badge" member of `object` ({"band":n,"settled":b}), or none when it is
// absent or not a valid badge.
inline SkillBadge ReadSkillBadge(const nlohmann::json& object) {
	const auto found = object.find("badge");
	if (found == object.end() || !found->is_object()) return {};
	const auto band = found->find("band"), settled = found->find("settled");
	if (band == found->end() || settled == found->end() || !band->is_number_unsigned() || !settled->is_boolean() ||
		band->get<std::uint64_t>() > SkillBadge::Classes) return {};
	const SkillBadge badge{static_cast<std::uint8_t>(band->get<std::uint64_t>()), settled->get<bool>()};
	return badge.Valid() ? badge : SkillBadge();
}

inline nlohmann::json SkillBadgeJson(const SkillBadge& badge) {
	return {{"band", badge.band}, {"settled", badge.settled}};
}

// {"table":n,"generation":n,"winner":s,"loser":s}. Reading one that is not
// that shape throws; whether its accounts are any good is the reader's call.
inline void to_json(nlohmann::json& json, const WonGame& game) {
	json = {{"table", game.table}, {"generation", game.generation}, {"winner", game.winner}, {"loser", game.loser}};
}
inline void from_json(const nlohmann::json& json, WonGame& game) {
	const auto& table = json.at("table");
	const auto& generation = json.at("generation");
	if (!table.is_number_unsigned() || table.get<std::uint64_t>() > 255 || !generation.is_number_unsigned())
		throw std::invalid_argument("won game");
	game.table = static_cast<std::uint8_t>(table.get<std::uint64_t>());
	game.generation = generation.get<std::uint64_t>();
	game.winner = json.at("winner").get<std::string>();
	game.loser = json.at("loser").get<std::string>();
}

}
