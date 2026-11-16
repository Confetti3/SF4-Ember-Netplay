#pragma once
// The room board's parts (Room.dc.html), drawn by ApplicationShell::DrawRoomBoard
// in RoomPanelTables.cxx: the tables' heading, the room's action bar and the
// focused row's line (RoomPanelTables.cxx), a Classic room's table cards
// (RoomTableCards.cxx), an Arcade room's cabinets in their place
// (RoomPanelArcade.cxx), and the Members over Chat column with the layout
// that sizes it (RoomPanelColumn.cxx: ApplicationShell::DrawRoomMembersPanel,
// DrawRoomChatPanel). The rows are built in RoomPanel.cxx.
// Every box that is a row goes through RoomBoardFrame::Press, so the pad
// reaches exactly what the pointer can (V2_KIT.md, Moving on a board).
#include "ApplicationShell.hxx"
#include "RoomControls.hxx"
#include "TableCardShape.hxx"
#include "V2.hxx"
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {

// A seat's fighter as the board shows it (a table card's seat, a cabinet's
// screen): the member's pick, else their main (`picked` false), with the
// costume, colour and Ultra they shared (-1 where not). This PC's own member
// shows this PC's own pick, so it is right as it changes.
// random: the local player's own Random pick, named "Random · Ryu" at their seat.
struct SeatLook { int fighter=-1, costume=-1, color=-1, ultra=-1; bool picked=false, random=false; };
SeatLook SeatLookOf(const ShellView& v,const room::Member& m);
// Settings › Room layout › Pictures.
FighterArt RoomFighterArt(const netplay::PlayerPreferences& preferences);
FighterPicture SeatPicture(const SeatLook& look,FighterArt art);
// "Ryu · Original · Ultra I" (room.seat_look), the fighter alone while the
// rest is not shared, or "Fighter not shared"; `brief` names the Ultra as
// "U1", for a narrow place.
std::string SeatCaption(const SeatLook& look,bool brief=false);
// A cabinet's screen with no cabinet round it (RoomPanelArcade.cxx), for a
// table card's seat: the bezel filling `b`, and on its glass `picture`
// standing on its foot in front of the screen's `tone`, the player's Ultra
// (`ultra`, 0 to 2, else none) in its corner and the scanlines over both, as
// an Arcade room's cabinets show their players. Bare glass while the art has
// no image. Returns the glass.
v2::Box DrawSeatScreen(ImDrawList* d,const v2::Box& b,const FighterPicture& picture,ImU32 tone,int ultra,float s);

// One frame of the board: its rows, the focus, the press being handled and
// where a focused popout row's choices open.
struct RoomBoardFrame {
    const ShellView& v;
    const std::vector<MenuEntry>& rows;
    MenuNavigation& nav;
    MenuAction& action;
    const MenuVisualFeedback& feedback;
    // The focused row this frame, and whether the focus moved since the last.
    std::string focus;
    bool focusChanged;
    // GameMenu::bodyChoices: the focused popout row's box and the column its choices open in.
    decltype(GameMenu::bodyChoices)& choices;
    float s;
    // How the board draws its fighters (RoomFighterArt).
    FighterArt art;

    // The row `id`, or null when the board has none this frame.
    const MenuEntry* Find(const char* id) const;
    // A row's box: a press is Select on it, the moving pointer focuses it.
    // True while hovered. With `choose` false a press only focuses it: a card
    // whose actions are its own buttons (your own place's strip, the offers).
    bool Press(const MenuEntry& e, const v2::Box& b, bool choose = true) const;
    // A box's ground and edge on the board.
    void Ground(ImDrawList* d, const v2::Box& b, ImU32 fill, ImU32 edge) const;
    // A panel's ground: over a watched game the game shows through it.
    void Panel(ImDrawList* d, const v2::Box& b) const;
    // A panel's Hide (Show once folded): a small row button whose top right
    // corner is at `corner`. Returns its width.
    float HideButton(ImDrawList* d, const MenuEntry& e, ImVec2 corner) const;
};

// A Classic room's floor this frame (RoomTableCards.cxx): each card's row,
// table, what it holds and its banner, and whether it is the member's own
// place; how the cards lie at the width they are drawn at, and where they stand.
struct TableFloor {
    struct Card { const MenuEntry* e; const room::Table* t; TableCardContent content; room_controls::TableBanner banner; bool own; };
    std::vector<Card> cards;
    float width = 0;
    TableCardShape shape{1, false, false, 1};
    TableCardPlaces places;
};
// The floor of the board's table rows `width` wide, `gridColumns` across with
// the Grid, its cards filling `height` less `after` (the action bar following
// them in the scroll) by the fill rule (PlaceTableCards): shaped beside the
// scroll's bar where even at their least they do not fit it.
TableFloor ShapeFloor(const RoomBoardFrame& f, const room_controls::PlaceExit& place, float width, float height, float after, int gridColumns, bool grid);
// What follows the cards in their scroll: draws it `width` wide at `at` and
// returns its height. Empty for nothing.
using FloorFollow = std::function<float(ImDrawList*, ImVec2, float)>;
// The floor in `tables`, in its own scroll, `follows` after its cards.
void DrawTableCards(const RoomBoardFrame& f, const v2::Box& tables, const TableFloor& floor, const room_controls::PlaceExit& place, const FloorFollow& follows);

} }
