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

// Whether `filter` lists `room`. `home` is the player's own area, None while it
// is not set: Near then lists every room, as it cannot say which are near. A
// room whose creator gave no area is never near and in no one area. A room
// that sent no type passes the type filter only under All, since it cannot be
// told to be of the type asked for.
inline bool Matches(const RoomFilter& filter, const Room& room, Area home) {
	using Place = RoomFilter::Place;
	if (filter.scope == Place::Near && home != Area::None && !AreaNear(home, room.area)) return false;
	if (filter.scope == Place::One && room.area != filter.area) return false;
	if (filter.type != RoomFilter::Type::All && room.type != WantedType(filter.type)) return false;
	if (filter.openSeats && !room.Joinable()) return false;
	return !filter.wiredOnly || room.wiredOnly;
}

} } }
