#pragma once
// What a public room shows of its members' points (PLAYER_CARD.md, "A room
// member's card" and "Levels on the room board"): the one place that decides
// whether a member has points to show and which. The board's seats,
// cabinets and member rows, and the member's card, only ask it.
#include "../netplay/Levels.hxx"
#include <cstdint>
#include <optional>
#include <vector>

namespace sf4e {
namespace room { struct Member; }
namespace ui {
struct ShellView;

// One fighter's points and frame tier, as a picture or a plate draws them.
struct SeatPoints {
    std::uint64_t points = 0;
    std::uint8_t tier = 0;
};

struct MemberPoints {
    // False: draw none (not a public room, no card token or card yet, no points
    // read). A card is public, so every member with a card has points to show.
    bool known = false;
    // Their account points (BP); 0 while no game is counted.
    netplay::levels::CardAccount account;
    // The fighters they have played, most points first (the lower fighter first on a tie).
    std::vector<netplay::levels::CardFighter> fighters;
    // How the card stands with this player: friends, a request either way, blocked. None for this PC's own member.
    netplay::levels::Relation relation = netplay::levels::Relation::None;

    // The points for `fighter`, or none: not known, or not among the fighters played.
    std::optional<SeatPoints> Of(int fighter) const;
};

// The member's points in the room's snapshot `view`. Others' come from their
// card (ShellView::memberCards, by the token in Member::card); this PC's own
// member, while no card was looked up for them, from the player's own points
// (ShellView::levels).
// The member's looked-up card (ShellView::memberCards by the token in Member::card), or null: what the Panel reads their chips,
// record, worn title and families from. Not the player's own member, whose card is their own points.
const netplay::levels::Card* CardOf(const ShellView& view, const room::Member& member);
MemberPoints PointsOf(const ShellView& view, const room::Member& member);
// PointsOf(view, member).Of(fighter) without copying and sorting the card:
// seats, cabinets and plates ask it every frame.
std::optional<SeatPoints> PointsOf(const ShellView& view, const room::Member& member, int fighter);
} }
