#pragma once
#include "SessionController.hxx"

namespace sf4e { namespace netplay {
// Cover the automatic route through native VS menus. A player still in
// Training or Options must be free to return to the main menu first.
inline bool MatchLoading(MatchState match, bool atMainMenu, bool entered) {
    return match == MatchState::Preparing && (atMainMenu || entered);
}
} }
