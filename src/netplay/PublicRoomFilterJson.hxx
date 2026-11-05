#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {
// The public rooms filter is one object in the netplay section:
// {"region": "all" | "near" | a relay region code, "type": "all" | "classic" |
// "arcade" | "king", "seats": bool, "wired": bool}. A profile from before the
// filter has none. A field that is missing or of another type, a region that is
// no longer known and a type this build does not know load as All (a flag as
// off), and the other fields load as saved.
inline publicrooms::RoomFilter ReadRoomFilter(const nlohmann::json& saved) {
	using publicrooms::RoomFilter;
	RoomFilter value;
	if (!saved.is_object()) return value;
	const auto found = saved.find("roomFilter");
	if (found == saved.end() || !found->is_object()) return value;
	const auto text = [&](const char* key) {
		const auto it = found->find(key);
		return it != found->end() && it->is_string() ? it->get<std::string>() : std::string();
	};
	const auto flag = [&](const char* key) {
		const auto it = found->find(key);
		return it != found->end() && it->is_boolean() && it->get<bool>();
	};
	const std::string region = text("region"), type = text("type");
	if (region == "near") value.scope = RoomFilter::Region::Near;
	else if (publicrooms::KnownFilterRegion(region)) { value.scope = RoomFilter::Region::One; value.region = region; }
	value.type = type == "classic" ? RoomFilter::Type::Classic : type == "arcade" ? RoomFilter::Type::Arcade :
		type == "king" ? RoomFilter::Type::King : RoomFilter::Type::All;
	value.openSeats = flag("seats");
	value.wiredOnly = flag("wired");
	return value;
}
inline void WriteRoomFilter(const PlayerPreferences& value, nlohmann::json& saved) {
	using publicrooms::RoomFilter;
	const auto& filter = value.roomFilter;
	const char* types[] = {"all", "classic", "arcade", "king"};
	saved["roomFilter"] = {
		{"region", filter.scope == RoomFilter::Region::All ? "all" : filter.scope == RoomFilter::Region::Near ? "near" : filter.region},
		{"type", types[static_cast<int>(filter.type)]}, {"seats", filter.openSeats}, {"wired", filter.wiredOnly}};
}
} }
