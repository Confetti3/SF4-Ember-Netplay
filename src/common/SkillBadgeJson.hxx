#pragma once
// The wire forms of a skill badge (the helper's "connected" event and the room
// snapshot's members) and of a won game (the room checkpoint and the room
// host's status line).
#include "SkillBadge.hxx"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace sf4e {

// A badge object, {"band":n,"settled":b} with "developer":true for the
// developer mark: band 0 is placing, any other a class. `out` is left alone
// and false returned when it is not a valid badge.
inline bool ParseSkillBadge(const nlohmann::json& value, SkillBadge& out) {
	if (!value.is_object()) return false;
	const auto band = value.find("band"), settled = value.find("settled"), developer = value.find("developer");
	if (band == value.end() || settled == value.end() || !band->is_number_unsigned() || !settled->is_boolean() ||
		band->get<std::uint64_t>() > SkillBadge::Classes || (developer != value.end() && !developer->is_boolean())) return false;
	const auto level = static_cast<std::uint8_t>(band->get<std::uint64_t>());
	const SkillBadge badge = developer != value.end() && developer->get<bool>() ? SkillBadge{SkillBadge::Kind::Developer, level, settled->get<bool>()} :
		level ? SkillBadge::OfClass(level, settled->get<bool>()) : SkillBadge{SkillBadge::Kind::Placing, 0, settled->get<bool>()};
	if (!badge.Valid()) return false;
	out = badge;
	return true;
}

// The "badge" member of `object`, or none when it is absent or not a valid badge.
inline SkillBadge ReadSkillBadge(const nlohmann::json& object) {
	const auto found = object.find("badge");
	SkillBadge badge;
	return found != object.end() && ParseSkillBadge(*found, badge) ? badge : SkillBadge();
}

inline nlohmann::json SkillBadgeJson(const SkillBadge& badge) {
	nlohmann::json json = {{"band", badge.band}, {"settled", badge.settled}};
	if (badge.kind == SkillBadge::Kind::Developer) json["developer"] = true;
	return json;
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
