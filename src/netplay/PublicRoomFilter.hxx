#pragma once
#include "../common/Area.hxx"
#include <string>

namespace sf4e { namespace netplay { namespace publicrooms {

// Which rooms the Public rooms list shows. Saved with the player's preferences,
// which C++14 targets include, so this header keeps to C++14; matching rooms is
// PublicRoomFilterMatch.hxx. The default shows every room.
struct RoomFilter {
	// Areas is the rooms in the ticked areas, `areas`; All is every room, nothing ticked.
	enum class Place { All, Near, Areas };
	enum class Type { All, Classic, Arcade, King };
	// Near is the rooms in or next to the player's own area (AreaNear). It and a set of
	// areas are the two ways to narrow by place, so ticking an area leaves Near.
	Place scope = Place::All;
	AreaSet areas;
	Type type = Type::All;
	// Only rooms with a free seat the player could take (not locked, not full).
	bool openSeats = false;
	// Only rooms that take wired players alone.
	bool wiredOnly = false;

	bool operator==(const RoomFilter& other) const {
		return scope == other.scope && areas == other.areas && type == other.type && openSeats == other.openSeats && wiredOnly == other.wiredOnly;
	}
	bool operator!=(const RoomFilter& other) const { return !(*this == other); }
	// Anything but the default: some room may be hidden.
	bool Active() const { return *this != RoomFilter(); }
	// Areas are named exactly when the scope is Areas.
	bool Valid() const { return (scope == Place::Areas) == !areas.Empty(); }

	// Quick join looks for a room near the player first unless the player chose areas. A room is
	// near only when it names an area, so Quick join then falls back to this filter itself.
	RoomFilter ForQuickJoin() const {
		RoomFilter quick = *this;
		if (quick.scope == Place::All) quick.scope = Place::Near;
		return quick;
	}

	// Ticks or unticks one area. Ticking leaves Near me; unticking the last one is All.
	void SetArea(Area area, bool ticked) {
		if (scope == Place::Near) areas = AreaSet();
		areas = areas.With(area, ticked);
		scope = areas.Empty() ? Place::All : Place::Areas;
	}
	// Near me on or off; either way no area stays ticked.
	void SetNear(bool on) {
		areas = AreaSet();
		scope = on ? Place::Near : Place::All;
	}
	// The quick choice Left and Right make: All to Near me and back, any ticked areas to All.
	// Near is skipped while `home`, the player's own area, is not set.
	void StepNear(Area home) { SetNear(scope == Place::All && home != Area::None); }
	void StepType(int delta) {
		constexpr int count = 4;
		type = static_cast<Type>((static_cast<int>(type) + (delta < 0 ? count - 1 : 1)) % count);
	}
};

} } }
