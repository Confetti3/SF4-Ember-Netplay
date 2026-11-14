#pragma once
#include <string>

namespace sf4e {

// Where a player plays from, coarsely: the player's own claim (AreaPreference),
// defaulted from Windows (AreaZones.hxx). It is not how they connect: the relay
// is a routing fact (NetworkRoute.hxx) and never names a place. A player card's
// Region chip, a public room's place, "near you" and the rooms filter read it.
// None is no area: not set, hidden, or a code this build does not know.
enum class Area : unsigned char {
	None,
	NaWest, NaCentral, NaEast, CentralAmerica, SouthAmerica,
	EuropeWest, EuropeEast, MiddleEastAfrica,
	SouthAsia, EastAsia, SouthEastAsia, Oceania,
};

namespace areas {
// Each area in enum order (Table[i].area is Area(i + 1)), with its wire code, which
// a card's style and a room carry, and its catalog key: area. and the code. The
// codes are a to z only, so a reader that checks a relay code's shape takes them.
// The same list is ember_protocol's `area::Area` and the rooms page's AREAS
// (server/ember-short/deploy/site/assets/rooms.js); `area::tests` reads this
// table and checks the three agree, and the catalog's names with them.
struct Row { Area area; const char* code; const char* key; };
constexpr int Count = 12;
constexpr Row Table[Count] = {
	{Area::NaWest, "naw", "area.naw"},
	{Area::NaCentral, "nac", "area.nac"},
	{Area::NaEast, "nae", "area.nae"},
	{Area::CentralAmerica, "cam", "area.cam"},
	{Area::SouthAmerica, "sam", "area.sam"},
	{Area::EuropeWest, "euw", "area.euw"},
	{Area::EuropeEast, "eue", "area.eue"},
	{Area::MiddleEastAfrica, "mea", "area.mea"},
	{Area::SouthAsia, "sas", "area.sas"},
	{Area::EastAsia, "eas", "area.eas"},
	{Area::SouthEastAsia, "sea", "area.sea"},
	{Area::Oceania, "oce", "area.oce"},
};

// The areas next to each other, each pair once. Near is the same area or one
// of these, both ways round, so the relation is symmetric by construction.
struct Pair { Area a, b; };
constexpr Pair Neighbours[] = {
	{Area::NaWest, Area::NaCentral},
	{Area::NaCentral, Area::NaEast},
	{Area::NaCentral, Area::CentralAmerica},
	{Area::NaEast, Area::CentralAmerica},
	{Area::CentralAmerica, Area::SouthAmerica},
	{Area::EuropeWest, Area::EuropeEast},
	{Area::EuropeWest, Area::MiddleEastAfrica},
	{Area::EuropeEast, Area::MiddleEastAfrica},
	{Area::MiddleEastAfrica, Area::SouthAsia},
	{Area::SouthAsia, Area::SouthEastAsia},
	{Area::SouthEastAsia, Area::EastAsia},
	{Area::SouthEastAsia, Area::Oceania},
};

inline const Row* RowOf(Area area) {
	const int index = static_cast<int>(area) - 1;
	return index >= 0 && index < Count ? &Table[index] : nullptr;
}
}

// The area's wire code, or empty for None.
inline std::string AreaCode(Area area) {
	const auto* row = areas::RowOf(area);
	return row ? row->code : std::string();
}
// The catalog key of the area's name, or null for None.
inline const char* AreaKey(Area area) {
	const auto* row = areas::RowOf(area);
	return row ? row->key : nullptr;
}
// The area a wire code names; None for any other text (a relay code included).
inline Area AreaFromCode(const std::string& code) {
	for (const auto& row : areas::Table) if (code == row.code) return row.area;
	return Area::None;
}
// Whether a room in `other` is near a player in `own`: the same area or a
// neighbour. Nothing is near an unknown area, and an unknown area is near nothing.
inline bool AreaNear(Area own, Area other) {
	if (own == Area::None || other == Area::None) return false;
	if (own == other) return true;
	for (const auto& pair : areas::Neighbours)
		if ((pair.a == own && pair.b == other) || (pair.a == other && pair.b == own)) return true;
	return false;
}

// The player's choice of area, as the country's: Automatic follows Windows
// (AreaFromWindows), Chosen is one they picked, Hidden shows none. Saved as
// "auto", "hidden" or the area's code.
struct AreaPreference {
	enum class Kind { Automatic, Chosen, Hidden };
	Kind kind = Kind::Automatic;
	Area area = Area::None;
	static AreaPreference Automatic() { return {}; }
	static AreaPreference Hidden() { AreaPreference p; p.kind = Kind::Hidden; return p; }
	static AreaPreference Of(Area area) {
		AreaPreference p;
		if (area != Area::None) { p.kind = Kind::Chosen; p.area = area; }
		return p;
	}
	bool Valid() const { return (kind == Kind::Chosen) == (area != Area::None); }
	std::string ToString() const { return kind == Kind::Chosen ? AreaCode(area) : kind == Kind::Hidden ? "hidden" : "auto"; }
	// Saved text that is none of the three reads as Automatic.
	static AreaPreference Parsed(const std::string& text) {
		if (text == "hidden") return Hidden();
		return Of(AreaFromCode(text));
	}
	// The choice `delta` places from this one along Automatic, each area in the
	// table's order, then Hidden, stopping at either end: Left and Right step by
	// one, and a pick from the list is the steps to it.
	AreaPreference Stepped(int delta) const {
		constexpr int last = areas::Count + 1;
		const int at = kind == Kind::Automatic ? 0 : kind == Kind::Hidden ? last : static_cast<int>(area);
		const int next = at + delta < 0 ? 0 : at + delta > last ? last : at + delta;
		return next == 0 ? Automatic() : next == last ? Hidden() : Of(static_cast<Area>(next));
	}
	bool operator==(const AreaPreference& rhs) const { return kind == rhs.kind && area == rhs.area; }
	bool operator!=(const AreaPreference& rhs) const { return !(*this == rhs); }
};

// The area a player shows: Automatic takes Windows' (`automatic`, None when
// Windows says too little), a pick its own, Hidden none.
inline Area ShownArea(const AreaPreference& preference, Area automatic) {
	switch (preference.kind) {
	case AreaPreference::Kind::Chosen: return preference.area;
	case AreaPreference::Kind::Hidden: return Area::None;
	default: return automatic;
	}
}

}
