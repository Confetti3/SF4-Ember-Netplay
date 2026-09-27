#pragma once
#include <string>

#include "NetworkLink.hxx"

namespace sf4e {
// One fighter of a native match as the HUD shows it. Name and link are taken
// together when the match starts, so a room that has moved on (a new pair at
// the table while a spectator still watches the old game) cannot put one
// player's link beside another player's name.
struct MatchSide {
    std::string name;
    NetworkLink link = NetworkLink::Unknown;
};
}
