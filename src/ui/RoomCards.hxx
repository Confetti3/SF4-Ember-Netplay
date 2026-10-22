#pragma once
// The room board's cards: a battle slot and a member. A card draws the view the
// board prepared and says what the pointer did to it. It reads no room state and
// changes none: the board owns navigation, selection, focus and dispatch, and
// applies the CardInteraction it gets back.
#include "../common/NetworkLink.hxx"
#include "NameMarks.hxx"
#include <imgui.h>
#include <array>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// The row a table card's banner takes, unscaled.
constexpr float TableBannerHeight=22;

// A member's ping (LatencyMark.hxx): its text in its band's colour, and the
// tip that explains it. With neither, nothing is drawn.
struct LatencyMark {
    std::string text,tip;
    // V2's chip: the estimate with its route, "Direct 44 ms"; none while unknown.
    std::string routed;
    ImU32 color=IM_COL32(181,169,155,255);
};

// Text cut to `width` with "...". The full text of anything it cut is added to
// `elided`, one per line, for FlushCardTip.
void DrawCardText(std::string& elided,ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred=false);
// Shows `elided` as a tooltip while the last item is hovered, then clears it.
void FlushCardTip(std::string& elided);
// The button every board row sits on. `focused` outlines it (and scrolls to it
// when `scrollTo`); `disabled` leaves it dead while a choice is open. True when pressed.
bool DrawCardButton(const std::string& id,ImVec2 size,bool focused,bool scrollTo,bool disabled);

// What the pointer did to a card this frame. `option` is the strip button it was
// on, or -1 for the card itself. `pointerMoved`: the mouse moved this frame, so a
// resting cursor can be told from one that chose.
struct CardInteraction {
    enum class Kind { None, Hovered, Pressed };
    Kind kind=Kind::None;
    int option=-1;
    bool pointerMoved=false;
};

// A button on a table card's footer strip: the seat chooser's options, or Ready and Leave on your own place.
// `seat` is the side a sit option would take (-1 when it takes none).
struct TableCardOption {
    std::string label;
    bool live=true;
    int seat=-1;
};
struct TableCardSeat {
    bool occupied=false;
    std::string name,caption;
    // The third line: a win streak (in the accent colour) takes it from idle time.
    std::string streak,idle;
    int fighter=-1;
    NetworkLink link=NetworkLink::Unknown;
    // Its flag and class lead the name.
    NameMarks marks;
    // Ready badge on the portrait, and the banner's outline around it.
    bool ready=false,marked=false;
    // A pill beside the link mark.
    LatencyMark latency;
};
struct TableCardView {
    std::string id;
    int slot=0;
    float height=0;
    std::string phase,middle,banner,footer;
    std::array<TableCardSeat,2> seats;
    // The strip over the footer; none when empty and the footer shows instead.
    // `choosing`: they are the seat chooser's, with `chosen` highlighted.
    std::vector<TableCardOption> options;
    bool choosing=false;
    int chosen=-1;
    bool focused=false,focusChanged=false,disabled=false;
    // The one table of a match's room (BluMint's), drawn as its stage: big portraits and score.
    bool stage=false;
};
CardInteraction DrawTableCard(const TableCardView& card);

// One cabinet on an Arcade room's floor: its number on the marquee, its player
// (name, flag and fighter) or none, and what it is doing. `accent` frames the
// cabinets of one match alike; `crown` marks a player on a streak.
struct CabinetTileView {
    std::string id,name,caption,status;
    int number=0,fighter=-1;
    CountryCode country;
    ImU32 accent=0;
    bool occupied=false,crown=false,focused=false,focusChanged=false,disabled=false;
};
// True when pressed.
bool DrawCabinetTile(const CabinetTileView& tile,ImVec2 size);
// The same cabinet in the Real Arcade view: a tall cabinet with its number
// tag over it and its stool in front, the player on its screen. `empty` is
// the line under an empty cabinet's screen. True when pressed.
bool DrawRealCabinet(const CabinetTileView& tile,ImVec2 size,const std::string& empty);
// The Real Arcade view's backdrop over `min`..`max`: a dark arcade lit by
// neon, with a floor that catches it.
void DrawArcadeBackdrop(ImVec2 min,ImVec2 max);

struct MemberCardView {
    std::string id,name,label,status;
    int mainFighter=-1;
    NetworkLink link=NetworkLink::Unknown;
    // Its flag and class go before the name.
    NameMarks marks;
    // Left of the link mark.
    LatencyMark latency;
    bool focused=false,focusChanged=false,disabled=false;
};
CardInteraction DrawMemberCard(const MemberCardView& card);
// The same member as a V2 list row in `min`-`max`: portrait, player tag, status
// and link mark. The list draws the row's ground, focus and press.
void DrawMemberRow(const MemberCardView& card,ImVec2 min,ImVec2 max);
} }
