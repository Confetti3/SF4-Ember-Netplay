#pragma once
// Which game this build is, for the friends feature: the product's room game
// (ProductIdentity::roomGame). A presence names its game so a friend in
// another game is shown as that and cannot be joined.
#include "../common/ProductIdentity.hxx"
#include <string>

namespace sf4e { namespace netplay {

// The game's id on the wire: 1 to 16 characters of [a-z0-9] ("usf4", "sfv").
inline const char* GameId() { return ember::Product().roomGame; }

// The game's short title for another player's presence ("USF4", "SFV", ...);
// empty for an id this build does not know.
inline const char* GameTitle(const std::string& id) {
	if (id == "usf4") return "USF4";
	if (id == "sfv") return "SFV";
	if (id == "umvc3") return "UMVC3";
	return "";
}

// A presence that names no game is this game's (bridges and builds from
// before the field).
inline bool SameGame(const std::string& id) { return id.empty() || id == GameId(); }

} }
