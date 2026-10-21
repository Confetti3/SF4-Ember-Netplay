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
// A line between two parts of the board that the player drags, `size` across
// the line's length: dots across its middle, lit with the line while hovered or
// dragged. The caller reads the drag from ImGui's last item.
void DrawGrip(const char* id,ImVec2 at,ImVec2 size);
// A part of the room's column: its heading, which hides or shows the part when
// pressed (`tip` on hover), an arrow in a box before the name. `titleRight` is
// where the name ends. True when pressed.
bool DrawPartHeading(const char* id,const std::string& title,bool shown,bool focused,const std::string& tip,float& titleRight);

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
};
CardInteraction DrawTableCard(const TableCardView& card);

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
} }
