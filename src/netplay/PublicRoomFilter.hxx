#pragma once
#include <algorithm>
#include <array>
#include <string>

namespace sf4e { namespace netplay { namespace publicrooms {

// The relay regions a player can filter public rooms by, in the order stepping
// goes (ui::RelayRegionName names each; "other" is no region to ask for).
// The saved filter is part of PlayerPreferences, which C++14 targets include,
// so this header keeps to C++14; matching rooms is PublicRoomFilterMatch.hxx.
inline const std::array<const char*, 4>& FilterRegions() {
	static const std::array<const char*, 4> regions = {{"use1", "usw1", "euc1", "aps1"}};
	return regions;
}
inline bool KnownFilterRegion(const std::string& code) {
	const auto& regions = FilterRegions();
	return std::find(regions.begin(), regions.end(), code) != regions.end();
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
		const auto& regions = FilterRegions();
		const int count = 2 + static_cast<int>(regions.size());
		int at = scope == Region::Near ? 1 : 0;
		if (scope == Region::One) {
			const auto found = std::find(regions.begin(), regions.end(), region);
			if (found != regions.end()) at = 2 + static_cast<int>(found - regions.begin());
		}
		const int step = delta < 0 ? count - 1 : 1;
		do at = (at + step) % count; while (at == 1 && home.empty());
		scope = at == 0 ? Region::All : at == 1 ? Region::Near : Region::One;
		region = at >= 2 ? regions[at - 2] : "";
	}
	void StepType(int delta) {
		constexpr int count = 4;
		type = static_cast<Type>((static_cast<int>(type) + (delta < 0 ? count - 1 : 1)) % count);
	}
};

} } }
