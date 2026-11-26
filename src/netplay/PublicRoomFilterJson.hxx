#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {
// The public rooms filter is one object in the netplay section:
// {"region": "all" | "near" | "areas", "areas": [area codes], "type": "all" |
// "classic" | "arcade" | "king", "seats": bool, "wired": bool, "opponents":
// "similar" | "any"}. Anything but "any" for opponents is Similar. "areas" is read
// only under "areas". Builds that saved one area wrote its code as the region
// ("sam"), which still loads as that one area. A profile from before the
// filter has none. A field that is missing or of another type, a region that is
// neither of those (the relay codes builds before areas saved, "usw1" among
// them), codes in "areas" that are not areas (they are dropped, and none left is
// All) and a type this build does not know load as All (a flag as off), and the
// other fields load as saved. A build from before several areas reads the new
// shape's region as unknown: its place is All, the other fields as saved.
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
	if (region == "near") value.SetNear(true);
	else if (region == "areas") {
		const auto codes = found->find("areas");
		if (codes != found->end() && codes->is_array())
			for (const auto& code : *codes) if (code.is_string()) value.SetArea(AreaFromCode(code.get<std::string>()), true);
	} else value.SetArea(AreaFromCode(region), true);
	value.type = type == "classic" ? RoomFilter::Type::Classic : type == "arcade" ? RoomFilter::Type::Arcade :
		type == "king" ? RoomFilter::Type::King : RoomFilter::Type::All;
	value.openSeats = flag("seats");
	value.wiredOnly = flag("wired");
	value.opponents = text("opponents") == "any" ? RoomFilter::Opponents::Any : RoomFilter::Opponents::Similar;
	return value;
}
inline void WriteRoomFilter(const PlayerPreferences& value, nlohmann::json& saved) {
	using publicrooms::RoomFilter;
	const auto& filter = value.roomFilter;
	const char* types[] = {"all", "classic", "arcade", "king"};
	saved["roomFilter"] = {
		{"region", filter.scope == RoomFilter::Place::All ? "all" : filter.scope == RoomFilter::Place::Near ? "near" : "areas"},
		{"areas", filter.areas.Codes()},
		{"type", types[static_cast<int>(filter.type)]}, {"seats", filter.openSeats}, {"wired", filter.wiredOnly},
		{"opponents", filter.opponents == RoomFilter::Opponents::Any ? "any" : "similar"}};
}
} }
