#pragma once
// The room board's parts: a member's row and the text the board's cards cut to
// fit. They draw the view the board prepared; they read no room state and
// change none.
#include "../common/NetworkLink.hxx"
#include "NameMarks.hxx"
#include "FighterArt.hxx"
#include "Theme.hxx"
#include "RoomPoints.hxx"
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// A member's ping (LatencyMark.hxx): its text in its band's colour, and the
// tip that explains it. With neither, nothing is drawn.
struct LatencyMark {
    std::string text,tip;
    // The chip with its route where the row has room, "Relay ~80 ms" or a
    // checked "Direct 9 ms"; none where the plain text says all of it.
    std::string routed;
    ImU32 color=palette::Muted;
};

// Text cut to `width` with "...". The full text of anything it cut is added to
// `elided`, one per line, for FlushCardTip.
void DrawCardText(std::string& elided,ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred=false);
// Shows `elided` as a tooltip while the last item is hovered, then clears it.
void FlushCardTip(std::string& elided);


// A question a table card asks before it acts, answered on the card:
// whether to do `offer` at `table`, or "stay". Leaving a seat that costs
// something asks LeaveSeat; an offer that would end the game this PC
// watches asks that offer. `asked`: the question is open (MenuNavigation::Ask).
// A new room, or leaving the board, drops it.
struct TableAsk {
    static constexpr const char* LeaveSeat="leave-seat";
    int table=-1;
    std::string offer;
    bool asked=false;
    // The table whose card asks whether to leave its seat, or -1.
    int Leaving() const { return offer==LeaveSeat?table:-1; }
};

struct MemberCardView {
    std::string id,name,label,status;
    // Their sets won and lost in the room ("2 W / 1 L"), none before the first:
    // after the status, and on the board's list only where it fits beside it.
    std::string record;
    // The fighter the row shows: the member's main (-1: none shared), in the
    // original outfit unless a caller names a costume and colour.
    int fighter=-1,costume=0,color=0;
    // The main's points, drawn as a band across its face (PointsOf); none when the room shows none.
    std::optional<SeatPoints> points;
    // Settings › Room layout › Pictures. A main has no costume of its own, so
    // Characters draws it in the original outfit.
    FighterArt art=FighterArt::Portrait;
    NetworkLink link=NetworkLink::Unknown;
    // Its flag and class go before the name.
    NameMarks marks;
    // Left of the link mark.
    LatencyMark latency;
};
// A member as a list row in `min`-`max`: portrait, player tag, status
// and link mark. The list draws the row's ground, focus and press.
void DrawMemberRow(const MemberCardView& card,ImVec2 min,ImVec2 max);
struct ShellView;
struct OtherCardView;
// A member's card on their page (PROFILE.md, "Another player's card"), for
// DrawOtherCard: the fighter they are playing on its tier edge, their flag,
// name and class, SF4's two lines (that fighter's points and their account's,
// 0 for a member with no card), and how they stand with this player as the
// chip. A private room shows no points lines.
OtherCardView MemberOtherCard(const ShellView& view,const room::Member& member,FighterArt art);
} }
