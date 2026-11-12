#include "RoomPoints.hxx"
#include "ShellView.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
namespace {
// The player's own points as the card a member would show: the account's and
// the fighters they have played.
netplay::levels::Card OwnCard(const netplay::levels::Own& own) {
    netplay::levels::Card card;
    card.account = {own.account.points, own.account.tier};
    for (std::size_t fighter = 0; fighter < own.fighters.size(); ++fighter) {
        const auto& level = own.fighters[fighter];
        if (level.points) card.fighters.push_back({static_cast<std::uint8_t>(fighter), level.points, level.tier});
    }
    return card;
}
}

std::optional<SeatPoints> MemberPoints::Of(int fighter) const {
    if (!known) return std::nullopt;
    for (const auto& played : fighters)
        if (played.fighter == fighter) return SeatPoints{played.points, played.tier};
    return std::nullopt;
}

namespace {
bool OwnPoints(const ShellView& view, const room::Member& member) {
    return member.id == view.room.localMember && view.levels.loaded && view.levels.value.kind == netplay::levels::Own::Kind::Counted;
}
// The member's looked-up card, or none.
const netplay::levels::Card* LookedUp(const ShellView& view, const room::Member& member) {
    if (!member.card.Present() || !view.memberCards) return nullptr;
    const auto found = view.memberCards->find(member.card.ToString());
    return found != view.memberCards->end() ? &found->second : nullptr;
}
}

MemberPoints PointsOf(const ShellView& view, const room::Member& member) {
    MemberPoints out;
    if (!view.room.serverOwned) return out;
    const netplay::levels::Card* card = LookedUp(view, member);
    netplay::levels::Card own;
    if (!card && OwnPoints(view, member)) {
        own = OwnCard(view.levels.value);
        card = &own;
    }
    if (!card) return out;
    out.known = true;
    out.account = card->account;
    out.relation = card->relation;
    for (const auto& played : card->fighters)
        if (played.points) out.fighters.push_back(played);
    std::sort(out.fighters.begin(), out.fighters.end(), [](const netplay::levels::CardFighter& a, const netplay::levels::CardFighter& b) {
        return a.points != b.points ? a.points > b.points : a.fighter < b.fighter;
    });
    return out;
}

std::optional<SeatPoints> PointsOf(const ShellView& view, const room::Member& member, int fighter) {
    if (!view.room.serverOwned) return std::nullopt;
    if (const auto* card = LookedUp(view, member)) {
        for (const auto& played : card->fighters)
            if (played.fighter == fighter && played.points) return SeatPoints{played.points, played.tier};
        return std::nullopt;
    }
    const auto& fighters = view.levels.value.fighters;
    if (!OwnPoints(view, member) || fighter < 0 || static_cast<std::size_t>(fighter) >= fighters.size()) return std::nullopt;
    const auto& level = fighters[static_cast<std::size_t>(fighter)];
    if (!level.points) return std::nullopt;
    return SeatPoints{level.points, level.tier};
}
} }
