#pragma once
#include "../common/Area.hxx"
#include <string>

namespace sf4e { namespace netplay { namespace publicrooms {

// Which rooms the Public rooms list shows. Saved with the player's preferences,
// which C++14 targets include, so this header keeps to C++14; matching rooms is
// PublicRoomFilterMatch.hxx. The default shows every room.
struct RoomFilter {
	enum class Place { All, Near, One };
	enum class Type { All, Classic, Arcade, King };
	// Near is the rooms in or next to the player's own area (AreaNear); One is `area`.
	Place scope = Place::All;
	Area area = Area::None;
	Type type = Type::All;
	// Only rooms with a free seat the player could take (not locked, not full).
	bool openSeats = false;
	// Only rooms that take wired players alone.
	bool wiredOnly = false;

	bool operator==(const RoomFilter& other) const {
		return scope == other.scope && area == other.area && type == other.type && openSeats == other.openSeats && wiredOnly == other.wiredOnly;
	}
	bool operator!=(const RoomFilter& other) const { return !(*this == other); }
	// Anything but the default: some room may be hidden.
	bool Active() const { return *this != RoomFilter(); }
	// An area is named exactly when the scope is One.
	bool Valid() const { return (scope == Place::One) == (area != Area::None); }

	// Quick join takes the nearest room unless the player chose an area.
	RoomFilter ForQuickJoin() const {
		RoomFilter quick = *this;
		if (quick.scope == Place::All) quick.scope = Place::Near;
		return quick;
	}

	// The place setting `delta` steps along All, Near me, then each area in the
	// table's order, wrapping. Near is skipped while `home`, the player's own
	// area, is not set.
	void StepPlace(int delta, Area home) {
		const int count = 2 + areas::Count;
		int at = scope == Place::All ? 0 : scope == Place::Near ? 1 : 1 + static_cast<int>(area);
		const int step = delta < 0 ? count - 1 : 1;
		do at = (at + step) % count; while (at == 1 && home == Area::None);
		scope = at == 0 ? Place::All : at == 1 ? Place::Near : Place::One;
		area = at >= 2 ? static_cast<Area>(at - 1) : Area::None;
	}
	void StepType(int delta) {
		constexpr int count = 4;
		type = static_cast<Type>((static_cast<int>(type) + (delta < 0 ? count - 1 : 1)) % count);
	}
};

} } }
