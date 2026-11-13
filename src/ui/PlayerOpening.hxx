#pragma once
// Who a row opens when it names a player (docs/design/PROFILE.md, "Where it opens from"): the key a list holds them by and
// what the row already knows of them, as ApplicationShell::OpenPlayer takes them. One function for each kind of row, so no
// screen builds a Known of its own: a room member, a friend, a request, a ranking row, a search hit, a recent opponent,
// and a side of a game a replay or an archived set lists.
#include "../common/Country.hxx"
#include "../common/SkillBadge.hxx"
#include "../netplay/Friends.hxx"
#include "../netplay/Profiles.hxx"
#include "../netplay/Rankings.hxx"
#include "../session/RoomTypes.hxx"
#include <string>

namespace sf4e { namespace ui { namespace player_opening {

struct Opening {
    netplay::profiles::Key key;
    netplay::profiles::Known known;
};

// A room member: by their card token, else (a private room, a member with no card) by their name and flag. Their picture is
// the fighter they picked in the room, and a Steam profile they share is offered.
Opening OpeningOf(const room::Member& member);
// A friend by their friendship's id, shown with their main.
Opening OpeningOf(const netplay::friends::Friend& entry);
// A request received, by its id.
Opening OpeningOf(const netplay::friends::Incoming& request);
// A row of the rankings board: by name and flag, with the class the board lists them in.
Opening OpeningOf(const netplay::rankings::BoardEntry& entry);
// A player Find a player found, by the card token the search gave.
Opening OpeningOf(const netplay::profiles::Hit& hit);
// A player this PC met lately, by the card token kept for them, shown with the fighter they played.
Opening OpeningOf(const netplay::profiles::Recent& recent);
// A side of a game a list shows (a replay on this PC, a set of the public archive), by its exact name and flag. `fighter` is
// the native fighter id the side played, -1 or an id the game does not know for none.
Opening OpeningOfSide(const std::string& name, const CountryCode& country, const SkillBadge& badge, int fighter);

} } }
