#include "RoomPoints.hxx"
#include "ShellView.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
namespace {
// The player's own points as the card a member would show: the fighters they have played.
netplay::levels::Card OwnCard(const netplay::levels::Own& own) {
    netplay::levels::Card card;
    card.levelsShown = true;
    for (std::size_t fighter = 0; fighter < own.fighters.size(); ++fighter) {
        const auto& level = own.fighters[fighter];
        if (level.points) card.fighters.push_back({static_cast<std::uint8_t>(fighter), level.points, level.tier});
    }
    return card;
}
}

std::optional<SeatPoints> MemberPoints::Of(int fighter) const {
    if (show != Show::Points) return std::nullopt;
    for (const auto& played : fighters)
        if (played.fighter == fighter) return SeatPoints{played.points, played.tier};
    return std::nullopt;
}

MemberPoints PointsOf(const ShellView& view, const room::Member& member) {
    MemberPoints out;
    if (!view.room.serverOwned) return out;
    const netplay::levels::Card* card = nullptr;
    if (member.card.Present() && view.memberCards) {
        const auto found = view.memberCards->find(member.card.ToString());
        if (found != view.memberCards->end()) card = &found->second;
    }
    netplay::levels::Card own;
    if (!card && member.id == view.room.localMember && view.levels.loaded && view.levels.value.kind == netplay::levels::Own::Kind::Counted) {
        own = OwnCard(view.levels.value);
        card = &own;
    }
    if (!card) return out;
    if (!card->levelsShown) {
        out.show = MemberPoints::Show::Private;
        return out;
    }
    out.show = MemberPoints::Show::Points;
    for (const auto& played : card->fighters)
        if (played.points) out.fighters.push_back(played);
    std::sort(out.fighters.begin(), out.fighters.end(), [](const netplay::levels::CardFighter& a, const netplay::levels::CardFighter& b) {
        return a.points != b.points ? a.points > b.points : a.fighter < b.fighter;
    });
    return out;
}
} }
