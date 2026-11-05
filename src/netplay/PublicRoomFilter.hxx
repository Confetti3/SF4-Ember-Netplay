#pragma once
#include "PublicRooms.hxx"
#include "../common/RoomRules.hxx"
#include <algorithm>
#include <iterator>
#include <optional>
#include <string>

namespace sf4e { namespace netplay { namespace publicrooms {

// The relay regions a player can filter public rooms by, in the order stepping
// goes (ui::RelayRegionName names each; "other" is no region to ask for).
inline constexpr const char* FilterRegions[] = {"use1", "usw1", "euc1", "aps1"};
inline bool KnownFilterRegion(const std::string& code) {
	return std::find(std::begin(FilterRegions), std::end(FilterRegions), code) != std::end(FilterRegions);
}

// Which rooms the Public rooms list shows. Saved with the player's preferences.
// The default shows every room.
struct RoomFilter {
	enum class Region { All, Near, One };
	enum class Type { All, Classic, Arcade, King };
	// Near is the region of this PC's home relay; One is `region`.
	Region scope = Region::All;
	std::string region;
	Type type = Type::All;
	// Only rooms with a free seat the player could take (not locked, not full).
	bool openSeats = false;
	// Only rooms that take wired players alone.
	bool wiredOnly = false;

	bool operator==(const RoomFilter& other) const {
		return scope == other.scope && region == other.region && type == other.type && openSeats == other.openSeats && wiredOnly == other.wiredOnly;
	}
	bool operator!=(const RoomFilter& other) const { return !(*this == other); }
	// Anything but the default: some room may be hidden.
	bool Active() const { return *this != RoomFilter(); }
	// A region is named exactly when the scope is One, and it is a known one.
	bool Valid() const {
		return scope == Region::One ? KnownFilterRegion(region) : region.empty();
	}

	// Whether `room` is listed. `home` is the region of this PC's home relay,
	// empty while it is unknown: Near then lists every region, as it cannot say
	// which are near. A room that sent no type passes the type filter only
	// under All, since it cannot be told to be of the type asked for.
	bool Matches(const Room& room, const std::string& home) const {
		if (scope == Region::Near && !home.empty() && room.region != home) return false;
		if (scope == Region::One && room.region != region) return false;
		if (type != Type::All && room.type != Wanted(type)) return false;
		if (openSeats && !room.Joinable()) return false;
		return !wiredOnly || room.wiredOnly;
	}

	// Quick join takes the nearest room unless the player chose a region.
	RoomFilter ForQuickJoin() const {
		RoomFilter quick = *this;
		if (quick.scope == Region::All) quick.scope = Region::Near;
		return quick;
	}

	// The region setting `delta` steps along All, Near me, then each known region,
	// wrapping. Near is skipped while `home` is unknown. A region that is not a
	// known one steps from All.
	void StepRegion(int delta, const std::string& home) {
		constexpr int count = 2 + static_cast<int>(std::size(FilterRegions));
		int at = scope == Region::Near ? 1 : 0;
		if (scope == Region::One) {
			const auto found = std::find(std::begin(FilterRegions), std::end(FilterRegions), region);
			if (found != std::end(FilterRegions)) at = 2 + static_cast<int>(found - std::begin(FilterRegions));
		}
		const int step = delta < 0 ? count - 1 : 1;
		do at = (at + step) % count; while (at == 1 && home.empty());
		scope = at == 0 ? Region::All : at == 1 ? Region::Near : Region::One;
		region = at >= 2 ? FilterRegions[at - 2] : "";
	}
	void StepType(int delta) {
		constexpr int count = 4;
		type = static_cast<Type>((static_cast<int>(type) + (delta < 0 ? count - 1 : 1)) % count);
	}
	// The room type a filter asks for; none under All.
	static std::optional<room::RoomType> Wanted(Type type) {
		switch (type) {
		case Type::Classic: return room::ClassicRoom;
		case Type::Arcade: return room::ArcadeRoom;
		case Type::King: return room::KingRoom;
		default: return std::nullopt;
		}
	}
};

} } }
