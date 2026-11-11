#pragma once
#include "PublicRoomFilter.hxx"
#include "PublicRooms.hxx"
#include "../common/RoomRules.hxx"
#include <optional>
#include <string>

namespace sf4e { namespace netplay { namespace publicrooms {

// The room type a filter asks for; none under All.
inline std::optional<room::RoomType> WantedType(RoomFilter::Type type) {
	switch (type) {
	case RoomFilter::Type::Classic: return room::RoomType::Classic;
	case RoomFilter::Type::Arcade: return room::RoomType::Arcade;
	case RoomFilter::Type::King: return room::RoomType::King;
	default: return std::nullopt;
	}
}

// Whether `filter` lists `room`. `home` is the region of this PC's home relay,
// empty while it is unknown: Near then lists every region, as it cannot say
// which are near. A room that sent no type passes the type filter only under
// All, since it cannot be told to be of the type asked for.
inline bool Matches(const RoomFilter& filter, const Room& room, const std::string& home) {
	using Region = RoomFilter::Region;
	if (filter.scope == Region::Near && !home.empty() && room.region != home) return false;
	if (filter.scope == Region::One && room.region != filter.region) return false;
	if (filter.type != RoomFilter::Type::All && room.type != WantedType(filter.type)) return false;
	if (filter.openSeats && !room.Joinable()) return false;
	return !filter.wiredOnly || room.wiredOnly;
}

} } }
