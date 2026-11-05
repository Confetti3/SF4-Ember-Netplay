#pragma once
// The two plain structs a room's match carries on the session wire: the
// round time and each fighter's pick. SF4's engine headers alias them
// (Dimps__Math.hxx, Dimps__GameEvents.hxx), so the engine and the wire share
// this one definition and SF4's bytes and JSON are what they were. Another
// game fills the same fields from its own roster: charaID is its roster
// index, costume and color its own, and the SF4-only fields stay 0
// (EMBER_LIBRARY.md section 6: the wire keeps today's field names).
#include <nlohmann/json.hpp>

#include "SelectionValue.hxx"

namespace sf4e { namespace wire {

struct FixedPoint {
	unsigned short fractional;
	short integral;
};
static_assert(sizeof(FixedPoint) == 4, "SF4's engine reads FixedPoint in place");
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FixedPoint, fractional, integral);

struct ConfirmedCharaConditions {
	unsigned char charaID;
	unsigned char costume;
	unsigned char color;
	unsigned char _unused;
	unsigned char personalAction;
	unsigned char winQuote;
	unsigned char ultraCombo;
	unsigned char handicap;
	unsigned char unc_edition;
};
static_assert(sizeof(ConfirmedCharaConditions) == 9, "SF4's engine reads the pick in place");

inline void to_json(nlohmann::json& j, const ConfirmedCharaConditions& c) {
	j = {{"charaID", c.charaID}, {"costume", c.costume}, {"color", c.color},
		{"_unused", c._unused}, {"personalAction", c.personalAction}, {"winQuote", c.winQuote},
		{"ultraCombo", c.ultraCombo}, {"handicap", c.handicap}, {"unc_edition", c.unc_edition}};
}

inline void from_json(const nlohmann::json& j, ConfirmedCharaConditions& c) {
	using sf4e::selection::ReadByte;
	// Decode into a temporary so a malformed field never partially changes a pick.
	ConfirmedCharaConditions decoded{};
	decoded.charaID = ReadByte(j.at("charaID"));
	decoded.costume = ReadByte(j.at("costume"));
	decoded.color = ReadByte(j.at("color"));
	decoded._unused = ReadByte(j.at("_unused"));
	decoded.personalAction = ReadByte(j.at("personalAction"));
	decoded.winQuote = ReadByte(j.at("winQuote"));
	decoded.ultraCombo = ReadByte(j.at("ultraCombo"));
	decoded.handicap = ReadByte(j.at("handicap"));
	decoded.unc_edition = ReadByte(j.at("unc_edition"));
	c = decoded;
}

} } // namespace sf4e::wire
