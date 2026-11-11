#pragma once
// The room board's parts: a member's row and the text the board's cards cut to
// fit. They draw the view the board prepared; they read no room state and
// change none.
#include "../common/NetworkLink.hxx"
#include "NameMarks.hxx"
#include "FighterArt.hxx"
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
    // The chip: the estimate with its route, "Direct 44 ms"; none while unknown.
    std::string routed;
    ImU32 color=IM_COL32(181,169,155,255);
};

// Text cut to `width` with "...". The full text of anything it cut is added to
// `elided`, one per line, for FlushCardTip.
void DrawCardText(std::string& elided,ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred=false);
// Shows `elided` as a tooltip while the last item is hovered, then clears it.
void FlushCardTip(std::string& elided);

// A button on a table card's footer strip: the seat chooser's options, or Ready and Leave on your own place.
// `seat` is the side a sit option would take (-1 when it takes none).
struct TableCardOption {
    std::string label;
    bool live=true;
    int seat=-1;
};

struct MemberCardView {
    std::string id,name,label,status;
    // Their sets won and lost in the room ("2 W / 1 L"), none before the first:
    // after the status, and on the board's list only where it fits beside it.
    std::string record;
    int mainFighter=-1;
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
