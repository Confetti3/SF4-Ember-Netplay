#include "PlayerOpening.hxx"
#include "../common/FighterCatalog.hxx"

namespace sf4e { namespace ui { namespace player_opening {
namespace {
namespace profiles = netplay::profiles;
// The fighter a row shows, or none for an id the game does not know.
int KnownFighter(int fighter) { return fighter >= 0 && selection::FindFighter(fighter) ? fighter : -1; }
}

Opening OpeningOf(const room::Member& member) {
    Opening opening;
    opening.key = member.card.Present() ? profiles::Key::OfCard(member.card.ToString()) : profiles::Key::OfName(member.name, member.country);
    opening.known.name = member.name;
    opening.known.country = member.country;
    opening.known.badge = member.badge;
    opening.known.fighter = KnownFighter(member.fighter);
    opening.known.steam = member.steamId;
    return opening;
}

Opening OpeningOf(const netplay::friends::Friend& entry) {
    Opening opening;
    opening.key = profiles::Key::OfFriend(entry.id);
    opening.known.name = entry.name;
    opening.known.country = entry.country;
    opening.known.badge = entry.badge;
    opening.known.fighter = entry.hasMain ? KnownFighter(entry.main.fighter) : -1;
    return opening;
}

Opening OpeningOf(const netplay::friends::Incoming& request) {
    Opening opening;
    opening.key = profiles::Key::OfRequest(request.id);
    opening.known.name = request.name;
    opening.known.country = request.country;
    return opening;
}

Opening OpeningOf(const netplay::rankings::BoardEntry& entry) {
    Opening opening;
    opening.key = profiles::Key::OfName(entry.listing.name, entry.listing.country);
    opening.known.name = entry.listing.name;
    opening.known.country = entry.listing.country;
    opening.known.badge = SkillBadge::OfClass(entry.band, true);
    return opening;
}

Opening OpeningOf(const profiles::Hit& hit) {
    Opening opening;
    opening.key = profiles::Key::OfCard(hit.card);
    opening.known.name = hit.name;
    opening.known.country = hit.country;
    opening.known.badge = hit.badge;
    opening.known.fighter = hit.hasMain ? KnownFighter(hit.main) : -1;
    return opening;
}

Opening OpeningOf(const profiles::Recent& recent) {
    Opening opening;
    opening.key = profiles::Key::OfCard(recent.card);
    opening.known.name = recent.name;
    opening.known.country = recent.country;
    opening.known.fighter = KnownFighter(recent.fighter);
    return opening;
}

Opening OpeningOfSide(const std::string& name, const CountryCode& country, const SkillBadge& badge, int fighter) {
    Opening opening;
    opening.key = profiles::Key::OfName(name, country);
    opening.known.name = name;
    opening.known.country = country;
    opening.known.badge = badge;
    opening.known.fighter = KnownFighter(fighter);
    return opening;
}

} } }
